/**
 * The ONE module that imports the PC status-bar (tray) API:
 *   import { statusBarManager } from '@kit.DeskTopExtensionKit';
 *   (@hms.pcService.statusBarManager, @syscap
 *   SystemCapability.PCService.StatusBarManager, @since 5.0.0(12) — no
 *   permission, not a system API; verified in the local SDK d.ts
 *   sdk/default/hms/ets/api/@hms.pcService.statusBarManager.d.ts).
 *
 * It is loaded ONLY through a dynamic import from TianguanTrayService.ets, and
 * only after the device gate AND canIUse(SystemCapability.PCService.StatusBarManager)
 * both passed: a static import would be evaluated at app start and take the UI
 * process down on a device without the kit's runtime module. Raw platform calls
 * live here and nowhere else; every judgement belongs to the model/service.
 *
 * Signatures used (d.ts line numbers):
 *   addToStatusBar(context, StatusBarItem): void                     :453
 *   updateStatusBarMenu(context, StatusBarGroupMenu[]): void         :532
 *   updateStatusBarHoverTips(context, hoverTips): Promise<void>      :652 (6.0.2(22)+)
 *   removeFromStatusBar(context): void                               :493
 *   on('statusBarIconClick'|'rightMenuClick', Callback<EventData>)   :710/:740
 *   off(...)                                                         :724/:754
 *   isStatusBarCapabilitySupported(context): Promise<boolean>        :792 (26.0.0+)
 */
import { statusBarManager } from '@kit.DeskTopExtensionKit';
import common from '@ohos.app.ability.common';
import emitter from '@ohos.events.emitter';
import image from '@ohos.multimedia.image';
import {
  TRAY_ICON_BLACK_RESOURCE, TRAY_ICON_WHITE_RESOURCE, TRAY_KEY_ICON_CLICK_TYPE,
  TRAY_KEY_MENU_CODE, TRAY_MODULE_NAME, TRAY_QUICK_HEIGHT_VP, TRAY_QUICK_TITLE
} from './TianguanTrayConstants';
import type { TrayMenuSpec } from './TianguanTrayModel';

export interface TrayPlatformEvent {
  kind: string;
  menuCode: string;
}

export type TrayPlatformHandler = (event: TrayPlatformEvent) => void;

let handler: TrayPlatformHandler | undefined = undefined;
let listening: boolean = false;
let whiteIcon: image.PixelMap | undefined = undefined;
let blackIcon: image.PixelMap | undefined = undefined;

export function setPlatformHandler(next: TrayPlatformHandler | undefined): void {
  handler = next;
}

function emitEvent(kind: string, menuCode: string): void {
  const target: TrayPlatformHandler | undefined = handler;
  if (target === undefined) {
    return;
  }
  try {
    target({ kind: kind, menuCode: menuCode });
  } catch (error) {
    // A throwing listener must never escape into a platform callback.
  }
}

function eventValue(data: emitter.EventData, key: string): string {
  const bag: Record<string, Object> | undefined = data.data;
  if (bag === undefined || bag === null) {
    return '';
  }
  const value: Object | undefined = bag[key];
  return typeof value === 'string' ? value : '';
}

function onIconClick(data: emitter.EventData): void {
  emitEvent('icon-click', eventValue(data, TRAY_KEY_ICON_CLICK_TYPE));
}

function onRightMenuClick(data: emitter.EventData): void {
  emitEvent('menu-click', eventValue(data, TRAY_KEY_MENU_CODE));
}

/** The platform's own pre-flight answer; a ROM older than 26.0.0 cannot answer. */
export async function platformCapabilitySupported(context: common.Context): Promise<boolean> {
  if (typeof statusBarManager.isStatusBarCapabilitySupported !== 'function') {
    return true; // Pre-26 ROM: the check does not exist, do not turn the feature off.
  }
  return await statusBarManager.isStatusBarCapabilitySupported(context);
}

