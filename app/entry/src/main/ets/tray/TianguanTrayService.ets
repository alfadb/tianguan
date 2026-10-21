/**
 * The app-facing tray module. EntryAbility starts it once per process and wires
 * the session opener; the SessionRegistry listener pushes session lines in.
 *
 * WHAT LIVES HERE: the device/syscap gate, the LAZY load of the platform module
 * (so a phone or an old ROM never evaluates the status-bar import), the
 * deduplicated + 500 ms-coalesced state pushes (the status-bar APIs answer
 * 1010710003 when called too frequently), and the routing of the tray's two
 * events. Every platform touch is wrapped: on a phone, a tablet, a ROM without
 * the capability or with the kit missing, the feature stays off permanently
 * (one hilog line); a runtime failure (icon/menu/tip update) only fails that
 * attempt, so the next start/flush may retry.
 *
 * Behaviour (mirrors the proven netbird tray): a persistent status-bar icon on
 * 2in1; LEFT click opens/activates the main window; RIGHT-click menu carries
 * 打开主窗口 plus one 打开 <名称> row per ACTIVE session (at most
 * TRAY_SESSION_ROW_CAP rows, then a 更多会话… row that only opens the manager);
 * the system's own mandatory 退出 row is left alone
 * (EntryAbility.onPrepareToTerminate does the full cleanup).
 */
import common from '@ohos.app.ability.common';
import deviceInfo from '@ohos.deviceInfo';
import hilog from '@ohos.hilog';
import Want from '@ohos.app.ability.Want';
import { TRAY_MENU_MAIN, TRAY_MENU_SESSION_PREFIX, TRAY_SESSION_ROW_CAP, trayDeviceSupported,
  trayHoverTips, trayMenuCodeKnown, trayMenuGroups, trayMenuSignature } from './TianguanTrayModel';
import type { TrayMenuSpec, TraySessionLine } from './TianguanTrayModel';
import { TRAY_UPDATE_DEBOUNCE_MS } from './TianguanTrayConstants';
import { activeSessions } from '../model/SessionRegistry';
import type { RuntimeSession } from '../model/SessionRegistry';

const DOMAIN: number = 0x0000;
const TAG: string = 'TianguanDiag';
const BUNDLE_NAME: string = 'cn.alfadb.tianguan';
const ENTRY_ABILITY_NAME: string = 'EntryAbility';
/** Last row when more active sessions exist than the menu can show. */
const TRAY_MENU_MORE: string = 'tg_tray_more_sessions';
const TRAY_MENU_MORE_TITLE: string = '更多会话…';

/** Opens one session's window (wired by EntryAbility to windows/RemoteWindows). */
export type TraySessionOpener = (sessionId: string) => void;

interface TrayServiceState {
  context: common.UIAbilityContext | undefined;
  started: boolean;
  unavailable: boolean;
  startInFlight: boolean;
  sessionOpener: TraySessionOpener | undefined;
  tips: string;
  menuSignature: string;
  pendingLines: TraySessionLine[] | undefined;
  timer: number;
}

const state: TrayServiceState = {
  context: undefined,
  started: false,
  unavailable: false,
  startInFlight: false,
  sessionOpener: undefined,
  tips: '',
  menuSignature: '',
  pendingLines: undefined,
  timer: -1
};

let platform: typeof import('./TianguanTrayPlatform') | undefined = undefined;

async function loadPlatform(): Promise<typeof import('./TianguanTrayPlatform') | undefined> {
  if (platform !== undefined) {
    return platform;
  }
  try {
    platform = await import('./TianguanTrayPlatform');
    return platform;
  } catch (error) {
    hilog.warn(DOMAIN, TAG, 'ui_tray platform-module-missing：%{public}s', String(error));
    state.unavailable = true;
    return undefined;
  }
}

/**
 * Menu rows may only show sessions the registry still considers active (F05):
 * a disconnected/failed session must never be offered from the tray.
 */
function activeTrayLines(lines: TraySessionLine[]): TraySessionLine[] {
  const active: RuntimeSession[] = activeSessions();
  const activeIds: Set<string> = new Set<string>();
  for (let i = 0; i < active.length; i++) {
    activeIds.add(active[i].sessionId);
  }
  const result: TraySessionLine[] = [];
  for (let i = 0; i < lines.length; i++) {
    if (activeIds.has(lines[i].sessionId)) {
      result.push(lines[i]);
    }
  }
  return result;
}

