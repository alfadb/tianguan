/**
 * Remote desktop page — one instance per session, hosted either in its own
 * sub-window (window name "remote-<sessionId>", the normal path) or inside the
 * main window (router fallback when sub-window creation failed; the session id
 * then arrives through router params).
 *
 * The session this page drives is NEVER guessed. It comes from one of exactly
 * two sources: router params (main-window fallback) or this page's OWN window
 * name, read from its UIContext and mapped back by sessionIdForWindowName().
 * When neither yields an id the page fails closed: the error is shown and every
 * input path stays unmounted. There is no "last window", FIFO or "first
 * registry session" fallback.
 *
 * The page is pure picture: no title row, no disconnect button and no padding,
 * so the Canvas fills the whole window. The connection name belongs to the
 * window title (set by RemoteWindows), and the disconnect path is closing that
 * window — never an action inside this page.
 *
 * Everything talks to the native layer through diag/SessionApi.ets with THIS
 * session's id; the status is parsed as JSON per docs/napi-contract.md (never
 * the legacy "key=value" text). The 30 ms poll drives frames (takeFrame with
 * afterVersion/forceFull), the local cursor, the status text, the
 * trust-required panel and the clipboard edge (clipboard.version ->
 * clipboard/TianguanClipboard). The poll slows to 1 s (status only) while the
 * page is hidden.
 */
import { hilog } from '@kit.PerformanceAnalysisKit';
import { image } from '@kit.ImageKit';
import { KeyCode } from '@ohos.multimodalInput.keyCode';
import pointer from '@ohos.multimodalInput.pointer';
import { window } from '@kit.ArkUI';
import {
  copyCursor, parseFrameMeta, reconnectSession, releaseSessionInput, requestFullFrame,
  sendKey, sendPointer, sessionStatus, takeFrameRaw,
  SESSION_STATE_CONNECTED, SESSION_STATE_TRUST_REQUIRED
} from '../diag/SessionApi';
import type { CursorInfo, FrameMeta, SessionStatus } from '../diag/SessionApi';
import { findSession, markWindowOpen } from '../model/SessionRegistry';
import { trustFingerprint } from '../model/ConnectionStore';
import { onRemoteClipboardVersion, clipboardRegisterSession,
  clipboardUnregisterSession } from '../clipboard/TianguanClipboard';
import { sessionIdForWindowName, sessionKeyForWindow } from '../windows/RemoteWindows';

const DOMAIN: number = 0x0000;
const TAG: string = 'TianguanDiag';

interface RemoteRouterParams {
  sessionId?: string;
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
  @State private fingerprint: string = '';
  @State private needsTrust: boolean = false;
  private settings: RenderingContextSettings = new RenderingContextSettings(true);
  private ctx: CanvasRenderingContext2D = new CanvasRenderingContext2D(this.settings);
  private canvasReady: boolean = false;
  private sessionId: string = '';
  private inSubWindow: boolean = false;
  private framePixels: image.PixelMap | undefined = undefined;
  private frameReady: boolean = false;
  private regionBuffer: ArrayBuffer = new ArrayBuffer(0);
  @State private frameWidth: number = 0;
  @State private frameHeight: number = 0;
  private viewWidth: number = 0;
  private viewHeight: number = 0;
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
  private pageVisible: boolean = true;
  private lastState: string = '';
  // Single in-flight render: the 30 ms poll must never start overlapping draws.
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
  // True once a real MouseEvent arrived: the mouse path then owns input and the
  // synthetic touch events (one per click on PC) are ignored.
  private hasMouse: boolean = false;

