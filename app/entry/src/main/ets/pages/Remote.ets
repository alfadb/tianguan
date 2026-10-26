/**
 * Remote desktop page — one instance per session, hosted in its own independent
 * UIAbility window (RemoteAbility, launchType "multiton"; the system-assigned
 * window name is tianguanN).
 *
 * The session this page drives is NEVER guessed. The page reads its own window
 * name from its UIContext and resolves it through the AppStorage binding
 * RemoteAbility wrote before loadContent(): remote.session.<windowName> =
 * sessionId. Router params are still accepted for compatibility (legacy launch
 * paths), but a value conflicting with that binding fails closed. When neither
 * yields an id the page fails closed: the error is shown and every input path
 * stays unmounted. There is no "last window", FIFO or "first registry session"
 * fallback.
 *
 * The page is pure picture: no title row, no disconnect button, no overlay and
 * no padding, so nothing covers the remote picture. The connection name belongs
 * to the window title (set by RemoteAbility). On a desktop the window's
 * title-bar X is the disconnect path. A phone has no title bar, so there the
 * system back action (onBackPress) is the disconnect: it ends this session and
 * closes this window instead of merely backgrounding it, through the same
 * programmatic path the connection list uses. Losing the view without a back
 * action (minimise, another window on top) still must NOT disconnect.
 *
 * Presentation (USE_GL_RENDER = true): an XComponent(SURFACE) whose surface is
 * attached, per session, to the native EGL/GLES renderer (app/native/render.cxx
 * — attachRenderSurface/detachRenderSurface). The render thread IS the frame
 * consumer: it pulls the BGRA mirror through takeFrameStruct, uploads changed
 * rectangles into one fixed texture and presents with eglSwapBuffers at the
 * surface's vsync. That path replaced the Canvas + per-frame drawImage
 * pipeline, which made RenderService cache a texture plus a Vulkan buffer per
 * picture version until the 4096 MB application GPU quota killed the process
 * (see .work/glspike/REPORT.md). ArkTS therefore never calls takeFrame in this
 * mode; it only handles status, the trust panel, the clipboard edge, input and
 * the local cursor overlay. Setting USE_GL_RENDER = false restores the Canvas
 * path unchanged (both paths must never be mounted at the same time).
 *
 * View transform (GL mode): a two-finger pinch zooms about the pinch centre
 * (zoom 1 = the contain fit, never smaller) and a two-finger drag pans; both go
 * through the native setViewTransform (coalesced to one call per frame-ish) and
 * are clamped again by the renderer. Input mapping does not recompute the
 * contain rectangle: it converts vp to surface pixels and uses the geometry the
 * renderer reports in renderStats (contentLeft/contentTop/contentScale*), so
 * clicks and the local cursor stay exact at any zoom/pan. A session
 * (re)connect, a surface attach or a resolution change resets the transform to
 * the contain fit, and a window resize re-clamps the pan to the new surface.
 * Two-finger touches are never forwarded as remote pointer input: a
 * single-finger touch only presses the remote button after a short delay, so
 * the second finger of a pinch can no longer turn that press into a click.
 * Three or more fingers are ignored completely (no view transform, no remote
 * pointer event): those sequences belong to the system.
 *
 * Everything talks to the native layer through diag/SessionApi.ets with THIS
 * session's id; the status is parsed as JSON per docs/napi-contract.md (never
 * the legacy "key=value" text). The 250 ms poll (30 ms on the Canvas path)
 * drives the status text, the trust-required panel and the clipboard edge
 * (clipboard.version -> clipboard/TianguanClipboard); frames no longer depend
 * on it in the GL mode. The local cursor follows pointer events directly; the
 * poll only re-places it after a shape or size change. Two 2 s debug-level
 * hilog summaries report the render statistics ("TianguanPerf") and the poll
 * stage timings ("TianguanPoll": sessionStatus / applyStatus /
 * pollRenderStats). The poll slows to 1 s
 * (status only) while the page is hidden, and the native renderer is parked
 * for that time.
 */
import { hilog } from '@kit.PerformanceAnalysisKit';
import { image } from '@kit.ImageKit';
import { systemDateTime } from '@kit.BasicServicesKit';
import { KeyCode } from '@ohos.multimodalInput.keyCode';
import pointer from '@ohos.multimodalInput.pointer';
import { display, window } from '@kit.ArkUI';
import {
  copyCursor, parseFrameMeta, reconnectSession, releaseSessionInput, requestFullFrame,
  sendKey, sendPointer, sessionStatus, takeFrameRaw,
  attachRenderSurface, detachRenderSurface, renderStats, setViewTransform, suspendRenderSurface,
  SESSION_STATE_CONNECTED, SESSION_STATE_TRUST_REQUIRED
} from '../diag/SessionApi';
import type { CursorInfo, FrameMeta, SessionStatus } from '../diag/SessionApi';
import { findSession, markWindowOpen, closeSessionById } from '../model/SessionRegistry';
import { findConnection, pinKeyFor, trustFingerprint } from '../model/ConnectionStore';
import { onRemoteClipboardVersion, clipboardRegisterSession,
  clipboardUnregisterSession, clipboardSetSessionVisible,
  clipboardRequestReadPermission } from '../clipboard/TianguanClipboard';
import { closeRemoteWindow, sessionKeyForWindow } from '../windows/RemoteWindows';

const DOMAIN: number = 0x0000;
const TAG: string = 'TianguanDiag';
// Presentation switch: true = XComponent(SURFACE) + native EGL renderer (the
// default), false = the previous Canvas + per-frame drawImage path (kept for
// comparison and rollback; it leaks one RenderService texture per frame on
// this HarmonyOS build). Only one path is ever mounted.
const USE_GL_RENDER: boolean = true;
// Status poll while visible. The GL path needs no frame polling (the native
// renderer pulls frames itself), so it only watches status/cursor/clipboard;
// the Canvas path keeps its historical 30 ms frame cadence.
const POLL_INTERVAL_MS: number = USE_GL_RENDER ? 250 : 30;
// Two-finger view gestures (GL mode only). setViewTransform is a synchronous
// NAPI call, so gesture updates are coalesced to at most one call per interval
// instead of one per onActionUpdate.
const VIEW_SEND_INTERVAL_MS: number = 33;
// After a transform reset a poll can still return stats produced BEFORE the
// reset; for this long the local zoom/pan must not be re-adopted from them.
const VIEW_SYNC_GUARD_MS: number = 600;
// Used until the renderer reports its own maxZoom.
const MAX_VIEW_ZOOM_FALLBACK: number = 4;
// Touch path only: a single finger does not press the remote button on
// TouchType.Down. The press is deferred until the touch's intent is known:
//   - Move past TAP_SLOP_VP   -> left-button drag (press, then the moves)
//   - Up while still pending  -> quick tap (press + release together)
//   - hold still for LONG_PRESS_MS -> right click, the phone's only way to
//     open a remote context menu and paste without a physical keyboard
// A second finger cancels the pending press for the whole gesture (pressing on
// Down and releasing when the pinch started was one complete click on the
// remote: the reported "pinch clicks the remote desktop" bug).
const LONG_PRESS_MS: number = 550;
// Finger travel (vp) that turns a pending tap into a left-button drag. Below
// it the touch is still a tap or a long press; the long-press timer re-checks
// it when it fires, so a drag can never turn into a right click.
const TAP_SLOP_VP: number = 3;
// Three or more fingers belong to the system (screenshot, split screen, ...):
// from a third finger until every finger is up this page neither transforms
// the view nor sends any remote pointer event.
const MIN_BLOCKED_FINGERS: number = 3;
// PC turns one physical mouse action into a real MouseEvent AND a synthetic
// TouchEvent on the same component; handling both forwarded the click twice.
// Only that one touch is the mouse's duplicate, so it is dropped when it
// arrives within this window of the last real MouseEvent and starts no further
// than MOUSE_TOUCH_DEDUPE_VP (vp) from the mouse point. A real finger on a
// touchscreen is neither that close nor that close in time to a mouse action,
// and two or more fingers never have a mouse counterpart at all.
const MOUSE_TOUCH_DEDUPE_MS: number = 150;
const MOUSE_TOUCH_DEDUPE_VP: number = 10;

interface RemoteRouterParams {
  sessionId?: string;
}

/**
 * Presented rectangle of the remote framebuffer, in surface physical pixels
 * with a top-left origin. Mirrors tianguan::ViewGeometry in
 * app/native/view_geometry.h (the renderer's single source of truth).
 */
interface ViewGeometry {
  zoom: number;
  left: number;
  top: number;
  contentW: number;
  contentH: number;
  scaleX: number;  // surface pixels per remote pixel
  scaleY: number;
  panX: number;    // effective pan after clamping (surface pixels)
  panY: number;
}

/**
 * XComponentController subclass: surface lifetime events arrive as method
 * overrides (since API 12), not as component attributes. The page installs
 * plain callbacks so the controller stays decoupled from the struct.
 */
class RemoteSurfaceController extends XComponentController {
  onSurfaceReady: (surfaceId: string) => void = () => {};
  onSurfaceGone: (surfaceId: string) => void = () => {};

  onSurfaceCreated(surfaceId: string): void {
    this.onSurfaceReady(surfaceId);
  }

  onSurfaceDestroyed(surfaceId: string): void {
    this.onSurfaceGone(surfaceId);
  }
}

@Entry
@Component
struct Remote {
  @State private statusText: string = '连接中…';
  // True once bootstrap() knows which session this page shows: from then on the
  // Canvas is always part of the tree. It must never wait for a first frame,
  // because the first frame can only arrive through this Canvas.
  @State private sessionResolved: boolean = false;
  // Resolution failed: no session can be attributed to this page. Input stays
  // unmounted and the only action is going back to the list.
  @State private resolutionFailed: boolean = false;
  // "There is already drawable content": only used for secondary UI (the
  // waiting hint and the local cursor layer), never to gate the Canvas.
  @State private canvasShown: boolean = false;
  @State private cursorPm: image.PixelMap | undefined = undefined;
  @State private cursorLeft: number = -1;
  @State private cursorTop: number = -1;
  @State private cursorDrawW: number = 0;
  @State private cursorDrawH: number = 0;
  // Latest cursor shape/hotspot from the server. The position is recomputed from
  // these on every pointer event; it is not persisted in @State because only
  // the rendered rectangle needs to trigger a UI update.
  private cursorShapeW: number = 0;
  private cursorShapeH: number = 0;
  private cursorHotX: number = 0;
  private cursorHotY: number = 0;
  private cursorUsable: boolean = false;
  @State private fingerprint: string = '';
  @State private needsTrust: boolean = false;
  private settings: RenderingContextSettings = new RenderingContextSettings(true);
  private ctx: CanvasRenderingContext2D = new CanvasRenderingContext2D(this.settings);
  private canvasReady: boolean = false;
  private sessionId: string = '';
  private framePixels: image.PixelMap | undefined = undefined;
  private frameReady: boolean = false;
  private regionBuffer: ArrayBuffer = new ArrayBuffer(0);
  @State private frameWidth: number = 0;
  @State private frameHeight: number = 0;
  private viewWidth: number = 0;
  private viewHeight: number = 0;
  // Last size reported by onAreaChange. A pure window move also fires the
  // callback; only a changed width/height loses the Canvas and needs a full
  // frame.
  private lastAreaWidth: number = -1;
  private lastAreaHeight: number = -1;
  private lastVersion: number = -1;
  private forceFull: boolean = true;
  private lastGeneration: number = -1;
  private lastCursorVersion: number = -1;
  private lastClipboardVersion: number = -1;
  private badStatusCount: number = 0;
  private timer: number = -1;
  private idleTimer: number = -1;
  // Buttons currently held, in RFB mask form (0 = none).
  private pressedMask: number = 0;
  private lastTapMs: number = 0;
  private lastTapX: number = -1;
  private lastTapY: number = -1;
  private shiftDown: boolean = false;
  private pressedKeysym: Map<number, number> = new Map<number, number>();
  // Last remote pixel the pointer was actually at; the release uses it when the
  // physical pointer is outside the picture (letterbox) by then.
  private lastRemoteX: number = -1;
  private lastRemoteY: number = -1;
  private disposed: boolean = false;
  private tornDown: boolean = false;
  // A back press already asked this window to disconnect/close. A second back
  // press while that is still in flight must not be swallowed again.
  private backCloseRequested: boolean = false;
  private pageVisible: boolean = true;
  private lastState: string = '';
  // Single in-flight render: the Canvas 30 ms poll must never start
  // overlapping draws.
  private applying: boolean = false;
  // Bumped whenever the local mirror is invalidated; a render that started
  // under an older epoch must not commit its version or draw.
  private renderEpoch: number = 0;
  // PixelMaps dropped while a render was inside writePixels(); released when
  // that render returns so a pixel map is never released under an active write.
  private staleFramePixels: image.PixelMap[] = [];
  private ownWindowName: string = '';
  private ownWindow: window.Window | undefined = undefined;
  private windowEventCallback: ((event: window.WindowEventType) => void) | undefined = undefined;
  private lastTouchX: number = -1;
  private lastTouchY: number = -1;
  private viewPointerX: number = -1;
  private viewPointerY: number = -1;
  // True once a real MouseEvent arrived (diagnostics only: the touch path is no
  // longer disabled by it, single-finger touches are de-duplicated against the
  // mouse instead, see isMouseDuplicateTouch()).
  private hasMouse: boolean = false;
  // Last real MouseEvent, used to recognise the synthetic TouchEvent PC derives
  // from the same mouse action. Time in ms (0 = no mouse event yet) and the
  // event point in vp, the same coordinate space onTouch reports.
  private lastMouseEventMs: number = 0;
  private lastMouseX: number = -1;
  private lastMouseY: number = -1;
  // --- Rendering timings -------------------------------------------------
  // Rolling accumulators (microseconds / counts). A summary is logged every
  // 2 s; nothing is allocated per frame and no timing call changes behaviour.
  private perfWindowStart: number = 0;
  private perfPollLast: number = 0;
  private perfPollSum: number = 0;
  private perfPollCount: number = 0;
  private perfPollMax: number = 0;
  private perfTakeSum: number = 0;
  private perfTakeCount: number = 0;
  private perfTakeMax: number = 0;
  private perfWriteSum: number = 0;
  private perfWriteCount: number = 0;
  private perfWriteMax: number = 0;
  private perfDrawSum: number = 0;
  private perfDrawCount: number = 0;
  private perfDrawMax: number = 0;
  // Whole pollOnce() call and its three stages: sessionStatus (sync NAPI +
  // JSON parse), applyStatus (ArkUI @State writes) and pollRenderStats (sync
  // NAPI read). Logged as the debug-level "TianguanPoll" line every 2 s; the
  // poll cadence itself is unchanged (250 ms on the GL path, 30 ms on Canvas).
  private perfOnceSum: number = 0;
  private perfOnceCount: number = 0;
  private perfOnceMax: number = 0;
  private perfStatusSum: number = 0;
  private perfStatusCount: number = 0;
  private perfStatusMax: number = 0;
  private perfApplySum: number = 0;
  private perfApplyCount: number = 0;
  private perfApplyMax: number = 0;
  private perfRenderSum: number = 0;
  private perfRenderCount: number = 0;
  private perfRenderMax: number = 0;
  private perfApplyingSkips: number = 0;
  private perfAppliedFrames: number = 0;
  private perfFullFrames: number = 0;
  // Last values logged by the 2 s summary: whether the server-drawn cursor is
  // blank (0x0 shape means the server painted the pointer into the picture),
  // the latest cursor shape size, and the last applied update rectangle. These
  // are log-only mirrors; they never feed back into rendering or input.
  private lastCursorBlank: boolean = true;
  private lastCursorW: number = 0;
  private lastCursorH: number = 0;
  private lastMetaX: number = -1;
  private lastMetaY: number = -1;
  private lastMetaW: number = 0;
  private lastMetaH: number = 0;
  private lastMetaFull: boolean = false;
  // --- Native GL presenter (USE_GL_RENDER) --------------------------------
  private glController: RemoteSurfaceController = new RemoteSurfaceController();
  // Surface currently attached to the native renderer ('' = none). Attaching
  // twice to the same surface would restart the render thread needlessly.
  private glAttachedSurfaceId: string = '';
  // Last surface id this page saw, kept so a failed renderer can be re-tried
  // even when no new surface event arrives (the surface still exists).
  private glLastSurfaceId: string = '';
  // Bounded re-attach attempts after a renderer failure; reset as soon as
  // the renderer is running again.
  private glReattachCount: number = 0;
  // Latest renderer snapshot, for the 2 s summary line and first-frame gating.
  private renderState: string = '';
  private renderFps: number = 0;
  private renderFrames: number = 0;
  private renderUploads: number = 0;
  private renderSkips: number = 0;
  private renderSwapFailures: number = 0;
  private renderTexW: number = 0;
  private renderTexH: number = 0;
  private lastRenderError: string = '';
  // --- Two-finger view transform (USE_GL_RENDER) ---------------------------
  // zoom is a multiple of the contain fit (1 = the old letterboxed layout,
  // never below 1); panX/panY are surface-pixel offsets from the centred
  // position. Native is the authority (it clamps both while drawing); these
  // fields hold what this page asked for, and are re-synced from renderStats
  // once the renderer has drawn it.
  private viewZoom: number = 1;
  private viewPanX: number = 0;
  private viewPanY: number = 0;
  private viewMaxZoom: number = MAX_VIEW_ZOOM_FALLBACK;
  // Presented geometry from the last renderStats(): exactly what is on screen.
  // Input mapping (and the local cursor overlay) read these values; the contain
  // layout is never recomputed locally while a snapshot is available.
  private statsGeometryValid: boolean = false;
  private statsSurfaceW: number = 0;
  private statsSurfaceH: number = 0;
  private statsContentLeft: number = 0;
  private statsContentTop: number = 0;
  private statsContentScale: number = 0;
  private statsContentScaleY: number = 0;
  private statsContentW: number = 0;
  private statsContentH: number = 0;
  // Last resort vp->surface-pixel factor when no stats snapshot exists yet.
  private densityPixels: number = 0;
  private viewSyncGuardUntil: number = 0;
  private viewSendTimer: number = -1;
  private viewSendAt: number = 0;
  private geometryRefreshTimer: number = -1;
  private lastViewTransformError: string = '';
  // Live two-finger gesture. The pinch and the two-finger pan recognizers run
  // in parallel; the last event of either re-applies the same combined state.
  private gestureActive: boolean = false;
  private gestureStartZoom: number = 1;
  private gestureStartPanX: number = 0;
  private gestureStartPanY: number = 0;
  private gestureAnchorValid: boolean = false;
  private gestureAnchorRemoteX: number = 0;
  private gestureAnchorRemoteY: number = 0;
  private gestureCenterSurfX: number = 0;
  private gestureCenterSurfY: number = 0;
  private gestureScale: number = 1;
  private gesturePanRawX: number = 0;
  private gesturePanRawY: number = 0;
  private gesturePanBaseX: number = 0;
  private gesturePanBaseY: number = 0;
  // True while two fingers are down: single-finger remote pointer input is
  // frozen (and any held button released) until a fresh one-finger touch.
  private multiTouchActive: boolean = false;
  // --- Touch press / long press / three-finger block -----------------------
  // Identifier of every finger currently down, maintained from changedTouches
  // (Down adds, Up removes, Cancel clears) and healed from touches on
  // Down/Move. event.touches can be empty on Up/Cancel, so the finger count
  // that gates the deferred press and the three-finger block comes from here.
  private touchFingerIds: Set<number> = new Set<number>();
  // Deferred single-finger press: armed on Down and resolved by the first Move
  // past TAP_SLOP_VP (press now, drag), by Up (tap: press + release) or by the
  // long-press timer (right click). A second finger cancels it.
  private pressPending: boolean = false;
  // True once the deferred left press went out; the matching release is sent
  // on Up.
  private pressSent: boolean = false;
  private longPressTimer: number = -1;
  // True once the timer fired and the right click went out: no pointer event
  // is sent any more until the finger lifts, so a long press never adds a
  // stray left click on release.
  private longPressFired: boolean = false;
  // Down point in vp; TAP_SLOP_VP is measured from here.
  private touchDownX: number = -1;
  private touchDownY: number = -1;
  // True from the third finger until every finger is up: pinch/two-finger pan
  // callbacks return immediately (the transform is frozen where it was) and no
  // remote pointer event is sent.
  private multiTouchBlocked: boolean = false;