/**
 * Menu tree for the (already active-filtered) lines. The model caps session
 * rows at TRAY_SESSION_ROW_CAP; when more sessions are active, append an
 * explicit last row that only brings the main window up.
 */
function menuGroupsFor(lines: TraySessionLine[]): TrayMenuSpec[][] {
  const groups: TrayMenuSpec[][] = trayMenuGroups(lines);
  if (lines.length > TRAY_SESSION_ROW_CAP) {
    const last: TrayMenuSpec[] = groups[groups.length - 1];
    last.push({ title: TRAY_MENU_MORE_TITLE, menuCode: TRAY_MENU_MORE });
  }
  return groups;
}

/** Called ONCE per UI process from EntryAbility.onCreate. */
export async function startTray(context: common.UIAbilityContext): Promise<boolean> {
  state.context = context;
  if (state.unavailable || state.started) {
    return state.started;
  }
  if (state.startInFlight) {
    return false;
  }
  const deviceType: string = typeof deviceInfo.deviceType === 'string' ? deviceInfo.deviceType : '';
  if (!trayDeviceSupported(deviceType)) {
    // Phone/tablet/anything else: the capability does not exist there.
    hilog.info(DOMAIN, TAG, 'ui_tray skipped device-not-2in1 type=%{public}s', deviceType);
    state.unavailable = true;
    return false;
  }
  if (!canIUse('SystemCapability.PCService.StatusBarManager')) {
    hilog.info(DOMAIN, TAG, 'ui_tray skipped syscap-absent');
    state.unavailable = true;
    return false;
  }
  state.startInFlight = true;
  try {
    const module = await loadPlatform();
    if (module === undefined) {
      state.unavailable = true;
      return false;
    }
    let capability = true;
    try {
      capability = await module.platformCapabilitySupported(context);
    } catch (error) {
      // A transient failure of the capability probe is NOT proof that the
      // device lacks the feature: leave it retryable (F20).
      hilog.warn(DOMAIN, TAG, 'ui_tray capability-check-failed（稍后可重试）：%{public}s', String(error));
      return false;
    }
    if (!capability) {
      // The ROM itself says the status-bar capability is unsupported: permanent.
      hilog.info(DOMAIN, TAG, 'ui_tray skipped capability-unsupported');
      state.unavailable = true;
      return false;
    }
    const lines: TraySessionLine[] = activeTrayLines(state.pendingLines ?? []);
    const groups: TrayMenuSpec[][] = menuGroupsFor(lines);
    const tips = trayHoverTips(lines.length);
    await module.platformAdd(context, groups, tips);
    module.setPlatformHandler(onPlatformEvent);
    module.platformListen();
    state.started = true;
    state.tips = tips;
    state.menuSignature = trayMenuSignature(groups);
    hilog.info(DOMAIN, TAG, 'ui_tray started device=%{public}s', deviceType);
    if (state.pendingLines !== undefined) {
      scheduleFlush();
    }
    return true;
  } catch (error) {
    // Icon decode / add-to-status-bar failures are runtime failures: do NOT
    // lock the feature off — a later startTray() call may succeed (F20/F21).
    hilog.warn(DOMAIN, TAG, 'ui_tray start-failed（稍后可重试）：%{public}s', String(error));
    return false;
  } finally {
    state.startInFlight = false;
  }
}

export function setTraySessionOpener(opener: TraySessionOpener | undefined): void {
  state.sessionOpener = opener;
}

/** Registry change hook: store the lines, arm ONE coalesced flush. */
export function updateTraySessions(lines: TraySessionLine[]): void {
  // Keep only sessions the registry still considers active (F05).
  state.pendingLines = activeTrayLines(lines);
  if (!state.started) {
    return;
  }
  scheduleFlush();
}

function scheduleFlush(): void {
  if (state.timer >= 0) {
    return;
  }
  state.timer = setTimeout((): void => {
    state.timer = -1;
    flushTray();
  }, TRAY_UPDATE_DEBOUNCE_MS);
}

