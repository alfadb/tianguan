/**
 * RemoteAbility: one independent UIAbility instance per remote session.
 *
 * Declared with launchType: "multiton" in module.json5, so every startAbility
 * creates a NEW instance with its own task and its own top-level window. This is
 * what puts one card per session into the system window picker (shown when the
 * single Dock icon is clicked) and makes that window directly activatable.
 *
 * The system assigns the window name (tianguanN); it is NOT derived from the
 * session. The session id therefore arrives through want.parameters, and this
 * ability publishes the identity the page needs before loadContent():
 *
 *   remote.window.<sessionId>   = <this window's name>
 *   remote.session.<windowName> = <this session>
 *
 * pages/Remote only knows its own window name and reads the second key (the
 * first key is the open/activate registry used by RemoteWindows). AppStorage is
 * shared by every ability instance of this process.
 *
 * Closing the window (title-bar X) is the user's "disconnect": onDestroy ends
 * that session and only that one. Programmatic closes (connection deleted,
 * application quit) raise a flag first, so those paths only drop the window.
 *
 * Full quit: EntryAbility cannot terminate sibling ability instances with its
 * own terminateSelf(), so it writes remote.quit = true and waits briefly; the
 * 200 ms check below makes every remote instance terminate itself.
 */
import { UIAbility } from '@kit.AbilityKit';
import type { Want } from '@kit.AbilityKit';
import { window, display } from '@kit.ArkUI';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { closeSessionById } from '../model/SessionRegistry';
import {
  REMOTE_QUIT_KEY, clearRemoteWindowRegistry, remoteCloseKeyForSession,
  remoteWindowKeyForSession, sessionKeyForWindow, titleForSession
} from '../windows/RemoteWindows';

const DOMAIN: number = 0x0000;
const TAG: string = 'TianguanDiag';
/** Light enough to react before the main ability's ~400 ms quit grace ends. */
const TERMINATION_POLL_MS: number = 200;

/**
 * Placement of a new remote window. The old default (1620x1080) is kept as the
 * upper bound; smaller screens get a proportional size, and the window is
 * centred on the display the system put it on. Nothing here is persisted:
 * a later manual move/resize by the user must stay untouched.
 */
const WINDOW_MAX_WIDTH: number = 1620;
const WINDOW_MAX_HEIGHT: number = 1080;
const WINDOW_MIN_WIDTH: number = 640;
const WINDOW_MIN_HEIGHT: number = 400;
const WINDOW_WIDTH_RATIO: number = 0.92;
const WINDOW_HEIGHT_RATIO: number = 0.88;
/** Keep the window (including its decoration) clear of the screen edges, in px. */
const WINDOW_SCREEN_MARGIN: number = 12;
/**
 * The system applies its own initial window placement a moment AFTER
 * onWindowStageCreate, overwriting a move issued there. Apply ours once that
 * initial layout has settled, then verify the window's own rectangle (the WMS
 * layout list is logged as a cross-check) and retry if the move was clobbered.
 */
const PLACEMENT_DELAY_MS: number = 400;
const PLACEMENT_SETTLE_MS: number = 200;
const PLACEMENT_RETRY_MS: number = 300;
const PLACEMENT_MAX_ATTEMPTS: number = 3;
const PLACEMENT_TOLERANCE_PX: number = 2;

interface ScreenInfo {
  width: number;
  height: number;
  displayId: number;
}

interface WindowPlacement {
  width: number;
  height: number;
  left: number;
  top: number;
}

/** Target rectangle: capped at 1620x1080, screen-proportional, 12 px clear. */
function computePlacement(screenWidth: number, screenHeight: number): WindowPlacement {
  const usableWidth: number = Math.max(1, screenWidth - WINDOW_SCREEN_MARGIN * 2);
  const usableHeight: number = Math.max(1, screenHeight - WINDOW_SCREEN_MARGIN * 2);
  let width: number = Math.min(WINDOW_MAX_WIDTH, Math.round(screenWidth * WINDOW_WIDTH_RATIO));
  let height: number = Math.min(WINDOW_MAX_HEIGHT, Math.round(screenHeight * WINDOW_HEIGHT_RATIO));
  width = Math.max(WINDOW_MIN_WIDTH, Math.min(width, usableWidth));
  height = Math.max(WINDOW_MIN_HEIGHT, Math.min(height, usableHeight));
  const left: number = Math.max(WINDOW_SCREEN_MARGIN,
    Math.round((screenWidth - width) / 2));
  const top: number = Math.max(WINDOW_SCREEN_MARGIN,
    Math.round((screenHeight - height) / 2));
  return { width: width, height: height, left: left, top: top };
}