  aboutToAppear(): void {
    this.glController.onSurfaceReady = (surfaceId: string): void => {
      this.attachRendererSurface(surfaceId);
    };
    this.glController.onSurfaceGone = (surfaceId: string): void => {
      if (this.glAttachedSurfaceId === surfaceId) {
        this.detachRendererSurface('surface-destroyed');
      }
    };
    this.bootstrap();
  }

  onPageShow(): void {
    this.setPageVisible(true);
  }

  onPageHide(): void {
    this.setPageVisible(false);
  }

  private async bootstrap(): Promise<void> {
    const resolvedId = await this.resolveSessionId();
    // The page can be destroyed while the id is being resolved (e.g. the
    // half-built window of a failed open). A dead page must not register
    // clipboard hooks or start timers.
    if (this.disposed) {
      return;
    }
    this.sessionId = resolvedId;
    if (this.sessionId.length === 0) {
      this.resolutionFailed = true;
      this.statusText = '无法确定本页所属的会话（窗口身份未登记）';
      hilog.error(DOMAIN, TAG, 'remote 页面无法确定会话归属，已禁用输入');
      return;
    }
    hilog.info(DOMAIN, TAG, 'remote 页面启动 session=%{public}s window=%{public}s',
      this.sessionId, this.ownWindowName);
    markWindowOpen(this.sessionId, true);
    clipboardRegisterSession(this.sessionId);
    // onPageShow may have run (or not) before the session id was known; keep the
    // clipboard visibility in sync with the page's real visibility either way.
    clipboardSetSessionVisible(this.sessionId, this.pageVisible);
    // READ_PASTEBOARD is user_grant: ask once per process with this page's host
    // context. Until it is granted the clipboard module does not read at all.
    this.requestClipboardReadPermission();
    this.attachWindowEvents();
    // Mount the Canvas now; onReady() can fire (and frames can start flowing)
    // even before the first connection status arrives.
    this.sessionResolved = true;
    this.startPolling();
  }

  /**
   * Runtime READ_PASTEBOARD request (one dialog per process; the clipboard
   * module ignores repeated calls). Without a host context the request is
   * skipped and the clipboard module stays gated — it never reads without the
   * permission.
   */
  private requestClipboardReadPermission(): void {
    try {
      const context = this.getUIContext().getHostContext();
      if (context === undefined) {
        hilog.warn(DOMAIN, TAG, 'remote 页面拿不到 host context，跳过剪贴板读取授权申请');
        return;
      }
      clipboardRequestReadPermission(context);
    } catch (error) {
      hilog.warn(DOMAIN, TAG,
        'remote 页面申请剪贴板读取权限失败：%{public}s', String(error));
    }
  }

  aboutToDisappear(): void {
    // Losing the view is NOT itself a user close: the session keeps running
    // until RemoteAbility.onDestroy decides otherwise (window X = disconnect).
    // This page never closes it here.
    this.disposed = true;
    this.teardown();
  }

  private teardown(): void {
    if (this.tornDown) {
      return;
    }
    this.tornDown = true;
    this.stopFrameTimer();
    this.stopIdleTimer();
    // Drop pending view-transform work before the surface goes away: the
    // timers would otherwise fire against a detached renderer.
    this.cancelViewSendTimer();
    this.cancelGeometryRefresh();
    this.cancelLongPress('teardown');
    this.resetTouchPressState();
    this.touchFingerIds.clear();
    this.multiTouchBlocked = false;
    this.multiTouchActive = false;
    this.gestureActive = false;
    this.statsGeometryValid = false;
    this.detachWindowEvents();
    this.detachRendererSurface('teardown');
    this.dropFramePixels();
    this.releaseCursorPixelMap();
    if (this.sessionId.length === 0) {
      return;
    }
    this.releaseAllInputs('teardown');
    clipboardUnregisterSession(this.sessionId);
    markWindowOpen(this.sessionId, false);
    this.setHostCursorVisible(true);
  }

  // -------------------------------------------------------------------------
  // Session id resolution. Exactly two evidence sources, in order:
  //   1) this page's own window name -> the AppStorage binding written by
  //      RemoteAbility before loadContent();
  //   2) router params — kept for compatibility with legacy launch paths.
  // A value that conflicts with the window binding is an identity conflict, not
  // something to pick from. Anything else is a guess and deliberately absent.
  // -------------------------------------------------------------------------

  private async resolveSessionId(): Promise<string> {
    // Which window is this page actually in? getWindowName() answers for THIS
    // page's UIContext (@ohos.arkui.UIContext.d.ts, API 12), unlike
    // getLastWindow(), which returns the topmost window and is therefore not
    // evidence about the window hosting this page.
    let windowName = '';
    try {
      const name = this.getUIContext().getWindowName();
      if (name !== undefined && name.length > 0) {
        windowName = name;
      }
    } catch (error) {
      hilog.warn(DOMAIN, TAG, 'remote 读取本窗口名失败：%{public}s', String(error));
    }
    this.ownWindowName = windowName;
    // 1) The ability published this window's session BEFORE loadContent().
    let windowSession = '';
    if (windowName.length > 0) {
      const mapped = AppStorage.get<string>(sessionKeyForWindow(windowName));
      if (mapped !== undefined && mapped.length > 0) {
        windowSession = mapped;
      }
    }
    // 2) Router params (legacy paths). They must AGREE with the window binding
    // when both exist; otherwise the page cannot know which identity is real.
    let routedSession = '';
    try {
      const params = this.getUIContext().getRouter().getParams() as RemoteRouterParams | undefined;
      if (params !== undefined && params !== null &&
          typeof params.sessionId === 'string' && params.sessionId.length > 0) {
        routedSession = params.sessionId;
      }
    } catch (error) {
      // No router context — the window binding above still applies.
    }
    if (windowSession.length > 0 && routedSession.length > 0 && windowSession !== routedSession) {
      hilog.error(DOMAIN, TAG, 'remote 窗口身份冲突 window=%{public}s bound=%{public}s routed=%{public}s',
        windowName, windowSession, routedSession);
      return '';
    }
    if (windowSession.length > 0) {
      return windowSession;
    }
    // Neither source answers: fail closed. No FIFO/registry fallback.
    return routedSession;
  }

  private setHostCursorVisible(visible: boolean): void {
    try {
      pointer.setPointerVisible(visible).catch((error: Error): void => {
        hilog.error(DOMAIN, TAG, '光标可见性设置失败：%{public}s', String(error));
      });
    } catch (error) {
      hilog.error(DOMAIN, TAG, '光标可见性异常：%{public}s', String(error));
    }
  }

  // -------------------------------------------------------------------------
  // Page visibility and window focus.
  // -------------------------------------------------------------------------

  private attachWindowEvents(): void {
    const name = this.ownWindowName;
    if (name.length === 0) {
      // No named window on a host that does not expose one: the page lifecycle
      // below still covers hide/show and teardown.
      return;
    }
    try {
      // findWindow() is synchronous and throws when the name is unknown; the
      // name came from this page's own UIContext, so this is THIS window.
      const own = window.findWindow(name);
      const callback = (event: window.WindowEventType): void => {
        this.onWindowEvent(event);
      };
      own.on('windowEvent', callback);
      this.ownWindow = own;
      this.windowEventCallback = callback;
    } catch (error) {
      hilog.warn(DOMAIN, TAG, '监听窗口事件失败（改用页面生命周期）：%{public}s', String(error));
    }
  }

  private detachWindowEvents(): void {
    const own = this.ownWindow;
    const callback = this.windowEventCallback;
    this.ownWindow = undefined;
    this.windowEventCallback = undefined;
    if (own === undefined || callback === undefined) {
      return;
    }
    try {
      own.off('windowEvent', callback);
    } catch (error) {
      // The window may already be destroyed; the listener dies with it.
    }
  }

  private onWindowEvent(event: window.WindowEventType): void {
    if (event === window.WindowEventType.WINDOW_INACTIVE) {
      // Another window now owns the keyboard/mouse: anything held here would
      // otherwise stay pressed on the server.
      this.releaseAllInputs('window-inactive');
      this.endGesture();
    } else if (event === window.WindowEventType.WINDOW_HIDDEN) {
      this.setPageVisible(false);
    } else if (event === window.WindowEventType.WINDOW_SHOWN) {
      this.setPageVisible(true);
    }
  }

  // -------------------------------------------------------------------------
  // Native GL presenter lifecycle (USE_GL_RENDER). The XComponent's surface
  // events drive attach/detach; the page only adds the session binding and
  // parks the render thread while its window is hidden.
  // -------------------------------------------------------------------------

  private attachRendererSurface(surfaceId: string): void {
    if (!USE_GL_RENDER || this.disposed || surfaceId.length === 0) {
      return;
    }
    // The inner Stack (and with it the XComponent) only mounts once the
    // session is resolved, so sessionId is set by the time this runs.
    if (this.sessionId.length === 0) {
      return;
    }
    if (this.glAttachedSurfaceId === surfaceId) {
      return;
    }
    this.glLastSurfaceId = surfaceId;
    const previous = this.glAttachedSurfaceId;
    this.glAttachedSurfaceId = surfaceId;
    // A surface created while the window is hidden starts parked, so the
    // renderer never presents into an invisible surface.
    const result = attachRenderSurface(this.sessionId, surfaceId, !this.pageVisible);
    hilog.info(DOMAIN, TAG, 'attachRenderSurface session=%{public}s surface=%{public}s' +
      ' (was %{public}s) result=%{public}s', this.sessionId, surfaceId, previous, result);
    if (result !== 'OK:ATTACHED') {
      // Keep the id only when the native side accepted it, so a later surface
      // event can retry.
      this.glAttachedSurfaceId = previous;
      return;
    }
    // A newly attached renderer starts at the contain fit (zoom 1, pan 0):
    // drop whatever transform the previous surface had, so the page and the
    // renderer agree from the first frame on.
    this.resetViewTransform('surface-attached');
  }

  private detachRendererSurface(reason: string): void {
    if (!USE_GL_RENDER || this.sessionId.length === 0) {
      return;
    }
    if (this.glAttachedSurfaceId.length === 0) {
      return;
    }
    this.glAttachedSurfaceId = '';
    // Native side stops the thread and joins it on a reaper, so this returns
    // immediately even if a swap is still in flight.
    const result = detachRenderSurface(this.sessionId);
    hilog.info(DOMAIN, TAG, 'detachRenderSurface reason=%{public}s session=%{public}s' +
      ' result=%{public}s', reason, this.sessionId, result);
  }

