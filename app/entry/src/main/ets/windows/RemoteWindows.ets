/**
 * Opens one remote session per independent UIAbility instance.
 *
 * The ability is declared with launchType: "multiton" (RemoteAbility), so every
 * startAbility creates its own instance, its own task and its own top-level
 * window. The system names those windows (tianguanN, increasing, never reused),
 * so the process keeps a sessionId <-> window-name registry in AppStorage:
 *
 *   remote.window.<sessionId>  = <this session's window name>
 *   remote.session.<windowName> = <the session shown in that window>
 *
 * The ability writes both keys BEFORE it loads pages/Remote; the page only knows
 * its own window name and resolves itself through the second key. AppStorage is
 * shared by every ability instance of this process.
 *
 * Opening is "activate if present, start otherwise". findWindow() is synchronous
 * and throws 1300002 when the named window no longer exists, so a stale registry
 * entry is dropped instead of being trusted. Starting an ability is asynchronous
 * and multiton always creates a new instance, therefore:
 *   - the registry is polled (bounded) until the new instance publishes its
 *     window name;
 *   - concurrent opens of the SAME session share one in-flight attempt, so a
 *     double click or the list + tray firing together cannot create two windows.
 *
 * Closing the window ends its session (RemoteAbility.onDestroy). Programmatic
 * closes (connection deleted, app quit) set a flag first so that path only drops
 * the window.
 */
import { window } from '@kit.ArkUI';
import { hilog } from '@kit.PerformanceAnalysisKit';
import type { common, Want } from '@kit.AbilityKit';
import { closeSessionById, findSession } from '../model/SessionRegistry';

const DOMAIN: number = 0x0000;
const TAG: string = 'TianguanDiag';

export const REMOTE_ABILITY_NAME: string = 'RemoteAbility';
const WINDOW_KEY_PREFIX: string = 'remote.window.';
const SESSION_KEY_PREFIX: string = 'remote.session.';
const CLOSE_KEY_PREFIX: string = 'remote.close.';
/** Set by EntryAbility's full-quit flow: every remote instance terminates. */
export const REMOTE_QUIT_KEY: string = 'remote.quit';

/** How long startAbility may take to publish the new window's name. */
const START_WAIT_TIMEOUT_MS: number = 2000;
const START_WAIT_INTERVAL_MS: number = 50;
/** How long a programmatic close waits for the instance to terminate itself. */
const CLOSE_WAIT_TIMEOUT_MS: number = 1000;
/**
 * Delay before re-asserting an activated window. The tray menu owns the focus
 * while it is open and the system may raise the main window right after the
 * click is delivered; this retry runs once that interaction has settled.
 */
const ACTIVATE_RETRY_MS: number = 250;

export type RemoteOpenResult = 'opened' | 'activated' | 'failed' | 'no-stage';

/** Registry key: the window name that currently shows this session. */
export function remoteWindowKeyForSession(sessionId: string): string {
  return WINDOW_KEY_PREFIX + sessionId;
}

/** Registry key: the session shown by this window name. */
export function sessionKeyForWindow(windowName: string): string {
  return SESSION_KEY_PREFIX + windowName;
}

/**
 * Per-session programmatic-close request. RemoteAbility polls it and terminates
 * that one instance; onDestroy sees the same flag and must not close the session
 * again (the caller owns its lifetime).
 */
export function remoteCloseKeyForSession(sessionId: string): string {
  return CLOSE_KEY_PREFIX + sessionId;
}

/**
 * User-facing window title: the connection name, then host:port, then the
 * sessionId. openRemoteWindow can run before the registry has claimed the
 * session, when only the caller-supplied name is knowable — that is fine.
 */
export function titleForSession(sessionId: string, displayName?: string): string {
  if (displayName !== undefined && displayName.length > 0) {
    return displayName;
  }
  const session = findSession(sessionId);
  if (session !== undefined) {
    if (session.name.length > 0) {
      return session.name;
    }
    if (session.endpoint.length > 0) {
      return session.endpoint;
    }
  }
  return sessionId;
}

function log(action: string, detail: string): void {
  hilog.info(DOMAIN, TAG, 'remote-window %{public}s %{public}s', action, detail);
}

function delay(ms: number): Promise<void> {
  return new Promise<void>((resolve) => {
    setTimeout((): void => {
      resolve();
    }, ms);
  });
}

/**
 * findWindow() throws 1300002 when the window does not exist. A stale registry
 * entry is normal (the instance is still tearing down), never fatal.
 */
function findWindowByName(name: string): window.Window | undefined {
  if (name.length === 0) {
    return undefined;
  }
  try {
    return window.findWindow(name);
  } catch (error) {
    return undefined;
  }
}

