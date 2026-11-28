/**
 * Bubble radial-menu structure and fan layout (pure logic, no SDK imports).
 *
 * The session page's floating bubble opens a fan of actions. An entry may own a
 * secondary fan (children): tapping it swaps the visible fan for the children at
 * the same anchor instead of running an action. A Sunshine session uses this for
 * the two independent cursor switches, which keeps the main fan at four items.
 *
 * Plain TypeScript so the host-side unit test can compile this file with the SDK
 * tsc and run it under Node.
 */

export interface BubbleMenuEntry {
  /** Action id the page dispatches on. */
  id: string;
  /** Short label (also the hover / long-press hint). */
  label: string;
  /** Optional secondary fan opened by tapping this entry. */
  children?: BubbleMenuEntry[];
}

/**
 * The two cursor switches. 本地光标 is the client's own pointer; 远端光标 is the
 * cursor the HOST draws into the video. There is no 返回 item: tapping the
 * bubble (always back to the main fan) or outside the menu closes the menu. The
 * two items sit at the ends of a 90 deg arc, i.e. 90 deg apart, and the page
 * shows the on/off state by highlighting the button; labels are static.
 */
export function cursorSubmenuEntries(): BubbleMenuEntry[] {
  return [
    { id: 'local-cursor', label: '本地光标' },
    { id: 'remote-cursor', label: '远端光标' }
  ];
}

/**
 * The main fan. VNC/RDP get exactly the previous set (phone: 主列表 + 断开;
 * PC/tablet: 主列表/最小化 + 全屏 + 断开). A Sunshine session inserts 「光标」 (the
 * cursor switches' parent) before 断开, keeping 断开 at the +end of the arc.
 */
export function mainBubbleMenuEntries(pcUi: boolean, phoneUi: boolean,
  sunshine: boolean): BubbleMenuEntry[] {
  const entries: BubbleMenuEntry[] = [];
  if (pcUi) {
    entries.push({ id: 'minimize', label: '最小化' });
  } else {
    entries.push({ id: 'main-list', label: '主列表' });
  }
  if (!phoneUi) {
    entries.push({ id: 'fullscreen', label: '全屏' });
  }
  if (sunshine) {
    entries.push({ id: 'cursor', label: '光标 ▸', children: cursorSubmenuEntries() });
  }
  entries.push({ id: 'disconnect', label: '断开' });
  return entries;
}

/**
 * Evenly spaced fan angles from -span/2 to +span/2. For 2 and 3 buttons this is
 * the historical layout ([±span/2] and [-span/2, 0, +span/2]).
 */
export function bubbleMenuAnglesFor(count: number, centerDeg: number,
  spanDeg: number): number[] {
  const angles: number[] = [];
  if (count <= 0) {
    return angles;
  }
  if (count === 1) {
    angles.push(centerDeg);
    return angles;
  }
  const step = spanDeg / (count - 1);
  for (let i = 0; i < count; i++) {
    angles.push(centerDeg - spanDeg / 2 + step * i);
  }
  return angles;
}

/**
 * Preferred fan span. VNC/RDP keep the historical 110°; a four-item main fan
 * widens to 118° so the larger buttons keep their spacing, and the two-item
 * cursor sub-fan uses 90° so its two switches sit 90° apart (±45°).
 */
export function bubbleMenuSpanFor(count: number, submenu: boolean): number {
  if (submenu) {
    return 90;
  }
  return count >= 4 ? 118 : 110;
}

/** Axis-aligned rectangle in vp (top-left origin), used by the label self-check. */
export interface MenuRect {
  x: number;
  y: number;
  w: number;
  h: number;
}

function clampValue(value: number, low: number, high: number): number {
  if (high < low) {
    return low;
  }
  return value < low ? low : (value > high ? high : value);
}

/** X of a fan button, clamped inside the picture exactly like the page does. */
export function fanButtonX(anchorX: number, radius: number, angleDeg: number,
  safe: number, areaW: number): number {
  return clampValue(anchorX + radius * Math.cos(angleDeg * Math.PI / 180),
    safe, areaW - safe);
}

/** Y of a fan button, clamped inside the picture exactly like the page does. */
export function fanButtonY(anchorY: number, radius: number, angleDeg: number,
  safe: number, areaH: number): number {
  return clampValue(anchorY + radius * Math.sin(angleDeg * Math.PI / 180),
    safe, areaH - safe);
}

/**
 * Capsule label boxes of a sub-fan, one per button, chosen so that no label
 * overlaps ANY button or another label. Candidates, in order:
 *   1. left of the button,
 *   2. right of the button,
 *   3. radially outward from the button (towards the picture edge).
 * Every candidate is clamped into the picture first. The page renders exactly
 * these rectangles, and the unit test asserts the clearance property.
 */