  /** Reads the native renderer's snapshot; also gates the first-frame UI. */
  private pollRenderStats(): void {
    if (!USE_GL_RENDER || this.sessionId.length === 0) {
      return;
    }
    const stats = renderStats(this.sessionId);
    if (stats === undefined) {
      return;
    }
    this.renderState = stats.state;
    this.renderFps = stats.fps;
    this.renderFrames = stats.frames;
    this.renderUploads = stats.uploads;
    this.renderSkips = stats.skips;
    this.renderSwapFailures = stats.swapFailures;
    this.renderTexW = stats.texWidth;
    this.renderTexH = stats.texHeight;
    // Presented geometry: the native renderer reports the rectangle AND the
    // surface size it drew on, both in surface physical pixels. These values
    // are the truth for input mapping and for vp->pixel conversion; nothing
    // here recomputes a contain layout.
    if (stats.width > 0 && stats.height > 0) {
      this.statsSurfaceW = stats.width;
      this.statsSurfaceH = stats.height;
    }
    if (stats.maxZoom > 1) {
      this.viewMaxZoom = stats.maxZoom;
    }
    const geometryValid = stats.contentW > 0 && stats.contentH > 0 &&
      stats.contentScale > 0 && stats.contentScaleY > 0 &&
      stats.width > 0 && stats.height > 0;
    if (geometryValid) {
      this.statsContentLeft = stats.contentLeft;
      this.statsContentTop = stats.contentTop;
      this.statsContentScale = stats.contentScale;
      this.statsContentScaleY = stats.contentScaleY;
      this.statsContentW = stats.contentW;
      this.statsContentH = stats.contentH;
      this.statsGeometryValid = true;
      // Adopt the renderer's clamped zoom/pan (e.g. after a rotation re-clamped
      // the pan) — unless a gesture is driving them, or the snapshot may still
      // belong to a transform this page has already replaced.
      if (!this.gestureActive && Date.now() >= this.viewSyncGuardUntil &&
          Date.now() - this.viewSendAt > 120 && stats.zoom >= 1) {
        this.viewZoom = stats.zoom;
        this.viewPanX = stats.panX;
        this.viewPanY = stats.panY;
      }
    } else {
      this.statsGeometryValid = false;
    }
    if (stats.frames > 0 && !this.canvasShown) {
      this.canvasShown = true;
    }
    // Input mapping and cursor placement must follow the PRESENTED geometry
    // (the texture), not the lagging status poll: on a remote resolution
    // change this keeps clicks and the cursor correct immediately.
    if (stats.texWidth > 0 && stats.texHeight > 0 &&
        (stats.texWidth !== this.frameWidth || stats.texHeight !== this.frameHeight)) {
      this.frameWidth = stats.texWidth;
      this.frameHeight = stats.texHeight;
    }
    if (stats.state === 'running' && stats.frames > 0) {
      this.glReattachCount = 0;
    }
    if (stats.state === 'failed' && stats.error.length > 0 &&
        stats.error !== this.lastRenderError) {
      this.lastRenderError = stats.error;
      hilog.warn(DOMAIN, TAG, '原生渲染线程失败 session=%{public}s error=%{public}s',
        this.sessionId, stats.error);
    }
    // A failed renderer must not leave a permanent black screen: the surface
    // usually still exists, so retry the attach with the last known id (a
    // bounded number of times — a genuinely dead surface reports
    // ALREADY/INVALID and stops here). "session closed" is final; the
    // window/session is going away.
    if (stats.state === 'failed' && stats.error !== 'session closed' &&
        this.glLastSurfaceId.length > 0 && this.glAttachedSurfaceId.length > 0 &&
        this.glReattachCount < 5) {
      this.glReattachCount += 1;
      hilog.warn(DOMAIN, TAG, '渲染失败后重连 attempt=%{public}d/%{public}d session=%{public}s',
        this.glReattachCount, 5, this.sessionId);
      this.glAttachedSurfaceId = '';
      this.attachRendererSurface(this.glLastSurfaceId);
    }
  }

  private setPageVisible(visible: boolean): void {
    this.pageVisible = visible;
    if (this.disposed || this.sessionId.length === 0) {
      return;
    }
    // The pasteboard read permission only exists for a visible window, so the
    // clipboard 'update' listener follows this page's visibility.
    clipboardSetSessionVisible(this.sessionId, visible);
    if (visible) {
      // A hidden window may have had its surface destroyed; onSurfaceCreated
      // re-attaches in that case. Parked renderers resume here.
      if (USE_GL_RENDER) {
        suspendRenderSurface(this.sessionId, false);
      }
      this.startFrameTimer();
      // Content that changed while hidden was never fetched: repaint from a
      // complete frame right away instead of waiting for a diff.
      this.requestFull();
      if (!USE_GL_RENDER) {
        this.applyPending();
      }
    } else {
      // Nobody can see the frames, so stop fetching them; the 1 s status poll
      // keeps reconnects/trust prompts advancing.
      this.releaseAllInputs('page-hidden');
      // A gesture in flight must not survive the window going away.
      this.endGesture();
      if (USE_GL_RENDER) {
        suspendRenderSurface(this.sessionId, true);
      }
      this.stopFrameTimer();
      this.startIdleTimer();
    }
  }

  private startPolling(): void {
    if (this.pageVisible) {
      this.startFrameTimer();
    } else {
      this.startIdleTimer();
    }
  }

  private startFrameTimer(): void {
    // Exactly one poll runs at a time: fast (frames) or slow (status only).
    this.stopIdleTimer();
    if (this.timer >= 0) {
      return;
    }
    if (this.disposed || this.sessionId.length === 0) {
      return;
    }
    this.timer = setInterval((): void => { this.poll(); }, POLL_INTERVAL_MS);
  }

  private stopFrameTimer(): void {
    if (this.timer >= 0) {
      clearInterval(this.timer);
      this.timer = -1;
    }
  }

  private startIdleTimer(): void {
    this.stopFrameTimer();
    if (this.idleTimer >= 0) {
      return;
    }
    if (this.disposed || this.sessionId.length === 0) {
      return;
    }
    this.idleTimer = setInterval((): void => { this.poll(); }, 1000);
  }

  private stopIdleTimer(): void {
    if (this.idleTimer >= 0) {
      clearInterval(this.idleTimer);
      this.idleTimer = -1;
    }
  }

  // -------------------------------------------------------------------------
  // Input release: one entry point for focus loss, hide, teardown, reconnect
  // and leaving the connected state.
  // -------------------------------------------------------------------------

  private releaseAllInputs(reason: string): void {
    // A touch that has not pressed yet must not press after the release.
    this.cancelLongPress('release-inputs');
    this.resetTouchPressState();
    this.touchFingerIds.clear();
    this.multiTouchBlocked = false;
    this.multiTouchActive = false;
    const hadLocalState = this.pressedMask !== 0 || this.shiftDown ||
      this.pressedKeysym.size > 0;
    this.pressedMask = 0;
    this.shiftDown = false;
    this.pressedKeysym.clear();
    if (this.sessionId.length === 0) {
      return;
    }
    // The native session releases exactly the keys/buttons it recorded, so the
    // server never keeps a key down just because local bookkeeping was lost.
    if (hadLocalState) {
      hilog.info(DOMAIN, TAG, '释放输入 reason=%{public}s session=%{public}s',
        reason, this.sessionId);
    }
    releaseSessionInput(this.sessionId);
  }

  // -------------------------------------------------------------------------
  // Frame buffer lifetime.
  // -------------------------------------------------------------------------

  /** Invalidates the local mirror and any render that is currently in flight. */
  private invalidateRender(): void {
    this.renderEpoch += 1;
    this.dropFramePixels();
    this.lastVersion = -1;
    this.forceFull = true;
  }

  /** Asks the server for a non-incremental frame when one can be used now. */
  private requestFull(): void {
    this.forceFull = true;
    if (this.sessionId.length > 0 && this.pageVisible && !this.disposed) {
      requestFullFrame(this.sessionId);
    }
  }

  private dropFramePixels(): void {
    const old = this.framePixels;
    this.framePixels = undefined;
    this.frameReady = false;
    if (old === undefined) {
      return;
    }
    if (this.applying) {
      // A render is inside writePixels() on this PixelMap right now; releasing
      // it here would race the write. The render's finally block releases it.
      this.staleFramePixels.push(old);
      return;
    }
    this.releasePixelMap(old);
  }

  private releaseStalePixelMaps(): void {
    if (this.staleFramePixels.length === 0) {
      return;
    }
    const stale = this.staleFramePixels;
    this.staleFramePixels = [];
    for (let i = 0; i < stale.length; i++) {
      this.releasePixelMap(stale[i]);
    }
  }

  private releasePixelMap(pixelMap: image.PixelMap): void {
    try {
      pixelMap.release().catch((error: Error): void => {
        hilog.warn(DOMAIN, TAG, '释放 PixelMap 失败：%{public}s', String(error));
      });
    } catch (error) {
      hilog.warn(DOMAIN, TAG, '释放 PixelMap 异常：%{public}s', String(error));
    }
  }

  private releaseCursorPixelMap(): void {
    const previous = this.cursorPm;
    this.cursorPm = undefined;
    this.cursorLeft = -1;
    this.cursorUsable = false;
    this.cursorShapeW = 0;
    this.cursorShapeH = 0;
    if (previous !== undefined) {
      this.releasePixelMap(previous);
    }
  }

  private async ensureFrameBuffer(): Promise<void> {
    if (this.frameReady || this.frameWidth <= 0 || this.frameHeight <= 0) {
      return;
    }
    const width = this.frameWidth;
    const height = this.frameHeight;
    try {
      const empty = new ArrayBuffer(width * height * 4);
      const options: image.InitializationOptions = {
        size: { width: width, height: height },
        pixelFormat: image.PixelMapFormat.BGRA_8888,
        editable: true,
        alphaType: image.AlphaType.OPAQUE
      };
      const created = await image.createPixelMap(empty, options);
      // The size may have changed (or the page gone) while the PixelMap was
      // being created; keep the mirror consistent with the current frame size.
      if (this.disposed || width !== this.frameWidth || height !== this.frameHeight) {
        this.releasePixelMap(created);
        return;
      }
      this.framePixels = created;
      this.frameReady = true;
      // The mirror starts empty: ask for one complete frame. This is also a
      // real non-incremental request to the server — an idle desktop never
      // volunteers the whole picture by itself.
      this.requestFull();
    } catch (error) {
      hilog.error(DOMAIN, TAG, '创建帧缓冲失败：%{public}s', String(error));
    }
  }

  // A frame belongs to the generation the status snapshot (and therefore the
  // local PixelMap layout) was prepared for. Every reconnect bumps the native
  // generation, so anything else is a stale frame from the previous mirror.
  private static sameGeneration(frameGeneration: number, lastGeneration: number): boolean {
    return frameGeneration === lastGeneration;
  }

  // Applies the changed rectangle to the local framebuffer mirror and blits the
  // whole picture once — the same model vncviewer uses. Only ONE render may run
  // at a time: overlapping applies share regionBuffer/framePixels and could
  // commit versions that were never drawn.
  private async applyPending(): Promise<void> {
    if (this.applying) {
      // A render is in flight and this poll had to be dropped.
      this.perfApplyingSkips += 1;
      return;
    }
    if (this.disposed || !this.pageVisible) {
      return;
    }
    if (!this.canvasReady || this.viewWidth <= 0 || this.frameWidth <= 0) {
      return;
    }
    this.applying = true;
    const epoch = this.renderEpoch;
    try {
      await this.ensureFrameBuffer();
      if (epoch !== this.renderEpoch || this.disposed) {
        return;
      }
      const pixels = this.framePixels;
      if (pixels === undefined) {
        return;
      }
      const needed = this.frameWidth * this.frameHeight * 4;
      if (this.regionBuffer.byteLength < needed) {
        this.regionBuffer = new ArrayBuffer(needed);
      }
      // One call returns both pixels and rectangle, so a frame arriving in
      // between cannot mix sizes and tear the picture. The synchronous segment
      // is timed for the rolling perf summary.
      const takeStart = Remote.nowUs();
      const raw = takeFrameRaw(this.sessionId, this.regionBuffer, this.lastVersion, this.forceFull);
      const takeUs = Remote.nowUs() - takeStart;
      this.perfTakeSum += takeUs;
      this.perfTakeCount += 1;
      if (takeUs > this.perfTakeMax) {
        this.perfTakeMax = takeUs;
      }
      if (raw === 'NONE') {
        return;
      }
      if (raw.startsWith('ERROR')) {
        hilog.warn(DOMAIN, TAG, 'takeFrame：%{public}s', raw);
        if (raw.indexOf('BUFFER_TOO_SMALL') >= 0) {
          this.regionBuffer = new ArrayBuffer(0);
          this.forceFull = true;
        }
        return;
      }
      const meta: FrameMeta | undefined = parseFrameMeta(raw);
      if (meta === undefined || meta.w <= 0 || meta.h <= 0) {
        return;
      }
      // The status snapshot may be older than the frame: a reconnect between
      // the status read and this takeFrame (or while a previous await was in
      // flight) bumps the native generation and rebuilds the mirror with a new
      // layout. Writing such a frame would mix the old picture into the
      // PixelMap prepared for the new generation, so drop it entirely: no
      // PixelMap write, no lastVersion advance. Invalidate the local mirror so
      // the next render cycle rebuilds it from one complete new-generation
      // frame.
      if (!Remote.sameGeneration(meta.generation, this.lastGeneration)) {
        hilog.warn(DOMAIN, TAG,
          '丢弃旧代次帧：帧代次=%{public}d 状态代次=%{public}d session=%{public}s',
          meta.generation, this.lastGeneration, this.sessionId);
        this.forceFull = true;
        this.invalidateRender();
        return;
      }
      if (epoch !== this.renderEpoch || this.disposed || pixels !== this.framePixels) {
        return;
      }
      const area: image.PositionArea = {
        pixels: this.regionBuffer,
        offset: 0,
        stride: meta.w * 4,
        region: { size: { width: meta.w, height: meta.h }, x: meta.x, y: meta.y }
      };
      const writeStart = Remote.nowUs();
      await pixels.writePixels(area);
      const writeUs = Remote.nowUs() - writeStart;
      this.perfWriteSum += writeUs;
      this.perfWriteCount += 1;
      if (writeUs > this.perfWriteMax) {
        this.perfWriteMax = writeUs;
      }
      // The version may only advance once the pixels really landed AND were
      // drawn from the same mirror generation. A reconnect during the await
      // bumps both the epoch and the generation; either mismatch means the
      // pixels just written belong to a layout nobody will draw.
      if (epoch !== this.renderEpoch || this.disposed || pixels !== this.framePixels ||
          !Remote.sameGeneration(meta.generation, this.lastGeneration)) {
        return;
      }
      const drawStart = Remote.nowUs();
      this.drawFrame(pixels);
      const drawUs = Remote.nowUs() - drawStart;
      this.perfDrawSum += drawUs;
      this.perfDrawCount += 1;
      if (drawUs > this.perfDrawMax) {
        this.perfDrawMax = drawUs;
      }
      this.perfAppliedFrames += 1;
      this.lastMetaX = meta.x;
      this.lastMetaY = meta.y;
      this.lastMetaW = meta.w;
      this.lastMetaH = meta.h;
      this.lastMetaFull = meta.full === true;
      if (this.lastMetaFull) {
        this.perfFullFrames += 1;
      }
      this.lastVersion = meta.version;
      this.forceFull = false;
      if (!this.canvasShown) {
        this.canvasShown = true;
      }
    } catch (error) {
      hilog.error(DOMAIN, TAG, '应用画面失败：%{public}s', String(error));
      // A failed apply must not skip this version: ask for a complete frame.
      this.forceFull = true;
      this.requestFull();
    } finally {
      this.applying = false;
      this.releaseStalePixelMaps();
    }
  }

  /**
   * Blits the whole mirror into the Canvas at the current view scale, centred
   * with letterboxing. The mirror is always a complete remote frame, so a local
   * window resize only needs this redraw — the remote resolution did not change
   * and there is nothing new to ask the server for.
   */
  private drawFrame(pixels: image.PixelMap): void {
    if (this.viewWidth <= 0 || this.viewHeight <= 0 ||
        this.frameWidth <= 0 || this.frameHeight <= 0) {
      return;
    }
    const scale = Math.min(this.viewWidth / this.frameWidth, this.viewHeight / this.frameHeight);
    const offsetX = (this.viewWidth - this.frameWidth * scale) / 2;
    const offsetY = (this.viewHeight - this.frameHeight * scale) / 2;
    const dw = Math.round(this.frameWidth * scale);
    const dh = Math.round(this.frameHeight * scale);
    this.ctx.drawImage(pixels, Math.round(offsetX), Math.round(offsetY), dw, dh);
  }

