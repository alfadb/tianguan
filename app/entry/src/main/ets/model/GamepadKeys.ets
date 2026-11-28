/**
 * Raw gamepad-event probe: which keys belong to a controller (pure logic, no SDK
 * imports, unit-tested on the host).
 *
 * The probe logs every key event that reaches the session page and intercepts
 * the controller keys so the D-pad never moves the UI focus. Key codes come from
 * @ohos.multimodalInput.keyCode:
 *   - KEYCODE_DPAD_UP/DOWN/LEFT/RIGHT = 2012..2015
 *   - KEYCODE_BUTTON_A..THUMBR        = 2301..2315
 *   - 10167 (KEYCODE_INVALID): the trigger artefact the pad also reports; it is
 *     intercepted like a pad key but maps to no button (see model/Gamepad.ets).
 * A device is remembered as a gamepad as soon as one of its keys is in those
 * ranges; every later key from the same deviceId is intercepted too, so buttons
 * with codes outside the ranges still behave like controller keys.
 */

/** True for the D-pad / controller button key codes of the multimodal KeyCode. */
export function isGamepadKeyCode(keyCode: number): boolean {
  if (keyCode === 10167) {
    return true;
  }
  if (keyCode >= 2012 && keyCode <= 2015) {
    return true;
  }
  return keyCode >= 2301 && keyCode <= 2315;
}

/** True when this key comes from a device already identified as a gamepad. */
export function isGamepadDeviceKey(deviceId: number, knownGamepadIds: number[]): boolean {
  return knownGamepadIds.indexOf(deviceId) >= 0;
}

/**
 * One-line decision for the probe: a controller key (by code or by its device)
 * is logged and consumed; a keyboard key keeps the existing key path.
 */
export function isGamepadProbeKey(keyCode: number, deviceId: number,
  knownGamepadIds: number[]): boolean {
  return isGamepadKeyCode(keyCode) || isGamepadDeviceKey(deviceId, knownGamepadIds);
}