/** Drops the registry entry for a session, keeping the reverse key consistent. */
export function clearRemoteWindowRegistry(sessionId: string, windowName: string): void {
  try {
    const nameKey = remoteWindowKeyForSession(sessionId);
    if (AppStorage.get<string>(nameKey) === windowName) {
      AppStorage.delete(nameKey);
    }
  } catch (error) {
    // Key already gone.
  }
  try {
    const sessionKey = sessionKeyForWindow(windowName);
    if (AppStorage.get<string>(sessionKey) === sessionId) {
      AppStorage.delete(sessionKey);
    }
  } catch (error) {
    // Key already gone.
  }
}

/**
 * Brings an already-running session window forward. show() is the required
 * step; the focus hand-off is best effort and only legal while the source window
 * is focused (window.shiftAppWindowFocus requires it). While the tray menu is
 * open no app window is focused, so the immediate call usually cannot hand the
 * focus over — activateExisting() retries once after the menu has closed.
 */
async function showExisting(existing: window.Window, windowName: string,
  sessionId: string): Promise<boolean> {
  // Re-assert the binding in case the page is reloading this window.
  AppStorage.setOrCreate(sessionKeyForWindow(windowName), sessionId);
  try {
    await existing.show();
    return true;
  } catch (showError) {
    // The window exists but refused to show: do NOT start a second instance
    // for the same session, report the activation attempt as it is.
    hilog.warn(DOMAIN, TAG, '远程窗口已存在但无法显示：%{public}s', String(showError));
    return false;
  }
}

/** Hands the keyboard focus from the (focused) main window to the target. */
async function handFocusFromMain(target: window.Window): Promise<void> {
  try {
    const stage = AppStorage.get<window.WindowStage>('windowStage');
    if (stage === undefined) {
      return;
    }
    const main = stage.getMainWindowSync();
    if (!main.isFocused()) {
      // The tray menu owns the focus while it is open; the retry has to run
      // after it closes. Not being able to shift focus must not fail the open.
      hilog.info(DOMAIN, TAG, '激活远程窗口：源窗口未聚焦，跳过焦点转移');
      return;
    }
    await window.shiftAppWindowFocus(main.getWindowProperties().id,
      target.getWindowProperties().id);
  } catch (focusError) {
    // show() already raised the window; only the keyboard focus may not follow.
    hilog.warn(DOMAIN, TAG, '激活远程窗口焦点失败：%{public}s', String(focusError));
  }
}

/**
 * Activate the existing window for a session.
 *
 * The tray menu is the tricky caller: its click is delivered while the system
 * menu still owns the focus, and the main window can be raised (or the tray
 * menu's ability association can bring it forward) AFTER this handler returns.
 * A single show()+shift therefore sometimes leaves the main window on top of
 * the session. Re-assert the session once the menu has closed: if it is still
 * there and not already focused, show it again and hand it the focus.
 */
async function activateExisting(existing: window.Window, windowName: string,
  sessionId: string): Promise<void> {
  const shown: boolean = await showExisting(existing, windowName, sessionId);
  if (shown) {
    log('activate', windowName);
  }
  await handFocusFromMain(existing);
  setTimeout((): void => {
    const target = findWindowByName(windowName);
    if (target === undefined) {
      return;
    }
    try {
      if (target.isFocused()) {
        return;
      }
    } catch (error) {
      // Treat "cannot tell" as not focused; the retry is harmless.
    }
    showExisting(target, windowName, sessionId).then((raised: boolean): void => {
      if (!raised) {
        return;
      }
      log('reactivate', windowName);
      handFocusFromMain(target).catch((error: Error): void => {
        hilog.warn(DOMAIN, TAG, '重新激活远程窗口焦点失败：%{public}s', String(error));
      });
    }).catch((error: Error): void => {
      hilog.warn(DOMAIN, TAG, '重新激活远程窗口失败：%{public}s', String(error));
    });
  }, ACTIVATE_RETRY_MS);
}

/**
 * Opens (or activates) the window for one session. Never starts a second
 * instance for the same session.
 *
 * Single-flight per session: the list button, a double click and the tray menu
 * are separate callers; without this they could each start a multiton instance
 * (the platform does not deduplicate).
 */
const openingInFlight: Map<string, Promise<RemoteOpenResult>> =
  new Map<string, Promise<RemoteOpenResult>>();

export function openRemoteWindow(sessionId: string, displayName?: string,
  context?: common.UIAbilityContext): Promise<RemoteOpenResult> {
  if (sessionId.length === 0) {
    return Promise.resolve('failed');
  }
  const running = openingInFlight.get(sessionId);
  if (running !== undefined) {
    return running;
  }
  const attempt = openRemoteWindowOnce(sessionId, displayName, context);
  openingInFlight.set(sessionId, attempt);
  attempt.finally((): void => {
    openingInFlight.delete(sessionId);
  }).catch((): void => {
    // openRemoteWindowOnce handles every failure itself; this catch only keeps
    // the bookkeeping promise from becoming an unhandled rejection.
  });
  return attempt;
}