  /**
   * Repaints the existing mirror after the Canvas size changed. The old content
   * is cleared first so a changed aspect ratio cannot leave stale pixels in the
   * new letterbox area; when there is no drawable mirror yet the first-frame
   * path (ensureFrameBuffer + requestFull) owns the screen.
   */
  private redrawAfterResize(): void {
    const pixels = this.framePixels;
    if (!this.canvasShown || !this.frameReady || pixels === undefined || this.disposed) {
      return;
    }
    if (this.viewWidth <= 0 || this.viewHeight <= 0) {
      return;
    }
    this.ctx.clearRect(0, 0, this.viewWidth, this.viewHeight);
    this.drawFrame(pixels);
  }

  // Cursor is drawn locally, so moving the mouse needs no framebuffer update.
  private async updateCursorShape(cw: number, ch: number): Promise<void> {
    if (cw <= 0 || ch <= 0) {
      return;
    }
    try {
      const buffer = new ArrayBuffer(cw * ch * 4);
      copyCursor(this.sessionId, buffer);
      const options: image.InitializationOptions = {
        size: { width: cw, height: ch },
        pixelFormat: image.PixelMapFormat.RGBA_8888,
        editable: false,
        alphaType: image.AlphaType.UNPREMUL
      };
      const created = await image.createPixelMap(buffer, options);
      if (this.disposed) {
        this.releasePixelMap(created);
        return;
      }
      const previous = this.cursorPm;
      this.cursorPm = created;
      if (previous !== undefined) {
        this.releasePixelMap(previous);
      }
    } catch (error) {
      hilog.error(DOMAIN, TAG, '更新光标失败：%{public}s', String(error));
    }
  }

  // Monotonic, sub-millisecond clock. Date.now() only has ms resolution, which
  // is too coarse for takeFrame/drawImage segments. getUptime(..., true) is
  // NANOSECONDS, so /1000 yields microseconds (a /1e6 here would yield
  // milliseconds and make every 2 s window test effectively 2000 s long).
  // Timing must never change the render path, so a failed call falls back to
  // the wall clock instead of propagating into applyPending().
  private static nowUs(): number {
    try {
      return systemDateTime.getUptime(systemDateTime.TimeType.ACTIVE, true) / 1000;
    } catch (error) {
      return Date.now() * 1000;
    }
  }

  // Computes `[left, top, width, height]` for the local cursor, or an empty
  // array when the pointer is outside the picture. Pure so the poll fallback
  // and the per-event path place the cursor identically. left/top/scale* are
  // the picture geometry in vp (from renderStats in GL mode, or the contain fit
  // on the Canvas path), so the overlay follows zoom and pan.
  private static cursorRect(cw: number, ch: number, hotX: number, hotY: number,
                            left: number, top: number,
                            scaleX: number, scaleY: number,
                            contentW: number, contentH: number,
                            px: number, py: number): number[] {
    if (cw <= 0 || ch <= 0 || contentW <= 0 || contentH <= 0 ||
        scaleX <= 0 || scaleY <= 0 || px < 0 || py < 0) {
      return [];
    }
    if (px < left || py < top || px > left + contentW || py > top + contentH) {
      return [];
    }
    return [
      Math.round(px - hotX * scaleX),
      Math.round(py - hotY * scaleY),
      Math.max(1, Math.round(cw * scaleX)),
      Math.max(1, Math.round(ch * scaleY))
    ];
  }

  // The local cursor follows the physical pointer: no server round trip. Called
  // on every pointer event (instant) and from the poll as a fallback for shape
  // or size changes.
  private placeCursor(): void {
    if (!this.cursorUsable || this.cursorPm === undefined || !this.canvasShown ||
        this.disposed) {
      return;
    }
    let rect: number[] = [];
    const pxPerVpX = this.surfacePixelsPerVpX();
    const pxPerVpY = this.surfacePixelsPerVpY();
    if (USE_GL_RENDER && this.statsGeometryValid && pxPerVpX > 0 && pxPerVpY > 0) {
      // Same geometry input mapping uses: the cursor cannot drift away from
      // the remote pointer at zoom != 1.
      rect = Remote.cursorRect(this.cursorShapeW, this.cursorShapeH,
        this.cursorHotX, this.cursorHotY,
        this.statsContentLeft / pxPerVpX, this.statsContentTop / pxPerVpY,
        this.statsContentScale / pxPerVpX, this.statsContentScaleY / pxPerVpY,
        this.statsContentW / pxPerVpX, this.statsContentH / pxPerVpY,
        this.viewPointerX, this.viewPointerY);
    } else if (this.viewWidth > 0 && this.viewHeight > 0 &&
               this.frameWidth > 0 && this.frameHeight > 0) {
      const scale = Math.min(this.viewWidth / this.frameWidth, this.viewHeight / this.frameHeight);
      const contentW = this.frameWidth * scale;
      const contentH = this.frameHeight * scale;
      rect = Remote.cursorRect(this.cursorShapeW, this.cursorShapeH,
        this.cursorHotX, this.cursorHotY,
        (this.viewWidth - contentW) / 2, (this.viewHeight - contentH) / 2,
        scale, scale, contentW, contentH,
        this.viewPointerX, this.viewPointerY);
    }
    if (rect.length === 0) {
      // Pointer in the letterbox margin or not on the picture: leave the cursor
      // where it was instead of moving it into the black border.
      return;
    }
    // @State writes re-render the Image layer; skip writes whose rounded value
    // did not change.
    if (this.cursorLeft !== rect[0]) {
      this.cursorLeft = rect[0];
    }
    if (this.cursorTop !== rect[1]) {
      this.cursorTop = rect[1];
    }
    if (this.cursorDrawW !== rect[2]) {
      this.cursorDrawW = rect[2];
    }
    if (this.cursorDrawH !== rect[3]) {
      this.cursorDrawH = rect[3];
    }
  }

  private applyStatus(status: SessionStatus): void {
    const width = status.width === undefined ? 0 : status.width;
    const height = status.height === undefined ? 0 : status.height;
    if (!USE_GL_RENDER && width > 0 && height > 0 &&
        (width !== this.frameWidth || height !== this.frameHeight)) {
      // Canvas path: the local mirror must be rebuilt for the new size. The
      // GL path takes its geometry from the PRESENTED texture instead
      // (pollRenderStats), so the status poll never drags it back to a stale
      // resolution for one cycle.
      this.invalidateRender();
      this.frameWidth = width;
      this.frameHeight = height;
    }
    // A reconnect (generation bump) rebuilt the native mirror: the local
    // PixelMap now describes a picture that no longer exists. Drop it, so the
    // next frame starts from a fresh mirror, and ask for a complete frame
    // instead of waiting for the server to volunteer one. Inputs pressed on the
    // old generation must be released as well.
    const generation = status.generation === undefined ? 0 : status.generation;
    if (generation > 0) {
      if (this.lastGeneration >= 0 && generation !== this.lastGeneration) {
        this.releaseAllInputs('generation');
        // A reconnect rebuilds the native mirror and renderer state: the view
        // transform restarts at the contain fit on both sides.
        this.resetViewTransform('reconnect');
        this.invalidateRender();
        this.requestFull();
      }
      this.lastGeneration = generation;
    }
    // Leaving "connected" (reconnect, failure, trust prompt, disconnect) means
    // the server side may never have seen the key/button releases.
    if (this.lastState === SESSION_STATE_CONNECTED && status.state !== SESSION_STATE_CONNECTED) {
      this.releaseAllInputs('state-left-connected');
    }
    this.lastState = status.state;
    // Clipboard edge: one take per version bump.
    const clip = status.clipboard;
    if (clip !== undefined && typeof clip.version === 'number' &&
        clip.version !== this.lastClipboardVersion) {
      this.lastClipboardVersion = clip.version;
      onRemoteClipboardVersion(this.sessionId, clip.version);
    }
    // Cursor shape and position come from the server but are drawn locally.
    const cursor: CursorInfo | undefined = status.cursor;
    let cw = 0;
    let ch = 0;
    let hotX = 0;
    let hotY = 0;
    let cursorVersion = 0;
    let cursorBlank = false;
    if (cursor !== undefined) {
      cw = cursor.w;
      ch = cursor.h;
      hotX = cursor.hotX;
      hotY = cursor.hotY;
      cursorVersion = cursor.version;
      cursorBlank = cursor.blank === true;
    }
    // Log-only snapshot of what the server last told us about the cursor.
    this.lastCursorBlank = cursorBlank;
    this.lastCursorW = cw;
    this.lastCursorH = ch;
    if (cursorBlank) {
      // The server told us it drew the pointer into the picture itself.
      this.releaseCursorPixelMap();
    } else if (cw > 0 && ch > 0 && cursorVersion !== this.lastCursorVersion && this.canvasShown) {
      this.lastCursorVersion = cursorVersion;
      this.cursorShapeW = cw;
      this.cursorShapeH = ch;
      this.cursorHotX = hotX;
      this.cursorHotY = hotY;
      this.cursorUsable = true;
      this.updateCursorShape(cw, ch);
    }
    if (this.canvasShown && !cursorBlank) {
      // Fallback for shape/size changes only; pointer events reposition the
      // cursor immediately, without waiting for the next poll.
      this.placeCursor();
    }
    // State machine -> status text.
    if (status.state === SESSION_STATE_TRUST_REQUIRED) {
      this.needsTrust = true;
      this.fingerprint = status.fingerprint === undefined ? '' : status.fingerprint;
      this.statusText = '需要确认身份';
      return;
    }
    this.needsTrust = false;
    this.fingerprint = '';
    switch (status.state) {
      case SESSION_STATE_CONNECTED:
        this.statusText = '已连接（实时刷新）';
        if (!USE_GL_RENDER) {
          // GL mode: the native render thread consumes frames itself; the
          // Canvas path polls them here.
          this.applyPending();
        }
        return;
      case 'reconnecting': {
        const retry = status.retryInMs === undefined ? 0 : status.retryInMs;
        this.statusText = '连接中断，正在重连（' + Math.max(1, Math.ceil(retry / 1000)) + 's）';
        return;
      }
      case 'failed':
        this.statusText = '失败：' + (status.error.length > 0 ? status.error : status.errorCode);
        return;
      case 'disconnected':
        this.statusText = '已断开';
        return;
      case 'connecting':
        this.statusText = '连接中…';
        return;
      default:
        this.statusText = status.state;
        return;
    }
  }

  private poll(): void {
    const pollStart = Remote.nowUs();
    if (this.perfPollLast > 0) {
      const intervalUs = pollStart - this.perfPollLast;
      this.perfPollSum += intervalUs;
      this.perfPollCount += 1;
      if (intervalUs > this.perfPollMax) {
        this.perfPollMax = intervalUs;
      }
    }
    this.perfPollLast = pollStart;
    const onceStart = Remote.nowUs();
    this.pollOnce();
    const onceUs = Remote.nowUs() - onceStart;
    this.perfOnceSum += onceUs;
    this.perfOnceCount += 1;
    if (onceUs > this.perfOnceMax) {
      this.perfOnceMax = onceUs;
    }
    this.maybeLogPerf();
  }

  private pollOnce(): void {
    if (this.sessionId.length === 0 || this.disposed) {
      return;
    }
    const statusStart = Remote.nowUs();
    const status = sessionStatus(this.sessionId);
    const statusUs = Remote.nowUs() - statusStart;
    this.perfStatusSum += statusUs;
    this.perfStatusCount += 1;
    if (statusUs > this.perfStatusMax) {
      this.perfStatusMax = statusUs;
    }
    if (status === undefined) {
      this.badStatusCount += 1;
      if (this.badStatusCount >= 10 && this.badStatusCount % 50 === 10) {
        hilog.warn(DOMAIN, TAG, 'sessionStatus 解析失败 session=%{public}s', this.sessionId);
      }
      if (this.badStatusCount === 10) {
        this.statusText = '状态不可用（原生层未就绪）';
      }
      return;
    }
    this.badStatusCount = 0;
    const applyStart = Remote.nowUs();
    this.applyStatus(status);
    const applyUs = Remote.nowUs() - applyStart;
    this.perfApplySum += applyUs;
    this.perfApplyCount += 1;
    if (applyUs > this.perfApplyMax) {
      this.perfApplyMax = applyUs;
    }
    if (USE_GL_RENDER) {
      const renderStart = Remote.nowUs();
      this.pollRenderStats();
      const renderUs = Remote.nowUs() - renderStart;
      this.perfRenderSum += renderUs;
      this.perfRenderCount += 1;
      if (renderUs > this.perfRenderMax) {
        this.perfRenderMax = renderUs;
      }
    }
  }

  // "avg/max" in milliseconds for one accumulator.
  private static perfAvgMax(sumUs: number, count: number, maxUs: number): string {
    if (count === 0) {
      return '0.00/0.00';
    }
    return (sumUs / count / 1000).toFixed(2) + '/' + (maxUs / 1000).toFixed(2);
  }

  // Plain average in milliseconds (the TianguanPoll line prints avg and max as
  // separate fields).
  private static perfAvg(sumUs: number, count: number): string {
    if (count === 0) {
      return '0.00';
    }
    return (sumUs / count / 1000).toFixed(2);
  }

  private static perfMs(micros: number): string {
    return (micros / 1000).toFixed(2);
  }