export function submenuLabelRects(anchorX: number, anchorY: number, radius: number,
  angles: number[], buttonVp: number, safeVp: number, labelW: number, labelH: number,
  areaW: number, areaH: number): MenuRect[] {
  const r = buttonVp / 2;
  const gap = 8;
  const bx: number[] = [];
  const by: number[] = [];
  for (let i = 0; i < angles.length; i++) {
    bx.push(fanButtonX(anchorX, radius, angles[i], safeVp, areaW));
    by.push(fanButtonY(anchorY, radius, angles[i], safeVp, areaH));
  }
  const placed: MenuRect[] = [];
  for (let i = 0; i < angles.length; i++) {
    const rad = angles[i] * Math.PI / 180;
    const outward = r + gap + Math.max(labelW, labelH) / 2;
    const candidates: MenuRect[] = [
      { x: bx[i] - r - gap - labelW, y: by[i] - labelH / 2, w: labelW, h: labelH },
      { x: bx[i] + r + gap, y: by[i] - labelH / 2, w: labelW, h: labelH },
      { x: bx[i] + Math.cos(rad) * outward - labelW / 2,
        y: by[i] + Math.sin(rad) * outward - labelH / 2, w: labelW, h: labelH }
    ];
    let chosen: MenuRect | undefined = undefined;
    for (let c = 0; c < candidates.length; c++) {
      const rect = clampRectToArea(candidates[c], areaW, areaH);
      if (rectOverlapsAnyButton(rect, bx, by, r)) {
        continue;
      }
      if (rectOverlapsAny(rect, placed)) {
        continue;
      }
      chosen = rect;
      break;
    }
    if (chosen === undefined) {
      // No clean side (a very small picture): the outward box is the least bad.
      chosen = clampRectToArea(candidates[2], areaW, areaH);
    }
    placed.push(chosen);
  }
  return placed;
}

function clampRectToArea(rect: MenuRect, areaW: number, areaH: number): MenuRect {
  return {
    x: clampValue(rect.x, 2, Math.max(2, areaW - 2 - rect.w)),
    y: clampValue(rect.y, 2, Math.max(2, areaH - 2 - rect.h)),
    w: rect.w,
    h: rect.h
  };
}

function rectIntersects(a: MenuRect, b: MenuRect,
  radius: number): boolean {
  return a.x < b.x + b.w + radius && a.x + a.w > b.x - radius &&
    a.y < b.y + b.h + radius && a.y + a.h > b.y - radius;
}

function rectOverlapsAnyButton(rect: MenuRect, bx: number[], by: number[],
  radius: number): boolean {
  for (let j = 0; j < bx.length; j++) {
    const button: MenuRect = { x: bx[j] - radius, y: by[j] - radius,
      w: radius * 2, h: radius * 2 };
    if (rectIntersects(rect, button, 0)) {
      return true;
    }
  }
  return false;
}

function rectOverlapsAny(rect: MenuRect, placed: MenuRect[]): boolean {
  for (let i = 0; i < placed.length; i++) {
    if (rectIntersects(rect, placed[i], 0)) {
      return true;
    }
  }
  return false;
}

/**
 * Self-check used by the unit test: with the exact page geometry (same clamp,
 * same radius/angles/sizes) no sub-fan label rectangle may intersect ANY button
 * rectangle (the button's bounding square, which is stricter than its circle)
 * nor another label.
 */
export function labelsClearOfButtons(anchorX: number, anchorY: number, radius: number,
  angles: number[], buttonVp: number, safeVp: number, labelW: number, labelH: number,
  areaW: number, areaH: number): boolean {
  const r = buttonVp / 2;
  const rects = submenuLabelRects(anchorX, anchorY, radius, angles, buttonVp, safeVp,
    labelW, labelH, areaW, areaH);
  const bx: number[] = [];
  const by: number[] = [];
  for (let i = 0; i < angles.length; i++) {
    bx.push(fanButtonX(anchorX, radius, angles[i], safeVp, areaW));
    by.push(fanButtonY(anchorY, radius, angles[i], safeVp, areaH));
  }
  for (let i = 0; i < rects.length; i++) {
    if (rectOverlapsAnyButton(rects[i], bx, by, r)) {
      return false;
    }
    for (let j = i + 1; j < rects.length; j++) {
      if (rectIntersects(rects[i], rects[j], 0)) {
        return false;
      }
    }
  }
  return true;
}
