/**
 * Moonlight gamepad state machine (pure logic, no SDK imports).
 *
 * The session page feeds every gamepad key / axis event into this state and a
 * 16 ms ticker sends the resulting snapshot with sendGamepadState(). Bit values
 * and axis conventions follow Moonlight's controller state: `buttons` is a
 * 16-bit mask, triggers are 0..255, sticks are +-32767.
 *
 * Mapping measured on the USB Xbox Series pad of the test PC:
 *   2301 A, 2302 B, 2304 X, 2305 Y, 2307 LB, 2308 RB, 2311 BACK, 2312 PLAY,
 *   2313 SPECIAL, 2314 LS, 2315 RS, 2012..2015 D-pad up/down/left/right.
 *   10167 (KEYCODE_INVALID, a trigger artefact) is intercepted but ignored.
 * Axes: ABS_X/ABS_Y = left stick, ABS_Z/ABS_RZ = right stick (-1..1),
 *   ABS_BRAKE = LT, ABS_GAS = RT (0..1). The D-pad's ABS_HAT0X/Y is ignored
 *   because the key codes already carry the D-pad.
 */

/** Radial deadzone of both sticks, as a fraction of full deflection. */
export const GAMEPAD_DEADZONE: number = 0.12;

/** Raw input values as they arrive from the page (pre deadzone). */
export interface GamepadRaw {
  buttons: number;
  lt: number;   // 0..1
  rt: number;   // 0..1
  lx: number;   // -1..1
  ly: number;
  rx: number;
  rz: number;   // right stick Y (ABS_RZ)
}

/** What sendGamepadState() receives: Moonlight units. */
export interface GamepadOut {
  buttons: number;
  lt: number;   // 0..255
  rt: number;
  lx: number;   // -32767..32767
  ly: number;
  rx: number;
  ry: number;
}

export interface StickPair {
  x: number;
  y: number;
}

export function emptyGamepadRaw(): GamepadRaw {
  return { buttons: 0, lt: 0, rt: 0, lx: 0, ly: 0, rx: 0, rz: 0 };
}

/** Moonlight button bit of a gamepad key code; 0 = no button (incl. 10167). */
export function gamepadButtonMask(keyCode: number): number {
  if (keyCode === 2301) {
    return 0x1000; // A
  }
  if (keyCode === 2302) {
    return 0x2000; // B
  }
  if (keyCode === 2304) {
    return 0x4000; // X
  }
  if (keyCode === 2305) {
    return 0x8000; // Y
  }
  if (keyCode === 2307) {
    return 0x0100; // LB
  }
  if (keyCode === 2308) {
    return 0x0200; // RB
  }
  if (keyCode === 2311) {
    return 0x0020; // SELECT / View -> BACK
  }
  if (keyCode === 2312) {
    return 0x0010; // START / Menu -> PLAY
  }
  if (keyCode === 2313) {
    return 0x0400; // MODE / Xbox -> SPECIAL
  }
  if (keyCode === 2314) {
    return 0x0040; // LS
  }
  if (keyCode === 2315) {
    return 0x0080; // RS
  }
  if (keyCode === 2012) {
    return 0x0001; // UP
  }
  if (keyCode === 2013) {
    return 0x0002; // DOWN
  }
  if (keyCode === 2014) {
    return 0x0004; // LEFT
  }
  if (keyCode === 2015) {
    return 0x0008; // RIGHT
  }
  return 0;
}

/**
 * Returns the state with a button pressed / released. An unmapped code (such as
 * the ignored 10167) returns the very same object, so callers can use identity
 * to detect "no change".
 */
export function applyGamepadKey(raw: GamepadRaw, keyCode: number,
  pressed: boolean): GamepadRaw {
  const mask = gamepadButtonMask(keyCode);
  if (mask === 0) {
    return raw;
  }
  const buttons = pressed ? (raw.buttons | mask) : (raw.buttons & ~mask);
  if (buttons === raw.buttons) {
    return raw;
  }
  return { buttons: buttons, lt: raw.lt, rt: raw.rt, lx: raw.lx, ly: raw.ly,
    rx: raw.rx, rz: raw.rz };
}

/** AxisModel codes used by the mapping. */
const AXIS_X: number = 0;
const AXIS_Y: number = 1;
const AXIS_Z: number = 2;
const AXIS_RZ: number = 3;
const AXIS_GAS: number = 4;
const AXIS_BRAKE: number = 5;

