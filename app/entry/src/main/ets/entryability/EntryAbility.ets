/**
 * EntryAbility: connection-manager lifecycle host.
 *
 *  - Close-to-background: the main window's X never exits. windowStageClose is
 *    intercepted (callback returns true = keep the window); with the tray icon
 *    up (2in1) the window AND its dock entry disappear via
 *    UIAbilityContext.hideAbility(), falling back to minimize() on any refusal;
 *    the tray (or any hot start) brings it back with showAbility().
 *  - Full quit (the system tray's mandatory 退出 row arrives as
 *    onPrepareToTerminate): cancel the first request (return true), run the
 *    close/tray/terminateSelf() cleanup asynchronously, and wait (bounded) for
 *    the native close reaper before terminating. A second request within 5 s
 *    continues the termination immediately (return false) so quitting can
 *    never wedge.
 *  - The session registry poll (2 s) runs here so list/tray state keeps
 *    refreshing while the main window is hidden.
 */
import { UIAbility } from '@kit.AbilityKit';
import type { Want } from '@kit.AbilityKit';
import AbilityConstant from '@ohos.app.ability.AbilityConstant';
import { window } from '@kit.ArkUI';
import { hilog } from '@kit.PerformanceAnalysisKit';
import diag from 'libtianguan_diag.so';
import { startTray, removeTray, trayStarted, updateTraySessions,
  setTraySessionOpener } from '../tray/TianguanTrayService';
import type { TraySessionLine } from '../tray/TianguanTrayModel';
import { initConnectionStore } from '../model/ConnectionStore';
import { addSessionsListener, allSessions, closeAllSessions,
  startRegistryPolling, stopRegistryPolling } from '../model/SessionRegistry';
import { openRemoteWindow } from '../windows/RemoteWindows';
import { clipboardReset } from '../clipboard/TianguanClipboard';

const DOMAIN: number = 0x0000;
const TAG: string = 'TianguanDiag';
const SECOND_TERMINATE_PASS_MS: number = 5000;
/** Poll interval while waiting for the native close reapers. */
const QUIT_PENDING_POLL_MS: number = 100;
/** Hard cap: after this the quit continues even if a session is still reaping. */
const QUIT_CLOSE_TIMEOUT_MS: number = 3000;

interface BusinessFailure extends Error {
  code?: number;
}

/** Process-scoped hide flag (see hideOnClose): a fresh process reads false, so
 * nothing on the launch path ever hides a window on its own. */
let windowHidden: boolean = false;
/** Our own quit flow is running — later terminate requests pass through. */
let explicitQuit: boolean = false;
let lastTerminateRequestAt: number = 0;

export default class EntryAbility extends UIAbility {
  onCreate(want: Want): void {
    initConnectionStore(this.context);
    // Session lines -> tray menu (deduped + coalesced inside the service).
    addSessionsListener((): void => {
      const lines: TraySessionLine[] = allSessions().map((item): TraySessionLine => {
        return { sessionId: item.sessionId, name: item.name };
      });
      updateTraySessions(lines);
    });
    setTraySessionOpener((sessionId: string) => {
      openRemoteWindow(sessionId).then((result: string) => {
        if (result === 'fallback-main' || result === 'no-stage') {
          // No sub-window possible and no page context here: bring the manager
          // up; the user can open the session from the list.
          this.showHiddenWindow('tray-fallback');
        }
      });
    });
    startTray(this.context).catch((error: Error): void => {
      hilog.warn(DOMAIN, TAG, 'ui_tray start 异常：%{public}s', String(error));
    });
    startRegistryPolling();
  }

  onWindowStageCreate(windowStage: window.WindowStage): void {
    // Sub-windows are created from the stage, so keep it reachable.
    AppStorage.setOrCreate('windowStage', windowStage);
    windowStage.loadContent('pages/Index', (error) => {
      if (error.code) {
        hilog.error(DOMAIN, TAG, '页面加载失败：%{public}s', error.message);
      }
    });
    this.registerCloseToBackground(windowStage);
  }