export default class RemoteAbility extends UIAbility {
  private sessionId: string = '';
  private wantTitle: string = '';
  private windowName: string = '';
  private terminationTimer: number = -1;
  private terminating: boolean = false;
  private registryCleared: boolean = false;
  private placementDone: boolean = false;
  private placementInFlight: boolean = false;
  private placementAttempts: number = 0;

  onCreate(want: Want): void {
    const params: Record<string, Object> | undefined = want.parameters;
    if (params !== undefined) {
      if (params['sessionId'] !== undefined) {
        this.sessionId = String(params['sessionId']);
      }
      if (params['title'] !== undefined) {
        this.wantTitle = String(params['title']);
      }
    }
    // This is a brand-new instance: a close request left over from an earlier
    // window of the same session must not terminate it immediately.
    if (this.sessionId.length > 0) {
      try {
        AppStorage.delete(remoteCloseKeyForSession(this.sessionId));
      } catch (error) {
        // Key never existed.
      }
    }
    hilog.info(DOMAIN, TAG, 'remote ability create session=%{public}s title=%{public}s',
      this.sessionId, this.wantTitle);
    this.terminationTimer = setInterval((): void => {
      this.checkTermination();
    }, TERMINATION_POLL_MS);
  }

  onWindowStageCreate(windowStage: window.WindowStage): void {
    let main: window.Window | undefined = undefined;
    try {
      main = windowStage.getMainWindowSync();
      const properties = main.getWindowProperties();
      this.windowName = properties.name === undefined ? '' : properties.name;
    } catch (error) {
      hilog.error(DOMAIN, TAG, 'remote ability 读取窗口名失败：%{public}s', String(error));
    }
    // Publish both identity directions BEFORE the page loads: the page resolves
    // itself from its own window name, and RemoteWindows polls the first key.
    if (this.sessionId.length > 0 && this.windowName.length > 0) {
      AppStorage.setOrCreate<string>(remoteWindowKeyForSession(this.sessionId), this.windowName);
      AppStorage.setOrCreate<string>(sessionKeyForWindow(this.windowName), this.sessionId);
      hilog.info(DOMAIN, TAG, 'remote ability window ready session=%{public}s name=%{public}s',
        this.sessionId, this.windowName);
    } else {
      hilog.error(DOMAIN, TAG,
        'remote ability 无法登记窗口身份 session=%{public}s name=%{public}s',
        this.sessionId, this.windowName);
    }
    // The title bar X is the session's disconnect path: let it close. Registering
    // (returning false = keep default) only leaves a log line as evidence.
    try {
      windowStage.on('windowStageClose', (): boolean => {
        hilog.info(DOMAIN, TAG, 'remote ability windowStageClose session=%{public}s',
          this.sessionId);
        return false;
      });
    } catch (error) {
      hilog.warn(DOMAIN, TAG, 'remote ability windowStageClose 注册失败：%{public}s', String(error));
    }
    const title: string = this.sessionId.length > 0
      ? titleForSession(this.sessionId, this.wantTitle) : '天关';
    windowStage.loadContent('pages/Remote', (error) => {
      if (error.code) {
        hilog.error(DOMAIN, TAG, 'remote ability 页面加载失败：%{public}s', error.message);
        return;
      }
      this.applyWindowChrome(main, title);
      // The window exists now, but WMS still lays it out for the first time;
      // applying our rectangle immediately loses to that initial placement.
      setTimeout((): void => {
        this.placeWindow(main);
      }, PLACEMENT_DELAY_MS);
    });
  }

  /**
   * Size and centre the window on the display it was created on.
   *
   * The display is resolved from the window's own displayId first (an external
   * screen must not make the window smaller than its own screen allows) and
   * falls back to the default display. Both resize and move are best effort:
   * a failure leaves the system default placement and only logs, it must never
   * break an otherwise usable session window. placementDone makes this run once;
   * later manual moves/resizes are not observed or overridden.
   */
  private placeWindow(main: window.Window | undefined): void {
    if (main === undefined || this.placementDone || this.placementInFlight) {
      return;
    }
    this.placementAttempts += 1;
    const screen: ScreenInfo | undefined = this.resolveScreen(main);
    if (screen === undefined) {
      return;
    }
    this.placementInFlight = true;
    const target: WindowPlacement = computePlacement(screen.width, screen.height);
    this.logWindowRect(main, 'before placement');
    hilog.info(DOMAIN, TAG,
      'remote window placement target screen=%{public}dx%{public}d display=%{public}d rect=[%{public}d,%{public}d,%{public}d,%{public}d]',
      screen.width, screen.height, screen.displayId,
      target.left, target.top, target.width, target.height);
    // The *Async variants return only once the move/resize has taken effect.
    main.resizeAsync(target.width, target.height).then((): Promise<void> => {
      return main.moveWindowToAsync(target.left, target.top);
    }).then((): void => {
      this.placementInFlight = false;
      setTimeout((): void => {
        this.verifyPlacement(main, target, screen.displayId);
      }, PLACEMENT_SETTLE_MS);
    }).catch((error: Error): void => {
      hilog.warn(DOMAIN, TAG, 'remote window 调整位置/尺寸失败（第 %{public}d 次）：%{public}s',
        this.placementAttempts, String(error));
      this.placementInFlight = false;
      this.retryPlacement(main);
    });
  }