/**
 * Returns the state with one axis updated. Stick axes are stored raw (-1..1);
 * the deadzone and the +-32767 conversion happen in gamepadOutOf(). Ignored axes
 * (e.g. ABS_HAT0X/Y) return the same object.
 */
export function applyGamepadAxis(raw: GamepadRaw, axis: number, value: number): GamepadRaw {
  if (axis === AXIS_X) {
    return { buttons: raw.buttons, lt: raw.lt, rt: raw.rt, lx: value, ly: raw.ly,
      rx: raw.rx, rz: raw.rz };
  }
  if (axis === AXIS_Y) {
    return { buttons: raw.buttons, lt: raw.lt, rt: raw.rt, lx: raw.lx, ly: value,
      rx: raw.rx, rz: raw.rz };
  }
  if (axis === AXIS_Z) {
    return { buttons: raw.buttons, lt: raw.lt, rt: raw.rt, lx: raw.lx, ly: raw.ly,
      rx: value, rz: raw.rz };
  }
  if (axis === AXIS_RZ) {
    return { buttons: raw.buttons, lt: raw.lt, rt: raw.rt, lx: raw.lx, ly: raw.ly,
      rx: raw.rx, rz: value };
  }
  if (axis === AXIS_GAS) {
    return { buttons: raw.buttons, lt: raw.lt, rt: clampUnit(value), lx: raw.lx,
      ly: raw.ly, rx: raw.rx, rz: raw.rz };
  }
  if (axis === AXIS_BRAKE) {
    return { buttons: raw.buttons, lt: clampUnit(value), rt: raw.rt, lx: raw.lx,
      ly: raw.ly, rx: raw.rx, rz: raw.rz };
  }
  return raw;
}

/** Clamps a trigger value to 0..1. */
export function clampUnit(value: number): number {
  if (value < 0) {
    return 0;
  }
  if (value > 1) {
    return 1;
  }
  return value;
}

/**
 * Radial deadzone + rescale: a magnitude at or below the deadzone becomes
 * exactly 0; a larger magnitude is stretched linearly from [dz, 1] to [0, 1]
 * and converted to +-32767 while keeping the direction.
 *
 * `invertY` flips the Y sign AFTER the deadzone: Moonlight's stick Y is positive
 * up, while the HarmonyOS ABS_Y / ABS_RZ axes report up as negative (measured:
 * upper-left = -0.78), so the gamepad path always inverts Y.
 */
export function stickToInt16(x: number, y: number, deadzone: number = GAMEPAD_DEADZONE,
  invertY: boolean = false): StickPair {
  const magnitude = Math.sqrt(x * x + y * y);
  if (magnitude <= deadzone || magnitude === 0) {
    return { x: 0, y: 0 };
  }
  const scaled = Math.min(1, (magnitude - deadzone) / (1 - deadzone));
  const gain = scaled / magnitude;
  const signedY = invertY ? -y : y;
  return {
    x: Math.round(x * gain * 32767),
    y: Math.round(signedY * gain * 32767)
  };
}

/** Trigger 0..1 -> 0..255 (clamped, rounded). */
export function triggerTo255(value: number): number {
  return Math.round(clampUnit(value) * 255);
}

/** The exact payload sendGamepadState() expects. */
export function gamepadOutOf(raw: GamepadRaw): GamepadOut {
  // Moonlight wants +Y up; the HarmonyOS stick axes report up as -Y, so both
  // sticks invert Y (X is unchanged).
  const left = stickToInt16(raw.lx, raw.ly, GAMEPAD_DEADZONE, true);
  const right = stickToInt16(raw.rx, raw.rz, GAMEPAD_DEADZONE, true);
  return {
    buttons: raw.buttons,
    lt: triggerTo255(raw.lt),
    rt: triggerTo255(raw.rt),
    lx: left.x,
    ly: left.y,
    rx: right.x,
    ry: right.y
  };
}

/** True when two snapshots are identical (the send dedupe). */
export function sameGamepadOut(a: GamepadOut, b: GamepadOut): boolean {
  return a.buttons === b.buttons && a.lt === b.lt && a.rt === b.rt &&
    a.lx === b.lx && a.ly === b.ly && a.rx === b.rx && a.ry === b.ry;
}