  onNewWant(want: Want, launchParam: AbilityConstant.LaunchParam): void {
    this.showHiddenWindow('onNewWant');
  }

  onForeground(): void {
    // The platform has already put us in front; the flag must agree.
    windowHidden = false;
  }

  onDestroy(): void {
    stopRegistryPolling();
    closeAllSessions();
    clipboardReset();
    removeTray('destroy').catch((): void => {});
  }

  /**
   * The system tray's mandatory 退出 (and Dock removal requests) arrive here.
   * SDK semantics: true = cancel this termination, false = continue terminating.
   *
   * First request: return true and start the asynchronous cleanup, which ends
   * with terminateSelf(). Never do blocking work before returning: the close
   * itself is non-blocking (native reaper thread), and the bounded wait runs on
   * timer callbacks after this stack has unwound. Second request within 5 s:
   * return false immediately — even a stuck cleanup must not trap the user.
   */
  onPrepareToTerminate(): boolean {
    const now = Date.now();
    if (explicitQuit || (lastTerminateRequestAt > 0 && now - lastTerminateRequestAt < SECOND_TERMINATE_PASS_MS)) {
      hilog.info(DOMAIN, TAG, 'ui_terminate pass-through');
      return false;
    }
    lastTerminateRequestAt = now;
    // Set before returning so a second request can never start a second flow.
    explicitQuit = true;
    hilog.info(DOMAIN, TAG, 'ui_terminate veto-once full-quit');
    this.performFullQuit('system-exit').catch((error: Error): void => {
      hilog.error(DOMAIN, TAG, 'ui_terminate cleanup failed：%{public}s', String(error));
    });
    // Cancel this attempt; performFullQuit() terminates once it is done.
    return true;
  }

  /** The one quit path: disconnect everything, drop the tray, terminate. */
  private async performFullQuit(reason: string): Promise<void> {
    explicitQuit = true;
    // Yield before any work so the cancellation (return true above) reaches the
    // framework first and nothing here can run on its calling stack.
    await this.delay(0);
    try {
      // Non-blocking since F7: each native closeSession() removes the session
      // and hands its thread join to a detached reaper. This returns at once.
      closeAllSessions();
    } catch (error) {
      hilog.warn(DOMAIN, TAG, '退出清理会话失败：%{public}s', String(error));
    }
    const remaining: number = await this.waitForPendingCloses(QUIT_CLOSE_TIMEOUT_MS);
    if (remaining > 0) {
      // A session thread is stuck in an uncancellable getaddrinfo(): exit
      // anyway, loudly, rather than trapping the user in the app.
      hilog.warn(DOMAIN, TAG,
        'ui_terminate close-timeout pending=%{public}d，超时强退，可能有会话未优雅关闭',
        remaining);
    } else {
      hilog.info(DOMAIN, TAG, 'ui_terminate sessions-reaped');
    }
    try {
      clipboardReset();
    } catch (error) {
      hilog.warn(DOMAIN, TAG, '退出清理剪贴板失败：%{public}s', String(error));
    }
    await removeTray(reason).catch((error: Error): void => {
      hilog.warn(DOMAIN, TAG, '退出移除托盘失败：%{public}s', String(error));
    });
    try {
      await this.context.terminateSelf();
    } catch (error) {
      hilog.error(DOMAIN, TAG, 'terminateSelf 失败：%{public}s', String(error));
    }
  }

  /**
   * Polls pendingCloses() until every closed session has been reaped or the
   * deadline passes. Returns the number still pending (0 = clean).
   * Timer callbacks keep running while the ability is otherwise idle, so the
   * cleanup does not depend on the UI thread being woken by another event.
   */
  private async waitForPendingCloses(timeoutMs: number): Promise<number> {
    const deadline: number = Date.now() + timeoutMs;
    let remaining: number = this.pendingCloseCount();
    while (remaining > 0 && Date.now() < deadline) {
      await this.delay(QUIT_PENDING_POLL_MS);
      remaining = this.pendingCloseCount();
    }
    return remaining;
  }