  // One debug-level line every 2 s with the rendering segments. Timing never
  // changes the rendering path (no added await or synchronization point).
  private maybeLogPerf(): void {
    const now = Remote.nowUs();
    if (this.perfWindowStart <= 0) {
      this.perfWindowStart = now;
      return;
    }
    const elapsedMs = (now - this.perfWindowStart) / 1000;
    if (elapsedMs < 2000) {
      return;
    }
    const line = 'TianguanPerf ' + elapsedMs.toFixed(0) + 'ms' +
      ' session=' + this.sessionId +
      ' poll=' + Remote.perfAvgMax(this.perfPollSum, this.perfPollCount, this.perfPollMax) +
      ' take=' + Remote.perfAvgMax(this.perfTakeSum, this.perfTakeCount, this.perfTakeMax) +
      ' write=' + Remote.perfAvgMax(this.perfWriteSum, this.perfWriteCount, this.perfWriteMax) +
      ' draw=' + Remote.perfAvgMax(this.perfDrawSum, this.perfDrawCount, this.perfDrawMax) +
      ' takeN=' + this.perfTakeCount +
      ' applied=' + this.perfAppliedFrames +
      ' applyingSkip=' + this.perfApplyingSkips +
      ' cursor={blank:' + (this.lastCursorBlank ? 1 : 0) +
      ',w:' + this.lastCursorW +
      ',h:' + this.lastCursorH +
      ',visible:' + (this.cursorPm !== undefined && this.cursorLeft >= -64 ? 1 : 0) +
      ',left:' + this.cursorLeft +
      ',top:' + this.cursorTop + '}' +
      ' ptr={' + this.viewPointerX + ',' + this.viewPointerY + '}' +
      ' lastMeta={x:' + this.lastMetaX + ',y:' + this.lastMetaY +
      ',w:' + this.lastMetaW + ',h:' + this.lastMetaH +
      ',full:' + (this.lastMetaFull ? 1 : 0) + '}' +
      ' full=' + this.perfFullFrames + '/' + this.perfAppliedFrames +
      (USE_GL_RENDER ?
        ' render={state:' + this.renderState +
        ',fps:' + this.renderFps.toFixed(1) +
        ',frames:' + this.renderFrames +
        ',uploads:' + this.renderUploads +
        ',skips:' + this.renderSkips +
        ',swapFail:' + this.renderSwapFailures +
        ',tex:' + this.renderTexW + 'x' + this.renderTexH +
        ',view:' + this.viewStateText() +
        (this.lastRenderError.length > 0 ? ',error:' + this.lastRenderError : '') + '}' : '');
    // Debug level: these are periodic (2 s) timing summaries, not errors. The
    // default log level stays quiet; raise it only when diagnosing a stall.
    // The TianguanPoll segments are sessionStatus (sync NAPI + JSON parse),
    // applyStatus (ArkUI state writes) and pollRenderStats (sync NAPI read);
    // n is the number of polls in the window.
    const pollLine = 'TianguanPoll n=' + this.perfOnceCount +
      ' avg=' + Remote.perfAvg(this.perfOnceSum, this.perfOnceCount) +
      'ms max=' + Remote.perfMs(this.perfOnceMax) +
      'ms session=' + this.sessionId +
      ' status=' + Remote.perfAvgMax(this.perfStatusSum, this.perfStatusCount,
        this.perfStatusMax) +
      ' apply=' + Remote.perfAvgMax(this.perfApplySum, this.perfApplyCount,
        this.perfApplyMax) +
      ' render=' + Remote.perfAvgMax(this.perfRenderSum, this.perfRenderCount,
        this.perfRenderMax);
    hilog.debug(DOMAIN, TAG, '%{public}s', pollLine);
    hilog.debug(DOMAIN, TAG, '%{public}s', line);
    this.perfWindowStart = now;
    this.perfPollSum = 0;
    this.perfPollCount = 0;
    this.perfPollMax = 0;
    this.perfTakeSum = 0;
    this.perfTakeCount = 0;
    this.perfTakeMax = 0;
    this.perfWriteSum = 0;
    this.perfWriteCount = 0;
    this.perfWriteMax = 0;
    this.perfDrawSum = 0;
    this.perfDrawCount = 0;
    this.perfDrawMax = 0;
    this.perfOnceSum = 0;
    this.perfOnceCount = 0;
    this.perfOnceMax = 0;
    this.perfStatusSum = 0;
    this.perfStatusCount = 0;
    this.perfStatusMax = 0;
    this.perfApplySum = 0;
    this.perfApplyCount = 0;
    this.perfApplyMax = 0;
    this.perfRenderSum = 0;
    this.perfRenderCount = 0;
    this.perfRenderMax = 0;
    this.perfApplyingSkips = 0;
    this.perfAppliedFrames = 0;
    this.perfFullFrames = 0;
  }

  // -------------------------------------------------------------------------
  // Two-finger view transform: pinch zooms, two-finger drag pans (GL mode).
  //
  // Units: ArkUI gesture events are vp relative to the remote_area Column; the
  // native renderer's geometry is in surface physical pixels. The conversion
  // factor is the surface size renderStats reports divided by the component
  // size in vp — the actual surface this renderer draws on — and only falls
  // back to the display density when no snapshot exists yet.
  //
  // The picture geometry used for input mapping is NEVER recomputed there: it
  // is read from renderStats (contentLeft/contentTop/contentScale*), so clicks
  // keep matching what the renderer presented at any zoom/pan. The local
  // computeViewGeometry() below exists only to predict/limit gesture motion
  // (same formula and rounding as app/native/view_geometry.h) and is clamped
  // by the native side again on every setViewTransform.
  // -------------------------------------------------------------------------

  /** Compact one-line view state for the 2 s perf summary and gesture logs. */
  private viewStateText(): string {
    return 'zoom=' + this.viewZoom.toFixed(3) +
      ',pan=' + this.viewPanX.toFixed(1) + ',' + this.viewPanY.toFixed(1) +
      ',content=' + this.statsContentLeft + ',' + this.statsContentTop +
      ',' + this.statsContentW + 'x' + this.statsContentH +
      ',scale=' + this.statsContentScale.toFixed(4) + ',' + this.statsContentScaleY.toFixed(4) +
      ',surf=' + this.statsSurfaceW + 'x' + this.statsSurfaceH +
      ',vp=' + this.viewWidth + 'x' + this.viewHeight;
  }

  private deviceDensity(): number {
    if (this.densityPixels > 0) {
      return this.densityPixels;
    }
    try {
      const info = display.getDefaultDisplaySync();
      if (info.densityPixels > 0) {
        this.densityPixels = info.densityPixels;
      }
    } catch (error) {
      hilog.warn(DOMAIN, TAG, '读取屏幕密度失败：%{public}s', String(error));
    }
    return this.densityPixels > 0 ? this.densityPixels : 1;
  }

  /** Surface physical pixels per vp, for converting gesture coordinates. */
  private surfacePixelsPerVpX(): number {
    if (this.statsSurfaceW > 0 && this.viewWidth > 0) {
      return this.statsSurfaceW / this.viewWidth;
    }
    return this.deviceDensity();
  }

  private surfacePixelsPerVpY(): number {
    if (this.statsSurfaceH > 0 && this.viewHeight > 0) {
      return this.statsSurfaceH / this.viewHeight;
    }
    return this.deviceDensity();
  }

  private clampViewZoom(zoom: number): number {
    const max = this.viewMaxZoom > 1 ? this.viewMaxZoom : MAX_VIEW_ZOOM_FALLBACK;
    if (!Number.isFinite(zoom)) {
      return 1;
    }
    return Math.min(Math.max(zoom, 1), max);
  }

  /**
   * Predicted presentation rectangle for a requested zoom/pan in surface
   * pixels, computed exactly like app/native/view_geometry.h (float32 scale,
   * same rounding, same clamp). Gesture maths and the resize re-clamp use it;
   * input mapping reads the native renderStats fields instead.
   */
  private computeViewGeometry(zoom: number, panX: number, panY: number): ViewGeometry | undefined {
    const surfW = this.statsSurfaceW;
    const surfH = this.statsSurfaceH;
    const fbW = this.frameWidth;
    const fbH = this.frameHeight;
    if (surfW <= 0 || surfH <= 0 || fbW <= 0 || fbH <= 0) {
      return undefined;
    }
    const z = this.clampViewZoom(zoom);
    // The native geometry uses float32; Math.fround reproduces it exactly. This
    // matters at .5 rounding boundaries, where a double would predict a
    // rectangle one surface pixel away from the one the renderer actually drew.
    const base = Math.fround(Math.min(Math.fround(surfW / fbW), Math.fround(surfH / fbH)));
    const scale = Math.fround(base * Math.fround(z));
    const contentW = Math.max(1, Math.floor(Math.fround(Math.fround(fbW * scale) + 0.5)));
    const contentH = Math.max(1, Math.floor(Math.fround(Math.fround(fbH * scale) + 0.5)));
    // Integer centring uses truncation, exactly like the native int division.
    const centredLeft = Math.trunc((surfW - contentW) / 2);
    const centredTop = Math.trunc((surfH - contentH) / 2);
    let left = centredLeft;
    let panXEff = 0;
    if (contentW > surfW) {
      const minLeft = surfW - contentW;
      let wanted = centredLeft + (Number.isFinite(panX) ? panX : 0);
      if (wanted < minLeft) {
        wanted = minLeft;
      }
      if (wanted > 0) {
        wanted = 0;
      }
      left = wanted;
      panXEff = left - centredLeft;
    }
    let top = centredTop;
    let panYEff = 0;
    if (contentH > surfH) {
      const minTop = surfH - contentH;
      let wanted = centredTop + (Number.isFinite(panY) ? panY : 0);
      if (wanted < minTop) {
        wanted = minTop;
      }
      if (wanted > 0) {
        wanted = 0;
      }
      top = wanted;
      panYEff = top - centredTop;
    }
    return {
      zoom: z,
      left: left,
      top: top,
      contentW: contentW,
      contentH: contentH,
      scaleX: contentW / fbW,
      scaleY: contentH / fbH,
      panX: panXEff,
      panY: panYEff
    };
  }

  private clearStatsGeometry(): void {
    this.statsGeometryValid = false;
    this.statsContentLeft = 0;
    this.statsContentTop = 0;
    this.statsContentScale = 0;
    this.statsContentScaleY = 0;
    this.statsContentW = 0;
    this.statsContentH = 0;
  }

  private cancelViewSendTimer(): void {
    if (this.viewSendTimer >= 0) {
      clearTimeout(this.viewSendTimer);
      this.viewSendTimer = -1;
    }
  }

  private cancelGeometryRefresh(): void {
    if (this.geometryRefreshTimer >= 0) {
      clearTimeout(this.geometryRefreshTimer);
      this.geometryRefreshTimer = -1;
    }
  }

  /** Coalesces gesture updates into at most one setViewTransform per interval. */
  private scheduleViewTransform(): void {
    if (!USE_GL_RENDER || this.disposed) {
      return;
    }
    if (this.viewSendTimer >= 0) {
      return;
    }
    const elapsed = Date.now() - this.viewSendAt;
    const wait = elapsed >= VIEW_SEND_INTERVAL_MS ? 0 : VIEW_SEND_INTERVAL_MS - elapsed;
    this.viewSendTimer = setTimeout((): void => {
      this.flushViewTransform();
    }, wait);
  }

  private flushViewTransform(): void {
    this.cancelViewSendTimer();
    if (!USE_GL_RENDER || this.disposed || this.sessionId.length === 0 ||
        this.glAttachedSurfaceId.length === 0) {
      return;
    }
    this.viewSendAt = Date.now();
    const result = setViewTransform(this.sessionId, this.viewZoom, this.viewPanX, this.viewPanY);
    if (result !== 'OK' && result !== this.lastViewTransformError) {
      this.lastViewTransformError = result;
      hilog.warn(DOMAIN, TAG,
        'setViewTransform 失败 session=%{public}s result=%{public}s view=%{public}s',
        this.sessionId, result, this.viewStateText());
    }
  }

  /**
   * Reads renderStats again shortly after a transform or resize, so the
   * geometry used by input mapping catches up without waiting for the 250 ms
   * status poll (and a tap right after a pinch still lands on target).
   */
  private scheduleGeometryRefresh(delayMs: number): void {
    if (!USE_GL_RENDER || this.disposed) {
      return;
    }
    this.cancelGeometryRefresh();
    this.geometryRefreshTimer = setTimeout((): void => {
      this.geometryRefreshTimer = -1;
      if (this.disposed) {
        return;
      }
      this.pollRenderStats();
    }, delayMs);
  }

  /**
   * Drops the local zoom/pan and asks the native renderer for the contain fit.
   * Called when a session is (re)created/reconnected or a surface is attached:
   * the native renderer starts at 1/0 in those cases, so both sides must agree.
   */
  private resetViewTransform(reason: string): void {
    this.cancelViewSendTimer();
    this.gestureActive = false;
    this.gestureAnchorValid = false;
    this.gestureScale = 1;
    this.viewZoom = 1;
    this.viewPanX = 0;
    this.viewPanY = 0;
    this.clearStatsGeometry();
    // The next stats snapshot may still describe the transform before the
    // reset; do not adopt it back into the local state.
    this.viewSyncGuardUntil = Date.now() + VIEW_SYNC_GUARD_MS;
    if (!USE_GL_RENDER || this.sessionId.length === 0 ||
        this.glAttachedSurfaceId.length === 0) {
      return;
    }
    this.viewSendAt = Date.now();
    const result = setViewTransform(this.sessionId, 1, 0, 0);
    hilog.info(DOMAIN, TAG, '视图变换重置 reason=%{public}s session=%{public}s result=%{public}s',
      reason, this.sessionId, result);
    if (result !== 'OK') {
      this.lastViewTransformError = result;
      hilog.warn(DOMAIN, TAG, '重置视图变换失败 session=%{public}s result=%{public}s',
        this.sessionId, result);
    }
    this.scheduleGeometryRefresh(60);
  }

  /** Releases a button the first finger pressed, without a stray click. */
  private abortSingleFingerInput(): void {
    if (this.pressedMask === 0) {
      return;
    }
    this.pressedMask = 0;
    // Cancel the two-quick-taps double-click detection: this was a gesture.
    this.lastTapMs = 0;
    if (this.sessionId.length > 0 && this.lastRemoteX >= 0 && this.lastRemoteY >= 0) {
      hilog.debug(DOMAIN, TAG,
        '远端指针 source=abort mask=0 remote=%{public}d,%{public}d',
        this.lastRemoteX, this.lastRemoteY);
      sendPointer(this.sessionId, this.lastRemoteX, this.lastRemoteY, 0);
    }
  }

  // -------------------------------------------------------------------------
  // Touch press delay and three-finger block. The touch sequence is shared
  // with the two-finger view gestures: a press must not go out before it is
  // known that the touch is not a pinch, and three or more fingers must not
  // reach the remote at all.
  // -------------------------------------------------------------------------

  /**
   * Updates the set of fingers currently down and returns its size.
   * event.touches may be empty on Up/Cancel, so membership is tracked by the
   * stable TouchObject.id instead of a length: Down adds the changed fingers
   * (and re-syncs from touches, which lists every finger), Up removes the
   * changed ones and Cancel ends the whole sequence.
   */
  private updateTouchFingers(event: TouchEvent): number {
    const touches = event.touches;
    const changed = event.changedTouches;
    if (event.type === TouchType.Down) {
      // A second finger's Down reports both fingers in touches, so rebuilding
      // here also heals a missed Up.
      if (touches.length > 0) {
        this.touchFingerIds.clear();
        for (let i = 0; i < touches.length; i++) {
          this.touchFingerIds.add(touches[i].id);
        }
      }
      for (let i = 0; i < changed.length; i++) {
        this.touchFingerIds.add(changed[i].id);
      }
    } else if (event.type === TouchType.Up) {
      if (changed.length > 0) {
        // changedTouches names exactly the pointer that went up. touches may be
        // empty here, so the remaining fingers must come from this set only.
        for (let i = 0; i < changed.length; i++) {
          this.touchFingerIds.delete(changed[i].id);
        }
      } else if (touches.length > 0) {
        // No changed list: touches is the only evidence available.
        this.touchFingerIds.clear();
        for (let i = 0; i < touches.length; i++) {
          this.touchFingerIds.add(touches[i].id);
        }
      } else {
        this.touchFingerIds.clear();
      }
    } else if (event.type === TouchType.Cancel) {
      // A cancel ends every pointer of this sequence.
      this.touchFingerIds.clear();
    } else if (touches.length > 0) {
      // Move: touches is authoritative. Rebuilding keeps a missed Up from
      // leaving a phantom finger that would block single-finger input.
      this.touchFingerIds.clear();
      for (let i = 0; i < touches.length; i++) {
        this.touchFingerIds.add(touches[i].id);
      }
    }
    return this.touchFingerIds.size;
  }

  private static touchTypeName(type: TouchType): string {
    if (type === TouchType.Down) {
      return 'down';
    }
    if (type === TouchType.Up) {
      return 'up';
    }
    if (type === TouchType.Move) {
      return 'move';
    }
    return 'cancel';
  }

