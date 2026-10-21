/**
 * Shared constants for the tray modules. Kept separate so the pure model stays
 * free of any import it does not need, and so the platform module has one place
 * to read resource names from.
 */
export const TRAY_MODULE_NAME: string = 'entry';
export const TRAY_QUICK_TITLE: string = '天关';
export const TRAY_QUICK_HEIGHT_VP: number = 1;
export const TRAY_ICON_WHITE_RESOURCE: string = 'tray_icon_white';
export const TRAY_ICON_BLACK_RESOURCE: string = 'tray_icon_black';
export const TRAY_KEY_MENU_CODE: string = 'menuCode';
export const TRAY_KEY_ICON_CLICK_TYPE: string = 'iconClickType';
export const TRAY_UPDATE_DEBOUNCE_MS: number = 500;
