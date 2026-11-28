/**
 * Sunshine virtual-display mode selection (pure logic, no SDK imports).
 *
 * The Sunshine host exposes a virtual display whose supported modes are the
 * table below. A Sunshine session is restarted by the native backend whenever
 * the window asks for a new size (requestDesktopSize), so the ArkTS side must
 * pick a mode the host actually has instead of the window's raw pixel size.
 *
 * Rule (product decision): black bars first, scale close to 1:1 second.
 *   1. Landscape is `winW >= winH`; only modes of the same orientation are
 *      considered (a phone in portrait must get a portrait remote desktop).
 *   2. Modes above the 3840x2160 pixel budget are excluded up front.
 *   3. Every remaining candidate is scored once:
 *        scale       = min(winW / modeW, winH / modeH)   (contain fit)
 *        barFraction = 1 - (modeW*scale * modeH*scale) / (winW*winH)
 *        cost        = barFraction + 0.5 * |ln(scale)|
 *      Black bars dominate; the scale term breaks ties toward a desktop that is
 *      neither blown up nor shrunk (an ultrawide 3840x1640 window would prefer
 *      2560x1080 on bars alone — 1.2% vs 2.0% — but that is a 1.5x blow-up, so
 *      3440x1440 wins once the scale term is added). A tie takes the larger mode.
 *
 * Plain TypeScript so the host-side unit test can compile it with the SDK tsc
 * and run it under Node (see app/tests/arkts/connection_codec.test.mjs).
 */

export interface SunshineMode {
  width: number;
  height: number;
}

export interface SunshineModeGroup {
  /** Ratio label; only used for readability / diagnostics. */
  id: string;
  modes: SunshineMode[];
}

/**
 * The host virtual display's mode table (same list as the Sunshine host's
 * display modes). Order only breaks exact ties.
 */
export const SUNSHINE_MODE_GROUPS: SunshineModeGroup[] = [
  {
    id: '16:9',
    modes: [
      { width: 1280, height: 720 }, { width: 1366, height: 768 },
      { width: 1600, height: 900 }, { width: 1920, height: 1080 },
      { width: 2560, height: 1440 }, { width: 3840, height: 2160 },
      { width: 5120, height: 2880 }
    ]
  },
  {
    id: '16:10',
    modes: [
      { width: 1280, height: 800 }, { width: 1440, height: 900 },
      { width: 1680, height: 1050 }, { width: 1920, height: 1200 },
      { width: 2560, height: 1600 }, { width: 3840, height: 2400 }
    ]
  },
  {
    id: '3:2',
    modes: [
      { width: 1440, height: 960 }, { width: 1500, height: 1000 },
      { width: 1800, height: 1200 }, { width: 1920, height: 1280 },
      { width: 2160, height: 1440 }, { width: 2250, height: 1500 },
      { width: 2400, height: 1600 }, { width: 3000, height: 2000 }
    ]
  },
  {
    id: '4:3',
    modes: [
      { width: 1024, height: 768 }, { width: 1280, height: 960 },
      { width: 1400, height: 1050 }, { width: 1600, height: 1200 },
      { width: 1920, height: 1440 }, { width: 2048, height: 1536 },
      { width: 4096, height: 3072 }
    ]
  },
  { id: '5:4', modes: [{ width: 1280, height: 1024 }] },
  {
    id: '21:9',
    modes: [
      { width: 2560, height: 1080 }, { width: 3440, height: 1440 },
      { width: 5120, height: 2160 }
    ]
  },
  {
    id: '32:9',
    modes: [{ width: 3840, height: 1080 }, { width: 5120, height: 1440 }]
  },
  {
    id: '9:16',
    modes: [
      { width: 720, height: 1280 }, { width: 768, height: 1366 },
      { width: 900, height: 1600 }, { width: 1080, height: 1920 },
      { width: 1440, height: 2560 }, { width: 2160, height: 3840 }
    ]
  },
  {
    id: '10:16',
    modes: [
      { width: 800, height: 1280 }, { width: 900, height: 1440 },
      { width: 1050, height: 1680 }, { width: 1200, height: 1920 },
      { width: 1600, height: 2560 }, { width: 2400, height: 3840 }
    ]
  },
  {
    id: '2:3',
    modes: [
      { width: 960, height: 1440 }, { width: 1000, height: 1500 },
      { width: 1200, height: 1800 }, { width: 1280, height: 1920 },
      { width: 1440, height: 2160 }, { width: 1500, height: 2250 },
      { width: 1600, height: 2400 }, { width: 2000, height: 3000 }
    ]
  },
  {
    id: '3:4',
    modes: [
      { width: 768, height: 1024 }, { width: 960, height: 1280 },
      { width: 1050, height: 1400 }, { width: 1200, height: 1600 },
      { width: 1440, height: 1920 }, { width: 1536, height: 2048 }
    ]
  },
  {
    id: 'phone',
    modes: [
      { width: 1080, height: 2280 }, { width: 1080, height: 2340 },
      { width: 1080, height: 2400 }, { width: 1260, height: 2720 },
      { width: 1260, height: 2800 }, { width: 1440, height: 3120 }
    ]
  },
  {
    id: 'tablet',
    modes: [
      { width: 2160, height: 1620 }, { width: 2360, height: 1640 },
      { width: 2732, height: 2048 }
    ]
  },
  {
    id: 'foldable',
    modes: [
      { width: 2208, height: 1840 }, { width: 1840, height: 2208 },
      { width: 2224, height: 2496 }, { width: 2496, height: 2224 },
      { width: 2160, height: 2160 }
    ]
  }
];