  /**
   * True when this TouchEvent is the synthetic touch PC derives from the last
   * real mouse action (the mouse path already forwarded that action). The
   * caller must have updated the finger count first: only a one-finger sequence
   * can be such a duplicate, two or more fingers never have a mouse
   * counterpart, and the three-finger block has to be entered from the finger
   * count on every device.
   */
  private isMouseDuplicateTouch(event: TouchEvent): boolean {
    if (this.lastMouseEventMs <= 0 || this.lastMouseX < 0 || this.lastMouseY < 0) {
      return false;
    }
    const elapsed = Date.now() - this.lastMouseEventMs;
    if (elapsed >= MOUSE_TOUCH_DEDUPE_MS) {
      return false;
    }
    let x = this.lastTouchX;
    let y = this.lastTouchY;
    if (event.touches.length > 0) {
      x = event.touches[0].x;
      y = event.touches[0].y;
    } else if (event.changedTouches.length > 0) {
      x = event.changedTouches[0].x;
      y = event.changedTouches[0].y;
    }
    if (x < 0 || y < 0) {
      return false;
    }
    const dx = x - this.lastMouseX;
    const dy = y - this.lastMouseY;
    if (dx * dx + dy * dy >= MOUSE_TOUCH_DEDUPE_VP * MOUSE_TOUCH_DEDUPE_VP) {
      return false;
    }
    hilog.debug(DOMAIN, TAG,
      '丢弃与鼠标重复的触摸 type=%{public}s x=%{public}d y=%{public}d mouse=%{public}d,%{public}d dtMs=%{public}d fingers=%{public}d',
      Remote.touchTypeName(event.type), Math.round(x), Math.round(y),
      Math.round(this.lastMouseX), Math.round(this.lastMouseY), elapsed,
      this.touchFingerIds.size);
    return true;
  }

  /**
   * Drops the pending single-finger press and its long-press timer. Logged
   * when a press was still waiting, so the debug trace says why a long press
   * did not become a right click.
   */
  private cancelLongPress(reason: string): void {
    if (this.longPressTimer >= 0) {
      clearTimeout(this.longPressTimer);
      this.longPressTimer = -1;
    }
    if (this.pressPending) {
      hilog.debug(DOMAIN, TAG,
        '长按取消 reason=%{public}s fingers=%{public}d blocked=%{public}s',
        reason, this.touchFingerIds.size, String(this.multiTouchBlocked));
    }
    this.pressPending = false;
  }

  /** Forgets every piece of single-finger press state, sending nothing. */
  private resetTouchPressState(): void {
    if (this.longPressTimer >= 0) {
      clearTimeout(this.longPressTimer);
      this.longPressTimer = -1;
    }
    this.pressPending = false;
    this.pressSent = false;
    this.longPressFired = false;
    this.touchDownX = -1;
    this.touchDownY = -1;
  }

  /** True when the finger travelled more than TAP_SLOP_VP from its Down point. */
  private exceedsTapSlop(): boolean {
    if (this.touchDownX < 0 || this.touchDownY < 0) {
      return false;
    }
    const dx = this.lastTouchX - this.touchDownX;
    const dy = this.lastTouchY - this.touchDownY;
    return dx * dx + dy * dy > TAP_SLOP_VP * TAP_SLOP_VP;
  }

  /**
   * The touch is a drag, not a tap or a long press: send the deferred left
   * press now. pointerDown() sends move + press, so the drag starts at the
   * current point with the left button already down.
   */
  private sendDeferredPress(): void {
    if (!this.pressPending) {
      return;
    }
    this.pressPending = false;
    if (this.longPressTimer >= 0) {
      clearTimeout(this.longPressTimer);
      this.longPressTimer = -1;
    }
    hilog.debug(DOMAIN, TAG,
      '长按取消 reason=move 转为左键拖拽 x=%{public}d y=%{public}d',
      this.lastTouchX, this.lastTouchY);
    this.pointerDown(this.lastTouchX, this.lastTouchY, 1);
    this.pressSent = this.pressedMask !== 0;
  }

  /**
   * Sends a right click: move to the point, press the right button, release.
   * The mask comes from Remote.buttonMask so RFB and RDP receive exactly the
   * same right-button bit as a mouse right click. Nothing else is sent for
   * this touch until the finger lifts.
   */
  private sendLongPressRightClick(): void {
    const rightMask = Remote.buttonMask(MouseButton.Right);
    this.longPressFired = true;
    this.pressPending = false;
    hilog.debug(DOMAIN, TAG,
      '长按触发右键单击 mask=%{public}d x=%{public}d y=%{public}d',
      rightMask, this.lastTouchX, this.lastTouchY);
    this.sendPointerReport(this.lastTouchX, this.lastTouchY, 0);
    this.sendPointerReport(this.lastTouchX, this.lastTouchY, rightMask);
    this.sendPointerReport(this.lastTouchX, this.lastTouchY, 0);
  }

  /**
   * Arms the single-finger long-press timer on Down. If exactly one finger is
   * still down, no second finger joined and the finger stayed inside the tap
   * slop when it expires, the touch becomes a right click; every other outcome
   * is resolved by the move/Up/cancel paths.
   */
  private armLongPress(): void {
    this.resetTouchPressState();
    this.pressPending = true;
    this.touchDownX = this.lastTouchX;
    this.touchDownY = this.lastTouchY;
    hilog.debug(DOMAIN, TAG, '触摸长按计时开始 x=%{public}d y=%{public}d',
      this.lastTouchX, this.lastTouchY);
    this.longPressTimer = setTimeout((): void => {
      this.longPressTimer = -1;
      if (!this.pressPending) {
        return;
      }
      // hasMouse is deliberately NOT a condition here: a real mouse event in
      // the past (or hovering) must no longer disable the touch path, otherwise
      // a touchscreen long press never becomes a right click on PC.
      if (this.disposed || this.multiTouchBlocked ||
          this.touchFingerIds.size !== 1) {
        this.pressPending = false;
        hilog.debug(DOMAIN, TAG,
          '长按到期未触发 fingers=%{public}d blocked=%{public}s',
          this.touchFingerIds.size, String(this.multiTouchBlocked));
        return;
      }
      if (this.exceedsTapSlop()) {
        // The timer can win the race against a Move that crossed the slop:
        // a drag must not turn into a right click.
        this.sendDeferredPress();
        return;
      }
      this.sendLongPressRightClick();
    }, LONG_PRESS_MS);
  }

  /**
   * Three or more fingers: freeze the view where it is and stay out of the
   * way. The live gesture is ended (final transform flushed, baseline
   * dropped), so the frozen zoom/pan become the base of the next two-finger
   * gesture and nothing jumps when the extra fingers lift. Pinch/pan callbacks
   * return immediately while this is set.
   */
  private enterMultiTouchBlock(): void {
    if (this.multiTouchBlocked) {
      return;
    }
    this.multiTouchBlocked = true;
    this.multiTouchActive = true;
    this.cancelLongPress('three-fingers');
    // The only remote event that can still go out here is releasing a button
    // the first finger already pressed while dragging. Leaving the button down
    // would be worse.
    this.abortSingleFingerInput();
    this.resetTouchPressState();
    this.endGesture();
    hilog.info(DOMAIN, TAG,
      '三指及以上手势屏蔽 session=%{public}s fingers=%{public}d view=%{public}s',
      this.sessionId, this.touchFingerIds.size, this.viewStateText());
  }

  /** Every finger is up again: leave the block, zoom/pan stay where they are. */
  private leaveMultiTouchBlock(): void {
    if (!this.multiTouchBlocked) {
      return;
    }
    this.multiTouchBlocked = false;
    this.multiTouchActive = false;
    hilog.info(DOMAIN, TAG, '三指及以上手势结束 session=%{public}s view=%{public}s',
      this.sessionId, this.viewStateText());
  }

  /**
   * Path-independent three-finger enforcement for the gesture callbacks. The
   * recognizers stay registered for two fingers and keep reporting while extra
   * fingers are down, so a pinch/pan callback is the place where the finger
   * count must be checked even when the touch path never ran (on PC a touch
   * that looks like the mouse duplicate is dropped, although two or more
   * fingers are never dropped). Returns true when this callback must return
   * without touching zoom/pan or sending anything.
   */
  private blockGestureOnExtraFingers(event: GestureEvent | undefined): boolean {
    if (!USE_GL_RENDER) {
      return false;
    }
    if (this.multiTouchBlocked) {
      return true;
    }
    let listed = 0;
    if (event !== undefined && event.fingerList !== undefined) {
      listed = event.fingerList.length;
    }
    // fingerList is the GestureEvent's own count; the touch ids are the
    // fallback (and cover callbacks that carry no event).
    const fingers = listed > 0 ? listed : this.touchFingerIds.size;
    if (fingers < MIN_BLOCKED_FINGERS) {
      return false;
    }
    hilog.debug(DOMAIN, TAG,
      '三指及以上屏蔽 reason=gesture-callback fingers=%{public}d listed=%{public}d view=%{public}s',
      fingers, listed, this.viewStateText());
    this.enterMultiTouchBlock();
    return true;
  }

  private beginGesture(): void {
    if (!USE_GL_RENDER || this.disposed || this.sessionId.length === 0) {
      return;
    }
    // A pinch/pan recognizer has started: this touch can no longer become a
    // single-finger click or long press, so drop the pending press and never
    // press for the rest of the gesture.
    this.cancelLongPress('pinch');
    if (this.gestureActive) {
      return;
    }
    const geom = this.computeViewGeometry(this.viewZoom, this.viewPanX, this.viewPanY);
    if (geom === undefined) {
      // No presented geometry yet: a two-finger gesture must not invent one.
      return;
    }
    this.gestureActive = true;
    this.multiTouchActive = true;
    this.gestureStartZoom = geom.zoom;
    this.gestureStartPanX = geom.panX;
    this.gestureStartPanY = geom.panY;
    this.gestureScale = 1;
    this.gesturePanRawX = 0;
    this.gesturePanRawY = 0;
    this.gesturePanBaseX = 0;
    this.gesturePanBaseY = 0;
    this.gestureAnchorValid = false;
    // The first of the two fingers may already have pressed a remote button:
    // release it before the view starts moving. Any long-press state is over.
    this.abortSingleFingerInput();
    this.resetTouchPressState();
    hilog.info(DOMAIN, TAG, '双指手势开始 session=%{public}s view=%{public}s',
      this.sessionId, this.viewStateText());
  }

  /**
   * Anchors the pinch: remembers the remote pixel under the pinch centre and
   * restarts the scale/pan measurement from the current transform.
   */
  private setPinchAnchor(centerX: number, centerY: number): void {
    if (!this.gestureActive) {
      return;
    }
    const geom = this.computeViewGeometry(this.viewZoom, this.viewPanX, this.viewPanY);
    if (geom === undefined || geom.scaleX <= 0 || geom.scaleY <= 0) {
      return;
    }
    const pxPerVpX = this.surfacePixelsPerVpX();
    const pxPerVpY = this.surfacePixelsPerVpY();
    if (pxPerVpX <= 0 || pxPerVpY <= 0) {
      return;
    }
    this.gestureCenterSurfX = centerX * pxPerVpX;
    this.gestureCenterSurfY = centerY * pxPerVpY;
    this.gestureAnchorRemoteX = (this.gestureCenterSurfX - geom.left) / geom.scaleX;
    this.gestureAnchorRemoteY = (this.gestureCenterSurfY - geom.top) / geom.scaleY;
    this.gestureAnchorValid = true;
    this.gestureStartZoom = geom.zoom;
    this.gestureStartPanX = geom.panX;
    this.gestureStartPanY = geom.panY;
    this.gestureScale = 1;
    // The two-finger pan offset is measured from here on, so a pinch that
    // starts after the drag already moved does not double-count that motion.
    this.gesturePanBaseX = this.gesturePanRawX;
    this.gesturePanBaseY = this.gesturePanRawY;
  }

  /** Recomputes zoom/pan from the current gesture state and schedules a send. */
  private applyGestureView(): void {
    if (!this.gestureActive || this.disposed) {
      return;
    }
    const zoom = this.clampViewZoom(this.gestureStartZoom * this.gestureScale);
    // Centred layout at this zoom: its left/top are the "pan 0" reference.
    const centred = this.computeViewGeometry(zoom, 0, 0);
    if (centred === undefined) {
      return;
    }
    const dx = (this.gesturePanRawX - this.gesturePanBaseX) * this.surfacePixelsPerVpX();
    const dy = (this.gesturePanRawY - this.gesturePanBaseY) * this.surfacePixelsPerVpY();
    let panX = this.gestureStartPanX + dx;
    let panY = this.gestureStartPanY + dy;
    if (this.gestureAnchorValid) {
      // Keep the remote pixel that was under the pinch centre under the same
      // surface point: left = centre - anchor * scale, converted to a pan
      // request relative to the centred layout. The gesture's two-finger drag
      // adds on top of that.
      const scaleX = centred.contentW / this.frameWidth;
      const scaleY = centred.contentH / this.frameHeight;
      panX = this.gestureCenterSurfX - this.gestureAnchorRemoteX * scaleX - centred.left + dx;
      panY = this.gestureCenterSurfY - this.gestureAnchorRemoteY * scaleY - centred.top + dy;
    }
    const geom = this.computeViewGeometry(zoom, panX, panY);
    if (geom === undefined) {
      return;
    }
    this.viewZoom = geom.zoom;
    this.viewPanX = geom.panX;
    this.viewPanY = geom.panY;
    this.scheduleViewTransform();
  }

  private endGesture(): void {
    if (!this.gestureActive) {
      return;
    }
    this.gestureActive = false;
    this.gestureAnchorValid = false;
    this.gestureScale = 1;
    this.gesturePanRawX = 0;
    this.gesturePanRawY = 0;
    if (!USE_GL_RENDER || this.disposed) {
      return;
    }
    // Send the final transform now, then read the presented geometry back so a
    // tap immediately after the gesture maps against what is on screen.
    this.flushViewTransform();
    this.scheduleGeometryRefresh(80);
    hilog.info(DOMAIN, TAG, '双指手势结束 session=%{public}s view=%{public}s',
      this.sessionId, this.viewStateText());
  }

  private onPinchStart(event: GestureEvent): void {
    // Three or more fingers are the system's: no transform, no pointer event.
    if (this.blockGestureOnExtraFingers(event)) {
      return;
    }
    if (!USE_GL_RENDER || this.multiTouchBlocked) {
      return;
    }
    this.beginGesture();
    this.setPinchAnchor(event.pinchCenterX, event.pinchCenterY);
  }

  private onPinchUpdate(event: GestureEvent): void {
    if (this.blockGestureOnExtraFingers(event)) {
      return;
    }
    if (!USE_GL_RENDER || this.disposed || this.multiTouchBlocked) {
      return;
    }
    this.beginGesture();
    if (!this.gestureActive) {
      return;
    }
    if (!this.gestureAnchorValid) {
      this.setPinchAnchor(event.pinchCenterX, event.pinchCenterY);
    }
    this.gestureScale = event.scale > 0 ? event.scale : 1;
    this.applyGestureView();
  }

  private onPinchEnd(event: GestureEvent): void {
    if (this.blockGestureOnExtraFingers(event)) {
      return;
    }
    if (this.multiTouchBlocked) {
      return;
    }
    this.endGesture();
  }

  private onTwoFingerPanStart(event: GestureEvent): void {
    if (this.blockGestureOnExtraFingers(event)) {
      return;
    }
    if (!USE_GL_RENDER || this.multiTouchBlocked) {
      return;
    }
    this.beginGesture();
    if (!this.gestureActive) {
      return;
    }
    this.gesturePanRawX = event.offsetX;
    this.gesturePanRawY = event.offsetY;
    this.gesturePanBaseX = event.offsetX;
    this.gesturePanBaseY = event.offsetY;
  }

  private onTwoFingerPanUpdate(event: GestureEvent): void {
    if (this.blockGestureOnExtraFingers(event)) {
      return;
    }
    if (!USE_GL_RENDER || this.disposed || this.multiTouchBlocked) {
      return;
    }
    this.beginGesture();
    if (!this.gestureActive) {
      return;
    }
    this.gesturePanRawX = event.offsetX;
    this.gesturePanRawY = event.offsetY;
    this.applyGestureView();
  }

  private onTwoFingerPanEnd(event: GestureEvent): void {
    if (this.blockGestureOnExtraFingers(event)) {
      return;
    }
    if (this.multiTouchBlocked) {
      return;
    }
    this.endGesture();
  }