  /**
   * Confirm the window's own rectangle first (it is one layout pass behind right
   * after the move, hence the settle delay above). WMS's layout list is logged
   * as a cross-check; it cannot distinguish two session windows that overlap
   * exactly, so it alone must not be the pass criterion.
   */
  private verifyPlacement(main: window.Window, target: WindowPlacement,
    displayId: number): void {
    let matched: boolean = false;
    try {
      const rect = main.getWindowProperties().windowRect;
      matched = Math.abs(rect.left - target.left) <= PLACEMENT_TOLERANCE_PX &&
        Math.abs(rect.top - target.top) <= PLACEMENT_TOLERANCE_PX &&
        Math.abs(rect.width - target.width) <= PLACEMENT_TOLERANCE_PX &&
        Math.abs(rect.height - target.height) <= PLACEMENT_TOLERANCE_PX;
    } catch (error) {
      hilog.warn(DOMAIN, TAG, 'remote window 核对窗口矩形失败：%{public}s', String(error));
    }
    this.logWindowRect(main, 'after placement');
    window.getAllWindowLayoutInfo(displayId).then((layouts: Array<window.WindowLayoutInfo>): void => {
      const rects: string[] = [];
      for (const layout of layouts) {
        const rect = layout.windowRect;
        rects.push('[' + rect.left + ',' + rect.top + ',' +
          rect.width + ',' + rect.height + ']');
      }
      hilog.info(DOMAIN, TAG, 'remote window layouts display=%{public}d %{public}s',
        displayId, rects.join(' '));
      if (matched) {
        this.placementDone = true;
        hilog.info(DOMAIN, TAG, 'remote window placement confirmed rect=[%{public}d,%{public}d,%{public}d,%{public}d]',
          target.left, target.top, target.width, target.height);
        return;
      }
      hilog.warn(DOMAIN, TAG, 'remote window 居中尚未生效（第 %{public}d 次）',
        this.placementAttempts);
      this.retryPlacement(main);
    }).catch((error: Error): void => {
      hilog.warn(DOMAIN, TAG, 'remote window 读取窗口布局失败，按窗口自身矩形判断：%{public}s',
        String(error));
      if (matched) {
        this.placementDone = true;
        hilog.info(DOMAIN, TAG, 'remote window placement confirmed rect=[%{public}d,%{public}d,%{public}d,%{public}d]',
          target.left, target.top, target.width, target.height);
        return;
      }
      this.retryPlacement(main);
    });
  }

  private retryPlacement(main: window.Window): void {
    if (this.placementDone || this.placementAttempts >= PLACEMENT_MAX_ATTEMPTS) {
      hilog.warn(DOMAIN, TAG, 'remote window 放弃居中，保留系统默认摆放（已尝试 %{public}d 次）',
        this.placementAttempts);
      this.placementDone = true;
      return;
    }
    setTimeout((): void => {
      this.placeWindow(main);
    }, PLACEMENT_RETRY_MS);
  }