/** Decodes a packaged monochrome PNG once per process (24 vp at 2x = 48 px). */
async function decodeIconResource(context: common.Context, resourceName: string,
  cache: image.PixelMap | undefined): Promise<image.PixelMap | undefined> {
  if (cache !== undefined) {
    return cache;
  }
  try {
    const raw: Uint8Array = context.resourceManager.getMediaByNameSync(resourceName);
    const source: image.ImageSource = image.createImageSource(raw.buffer as ArrayBuffer);
    try {
      const size: image.Size = { width: 48, height: 48 };
      const options: image.DecodingOptions = { desiredSize: size, editable: false };
      return await source.createPixelMap(options);
    } finally {
      source.release().catch((): void => {});
    }
  } catch (error) {
    return undefined;
  }
}

/** The model's row specs -> the platform's menu tree. Event rows only:
 * notifyOnly + menuCode delivers the click through 'rightMenuClick'. */
function menuTree(groups: TrayMenuSpec[][]): statusBarManager.StatusBarGroupMenu[] {
  const tree: statusBarManager.StatusBarGroupMenu[] = [];
  for (let g = 0; g < groups.length; g++) {
    const specs: TrayMenuSpec[] = groups[g];
    const rows: statusBarManager.StatusBarMenuItem[] = [];
    for (let i = 0; i < specs.length; i++) {
      const action: statusBarManager.StatusBarMenuAction = {
        abilityName: 'EntryAbility',
        moduleName: TRAY_MODULE_NAME,
        notifyOnly: true
      };
      rows.push({
        title: specs[i].title, menuAction: action, menuCode: specs[i].menuCode
      });
    }
    tree.push(rows);
  }
  return tree;
}

export async function platformAdd(context: common.Context, groups: TrayMenuSpec[][],
  tips: string): Promise<void> {
  const white = await decodeIconResource(context, TRAY_ICON_WHITE_RESOURCE, whiteIcon);
  const black = await decodeIconResource(context, TRAY_ICON_BLACK_RESOURCE, blackIcon);
  const fallback = white !== undefined ? white : black;
  if (fallback === undefined) {
    throw new Error('tray icon resources undecodable');
  }
  whiteIcon = white;
  blackIcon = black;
  const icon: statusBarManager.StatusBarIcon = {
    white: white !== undefined ? white : fallback,
    black: black !== undefined ? black : fallback
  };
  const quick: statusBarManager.QuickOperation = {
    title: TRAY_QUICK_TITLE, height: TRAY_QUICK_HEIGHT_VP, abilityName: ''
  };
  const item: statusBarManager.StatusBarItem = {
    icons: icon, quickOperation: quick, statusBarGroupMenu: menuTree(groups), hoverTips: tips
  };
  statusBarManager.addToStatusBar(context, item);
}

export function platformUpdateMenu(context: common.Context, groups: TrayMenuSpec[][]): void {
  statusBarManager.updateStatusBarMenu(context, menuTree(groups));
}

export async function platformUpdateTips(context: common.Context, tips: string): Promise<void> {
  if (typeof statusBarManager.updateStatusBarHoverTips !== 'function') {
    return;
  }
  await statusBarManager.updateStatusBarHoverTips(context, tips);
}

/** Note error 1010710004: removal is refused while no window is foreground. */
export function platformRemove(context: common.Context): void {
  statusBarManager.removeFromStatusBar(context);
}

export function platformListen(): void {
  if (listening) {
    return;
  }
  statusBarManager.on('statusBarIconClick', onIconClick);
  statusBarManager.on('rightMenuClick', onRightMenuClick);
  listening = true;
}

export function platformUnlisten(): void {
  if (!listening) {
    return;
  }
  try {
    statusBarManager.off('statusBarIconClick', onIconClick);
    statusBarManager.off('rightMenuClick', onRightMenuClick);
  } finally {
    listening = false;
  }
}