  /**
   * Window/surface size changed (rotation, window resize). The native renderer
   * re-clamps the pan against the new surface size while drawing; re-sending
   * the transform triggers that redraw, and the following stats read adopts
   * the re-clamped pan.
   */
  private handleViewResize(): void {
    if (!USE_GL_RENDER || this.disposed) {
      return;
    }
    if (this.gestureActive) {
      this.endGesture();
    }
    this.scheduleViewTransform();
    this.scheduleGeometryRefresh(100);
  }

  private static buttonMask(button: MouseButton): number {
    if (button === MouseButton.Right) {
      return 4;
    }
    if (button === MouseButton.Middle) {
      return 2;
    }
    return 1;
  }

  private pointerDown(x: number, y: number, mask: number): void {
    // One physical click on PC can arrive twice: onMouse plus a synthetic
    // TouchEvent. Forwarding both turned a single click into a double click, so
    // ignore a press while another button is already being held.
    if (this.pressedMask !== 0) {
      return;
    }
    this.pressedMask = mask;
    // Move first: the working desktop path sends move, press, release; sending
    // the press without moving the pointer can be ignored by the server.
    this.sendPointerReport(x, y, 0);
    this.sendPointerReport(x, y, mask);
  }

  private pointerUp(x: number, y: number, fromTouch: boolean): void {
    if (this.pressedMask === 0) {
      return;
    }
    this.pressedMask = 0;
    // Release with the last known-good remote coordinates: the physical point
    // may be outside the picture (letterbox) by now, and dropping the release
    // would leave the button pressed on the server forever.
    const point = this.mapToRemote(x, y);
    let rx = point[0];
    let ry = point[1];
    if (rx < 0 || ry < 0) {
      rx = this.lastRemoteX;
      ry = this.lastRemoteY;
    }
    if (rx >= 0 && ry >= 0) {
      this.lastRemoteX = rx;
      this.lastRemoteY = ry;
      hilog.debug(DOMAIN, TAG,
        '远端指针 source=release mask=0 remote=%{public}d,%{public}d',
        rx, ry);
      sendPointer(this.sessionId, rx, ry, 0);
    }
    // Touch screens have no double-click gesture: two quick taps at the same
    // spot become a real double click. A mouse already sends its own.
    const now = Date.now();
    const near = Math.abs(x - this.lastTapX) < 24 && Math.abs(y - this.lastTapY) < 24;
    if (fromTouch && now - this.lastTapMs < 400 && near && rx >= 0 && ry >= 0) {
      hilog.debug(DOMAIN, TAG,
        '远端指针 source=doubleclick mask=1,0 remote=%{public}d,%{public}d', rx, ry);
      sendPointer(this.sessionId, rx, ry, 1);
      sendPointer(this.sessionId, rx, ry, 0);
      this.lastTapMs = 0;
      return;
    }
    this.lastTapMs = now;
    this.lastTapX = x;
    this.lastTapY = y;
  }

  // Maps a view point (vp) to remote pixels; returns -1,-1 when outside the
  // picture. GL mode maps through the geometry the native renderer reported for
  // the frame on screen (contentLeft/contentTop/contentScale*), converted from
  // vp to surface pixels first, so clicks stay exact at any zoom/pan. Only when
  // no snapshot exists (first frame) does it fall back to the old contain
  // formula in vp.
  private mapToRemote(x: number, y: number): number[] {
    if (this.frameWidth <= 0 || this.frameHeight <= 0 ||
        this.viewWidth <= 0 || this.viewHeight <= 0) {
      return [-1, -1];
    }
    if (USE_GL_RENDER && this.statsGeometryValid) {
      const pxPerVpX = this.surfacePixelsPerVpX();
      const pxPerVpY = this.surfacePixelsPerVpY();
      if (pxPerVpX > 0 && pxPerVpY > 0 && this.statsContentScale > 0 &&
          this.statsContentScaleY > 0) {
        const px = x * pxPerVpX;
        const py = y * pxPerVpY;
        if (px < this.statsContentLeft || py < this.statsContentTop ||
            px > this.statsContentLeft + this.statsContentW ||
            py > this.statsContentTop + this.statsContentH) {
          return [-1, -1];
        }
        return [
          Math.max(0, Math.min(this.frameWidth - 1,
            Math.round((px - this.statsContentLeft) / this.statsContentScale))),
          Math.max(0, Math.min(this.frameHeight - 1,
            Math.round((py - this.statsContentTop) / this.statsContentScaleY)))
        ];
      }
    }
    // Fallback (Canvas path / before the first stats snapshot): the historical
    // contain layout computed in vp.
    const scale = Math.min(this.viewWidth / this.frameWidth, this.viewHeight / this.frameHeight);
    const contentW = this.frameWidth * scale;
    const contentH = this.frameHeight * scale;
    const offsetX = (this.viewWidth - contentW) / 2;
    const offsetY = (this.viewHeight - contentH) / 2;
    if (x < offsetX || y < offsetY || x > offsetX + contentW || y > offsetY + contentH) {
      return [-1, -1];
    }
    return [
      Math.max(0, Math.min(this.frameWidth - 1, Math.round((x - offsetX) / scale))),
      Math.max(0, Math.min(this.frameHeight - 1, Math.round((y - offsetY) / scale)))
    ];
  }

  // The UIContext FocusController transfers synchronously, which is what a
  // click handler needs.
  private requestViewFocus(): void {
    try {
      this.getUIContext().getFocusController().activate(true);
      this.getUIContext().getFocusController().requestFocus('remote_area');
      this.getUIContext().getFocusController().requestFocus('page_root');
    } catch (error) {
      // Focus transfer is best effort: if it fails, keys simply may not arrive.
    }
  }

  private forwardWheel(event: AxisEvent): void {
    if (event.action !== AxisAction.UPDATE && event.action !== AxisAction.END) {
      return;
    }
    const vertical = event.getVerticalAxisValue();
    if (vertical === 0 || !this.canvasShown) {
      return;
    }
    const point = this.mapToRemote(this.viewPointerX, this.viewPointerY);
    if (point[0] < 0 || point[1] < 0) {
      return;
    }
    const mask = vertical > 0 ? 16 : 8;
    sendPointer(this.sessionId, point[0], point[1], mask);
    sendPointer(this.sessionId, point[0], point[1], 0);
  }

  private sendPointerReport(x: number, y: number, mask: number): void {
    const point = this.mapToRemote(x, y);
    if (point[0] < 0 || point[1] < 0) {
      return;
    }
    this.lastRemoteX = point[0];
    this.lastRemoteY = point[1];
    hilog.debug(DOMAIN, TAG,
      '远端指针 source=report mask=%{public}d remote=%{public}d,%{public}d',
      mask, point[0], point[1]);
    sendPointer(this.sessionId, point[0], point[1], mask);
  }

  private sendPointerAt(x: number, y: number, mask: number): void {
    const point = this.mapToRemote(x, y);
    if (point[0] >= 0 && point[1] >= 0) {
      this.lastRemoteX = point[0];
      this.lastRemoteY = point[1];
      sendPointer(this.sessionId, point[0], point[1], mask);
      return;
    }
    // Dragged outside the picture (letterbox): keep sending at the last valid
    // point while a button is held, so the release still ends the drag inside
    // the remote desktop.
    if (mask !== 0 && this.lastRemoteX >= 0 && this.lastRemoteY >= 0) {
      sendPointer(this.sessionId, this.lastRemoteX, this.lastRemoteY, mask);
    }
  }

  private keysymFor(event: KeyEvent): number {
    switch (event.keyCode) {
      case KeyCode.KEYCODE_ENTER:
      case KeyCode.KEYCODE_NUMPAD_ENTER:
        return 0xff0d;
      case KeyCode.KEYCODE_DEL:
        return 0xff08;
      case KeyCode.KEYCODE_FORWARD_DEL:
        return 0xffff;
      case KeyCode.KEYCODE_TAB:
        return 0xff09;
      case KeyCode.KEYCODE_ESCAPE:
        return 0xff1b;
      case KeyCode.KEYCODE_DPAD_UP:
        return 0xff52;
      case KeyCode.KEYCODE_DPAD_DOWN:
        return 0xff54;
      case KeyCode.KEYCODE_DPAD_LEFT:
        return 0xff51;
      case KeyCode.KEYCODE_DPAD_RIGHT:
        return 0xff53;
      case KeyCode.KEYCODE_MOVE_HOME:
        return 0xff50;
      case KeyCode.KEYCODE_MOVE_END:
        return 0xff57;
      case KeyCode.KEYCODE_PAGE_UP:
        return 0xff55;
      case KeyCode.KEYCODE_PAGE_DOWN:
        return 0xff56;
      case KeyCode.KEYCODE_SHIFT_LEFT:
        return 0xffe1;
      case KeyCode.KEYCODE_SHIFT_RIGHT:
        return 0xffe2;
      case KeyCode.KEYCODE_CTRL_LEFT:
        return 0xffe3;
      case KeyCode.KEYCODE_CTRL_RIGHT:
        return 0xffe4;
      case KeyCode.KEYCODE_ALT_LEFT:
        return 0xffe9;
      case KeyCode.KEYCODE_ALT_RIGHT:
        return 0xffea;
      default:
        break;
    }
    // HarmonyOS reports letters/digits as ranges, and keyText holds the constant
    // name ("KEYCODE_A") rather than the character, so map by key code.
    const kc = event.keyCode;
    if (kc >= KeyCode.KEYCODE_A && kc <= KeyCode.KEYCODE_Z) {
      return 0x61 + (kc - KeyCode.KEYCODE_A);
    }
    if (kc >= KeyCode.KEYCODE_0 && kc <= KeyCode.KEYCODE_9) {
      return 0x30 + (kc - KeyCode.KEYCODE_0);
    }
    const punctuation: Map<number, number> = new Map<number, number>([
      [KeyCode.KEYCODE_SPACE, 0x20],
      [KeyCode.KEYCODE_MINUS, 0x2d],
      [KeyCode.KEYCODE_EQUALS, 0x3d],
      [KeyCode.KEYCODE_LEFT_BRACKET, 0x5b],
      [KeyCode.KEYCODE_RIGHT_BRACKET, 0x5d],
      [KeyCode.KEYCODE_BACKSLASH, 0x5c],
      [KeyCode.KEYCODE_SEMICOLON, 0x3b],
      [KeyCode.KEYCODE_APOSTROPHE, 0x27],
      [KeyCode.KEYCODE_COMMA, 0x2c],
      [KeyCode.KEYCODE_PERIOD, 0x2e],
      [KeyCode.KEYCODE_SLASH, 0x2f],
      [KeyCode.KEYCODE_GRAVE, 0x60]
    ]);
    const punctuationSym = punctuation.get(kc);
    if (punctuationSym !== undefined) {
      return punctuationSym;
    }
    const text = event.keyText;
    if (text !== undefined && text.length === 1) {
      const code = text.charCodeAt(0);
      if (code >= 0x20) {
        return code;
      }
    }
    return 0;
  }

  private forwardKey(event: KeyEvent): void {
    // The page root stays mounted when resolution failed; never forward keys to
    // a session this page could not bind itself to.
    if (this.sessionId.length === 0 || !this.sessionResolved) {
      return;
    }
    // Track modifiers locally: the server did not apply the Shift keysym we
    // sent, so shifted characters are translated to their own keysym instead.
    if (event.keyCode === KeyCode.KEYCODE_SHIFT_LEFT ||
        event.keyCode === KeyCode.KEYCODE_SHIFT_RIGHT) {
      this.shiftDown = event.type === KeyType.Down;
    }
    let keysym = this.keysymFor(event);
    const down = event.type === KeyType.Down;
    if (this.shiftDown && keysym > 0) {
      const shifted = Remote.shiftedKeysym(keysym);
      if (shifted !== 0) {
        keysym = shifted;
      }
    }
    if (keysym === 0) {
      return;
    }
    if (down) {
      // Remember what was pressed per physical key so the release always uses
      // the same keysym even if the modifiers changed in between.
      this.pressedKeysym.set(event.keyCode, keysym);
      sendKey(this.sessionId, keysym, 1);
    } else if (event.type === KeyType.Up) {
      const pressedSym = this.pressedKeysym.get(event.keyCode);
      if (pressedSym !== undefined) {
        keysym = pressedSym;
        this.pressedKeysym.delete(event.keyCode);
      }
      sendKey(this.sessionId, keysym, 0);
    }
  }

  // US layout: the shifted symbol for the unshifted ASCII keysym.
  private static shiftedKeysym(sym: number): number {
    const pairs: Map<number, number> = new Map<number, number>([
      [0x61, 0x41], [0x62, 0x42], [0x63, 0x43], [0x64, 0x44], [0x65, 0x45],
      [0x66, 0x46], [0x67, 0x47], [0x68, 0x48], [0x69, 0x49], [0x6a, 0x4a],
      [0x6b, 0x4b], [0x6c, 0x4c], [0x6d, 0x4d], [0x6e, 0x4e], [0x6f, 0x4f],
      [0x70, 0x50], [0x71, 0x51], [0x72, 0x52], [0x73, 0x53], [0x74, 0x54],
      [0x75, 0x55], [0x76, 0x56], [0x77, 0x57], [0x78, 0x58], [0x79, 0x59],
      [0x7a, 0x5a],
      [0x31, 0x21], [0x32, 0x40], [0x33, 0x23], [0x34, 0x24], [0x35, 0x25],
      [0x36, 0x5e], [0x37, 0x26], [0x38, 0x2a], [0x39, 0x28], [0x30, 0x29],
      [0x2d, 0x5f], [0x3d, 0x2b], [0x5b, 0x7b], [0x5d, 0x7d],
      [0x5c, 0x7c], [0x3b, 0x3a], [0x27, 0x22], [0x2c, 0x3c],
      [0x2e, 0x3e], [0x2f, 0x3f], [0x60, 0x7e]
    ]);
    const shifted = pairs.get(sym);
    return shifted === undefined ? 0 : shifted;
  }

  /** 确认并信任: persist the fingerprint, reconnect this session. */
  private trustAndReconnect(): void {
    if (this.fingerprint.length === 0) {
      return;
    }
    const session = findSession(this.sessionId);
    const endpoint = session === undefined ? '' : session.endpoint;
    if (endpoint.length === 0) {
      // Without an endpoint the fingerprint cannot be stored, so reconnecting
      // would just land on "trust required" again. Fail closed rather than
      // claiming the server is trusted.
      this.statusText = '无法确定该服务器地址，请在连接列表里确认身份';
      return;
    }
    // Same protocol-scoped key as the connection list: VNC keeps the legacy
    // "host:port" key, RDP uses "rdp:host:port". An orphan session has no
    // connection entry, so it falls back to the endpoint it was adopted with.
    const entry = session !== undefined && session.connectionId.length > 0
      ? findConnection(session.connectionId) : undefined;
    trustFingerprint(entry === undefined ? endpoint : pinKeyFor(entry), this.fingerprint);
    reconnectSession(this.sessionId);
    // The reconnect is already in flight; do not wait for the next 1 s status
    // poll to learn the generation changed. Invalidate the in-flight render and
    // the local mirror NOW and make the next render request a complete frame.
    this.resetViewTransform('trust-reconnect');
    this.invalidateRender();
    this.forceFull = true;
    this.needsTrust = false;
    this.statusText = '已信任，正在重连…';
  }

  /**
   * The only action left when this page could not attribute itself to a
   * session. The page always lives in its own ability window, so there is no
   * list to go back to: try to close this window. Closing it also drops the
   * unknown instance; if the platform refuses a programmatic main-window close,
   * the title-bar X remains the normal path (RemoteAbility.onDestroy ends the
   * session it knows, if any).
   */
  private async leavePage(): Promise<void> {
    this.teardown();
    if (this.ownWindowName.length > 0) {
      try {
        await window.findWindow(this.ownWindowName).destroyWindow();
        return;
      } catch (error) {
        hilog.warn(DOMAIN, TAG, '关闭无法归属的远程窗口失败（请用标题栏关闭）：%{public}s',
          String(error));
      }
    }
    try {
      this.getUIContext().getRouter().back();
    } catch (error) {
      hilog.warn(DOMAIN, TAG, '返回连接列表失败：%{public}s', String(error));
    }
  }