function flushTray(): void {
  const module = platform;
  const context = state.context;
  if (module === undefined || context === undefined || !state.started) {
    return;
  }
  const lines: TraySessionLine[] = state.pendingLines ?? [];
  const groups: TrayMenuSpec[][] = menuGroupsFor(lines);
  const signature = trayMenuSignature(groups);
  if (signature !== state.menuSignature) {
    try {
      module.platformUpdateMenu(context, groups);
      // Commit the signature only after the platform accepted the update, so a
      // failed update is retried on the next flush instead of being lost (F20).
      state.menuSignature = signature;
      hilog.info(DOMAIN, TAG, 'ui_tray update menu sessions=%{public}d', lines.length);
    } catch (error) {
      hilog.warn(DOMAIN, TAG, 'ui_tray update-menu-failed（下次重试）：%{public}s', String(error));
    }
  }
  const tips = trayHoverTips(lines.length);
  if (tips !== state.tips) {
    module.platformUpdateTips(context, tips).then((): void => {
      state.tips = tips;
    }).catch((error: Error): void => {
      // Keep the old tips so the next flush retries this update.
      hilog.warn(DOMAIN, TAG, 'ui_tray update-tips-failed（下次重试）：%{public}s', String(error));
    });
  }
}

function onPlatformEvent(platformEvent: import('./TianguanTrayPlatform').TrayPlatformEvent): void {
  if (platformEvent.kind === 'icon-click') {
    openMainWindow();
    return;
  }
  if (platformEvent.kind !== 'menu-click') {
    return;
  }
  const code: string = platformEvent.menuCode;
  if (code === TRAY_MENU_MORE) {
    // The overflow row carries no session id; it only reopens the manager.
    openMainWindow();
    return;
  }
  if (!trayMenuCodeKnown(code)) {
    hilog.warn(DOMAIN, TAG, 'ui_tray unknown-menu-code：%{public}s', code);
    return;
  }
  if (code === TRAY_MENU_MAIN) {
    openMainWindow();
    return;
  }
  const opener = state.sessionOpener;
  if (opener === undefined) {
    hilog.warn(DOMAIN, TAG, 'ui_tray no-session-opener');
    return;
  }
  const sessionId = code.substring(TRAY_MENU_SESSION_PREFIX.length);
  hilog.info(DOMAIN, TAG, 'ui_tray open-session %{public}s', sessionId);
  try {
    opener(sessionId);
  } catch (error) {
    hilog.warn(DOMAIN, TAG, 'ui_tray open-session-failed：%{public}s', String(error));
  }
}

/**
 * showAbility() first (a window hidden by hideAbility() is NOT brought back by
 * a hot start alone), then startAbility on our own EntryAbility — the hot start
 * path that restores a merely-backgrounded window. Both refusals are logged and
 * ignored: each covers a case the other handles.
 */
export function openMainWindow(): void {
  const context = state.context;
  if (context === undefined) {
    return;
  }
  try {
    context.showAbility().catch((error: Error): void => {
      hilog.info(DOMAIN, TAG, 'ui_tray show-ability：%{public}s', String(error));
    });
  } catch (error) {
    hilog.info(DOMAIN, TAG, 'ui_tray show-ability-sync：%{public}s', String(error));
  }
  const want: Want = { bundleName: BUNDLE_NAME, abilityName: ENTRY_ABILITY_NAME };
  try {
    context.startAbility(want).catch((error: Error): void => {
      hilog.warn(DOMAIN, TAG, 'ui_tray start-ability：%{public}s', String(error));
    });
  } catch (error) {
    hilog.warn(DOMAIN, TAG, 'ui_tray start-ability-sync：%{public}s', String(error));
  }
}

/** Best-effort removal on the explicit-quit path; the icon dies with the
 * process anyway, and 1010710004 (no foreground window) is expected. */
export async function removeTray(reason: string): Promise<void> {
  const module = platform;
  const context = state.context;
  if (module === undefined || context === undefined || !state.started) {
    return;
  }
  if (state.timer >= 0) {
    clearTimeout(state.timer);
    state.timer = -1;
  }
  try {
    module.platformUnlisten();
  } catch (error) {
    hilog.warn(DOMAIN, TAG, 'ui_tray unlisten-failed：%{public}s', String(error));
  }
  try {
    module.platformRemove(context);
    hilog.info(DOMAIN, TAG, 'ui_tray removed reason=%{public}s', reason);
  } catch (error) {
    hilog.warn(DOMAIN, TAG, 'ui_tray remove-failed (icon dies with process) reason=%{public}s err=%{public}s',
      reason, String(error));
  }
  state.started = false;
}

export function trayStarted(): boolean {
  return state.started;
}