  aboutToAppear(): void {
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
      this.statusText = '无法确定本页所属的会话（缺少 router 参数，且本窗口名不是远程窗口）';
      hilog.error(DOMAIN, TAG, 'remote 页面无法确定会话归属，已禁用输入');
      return;
    }
    hilog.info(DOMAIN, TAG, 'remote 页面启动 session=%{public}s sub=%{public}s',
      this.sessionId, String(this.inSubWindow));
    markWindowOpen(this.sessionId, true);
    clipboardRegisterSession(this.sessionId);
    this.attachWindowEvents();
    // Mount the Canvas now; onReady() can fire (and frames can start flowing)
    // even before the first connection status arrives.
    this.sessionResolved = true;
    this.startPolling();
  }

  aboutToDisappear(): void {
    // Losing the view is NOT itself a user close: the session keeps running
    // (the main list and the tray own its lifetime). In a sub-window the user
    // closing the window reaches RemoteWindows' WINDOW_DESTROYED handler,
    // which is what ends the session; this page never closes it here.
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
    this.detachWindowEvents();
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
  //   1) router params — the main-window fallback opened by Index;
  //   2) this page's own window name — the sub-window created as
  //      windowNameForSession(sessionId).
  // Anything else is a guess and is deliberately absent (F01).
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
    const windowSession = windowName.length > 0 ? sessionIdForWindowName(windowName) : '';
    if (windowSession.length > 0) {
      this.inSubWindow = true;
    }
    // 1) Router params — the main-window fallback path opened by Index.
    try {
      const params = this.getUIContext().getRouter().getParams() as RemoteRouterParams | undefined;
      if (params !== undefined && params !== null &&
          typeof params.sessionId === 'string' && params.sessionId.length > 0) {
        return params.sessionId;
      }
    } catch (error) {
      // No router context — the sub-window path below still applies.
    }
    // 2) The sub-window's own name carries its session id by construction.
    // Cross-check the binding key written before the page was loaded: a
    // conflicting key means the window identity is inconsistent, so refuse to
    // attach instead of silently picking one of the two.
    if (windowSession.length > 0) {
      const mapped = AppStorage.get<string>(sessionKeyForWindow(windowName));
      if (mapped !== undefined && mapped.length > 0 && mapped !== windowSession) {
        hilog.error(DOMAIN, TAG, 'remote 窗口身份冲突 window=%{public}s key=%{public}s',
          windowName, mapped);
        return '';
      }
      return windowSession;
    }
    // Neither source answers: fail closed. No FIFO/registry fallback.
    return '';
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
      // No named window (main-window path on hosts that do not expose a name):
      // the page lifecycle below still covers hide/show and teardown.
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
    } else if (event === window.WindowEventType.WINDOW_HIDDEN) {
      this.setPageVisible(false);
    } else if (event === window.WindowEventType.WINDOW_SHOWN) {
      this.setPageVisible(true);
    }
  }

  private setPageVisible(visible: boolean): void {
    this.pageVisible = visible;
    if (this.disposed || this.sessionId.length === 0) {
      return;
    }
    if (visible) {
      this.startFrameTimer();
      // Content that changed while hidden was never fetched: repaint from a
      // complete frame right away instead of waiting for a diff.
      this.requestFull();
      this.applyPending();
    } else {
      // Nobody can see the frames, so stop fetching them; the 1 s status poll
      // keeps reconnects/trust prompts advancing.
      this.releaseAllInputs('page-hidden');
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
    this.timer = setInterval((): void => { this.poll(); }, 30);
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
    if (this.applying || this.disposed || !this.pageVisible) {
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
      // between cannot mix sizes and tear the picture.
      const raw = takeFrameRaw(this.sessionId, this.regionBuffer, this.lastVersion, this.forceFull);
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
      await pixels.writePixels(area);
      // The version may only advance once the pixels really landed AND were
      // drawn from the same mirror generation. A reconnect during the await
      // bumps both the epoch and the generation; either mismatch means the
      // pixels just written belong to a layout nobody will draw.
      if (epoch !== this.renderEpoch || this.disposed || pixels !== this.framePixels ||
          !Remote.sameGeneration(meta.generation, this.lastGeneration)) {
        return;
      }
      const scale = Math.min(this.viewWidth / this.frameWidth, this.viewHeight / this.frameHeight);
      const offsetX = (this.viewWidth - this.frameWidth * scale) / 2;
      const offsetY = (this.viewHeight - this.frameHeight * scale) / 2;
      const dw = Math.round(this.frameWidth * scale);
      const dh = Math.round(this.frameHeight * scale);
      this.ctx.drawImage(pixels, Math.round(offsetX), Math.round(offsetY), dw, dh);
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

  // The local cursor follows the physical pointer: no server round trip.
  private placeCursor(cw: number, ch: number, hotX: number, hotY: number): void {
    if (this.viewWidth <= 0 || this.frameWidth <= 0 || cw <= 0 || ch <= 0) {
      return;
    }
    if (this.viewPointerX < 0 || this.viewPointerY < 0) {
      return;
    }
    const scale = Math.min(this.viewWidth / this.frameWidth, this.viewHeight / this.frameHeight);
    this.cursorDrawW = Math.max(1, Math.round(cw * scale));
    this.cursorDrawH = Math.max(1, Math.round(ch * scale));
    this.cursorLeft = Math.round(this.viewPointerX - hotX * scale);
    this.cursorTop = Math.round(this.viewPointerY - hotY * scale);
  }

  private applyStatus(status: SessionStatus): void {
    const width = status.width === undefined ? 0 : status.width;
    const height = status.height === undefined ? 0 : status.height;
    if (width > 0 && height > 0 &&
        (width !== this.frameWidth || height !== this.frameHeight)) {
      // The mirror no longer matches: invalidate the in-flight render, drop the
      // old PixelMap (~9 MB at 1920x1200) and rebuild it for the new size.
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
    if (cursorBlank) {
      // The server told us it drew the pointer into the picture itself.
      this.releaseCursorPixelMap();
    } else if (cw > 0 && ch > 0 && cursorVersion !== this.lastCursorVersion && this.canvasShown) {
      this.lastCursorVersion = cursorVersion;
      this.updateCursorShape(cw, ch);
    }
    if (this.canvasShown && !cursorBlank) {
      this.placeCursor(cw, ch, hotX, hotY);
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
        this.applyPending();
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
    if (this.sessionId.length === 0 || this.disposed) {
      return;
    }
    const status = sessionStatus(this.sessionId);
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
    this.applyStatus(status);
  }

  // RFB button bits: 1 left, 2 middle, 4 right (8/16 are the wheel notches).
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
      sendPointer(this.sessionId, rx, ry, 0);
    }
    // Touch screens have no double-click gesture: two quick taps at the same
    // spot become a real double click. A mouse already sends its own.
    const now = Date.now();
    const near = Math.abs(x - this.lastTapX) < 24 && Math.abs(y - this.lastTapY) < 24;
    if (fromTouch && now - this.lastTapMs < 400 && near && rx >= 0 && ry >= 0) {
      sendPointer(this.sessionId, rx, ry, 1);
      sendPointer(this.sessionId, rx, ry, 0);
      this.lastTapMs = 0;
      return;
    }
    this.lastTapMs = now;
    this.lastTapX = x;
    this.lastTapY = y;
  }

  // Maps a view point to remote pixels; returns -1,-1 when outside the picture.
  private mapToRemote(x: number, y: number): number[] {
    if (this.frameWidth <= 0 || this.frameHeight <= 0 ||
        this.viewWidth <= 0 || this.viewHeight <= 0) {
      return [-1, -1];
    }
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
    trustFingerprint(endpoint, this.fingerprint);
    reconnectSession(this.sessionId);
    // The reconnect is already in flight; do not wait for the next 1 s status
    // poll to learn the generation changed. Invalidate the in-flight render and
    // the local mirror NOW and make the next render request a complete frame.
    this.invalidateRender();
    this.forceFull = true;
    this.needsTrust = false;
    this.statusText = '已信任，正在重连…';
  }

  /**
   * The only action left when this page could not attribute itself to a
   * session: a sub-window is destroyed (there is no list behind it), the
   * main-window fallback pops back to the list. Destroying the sub-window is
   * also its disconnect path (RemoteWindows' WINDOW_DESTROYED handler), which
   * is correct here: the window belongs to a session this page could not use.
   */
  private async leavePage(): Promise<void> {
    this.teardown();
    if (this.inSubWindow) {
      try {
        const name = this.ownWindowName;
        if (name.length > 0) {
          await window.findWindow(name).destroyWindow();
          return;
        }
      } catch (error) {
        hilog.warn(DOMAIN, TAG, '关闭无法归属的远程窗口失败：%{public}s', String(error));
      }
    }
    try {
      this.getUIContext().getRouter().back();
    } catch (error) {
      hilog.warn(DOMAIN, TAG, '返回连接列表失败：%{public}s', String(error));
    }
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
              Canvas(this.ctx)
                .width('100%')
                .height('100%')
                .draggable(false)
                .onReady(() => {
                  this.canvasReady = true;
                })
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
            this.viewWidth = Number(newValue.width);
            this.viewHeight = Number(newValue.height);
            // Canvas content is lost on resize: ask for one complete frame.
            // forceFull alone cannot wake an idle server, so request it for
            // real (non-incremental) once the size is known.
            this.requestFull();
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
            // On PC a mouse click also produces a synthetic TouchEvent. Once a
            // real MouseEvent has been seen, the mouse path owns the input;
            // handling both forwarded every click twice (single -> double).
            if (this.hasMouse) {
              return;
            }
            if (event.touches.length > 0) {
              this.lastTouchX = event.touches[0].x;
              this.lastTouchY = event.touches[0].y;
              this.viewPointerX = this.lastTouchX;
              this.viewPointerY = this.lastTouchY;
            }
            if (event.type === TouchType.Down) {
              this.requestViewFocus();
              this.pointerDown(this.lastTouchX, this.lastTouchY, 1);
            } else if (event.type === TouchType.Move) {
              this.sendPointerAt(this.lastTouchX, this.lastTouchY, this.pressedMask);
            } else if (event.type === TouchType.Up || event.type === TouchType.Cancel) {
              this.pointerUp(this.lastTouchX, this.lastTouchY, true);
            }
          })
          .onMouse((event: MouseEvent) => {
            this.hasMouse = true;
            this.viewPointerX = event.x;
            this.viewPointerY = event.y;
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
          // Consume drags so the system does not treat a drag on the remote
          // desktop as moving the application window.
          .parallelGesture(
            PanGesture({ fingers: 1, distance: 1 })
              .onActionStart(() => {})
              .onActionUpdate(() => {})
              .onActionEnd(() => {})
          )
          // Secondary UI only: the Canvas stays mounted underneath while the
          // first frame is on its way.
          if (!this.canvasShown) {
            Column({ space: 12 }) {
              LoadingProgress().width(36).height(36)
              Text(this.needsTrust ? '等待身份确认' : '等待首帧…').fontSize(16)
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
