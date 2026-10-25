// SPDX-License-Identifier: GPL-3.0-or-later
//
// FreeRDP 3.9.0 RDP client backend.
//
// The backend owns the whole attempt: create a FreeRDP client context, connect
// (TCP / TLS / CredSSP-NLA / activation), drive the event loop, translate
// update callbacks into the session's framebuffer and cursor mirror, translate
// queued ArkTS input into RDP input PDUs, and tear the context down again.
//
// It deliberately knows nothing about diagnostics.cxx's SessionState: the
// session thread passes an RdpHost bridge whose callbacks do the writes using
// the same invariants as the VNC path. This keeps the RDP backend free of the
// VNC headers and lets the host build exclude the backend entirely.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace tianguan {

// Numeric values mirror diagnostics.cxx's SessionPhase. They are duplicated
// here so the backend does not have to include the VNC-facing translation unit;
// the adapter in diagnostics.cxx static_asserts the mapping.
enum RdpPhase {
  RdpPhaseConnecting = 0,
  RdpPhaseConnected = 1,
  RdpPhaseDisconnected = 2,
  RdpPhaseFailed = 3,
  RdpPhaseTrustRequired = 4,
  RdpPhaseReconnecting = 5
};

// Everything one connection attempt needs. The caller copies the credentials
// out of the session under its lock and wipes this struct when runRdpSession()
// returns.
struct RdpOptions {
  std::string host;
  int port = 0;
  std::string username;
  std::string domain;
  std::string password;
};

// One rectangle of a decoded framebuffer update. `pixels` points at the first
// pixel of (x, y) in the decoded image; rows are `stride` bytes apart and each
// pixel is BGRA_8888.
struct RdpRect {
  int x = 0;
  int y = 0;
  int w = 0;
  int h = 0;
  int stride = 0;
  const uint8_t* pixels = nullptr;
};

// Input events drained from the shared session queue. The backend tracks the
// pressed-key set and the held-button mask itself so it can release exactly what
// it pressed.
struct RdpInputSink {
  void* ctx = nullptr;
  void (*pointer)(void* ctx, int x, int y, int mask) = nullptr;
  void (*key)(void* ctx, int keysym, int pressed) = nullptr;
  // releaseSessionInput() moved the release epoch: release every key/button
  // this attempt pressed, exactly like the VNC session loop does.
  void (*release)(void* ctx) = nullptr;
  // Local clipboard text copied by ArkTS (setClipboard). The backend keeps it
  // as the text to serve the next cliprdr format-data request and announces it
  // to the server with a CF_UNICODETEXT format list.
  void (*clipboard)(void* ctx, const char* text) = nullptr;
};

// The session-side bridge. Every callback is invoked on the session thread
// (FreeRDP dispatch runs inside freerdp_check_event_handles() there), except
// that stopRequested()/generation() may be consulted by the connect watchdog.
struct RdpHost {
  void* user = nullptr;

  bool (*stopRequested)(void* user) = nullptr;
  int (*generation)(void* user) = nullptr;
  // Same policy as the VNC loop's beginAttempt(): bump the generation when an
  // automatic retry starts an attempt. *lastApplied is owned by the backend.
  int (*beginAttempt)(void* user, int* lastApplied) = nullptr;
  // error/errorCode may be null for "" / "none".
  void (*setPhase)(void* user, int phase, const char* error,
                   const char* errorCode, int retryInMs) = nullptr;
  bool (*sleepUnlessStopped)(void* user, int milliseconds) = nullptr;
  void (*waitForReconnect)(void* user) = nullptr;

  // Rebuild the framebuffer mirror: called once a connection has negotiated its
  // desktop size and again for every DesktopResize. The callbacks below are
  // ignored unless generation still matches and stop is clear.
  void (*resizeFrame)(void* user, int generation, int width, int height) = nullptr;
  // One decoded update transaction: copy all rectangles, then bump the version
  // once and publish the delivery union exactly like framebufferUpdateEnd().
  void (*commitFrame)(void* user, int generation, const RdpRect* rects,
                      size_t count, int width, int height) = nullptr;
  // Cursor shape as RGBA_8888 (stride w*4). w/h <= 0 or pixels == nullptr
  // means "hide the cursor".
  void (*setCursor)(void* user, int generation, int width, int height,
                    int hotX, int hotY, const uint8_t* rgba) = nullptr;
  void (*setCursorBlank)(void* user, int generation) = nullptr;
  void (*setCursorPos)(void* user, int generation, int x, int y) = nullptr;

  // Drop pointer/key input queued for a previous connection; returns the
  // release epoch to start from.
  unsigned (*prepareInput)(void* user) = nullptr;
  // Swap the queued input out and feed it to the sink. *appliedRelease is
  // updated when a release was requested and performed.
  void (*drainInput)(void* user, const RdpInputSink* sink,
                     unsigned* appliedRelease) = nullptr;
  // requestFullFrame(): the next committed update must be treated as covering
  // the whole desktop.
  void (*consumeFullRequest)(void* user) = nullptr;

  // Remote clipboard text received through cliprdr (CF_UNICODETEXT), already
  // converted to UTF-8 and NUL-free. `length` may be 0. Invoked on the session
  // thread; the pointer is only valid for the duration of the call. The adapter
  // re-checks the generation under the clipboard lock before storing it.
  void (*sinkClipboard)(void* user, int generation, const char* text,
                        size_t length) = nullptr;
};

// Blocks until the host asks to stop. While running it performs the same
// connect / connected / reconnect with backoff cycle as the VNC session loop.
void runRdpSession(const RdpHost& host, const RdpOptions& options);

}  // namespace tianguan
