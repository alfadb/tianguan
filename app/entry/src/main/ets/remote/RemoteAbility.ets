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

export default class RemoteAbility extends UIAbility {
  private sessionId: string = '';
  private wantTitle: string = '';
  private windowName: string = '';
  private terminationTimer: number = -1;
  private terminating: boolean = false;
  private registryCleared: boolean = false;

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
    });
  }

  /**
   * Title and size after the content is up: setWindowTitle() has no effect
   * before loadContent(), and a resize failure must never break a window that is
   * otherwise usable. The default system size is an accepted fallback.
   */
  private applyWindowChrome(main: window.Window | undefined, title: string): void {
    if (main === undefined) {
      return;
    }
    main.setWindowTitle(title).catch((error: Error): void => {
      hilog.warn(DOMAIN, TAG, 'remote ability 设置窗口标题失败：%{public}s', String(error));
    });
    try {
      const area = display.getDefaultDisplaySync();
      const width: number = Math.max(320, Math.round(area.width * 0.92));
      const height: number = Math.max(240, Math.round(area.height * 0.9));
      main.resize(width, height).catch((error: Error): void => {
        // Default system size stays; this is cosmetic only.
        hilog.warn(DOMAIN, TAG, 'remote ability 调整窗口尺寸失败：%{public}s', String(error));
      });
    } catch (error) {
      hilog.warn(DOMAIN, TAG, 'remote ability 读取屏幕尺寸失败：%{public}s', String(error));
    }
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
