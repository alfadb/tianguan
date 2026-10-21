/**
 * Pure tray logic: no platform imports, no state. Decides whether the tray runs
 * at all, what the menu looks like for a set of sessions, and what the hover
 * tip says. Structure mirrors the proven netbird-harmonyos tray modules
 * (client/entry/src/main/ets/tray/) with the state swapped for Tianguan
 * sessions.
 */

/** One action row of the right-click menu. */
export interface TrayMenuSpec {
  title: string;
  menuCode: string;
}

/** A session line the tray can open. */
export interface TraySessionLine {
  sessionId: string;
  name: string;
}

export const TRAY_MENU_MAIN: string = 'tg_tray_open_main';
export const TRAY_MENU_MAIN_TITLE: string = '打开主窗口';
export const TRAY_MENU_SESSION_PREFIX: string = 'tg_tray_session:';
// The platform allows at most 20 level-1 items in total; keep headroom.
export const TRAY_SESSION_ROW_CAP: number = 18;

/** 2in1 (PC) is the only device class with the status-bar capability. */
export function trayDeviceSupported(deviceType: string): boolean {
  return deviceType === '2in1';
}

export function trayMenuCodeKnown(code: string): boolean {
  return code === TRAY_MENU_MAIN || code.startsWith(TRAY_MENU_SESSION_PREFIX);
}

export function sessionIdFromMenuCode(code: string): string {
  if (!code.startsWith(TRAY_MENU_SESSION_PREFIX)) {
    return '';
  }
  return code.substring(TRAY_MENU_SESSION_PREFIX.length);
}

/** Right-click menu: [打开主窗口] + one 打开 <名称> row per active session. */
export function trayMenuGroups(sessionLines: TraySessionLine[]): TrayMenuSpec[][] {
  const main: TrayMenuSpec[] = [{ title: TRAY_MENU_MAIN_TITLE, menuCode: TRAY_MENU_MAIN }];
  const rows: TrayMenuSpec[] = [];
  for (let i = 0; i < sessionLines.length && rows.length < TRAY_SESSION_ROW_CAP; i++) {
    rows.push({
      title: '打开 ' + sessionLines[i].name,
      menuCode: TRAY_MENU_SESSION_PREFIX + sessionLines[i].sessionId
    });
  }
  return rows.length > 0 ? [main, rows] : [main];
}

/** Dedup signature: the whole menu shape in one comparable string. */
export function trayMenuSignature(groups: TrayMenuSpec[][]): string {
  const parts: string[] = [];
  for (let g = 0; g < groups.length; g++) {
    const titles: string[] = [];
    for (let i = 0; i < groups[g].length; i++) {
      titles.push(groups[g][i].menuCode + '=' + groups[g][i].title);
    }
    parts.push(titles.join('|'));
  }
  return parts.join('||');
}

export function trayHoverTips(sessionCount: number): string {
  if (sessionCount <= 0) {
    return '天关 · 未连接';
  }
  return '天关 · ' + sessionCount + ' 个会话';
}