  /**
   * The system back action on a session window (phone back key / back gesture).
   *
   * The page is router-managed (@Entry), so onBackPress is the supported hook.
   * Returning true means this page handles the return itself: it does NOT let
   * the platform merely background the window while the session keeps running
   * (that was the phone's only behaviour without a title bar). Instead it takes
   * the same "closing the window = disconnecting" path as the desktop title-bar
   * X: close this session, then terminate this window.
   *
   * The session id is this window's own binding, so a back press can never close
   * another session. A second back press while the close is still in flight
   * falls back to the platform default rather than trapping the user on a page
   * whose own close did not complete.
   */
  onBackPress(): boolean {
    if (this.sessionId.length === 0) {
      // Nothing was ever bound to this window: just leave.
      this.leavePage();
      return true;
    }
    if (this.backCloseRequested) {
      return false;
    }
    this.backCloseRequested = true;
    this.endSessionAndWindow();
    return true;
  }

  /**
   * Ends this page's session and closes its window. This is exactly the path
   * the connection list's 断开 uses (`closeSessionById` + `closeRemoteWindow`):
   * the card drops back to 未连接 immediately, the window terminates itself via
   * RemoteAbility's programmatic-close flag (onDestroy then only drops the
   * window instead of closing a second time), and closeRemoteWindow falls back
   * to destroyWindow() if the instance does not react. teardown() runs first so
   * the renderer and input are already parked when the window goes away.
   */
  private endSessionAndWindow(): void {
    const sessionId = this.sessionId;
    if (sessionId.length === 0) {
      this.leavePage();
      return;
    }
    this.teardown();
    closeSessionById(sessionId);
    closeRemoteWindow(sessionId).then((): void => {}).catch((error: Error): void => {
      hilog.warn(DOMAIN, TAG, '返回时关闭会话窗口失败，直接销毁本窗口：%{public}s',
        String(error));
      this.leavePage();
    });
  }

  build() {
    Column({ space: 8 }) {
      if (this.needsTrust) {
        Column({ space: 8 }) {
          Text('服务端指纹：' + this.fingerprint).fontSize(14).fontWeight(FontWeight.Medium)
          Text('请与服务端核对。确认后记住该指纹，之后不一致将拒绝连接。').fontSize(12)
          Button('确认并信任').onClick(() => { this.trustAndReconnect(); })
        }
        .width('100%')
        .borderRadius(12)
        .backgroundColor($r('app.color.warning_surface'))
      }
      if (this.resolutionFailed) {
        Column({ space: 12 }) {
          Text('会话不可用').fontSize(16).fontWeight(FontWeight.Medium)
          Text(this.statusText).fontSize(13).textAlign(TextAlign.Center)
          Text('远程输入已禁用，避免作用到其它会话。').fontSize(12)
            .fontColor($r('app.color.text_secondary'))
          Button('返回连接列表').onClick(() => { this.leavePage(); })
        }
        .width('100%')
        .layoutWeight(1)
        .justifyContent(FlexAlign.Center)
      } else if (this.sessionResolved) {
        Stack({ alignContent: Alignment.BottomEnd }) {
          Column() {
            Stack({ alignContent: Alignment.TopStart }) {
              if (USE_GL_RENDER) {
                // The native EGL renderer owns this surface (frames, layout,
                // vsync); the controller subclass reports surface lifetime.
                // The XComponent itself stays transparent to hit testing so
                // every pointer/touch/wheel event reaches this Column exactly
                // as it did with the Canvas.
                XComponent({ type: XComponentType.SURFACE, controller: this.glController })
                  .onLoad((): void => {
                    // Surface creation is reported through the controller;
                    // onLoad only covers runtimes that report the id here.
                    try {
                      this.attachRendererSurface(this.glController.getXComponentSurfaceId());
                    } catch (error) {
                      hilog.warn(DOMAIN, TAG, '读取 surfaceId 失败：%{public}s', String(error));
                    }
                  })
                  .onDestroy((): void => {
                    this.detachRendererSurface('component-destroyed');
                  })
                  .width('100%')
                  .height('100%')
                  .hitTestBehavior(HitTestMode.None)
              } else {
                Canvas(this.ctx)
                  .width('100%')
                  .height('100%')
                  .draggable(false)
                  .onReady(() => {
                    this.canvasReady = true;
                  })
              }
              if (this.cursorPm !== undefined && this.cursorLeft >= -64) {
                Image(this.cursorPm)
                  .width(this.cursorDrawW)
                  .height(this.cursorDrawH)
                  .position({ x: this.cursorLeft, y: this.cursorTop })
                  .hitTestBehavior(HitTestMode.None)
                  .draggable(false)
              }
            }
            .width('100%')
            .height('100%')
          }
          .width('100%')
          .height('100%')
          .id('remote_area')
          .key('remote_area')
          .draggable(false)
          .focusable(true)
          .focusOnTouch(true)
          .defaultFocus(true)
          .tabIndex(0)
          .onAreaChange((oldValue: Area, newValue: Area) => {
            const width = Number(newValue.width);
            const height = Number(newValue.height);
            this.viewWidth = width;
            this.viewHeight = height;
            // A pure window move also fires onAreaChange; a real size change
            // only changes how the existing mirror is scaled. Resizing this
            // window does NOT change the remote resolution, so the complete
            // picture is already here: repaint it at the new scale instead of
            // asking the server for a redundant full frame. Full-frame requests
            // remain on the first frame, generation changes, desktop-size
            // changes and page re-show (those need new pixels).
            const resized = width !== this.lastAreaWidth || height !== this.lastAreaHeight;
            this.lastAreaWidth = width;
            this.lastAreaHeight = height;
            if (resized) {
              this.redrawAfterResize();
              if (USE_GL_RENDER) {
                this.handleViewResize();
              }
            }
            this.placeCursor();
          })
          // Keys are handled on the page root only: key events bubble, so two
          // handlers forwarded every key twice and broke Shift combinations.
          .onAxisEvent((event: AxisEvent) => {
            this.forwardWheel(event);
          })
          .onHover((isHover: boolean) => {
            this.setHostCursorVisible(!isHover);
          })
          .onTouch((event: TouchEvent) => {
            // On PC one physical mouse action also produces a synthetic
            // TouchEvent. The mouse path keeps that action; only this single
            // touch is dropped (isMouseDuplicateTouch) so handling both can no
            // longer forward every click twice. Everything else runs on any
            // device: the touch path is not disabled by a mouse having been
            // seen (that is what made the PC touchscreen unable to click or
            // long-press at all).
            // Track the fingers by identifier: event.touches may be empty on
            // Up/Cancel, and the count gates the long press and the
            // three-finger block below. It is updated before the de-duplication
            // so three or more fingers always enter the block.
            const fingers = this.updateTouchFingers(event);
            if (USE_GL_RENDER) {
              if (fingers >= MIN_BLOCKED_FINGERS) {
                // Three or more fingers are the system's: no view transform,
                // and no remote pointer event from here on.
                this.enterMultiTouchBlock();
              } else if (fingers === 0) {
                // Every finger is up: a three-finger block is over.
                this.leaveMultiTouchBlock();
              }
            }
            // A mouse never produces a second finger, so only a one-finger
            // sequence can be its duplicate. The drop happens after the count
            // above so the block still works on a device with a mouse.
            if (fingers <= 1 && this.isMouseDuplicateTouch(event)) {
              return;
            }
            // Two fingers belong to the view gestures in GL mode: never forward
            // them as a remote pointer, and release a button the first finger
            // may already have pressed before the second one landed. The flag
            // clears on a fresh one-finger Down, so a stray Up ordering can
            // never let the remaining gesture finger click the remote.
            if (USE_GL_RENDER &&
                (fingers >= 2 || this.multiTouchActive || this.multiTouchBlocked)) {
              this.multiTouchActive = true;
              // No press for a gesture: a second finger cancels the pending
              // press and long-press timer, and a press already sent is
              // released here.
              this.cancelLongPress('multi-finger');
              this.abortSingleFingerInput();
              this.resetTouchPressState();
              const freshSingleDown = !this.multiTouchBlocked &&
                event.type === TouchType.Down && fingers === 1;
              if (!freshSingleDown) {
                if (!this.multiTouchBlocked && fingers === 0) {
                  this.multiTouchActive = false;
                }
                return;
              }
              // A new one-finger touch starts here: the gesture is over.
              this.multiTouchActive = false;
            }
            if (event.touches.length > 0) {
              this.lastTouchX = event.touches[0].x;
              this.lastTouchY = event.touches[0].y;
              this.viewPointerX = this.lastTouchX;
              this.viewPointerY = this.lastTouchY;
              // Move the local cursor now, not on the next poll.
              this.placeCursor();
            }
            if (event.type === TouchType.Down) {
              this.requestViewFocus();
              // Do not press yet: this touch may still become a pinch, a tap
              // or a long press. The mask-0 move keeps the remote pointer (and
              // its hover) where the press will happen.
              this.sendPointerAt(this.lastTouchX, this.lastTouchY, 0);
              this.armLongPress();
            } else if (event.type === TouchType.Move) {
              if (this.longPressFired) {
                // The right click already went out on the timer: send nothing
                // at all until the finger lifts (the local cursor above still
                // follows the finger).
                return;
              }
              if (this.pressPending && this.exceedsTapSlop()) {
                // The finger travelled: this is a drag, not a tap or a long
                // press. Press the left button first so the move below drags
                // with it held.
                this.sendDeferredPress();
              }
              this.sendPointerAt(this.lastTouchX, this.lastTouchY, this.pressedMask);
            } else if (event.type === TouchType.Up) {
              if (this.longPressFired) {
                // The right click was already sent when the timer expired;
                // this touch must not add a left click on release.
                hilog.debug(DOMAIN, TAG, '长按右键抬手，不补发指针事件');
              } else if (this.pressPending) {
                // Quick tap: lifted before the long-press timer and never
                // moved past the slop, so no press went out yet. Send press +
                // release now so the tap still clicks exactly once.
                this.cancelLongPress('tap');
                this.pointerDown(this.lastTouchX, this.lastTouchY, 1);
                this.pointerUp(this.lastTouchX, this.lastTouchY, true);
              } else if (this.pressSent) {
                this.pointerUp(this.lastTouchX, this.lastTouchY, true);
              }
              this.resetTouchPressState();
            } else if (event.type === TouchType.Cancel) {
              // A cancelled touch must not turn into a click or a right click.
              this.cancelLongPress('cancel');
              this.pointerUp(this.lastTouchX, this.lastTouchY, true);
              this.resetTouchPressState();
            }
          })
          .onMouse((event: MouseEvent) => {
            if (!this.hasMouse) {
              this.hasMouse = true;
              hilog.debug(DOMAIN, TAG,
                '首个真实鼠标事件 action=%{public}d x=%{public}d y=%{public}d：触摸路径不再整体禁用（只按时间/距离丢弃本次合成触摸）',
                event.action, Math.round(event.x), Math.round(event.y));
            }
            // Remember where and when the mouse last acted: the synthetic touch
            // derived from this action is recognised by these two, see
            // isMouseDuplicateTouch().
            this.lastMouseEventMs = Date.now();
            this.lastMouseX = event.x;
            this.lastMouseY = event.y;
            // Any press or long-press still waiting for the touch must not fire
            // once the mouse owns this action.
            this.cancelLongPress('mouse');
            this.viewPointerX = event.x;
            this.viewPointerY = event.y;
            // The local cursor follows every pointer event; the poll only acts
            // as the fallback for server-side shape/size changes.
            this.placeCursor();
            if (event.action === MouseAction.Press) {
              // Focus normally follows touch, not mouse clicks, so grab it here;
              // otherwise key events never reach onKeyEvent.
              this.requestViewFocus();
              this.pointerDown(event.x, event.y, Remote.buttonMask(event.button));
            } else if (event.action === MouseAction.Release) {
              this.pointerUp(event.x, event.y, false);
            } else {
              this.sendPointerAt(event.x, event.y, this.pressedMask);
            }
          })
          // Gestures on the picture area, all in one parallel group:
          //  - the one-finger pan only consumes drags so the system does not
          //    treat a drag on the remote desktop as moving the app window
          //    (its handlers stay empty on purpose);
          //  - two-finger pinch/drag drive the native view transform (GL mode;
          //    the handlers are inert on the Canvas path). Pinch zooms about
          //    the pinch centre, the two-finger pan moves the picture, and both
          //    are clamped again natively. Gesture recognizers do not consume
          //    the single-finger/mouse remote pointer events: onTouch/onMouse
          //    keep their existing paths.
          //  - three or more fingers are ignored: a recognizer may keep
          //    reporting while extra fingers are down, so every handler checks
          //    multiTouchBlocked AND the finger count itself (blockGestureOnExtraFingers),
          //    which does not depend on the touch path having run.
          .parallelGesture(
            GestureGroup(GestureMode.Parallel,
              PanGesture({ fingers: 1, distance: 1 })
                .onActionStart(() => {})
                .onActionUpdate(() => {})
                .onActionEnd(() => {}),
              PinchGesture({ fingers: 2, distance: 1 })
                .onActionStart((event: GestureEvent) => { this.onPinchStart(event); })
                .onActionUpdate((event: GestureEvent) => { this.onPinchUpdate(event); })
                .onActionEnd((event: GestureEvent) => { this.onPinchEnd(event); })
                .onActionCancel((event: GestureEvent) => { this.onPinchEnd(event); }),
              PanGesture({ fingers: 2, distance: 1 })
                .onActionStart((event: GestureEvent) => { this.onTwoFingerPanStart(event); })
                .onActionUpdate((event: GestureEvent) => { this.onTwoFingerPanUpdate(event); })
                .onActionEnd((event: GestureEvent) => { this.onTwoFingerPanEnd(event); })
                .onActionCancel((event: GestureEvent) => { this.onTwoFingerPanEnd(event); })
            )
          )
          // Secondary UI only: the Canvas stays mounted underneath while the
          // first frame is on its way.
          if (!this.canvasShown) {
            Column({ space: 12 }) {
              if (this.lastState === 'failed' || this.statusText.indexOf('失败') === 0) {
                // A failed session never delivers a first frame: show WHY
                // instead of spinning on "等待首帧…" forever.
                Text(this.statusText)
                  .fontSize(16)
                  .fontColor($r('app.color.danger'))
                  .textAlign(TextAlign.Center)
              } else {
                LoadingProgress().width(36).height(36)
                Text(this.needsTrust ? '等待身份确认' : '等待首帧…').fontSize(16)
              }
            }
            .width('100%')
            .height('100%')
            .justifyContent(FlexAlign.Center)
            .hitTestBehavior(HitTestMode.None)
          }
        }
        .width('100%')
        .layoutWeight(1)
      } else {
        Column({ space: 12 }) {
          LoadingProgress().width(36).height(36)
          Text(this.statusText).fontSize(16)
        }
        .width('100%')
        .layoutWeight(1)
        .justifyContent(FlexAlign.Center)
      }
    }
      .width('100%')
      .height('100%')
      .backgroundColor($r('app.color.page_background'))
      .alignItems(HorizontalAlign.Start)
      // The page root also watches keys: key events bubble up from whichever
      // child holds focus, so this path does not depend on the view itself.
      .id('page_root')
      .key('page_root')
      .focusable(true)
      .defaultFocus(true)
      .onKeyEvent((event: KeyEvent) => {
        this.forwardKey(event);
      })
  }
}
