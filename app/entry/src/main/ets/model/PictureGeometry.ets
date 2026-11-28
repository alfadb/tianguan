/**
 * Presented-picture geometry for the remote page: the contain fit of the remote
 * framebuffer inside the XComponent surface.
 *
 * This is the ONE source of truth for both input mapping (sendPointer) and the
 * local-cursor visibility rule: a point is "on the picture" exactly when this
 * rectangle contains it, so the black bars (letterbox / pillarbox) never have a
 * remote pointer and must not have the system pointer hidden either.
 *
 * In GL mode the page replaces `left/top/width/height` with the presenter's
 * renderStats() values (zoom/pan included) and keeps the same frame size; the
 * mapping and the hit test therefore always use what was actually drawn.
 *
 * Plain TypeScript (no SDK imports) so the host-side unit test can compile this
 * file with the SDK tsc and run it under Node.
 */

export interface PictureGeometry {
  /** Drawn rectangle in vp, top-left origin. */
  left: number;
  top: number;
  width: number;
  height: number;
  /** Remote framebuffer size (pixels) this rectangle presents. */
  frameWidth: number;
  frameHeight: number;
}

function isPositiveFinite(value: number): boolean {
  return typeof value === 'number' && Number.isFinite(value) && value > 0;
}

/**
 * Contain fit of a frameWidth x frameHeight framebuffer inside a
 * viewWidth x viewHeight area, or undefined when any size is unknown (0 / NaN).
 * Mirrors the historical layout used for the Canvas path.
 */
export function containGeometry(frameWidth: number, frameHeight: number,
  viewWidth: number, viewHeight: number): PictureGeometry | undefined {
  if (!isPositiveFinite(frameWidth) || !isPositiveFinite(frameHeight) ||
      !isPositiveFinite(viewWidth) || !isPositiveFinite(viewHeight)) {
    return undefined;
  }
  const scale = Math.min(viewWidth / frameWidth, viewHeight / frameHeight);
  if (!isPositiveFinite(scale)) {
    return undefined;
  }
  const width = frameWidth * scale;
  const height = frameHeight * scale;
  return {
    left: (viewWidth - width) / 2,
    top: (viewHeight - height) / 2,
    width: width,
    height: height,
    frameWidth: frameWidth,
    frameHeight: frameHeight
  };
}

/**
 * True when (x, y) is inside the drawn picture. False for a degenerate
 * rectangle (nothing connected / no frame yet), so the caller keeps the normal
 * system pointer in that case.
 */
export function pointInPicture(x: number, y: number,
  geometry: PictureGeometry | undefined): boolean {
  if (geometry === undefined || !Number.isFinite(x) || !Number.isFinite(y)) {
    return false;
  }
  if (!isPositiveFinite(geometry.width) || !isPositiveFinite(geometry.height)) {
    return false;
  }
  return x >= geometry.left && y >= geometry.top &&
    x <= geometry.left + geometry.width && y <= geometry.top + geometry.height;
}

/**
 * Remote framebuffer point for a local (x, y), or [-1, -1] when the point is on
 * a black bar / outside the picture. This is what sendPointer() uses.
 */
export function remotePointInPicture(x: number, y: number,
  geometry: PictureGeometry): number[] {
  if (!pointInPicture(x, y, geometry)) {
    return [-1, -1];
  }
  const remoteX = Math.round((x - geometry.left) * geometry.frameWidth / geometry.width);
  const remoteY = Math.round((y - geometry.top) * geometry.frameHeight / geometry.height);
  return [
    Math.max(0, Math.min(geometry.frameWidth - 1, remoteX)),
    Math.max(0, Math.min(geometry.frameHeight - 1, remoteY))
  ];
}

/**
 * System-pointer visibility rule of the remote page:
 *   - local-cursor mode (Sunshine): the host hides its cursor and the system
 *     pointer IS the cursor, so it must stay visible everywhere;
 *   - otherwise hide the system pointer only while it is on the picture, so the
 *     black bars keep the normal pointer.
 */
export function hidesSystemPointer(onPicture: boolean, localCursorMode: boolean): boolean {
  return onPicture && !localCursorMode;
}