async function openRemoteWindowOnce(sessionId: string, displayName?: string,
  context?: common.UIAbilityContext): Promise<RemoteOpenResult> {
  // A stale "close" request left by closeRemoteWindow must never terminate the
  // instance this open is about to start.
  try {
    AppStorage.delete(remoteCloseKeyForSession(sessionId));
  } catch (error) {
    // Key never existed.
  }
  // 1. Existing window for this session: just bring it back.
  const registered = AppStorage.get<string>(remoteWindowKeyForSession(sessionId));
  if (registered !== undefined && registered.length > 0) {
    const existing = findWindowByName(registered);
    if (existing !== undefined) {
      await activateExisting(existing, registered, sessionId);
      return 'activated';
    }
    // The registry outlived its window (instance still tearing down, process
    // interrupted). findWindow failing is the authority: drop the stale entry.
    clearRemoteWindowRegistry(sessionId, registered);
    log('stale-registry', registered);
  }
  // 2. No window: start one RemoteAbility instance.
  if (context === undefined) {
    log('no-stage', sessionId);
    return 'no-stage';
  }
  const title: string = titleForSession(sessionId, displayName);
  const want: Want = {
    bundleName: context.abilityInfo.bundleName,
    abilityName: REMOTE_ABILITY_NAME,
    parameters: { sessionId: sessionId, title: title }
  };
  try {
    await context.startAbility(want);
  } catch (error) {
    hilog.error(DOMAIN, TAG, '启动远程窗口失败：%{public}s', String(error));
    closeSessionById(sessionId);
    return 'failed';
  }
  // 3. Wait until the new instance publishes its window name and the window is
  // really findable. Writing to the registry happens before loadContent, so the
  // name appears before the page exists; findWindow is the second check.
  const deadline: number = Date.now() + START_WAIT_TIMEOUT_MS;
  while (Date.now() < deadline) {
    const name = AppStorage.get<string>(remoteWindowKeyForSession(sessionId));
    if (name !== undefined && name.length > 0 && findWindowByName(name) !== undefined) {
      log('open', name + ' session=' + sessionId);
      return 'opened';
    }
    await delay(START_WAIT_INTERVAL_MS);
  }
  // No window means no session: closing it keeps the list truthful instead of
  // leaving an invisible live session behind.
  hilog.error(DOMAIN, TAG, '远程窗口未在 %{public}d ms 内出现，关闭该会话 session=%{public}s',
    START_WAIT_TIMEOUT_MS, sessionId);
  const lateName: string = AppStorage.get<string>(remoteWindowKeyForSession(sessionId)) ?? '';
  clearRemoteWindowRegistry(sessionId, lateName);
  closeSessionById(sessionId);
  return 'failed';
}

/** Waits (bounded) until the named window is gone. */
async function waitForWindowGone(name: string, timeoutMs: number): Promise<boolean> {
  const deadline: number = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    if (findWindowByName(name) === undefined) {
      return true;
    }
    await delay(START_WAIT_INTERVAL_MS);
  }
  return findWindowByName(name) === undefined;
}

/**
 * Closes the session window programmatically, if one exists.
 *
 * A remote window is an ability main window, so the supported way to end it is
 * that instance's terminateSelf(); it polls the per-session close key and does
 * that within ~200 ms. destroyWindow() is only a last resort if the instance
 * does not react (it may be refused for a main window; the failure is logged).
 *
 * The registry is cleared here as well as in the ability's onDestroy: callers
 * must never observe a key that points at a window this module already removed.
 */
export async function closeRemoteWindow(sessionId: string): Promise<void> {
  const name: string = AppStorage.get<string>(remoteWindowKeyForSession(sessionId)) ?? '';
  const existing = findWindowByName(name);
  if (existing === undefined) {
    // Nothing to close; a stale close request must not terminate a future
    // instance (openRemoteWindow also clears it defensively).
    try {
      AppStorage.delete(remoteCloseKeyForSession(sessionId));
    } catch (error) {
      // Key never existed.
    }
    clearRemoteWindowRegistry(sessionId, name);
    return;
  }
  // Programmatic: the caller owns the session lifetime (Index already closed
  // it before deleting the connection), so onDestroy must only drop the window.
  AppStorage.setOrCreate<boolean>(remoteCloseKeyForSession(sessionId), true);
  const gone: boolean = await waitForWindowGone(name, CLOSE_WAIT_TIMEOUT_MS);
  if (!gone) {
    hilog.warn(DOMAIN, TAG, '远程实例未自行退出，尝试直接销毁窗口：%{public}s', name);
    try {
      await existing.destroyWindow();
    } catch (error) {
      hilog.warn(DOMAIN, TAG, '关闭远程窗口失败：%{public}s', String(error));
    }
  }
  clearRemoteWindowRegistry(sessionId, name);
  log('close', name + ' session=' + sessionId);
}
