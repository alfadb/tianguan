/**
 * One independent sub-window per session ("remote-<sessionId>"), created from
 * the stored WindowStage. Opening the same session again ACTIVATES the existing
 * window instead of creating a new one. When a sub-window cannot be created the
 * caller is told to fall back to the main-window router path (pages/Remote also
 * accepts router params, so the same page serves both paths).
 *
 * The sessionId is handed to the page through ONE deterministic channel: the
 * window name itself ("remote-<sessionId>"), which the page reads from its own
 * UIContext (getWindowName()) and maps back with sessionIdForWindowName(). The
 * AppStorage key is still written BEFORE setUIContent as a redundant binding;
 * the page cross-checks it and refuses to attach when the two disagree.
 * (window.Window.setUIContent has no LocalStorage overload in the 26.0.0.821
 * SDK — only loadContent() takes a LocalStorage — hence the key.) Nothing here
 * ever guesses a session: the name and the key are both derived from the
 * sessionId by the exported functions below.
 */
import { window } from '@kit.ArkUI';
import { hilog } from '@kit.PerformanceAnalysisKit';

const DOMAIN: number = 0x0000;
const TAG: string = 'TianguanDiag';

const WINDOW_NAME_PREFIX: string = 'remote-';
const SESSION_KEY_PREFIX: string = 'remote.session.';

/** The one window name for a session (inverse of sessionIdForWindowName). */
export function windowNameForSession(sessionId: string): string {
  return WINDOW_NAME_PREFIX + sessionId;
}

/** The session a window name carries; '' when the name is not a remote window. */
export function sessionIdForWindowName(windowName: string): string {
  if (!windowName.startsWith(WINDOW_NAME_PREFIX)) {
    return '';
  }
  const sessionId: string = windowName.substring(WINDOW_NAME_PREFIX.length);
  return sessionId.length > 0 ? sessionId : '';
}

/**
 * AppStorage key for the page in this window. The page only knows its window
 * name, so the key must be derivable from it alone (F01).
 */
export function sessionKeyForWindow(windowName: string): string {
  return SESSION_KEY_PREFIX + windowName;
}

export type RemoteOpenResult = 'opened' | 'activated' | 'fallback-main' | 'no-stage';

function log(action: string, detail: string): void {
  hilog.info(DOMAIN, TAG, 'remote-window %{public}s %{public}s', action, detail);
}

/**
 * Creates the child window WITH system decorations. createSubWindow() alone makes
 * an undecorated window: no title bar and no frame, so it can neither be dragged
 * nor resized by the user. createSubWindowWithOptions({decorEnabled:true}) gives
 * the normal title bar; fall back to the plain call on ROMs without it.
 */
async function createDecoratedSubWindow(stage: window.WindowStage, name: string,
  sessionId: string): Promise<window.Window> {
  const title: string = '远程会话 ' + sessionId;
  try {
    // zLevelAboveParentLoosened (API 26, 2in1): without it a child window is
    // always stacked above its parent AND the system refuses to show
    // maximize/minimize title buttons ("Invalid window type..."). With it the
    // main window can be raised above the remote window and the buttons work.
    // maximizeSupported is required for a child window to be maximizable at all
    // (Window.maximize doc: "set maximizeSupported to true when creating").
    const options: window.SubWindowOptions = {
      title: title,
      decorEnabled: true,
      zLevelAboveParentLoosened: true,
      maximizeSupported: true
    };
    return await stage.createSubWindowWithOptions(name, options);
  } catch (error) {
    log('decor-loosened-unavailable', String(error));
  }
  try {
    const plainDecor: window.SubWindowOptions = {
      title: title,
      decorEnabled: true
    };
    return await stage.createSubWindowWithOptions(name, plainDecor);
  } catch (error) {
    log('decor-unavailable', String(error));
    return await stage.createSubWindow(name);
  }
}

/**
 * Opens (or activates) the window for one session. Never destroys an existing
 * window for that session.
 */