  /**
   * pendingCloses() never joins a thread. A missing export (older .so) or a
   * throw is treated as 0: the quit must not be blocked by the status probe.
   */
  private pendingCloseCount(): number {
    try {
      const value: number = diag.pendingCloses();
      return value > 0 ? Math.floor(value) : 0;
    } catch (error) {
      hilog.warn(DOMAIN, TAG, 'ui_terminate pendingCloses unavailable：%{public}s', String(error));
      return 0;
    }
  }

  private delay(ms: number): Promise<void> {
    return new Promise<void>((resolve) => {
      setTimeout((): void => {
        resolve();
      }, ms);
    });
  }

  // -------------------------------------------------------------------------
  // Close-to-background (X = hide, not exit).
  // -------------------------------------------------------------------------

  private registerCloseToBackground(windowStage: window.WindowStage): void {
    try {
      windowStage.on('windowStageClose', (): boolean => {
        hilog.info(DOMAIN, TAG, 'ui_close stage=windowStageClose');
        this.hideOnClose(windowStage);
        // true = do not close (destroy) the main window.
        return true;
      });
      hilog.info(DOMAIN, TAG, 'ui_close registered windowStageClose');
    } catch (error) {
      const code: number | undefined = (error as BusinessFailure).code;
      // 801 on a non-free-window device or an old ROM: the X keeps its default
      // behaviour (terminate), and onPrepareToTerminate runs the full cleanup.
      hilog.warn(DOMAIN, TAG, 'ui_close register-failed code=%{public}s', String(code ?? 'none'));
    }
  }

  /**
   * hideAbility() requires the app to be on the status bar (16000067
   * otherwise), so it is only tried with the tray icon up; every refusal falls
   * back to minimize() — the window stays in the dock but the app and the
   * sessions keep running.
   */
  private hideOnClose(windowStage: window.WindowStage): void {
    if (!trayStarted()) {
      this.minimizeOnClose(windowStage);
      return;
    }
    try {
      this.context.hideAbility().then((): void => {
        windowHidden = true;
        hilog.info(DOMAIN, TAG, 'ui_close hide ok (window + dock entry gone)');
      }).catch((error: Error): void => {
        const code: number | undefined = (error as BusinessFailure).code;
        hilog.warn(DOMAIN, TAG, 'ui_close hide failed code=%{public}s fallback=minimize',
          String(code ?? 'none'));
        this.minimizeOnClose(windowStage);
      });
    } catch (error) {
      const code: number | undefined = (error as BusinessFailure).code;
      hilog.warn(DOMAIN, TAG, 'ui_close hide threw code=%{public}s fallback=minimize',
        String(code ?? 'none'));
      this.minimizeOnClose(windowStage);
    }
  }

  /** The reopen primitive: only a really-hidden process acts. */
  private showHiddenWindow(trigger: string): void {
    if (!windowHidden) {
      return;
    }
    try {
      this.context.showAbility().then((): void => {
        windowHidden = false;
        hilog.info(DOMAIN, TAG, 'ui_close show ok trigger=%{public}s', trigger);
      }).catch((error: Error): void => {
        hilog.warn(DOMAIN, TAG, 'ui_close show failed trigger=%{public}s err=%{public}s',
          trigger, String(error));
      });
    } catch (error) {
      hilog.warn(DOMAIN, TAG, 'ui_close show threw trigger=%{public}s err=%{public}s',
        trigger, String(error));
    }
  }

  private minimizeOnClose(windowStage: window.WindowStage): void {
    try {
      windowStage.getMainWindowSync().minimize().then((): void => {
        hilog.info(DOMAIN, TAG, 'ui_close minimize ok (background kept)');
      }).catch((error: Error): void => {
        hilog.warn(DOMAIN, TAG, 'ui_close minimize failed err=%{public}s', String(error));
      });
    } catch (error) {
      hilog.warn(DOMAIN, TAG, 'ui_close minimize threw err=%{public}s (window stays open)',
        String(error));
    }
  }
}
