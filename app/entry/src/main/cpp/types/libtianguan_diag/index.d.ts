export const runDiagnostics: () => Promise<string>;
export const runNetworkTest: (filesDir: string) => Promise<string>;

// Multi-session API (docs/napi-contract.md).
export const createSession: (optionsJson: string) => string;
// closeSession() is non-blocking: it detaches the session and returns CLOSED
// while a background reaper joins the thread. pendingCloses() reports how many
// such reapers have not finished yet (0 when everything is torn down).
export const closeSession: (sessionId: string) => string;
export const pendingCloses: () => number;
export const listSessions: () => string;
export function sessionStatus(): string;
export function sessionStatus(sessionId: string): string;
export const reconnectSession: (sessionId: string) => string;
export const takeFrame: (sessionId: string, buffer: ArrayBuffer, afterVersion: number,
  forceFull: boolean) => string;
export function sendPointer(x: number, y: number, mask: number): string;
export function sendPointer(sessionId: string, x: number, y: number, mask: number): string;
export function sendKey(keysym: number, pressed: number): string;
export function sendKey(sessionId: string, keysym: number, pressed: number): string;
export const releaseSessionInput: (sessionId: string) => string;
export const setClipboard: (sessionId: string, text: string) => string;
export const takeClipboard: (sessionId: string) => string;
export function requestFullFrame(): string;
export function requestFullFrame(sessionId: string): string;
export function copyCursor(buffer: ArrayBuffer): string;
export function copyCursor(sessionId: string, buffer: ArrayBuffer): string;

// Legacy single-session API: still exported for pages that have not moved to
// the explicit-session calls yet.
export const startSession: (filesDir: string) => string;
export const stopSession: () => string;
export const copyRegion: (buffer: ArrayBuffer) => string;
export const takeRegion: (buffer: ArrayBuffer) => string;

// Remote-page presenter: the native EGL/GLES render thread bound to one
// session's XComponent(SURFACE). While attached, the renderer is the frame
// consumer — ArkTS must not call takeFrame for the same session. The third
// argument (optional) starts the renderer parked (surface created while the
// window is hidden).
export const attachRenderSurface: (sessionId: string, surfaceId: string,
  startSuspended?: boolean) => string;
export const detachRenderSurface: (sessionId: string) => string;
// JSON stats: state/fps/uploads/skips/surface+texture size/EGL error plus the
// presented geometry (contentLeft/contentTop/contentScale/contentScaleY/
// contentW/contentH/zoom/panX/panY/maxZoom); ArkTS maps input from those
// native values. 'ERROR:NO_RENDERER' when nothing is attached.
export const renderStats: (sessionId: string) => string;
// View transform on top of the contain fit: zoom is clamped to
// [1, maxZoom], panX/panY are surface-pixel offsets from the centred position
// (clamped by the renderer while drawing). Returns 'OK' or an error string.
export const setViewTransform: (sessionId: string, zoom: number, panX: number,
  panY: number) => string;
// true parks the render thread (window hidden), false resumes it.
export const suspendRenderSurface: (sessionId: string, suspended: boolean) => string;