export async function openRemoteWindow(sessionId: string): Promise<RemoteOpenResult> {
  const name = windowNameForSession(sessionId);
  // 1. Existing window for this session: just bring it back. findWindow is
  // synchronous and THROWS 1300002 when the named window does not exist; it
  // never returns undefined (F26).
  let existing: window.Window | undefined = undefined;
  try {
    existing = window.findWindow(name);
  } catch (error) {
    existing = undefined;
  }
  if (existing !== undefined) {
    // Re-assert the identity key in case the page is reloading this window.
    AppStorage.setOrCreate(sessionKeyForWindow(name), sessionId);
    try {
      await existing.show();
      try {
        const stage = AppStorage.get<window.WindowStage>('windowStage');
        if (stage !== undefined) {
          const mainId = stage.getMainWindowSync().getWindowProperties().id;
          await window.shiftAppWindowFocus(mainId, existing.getWindowProperties().id);
        }
      } catch (focusError) {
        // Focus shift is best effort; show() already raised the window.
        hilog.warn(DOMAIN, TAG, '激活远程窗口焦点失败：%{public}s', String(focusError));
      }
      log('activate', name);
    } catch (showError) {
      // The window exists but refused to show: return without creating a
      // duplicate (a second window with the same name would be invalid).
      hilog.warn(DOMAIN, TAG, '远程窗口已存在但无法显示：%{public}s', String(showError));
    }
    return 'activated';
  }
  // 2. Create the sub-window.
  const stage = AppStorage.get<window.WindowStage>('windowStage');
  if (stage === undefined) {
    log('no-stage', name);
    return 'no-stage';
  }
  let created: window.Window | undefined = undefined;
  try {
    // Size the sub-window relative to the main window: a fixed size would be
    // larger than a phone screen and land off-screen.
    const main = stage.getMainWindowSync();
    const rect = main.getWindowProperties().windowRect;
    const width = Math.max(320, Math.round(rect.width * 0.92));
    const height = Math.max(240, Math.round(rect.height * 0.9));
    created = await createDecoratedSubWindow(stage, name, sessionId);
    // Redundant binding for the page, written BEFORE setUIContent. The window
    // name is the primary identity channel; the page refuses to attach when
    // this key and the name disagree.
    AppStorage.setOrCreate(sessionKeyForWindow(name), sessionId);
    // The key must not outlive the window, whichever way it closes (close
    // button, system X, app quit). The page path only closes programmatically.
    try {
      created.on('windowEvent', (event: window.WindowEventType): void => {
        if (event === window.WindowEventType.WINDOW_DESTROYED) {
          try {
            AppStorage.delete(sessionKeyForWindow(name));
          } catch (deleteError) {
            // Key already gone.
          }
        }
      });
    } catch (listenError) {
      hilog.warn(DOMAIN, TAG, '监听窗口销毁失败：%{public}s', String(listenError));
    }
    // Everything up to and including setUIContent can still fail; if it does,
    // the window is destroyed and the caller falls back to the main window.
    // The page's aboutToDisappear() no longer ends the session, so a failure
    // after the page was loaded cannot kill the fallback's session either.
    await created.resize(width, height);
    await created.show();
    // Window-state APIs must run after show(): calling them earlier fails with
    // "This window state is abnormal" (observed on device).
    // Keep the title bar interactive: dragging inside the remote desktop is
    // already blocked by draggable(false) on the view, so the window only moves
    // when the title bar itself is dragged.
    try {
      created.setWindowTitleMoveEnabled(true);
    } catch (moveError) {
      hilog.warn(DOMAIN, TAG, '启用窗口标题拖动失败：%{public}s', String(moveError));
    }
    // Load the page BEFORE moving the window focus: the SDK requires the focus
    // target to have loadContent()/setUIContent() applied AND effective before
    // shiftAppWindowFocus(), otherwise an invisible window can gain focus and
    // cause function exceptions (@ohos.window.d.ts, shiftAppWindowFocus NOTE).
    await created.setUIContent('pages/Remote');
    // Decorated sub-windows default to a close button only; show maximize and
    // minimize as well so the window behaves like a normal desktop window.
    try {
      created.setWindowTitleButtonVisible(true, true, true);
    } catch (buttonError) {
      hilog.warn(DOMAIN, TAG, '显示标题栏按钮失败：%{public}s', String(buttonError));
    }
    // Focus shift is best effort: the window is already sized, shown and has
    // its page, so a failure here only means the keyboard focus may not have
    // followed. Never destroy a usable window for that.
    try {
      await created.setWindowFocusable(true);
      const mainId = main.getWindowProperties().id;
      const subId = created.getWindowProperties().id;
      await window.shiftAppWindowFocus(mainId, subId);
    } catch (focusError) {
      hilog.warn(DOMAIN, TAG, '转移窗口焦点失败：%{public}s', String(focusError));
    }
    log('open', name);
    return 'opened';
  } catch (error) {
    hilog.error(DOMAIN, TAG, '打开远程子窗口失败（降级主窗口）：%{public}s', String(error));
    // A half-built window must not leave its identity key behind.
    try {
      AppStorage.delete(sessionKeyForWindow(name));
    } catch (deleteError) {
      // Key never created.
    }
    if (created !== undefined) {
      try {
        await created.destroyWindow();
      } catch (destroyError) {
        hilog.warn(DOMAIN, TAG, '销毁半建窗口失败：%{public}s', String(destroyError));
      }
    }
    return 'fallback-main';
  }
}

/** Closes the sub-window of a session, if one exists, and drops its key. */
export async function closeRemoteWindow(sessionId: string): Promise<void> {
  const name = windowNameForSession(sessionId);
  // findWindow throws 1300002 when there is no such window (F26).
  let existing: window.Window | undefined = undefined;
  try {
    existing = window.findWindow(name);
  } catch (error) {
    existing = undefined;
  }
  if (existing !== undefined) {
    try {
      await existing.destroyWindow();
      log('close', name);
    } catch (error) {
      // The window may already be going away; the key is cleared below anyway.
      hilog.warn(DOMAIN, TAG, '关闭远程子窗口失败：%{public}s', String(error));
    }
  }
  // Window is gone: its identity must not outlive it.
  try {
    AppStorage.delete(sessionKeyForWindow(name));
  } catch (error) {
    // Key already gone.
  }
}
