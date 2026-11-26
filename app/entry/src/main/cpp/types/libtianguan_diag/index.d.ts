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
// Drops the session's expected/observed server fingerprint and reconnects
// through the same trigger as reconnectSession(). The next attempt runs with no
// pin, so it stops at trust-required and reports the fingerprint the server
// presents now; the user must confirm again and the UI then saves it and calls
// reconnectSession(). Returns "OK" or "INVALID_SESSION". Never trusts anything.
export const forgetTrustedFingerprint: (sessionId: string) => string;
// RDP only: set the remote CapsLock/NumLock toggle state to the given absolute
// values through the Client Synchronize Event (Sync PDU). The key events
// themselves are not forwarded on RDP. VNC is a no-op. Returns "OK" or
// "INVALID_SESSION"; the session thread performs the send.
export const syncRemoteLockState: (sessionId: string, capsLock: boolean,
  numLock: boolean) => string;
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
// VNC and RDP: ask the server to resize the remote desktop (VNC
// ExtendedDesktopSize / RDP DISPLAY_CONTROL). Synchronous boolean: true when
// the request was accepted and queued for the session thread, false for an
// unknown/disconnected/other-protocol session or an out-of-range size
// (VNC [1, 4096]; RDP [200, 4096]). The server's verdict is
// sessionStatus().resizeSupport.
export const requestDesktopSize: (sessionId: string, width: number,
  height: number) => boolean;
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