/** Pixels the host can reasonably encode for this client (3840x2160). */
export const SUNSHINE_MODE_PIXEL_CAP: number = 3840 * 2160;
/** Weight of the |ln(scale)| term in the score (see selectSunshineMode). */
export const SUNSHINE_SCALE_WEIGHT: number = 0.5;
/** Used when the window size is unknown/invalid. */
export const SUNSHINE_MODE_FALLBACK: SunshineMode = { width: 1920, height: 1080 };

function isLandscapeMode(mode: SunshineMode): boolean {
  return mode.width >= mode.height;
}

function modePixels(mode: SunshineMode): number {
  return mode.width * mode.height;
}

/**
 * Picks the host mode with the smallest black-bar loss for a window of
 * winW x winH (physical px), with a mild penalty for a desktop that has to be
 * blown up or shrunk:
 *   cost = barFraction + 0.5 * |ln(scale)|
 * (see the file header for the exact definitions). Modes above the 3840x2160
 * pixel budget are excluded before scoring; a tie takes the larger mode.
 *
 * Returns 1920x1080 for a non-positive / non-finite window.
 */
export function selectSunshineMode(winW: number, winH: number): SunshineMode {
  if (!Number.isFinite(winW) || !Number.isFinite(winH) || winW <= 0 || winH <= 0) {
    return { width: SUNSHINE_MODE_FALLBACK.width, height: SUNSHINE_MODE_FALLBACK.height };
  }
  const landscape = winW >= winH;
  const winArea = winW * winH;
  let chosen: SunshineMode | undefined = undefined;
  let bestCost = Number.POSITIVE_INFINITY;
  for (let i = 0; i < SUNSHINE_MODE_GROUPS.length; i++) {
    const group = SUNSHINE_MODE_GROUPS[i];
    for (let j = 0; j < group.modes.length; j++) {
      const mode = group.modes[j];
      if (isLandscapeMode(mode) !== landscape) {
        continue;
      }
      // Pixel budget: an over-budget mode is never a candidate.
      if (modePixels(mode) > SUNSHINE_MODE_PIXEL_CAP) {
        continue;
      }
      const scale = Math.min(winW / mode.width, winH / mode.height);
      if (!Number.isFinite(scale) || scale <= 0) {
        continue;
      }
      const shownArea = mode.width * scale * mode.height * scale;
      const barFraction = Math.max(0, 1 - shownArea / winArea);
      const cost = barFraction + SUNSHINE_SCALE_WEIGHT * Math.abs(Math.log(scale));
      if (chosen === undefined || cost < bestCost ||
          (cost === bestCost && modePixels(mode) > modePixels(chosen))) {
        chosen = mode;
        bestCost = cost;
      }
    }
  }
  if (chosen === undefined) {
    return { width: SUNSHINE_MODE_FALLBACK.width, height: SUNSHINE_MODE_FALLBACK.height };
  }
  return { width: chosen.width, height: chosen.height };
}