  /** Screen size in window coordinates: current display first, then default. */
  private resolveScreen(main: window.Window): ScreenInfo | undefined {
    let displayId: number = 0;
    try {
      const properties = main.getWindowProperties();
      if (properties.displayId !== undefined) {
        displayId = properties.displayId;
      }
    } catch (error) {
      hilog.warn(DOMAIN, TAG, 'remote window 读取窗口属性失败：%{public}s', String(error));
    }
    try {
      const area = display.getDisplayByIdSync(displayId);
      if (area.width > 0 && area.height > 0) {
        return { width: area.width, height: area.height, displayId: displayId };
      }
      hilog.warn(DOMAIN, TAG, 'remote window 显示 %{public}d 尺寸无效，回退默认屏', displayId);
    } catch (error) {
      hilog.warn(DOMAIN, TAG, 'remote window 读取显示 %{public}d 尺寸失败，回退默认屏：%{public}s',
        displayId, String(error));
    }
    try {
      const area = display.getDefaultDisplaySync();
      if (area.width > 0 && area.height > 0) {
        return { width: area.width, height: area.height, displayId: 0 };
      }
      hilog.warn(DOMAIN, TAG, 'remote window 默认屏尺寸无效，放弃居中');
    } catch (error) {
      hilog.warn(DOMAIN, TAG, 'remote window 读取默认屏尺寸失败：%{public}s', String(error));
    }
    return undefined;
  }

  private logWindowRect(main: window.Window, stage: string): void {
    try {
      const properties = main.getWindowProperties();
      const rect = properties.windowRect;
      hilog.info(DOMAIN, TAG,
        'remote window rect %{public}s id=%{public}d name=%{public}s display=%{public}d [%{public}d,%{public}d,%{public}d,%{public}d]',
        stage, properties.id, properties.name ?? '', properties.displayId ?? -1,
        rect.left, rect.top, rect.width, rect.height);
    } catch (error) {
      hilog.warn(DOMAIN, TAG, 'remote window 读取窗口矩形失败：%{public}s', String(error));
    }
  }

  /**
   * Title after the content is up: setWindowTitle() has no effect before
   * loadContent(). A failure is cosmetic only.
   */
  private applyWindowChrome(main: window.Window | undefined, title: string): void {
    if (main === undefined) {
      return;
    }
    main.setWindowTitle(title).catch((error: Error): void => {
      hilog.warn(DOMAIN, TAG, 'remote ability 设置窗口标题失败：%{public}s', String(error));
    });
  }

  /**
   * Full quit (EntryAbility writes remote.quit) and per-session programmatic
   * close (RemoteWindows writes remote.close.<sessionId>) both end with this
   * instance calling terminateSelf(); the main ability's terminateSelf() only
   * ends itself.
   */
  private checkTermination(): void {
    if (this.terminating) {
      return;
    }
    const quitting: boolean = AppStorage.get<boolean>(REMOTE_QUIT_KEY) === true;
    const closeRequested: boolean = this.sessionId.length > 0 &&
      AppStorage.get<boolean>(remoteCloseKeyForSession(this.sessionId)) === true;
    if (!quitting && !closeRequested) {
      return;
    }
    this.terminating = true;
    hilog.info(DOMAIN, TAG, 'remote ability terminate session=%{public}s quit=%{public}s close=%{public}s',
      this.sessionId, String(quitting), String(closeRequested));
    try {
      this.context.terminateSelf().catch((error: Error): void => {
        this.terminating = false;
        hilog.warn(DOMAIN, TAG, 'remote ability terminateSelf 失败：%{public}s', String(error));
      });
    } catch (error) {
      this.terminating = false;
      hilog.warn(DOMAIN, TAG, 'remote ability terminateSelf 异常：%{public}s', String(error));
    }
  }

  private clearRegistry(): void {
    if (this.registryCleared) {
      return;
    }
    this.registryCleared = true;
    if (this.windowName.length === 0 || this.sessionId.length === 0) {
      return;
    }
    clearRemoteWindowRegistry(this.sessionId, this.windowName);
  }

  onWindowStageDestroy(): void {
    this.clearRegistry();
  }

  onDestroy(): void {
    if (this.terminationTimer >= 0) {
      clearInterval(this.terminationTimer);
      this.terminationTimer = -1;
    }
    this.clearRegistry();
    const quitting: boolean = AppStorage.get<boolean>(REMOTE_QUIT_KEY) === true;
    const closeRequested: boolean = this.sessionId.length > 0 &&
      AppStorage.get<boolean>(remoteCloseKeyForSession(this.sessionId)) === true;
    if (this.sessionId.length > 0 && !quitting && !closeRequested) {
      // The user closed this window: end its session, never the others.
      hilog.info(DOMAIN, TAG, 'remote ability closed, disconnecting session=%{public}s',
        this.sessionId);
      closeSessionById(this.sessionId);
    }
    if (this.sessionId.length > 0) {
      try {
        AppStorage.delete(remoteCloseKeyForSession(this.sessionId));
      } catch (error) {
        // Key already gone.
      }
    }
    hilog.info(DOMAIN, TAG, 'remote ability destroy session=%{public}s window=%{public}s',
      this.sessionId, this.windowName);
  }
}
