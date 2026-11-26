// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <string>
#include <cstddef>
namespace tianguan {
std::string runDiagnostics();
std::string runNetworkTest(const std::string& dataDir);

// Multi-session API (docs/napi-contract.md). Every runtime call takes the
// sessionId returned by createSession().
std::string createSession(const std::string& optionsJson);
// Non-blocking close (F7): removes the session from the registry, sets stop,
// shuts the socket down (waking a blocked read) and hands the thread join to a
// detached reaper thread. Returns "CLOSED" immediately; it never waits for the
// session thread, whose getaddrinfo() cannot be cancelled. "INVALID_SESSION"
// when the id is not (or no longer) in the registry.
std::string closeSession(const std::string& sessionId);
// Number of closed sessions whose thread has not been reaped yet: the quit
// flow polls this until 0 or a deadline and then terminates. The value is a
// snapshot; it never blocks and may stay > 0 for as long as getaddrinfo() runs.
// A process that exits with reapers still running is safe: a reaper only owns
// its session object and a process-lifetime counter, so process teardown simply
// kills it.
size_t pendingCloses();
std::string listSessions();
std::string sessionStatus(const std::string& sessionId);
std::string reconnectSession(const std::string& sessionId);
// Drops this session's expected/observed fingerprint and reconnects through the
// same generation-bumping trigger as reconnectSession(): the next attempt runs
// with no pin, stops at state=trust-required and reports the fingerprint the
// server presents now. Allowed in any phase; returns "OK" or "INVALID_SESSION".
// It trusts nothing and never touches the ArkTS trust store: the user still has
// to confirm, and the UI still saves the pin and calls reconnectSession().
std::string forgetTrustedFingerprint(const std::string& sessionId);
// Writes the pending region or a full BGRA_8888 frame into dst. Returns JSON
// metadata, "NONE" when there is nothing newer than afterVersion, or
// "ERROR:BUFFER_TOO_SMALL" without touching dst.
// afterVersion is the version of the last successful handoff to this consumer.
// A partial region is only safe when it matches: it is then the union of all
// changes committed since, cut out of the full mirror. Any other value (missed
// handoff, reconnect, resize, first call) makes this return the whole mirror.
std::string takeFrame(const std::string& sessionId, void* dst, size_t capacity,
                      int afterVersion, bool forceFull);

// Struct form of takeFrame() for in-process consumers (the native GL
// renderer): identical lock, delivery semantics and baseline, but no JSON.
// The status tells the caller which takeFrame() string the answer would be.
struct FrameGrab {
  bool delivered = false;  // pixels were copied into dst
  bool full = false;       // the copy covers the whole mirror
  int x = 0, y = 0, w = 0, h = 0;  // rectangle copied into dst (packed rows)
  int version = 0;         // mirror version after this grab
  int generation = 0;      // connection that produced that version
  int fbWidth = 0, fbHeight = 0;   // current mirror geometry
};
enum FrameGrabStatus {
  FrameGrabNone,            // "NONE"
  FrameGrabDelivered,       // metadata above is valid
  FrameGrabInvalidSession,  // "INVALID_SESSION"
  FrameGrabTooSmall         // "ERROR:BUFFER_TOO_SMALL"
};
// Optional per-call timings for a timing consumer (the native renderer's
// per-second summary). Filled only when the out pointer is non-null; the
// measurement adds two clock reads and no synchronization.
struct FrameGrabTiming {
  long long lockWaitUs = 0;  // waiting for the pix mutex
  long long copyUs = 0;      // copying the rectangle into dst (0 on no copy)
};
FrameGrabStatus takeFrameStruct(const std::string& sessionId, void* dst,
                                size_t capacity, int afterVersion, bool forceFull,
                                FrameGrab& out, FrameGrabTiming* timing = nullptr);
// Copies that session's current cursor image (RGBA_8888) into dst and returns
// the number of bytes written; 0 when the session has no cursor image.
size_t copyCursorFor(const std::string& sessionId, void* dst, size_t capacity);
std::string sendPointer(const std::string& sessionId, int x, int y, int mask);
std::string sendKey(const std::string& sessionId, int keysym, int pressed);
// RDP only: set the remote CapsLock/NumLock state to the given absolute values
// with the RDP Client Synchronize Event (Sync PDU). The page calls this when its
// local lock state changes; the CapsLock/NumLock key events themselves are not
// forwarded. A newer call replaces a pending older one; the session thread
// sends it, never the caller. VNC is a no-op. Returns "OK", or
// "INVALID_SESSION" for an unknown session.
std::string syncRemoteLockState(const std::string& sessionId, bool capsLock,
                                bool numLock);
// Releases the buttons/keys this session pressed and drops queued pointer/key
// input. Queued clipboard text is deliberately KEPT: it was already reported to
// the caller as delivered and is unrelated to releasing input, so dropping it
// would lose that text without any retry.
std::string releaseSessionInput(const std::string& sessionId);
// True when a queued input with this kind survives releaseSessionInput().
// kind matches the native input queue (0 = pointer, 1 = key, 2 = clipboard).
// Inline so the host self-check can assert the retained-kind policy without a
// live session.
inline bool keepQueuedInputOnRelease(int kind) {
  return kind == 2;
}
std::string setClipboard(const std::string& sessionId, const std::string& text);
std::string takeClipboard(const std::string& sessionId);
std::string requestFullFrame(const std::string& sessionId);
// VNC and RDP: queues a client-initiated resize request asking the server to
// change the framebuffer to width x height. VNC uses ExtendedDesktopSize,
// RDP uses the DISPLAY_CONTROL (MS-RDPEDISP) monitor-layout PDU. The write
// itself happens on the session thread (socket ownership); at most one request
// is in flight, a newer call replaces a size that has not been sent yet.
// Returns false when the session does not exist, is not connected, is not VNC
// or RDP, or the size is out of range: [1, 4096] for VNC, [200, 4096] for RDP
// (DISPLAY_CONTROL's own minimum is 200). Acceptance is reported by the status
// JSON field resizeSupport ("unknown" / "ok" / "rejected"; RDP has no reject
// answer and stays "unknown" when the request is ignored).
bool requestDesktopSize(const std::string& sessionId, int width, int height);

// Legacy single-session wrappers. They resolve the "default" session that
// startSession() creates, so the existing ArkTS pages keep compiling until they
// move to the explicit-session API above.
std::string startSession(const std::string& dataDir);
std::string stopSession();
std::string sessionStatus();
std::string sendPointer(int x, int y, int mask, bool flush = false);
std::string sendKey(int keysym, int pressed, bool flush = false);
std::string requestFullFrame();
// Copy the last changed rectangle (BGRA8888) or the cursor image (RGBA8888)
// into the caller's buffer; returns the number of bytes written.
size_t copyRegion(void* dst, size_t capacity);
size_t copyCursor(void* dst, size_t capacity);
// Copy the pending changed region together with its rectangle in one step.
// Returns "x,y,w,h", or "NONE" when there is nothing new to draw.
std::string takeRegion(void* dst, size_t capacity);
}
