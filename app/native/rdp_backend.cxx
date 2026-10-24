// SPDX-License-Identifier: GPL-3.0-or-later
//
// FreeRDP 3.9.0 client backend for the RDP sessions of libtianguan_diag.
//
// This file is only compiled into the OHOS target (TIANGUAN_WITH_RDP): the
// host build cannot link the statically built FreeRDP/OpenSSL prefix because
// the host OpenSSL is a shared system library.
//
// Layout of one attempt (same shape as the VNC session loop in diagnostics.cxx):
//
//   beginAttempt() -> connecting -> [connect: TCP, TLS, NLA, activation]
//     failure: auth -> failed/auth-failed, protocol -> failed/protocol,
//              transport -> reconnecting/network with 1s..4s backoff
//     success: connected (PostConnect) -> 10 ms event loop -> connection lost
//              -> reconnecting/network with backoff, or reconnect on a new
//              generation, or clean exit when stop is set.
//
// Every callback runs on the session thread. Only the session thread writes
// frames/cursor through the RdpHost bridge, and every write is gated on
// stop/generation so a superseded attempt can never touch the current mirror.
#include "rdp_backend.h"

#include <freerdp/client.h>
#include <freerdp/error.h>
#include <freerdp/freerdp.h>
#include <freerdp/gdi/gdi.h>
#include <freerdp/graphics.h>
#include <freerdp/input.h>
#include <freerdp/pointer.h>
#include <freerdp/scancode.h>
#include <freerdp/settings.h>
#include <freerdp/update.h>
#include <winpr/crt.h>
#include <winpr/input.h>
#include <winpr/synch.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <new>
#include <string>
#include <thread>
#include <vector>

namespace tianguan {
namespace {

// Requested desktop size. The server may negotiate something else; the mirror
// follows the negotiated size reported by PostConnect/DesktopResize.
const int kRequestedWidth = 1920;
const int kRequestedHeight = 1200;
// The VNC path already refuses framebuffers above 4096x4096; keep the RDP
// mirror under the same bound instead of trusting a server's dimensions.
const int kMaxDesktopSide = 4096;
// Stop/generation changes are observed at this granularity while connected.
const int kLoopWaitMs = 10;
const int kConnectWatchdogMs = 10;
const int kInitialBackoffMs = 1000;
const int kMaxBackoffMs = 4000;
const size_t kMaxErrorChars = 240;
const int kMaxCursorSide = 1024;

// One connection attempt. The head must be the FreeRDP client context: every
// FreeRDP callback receives an rdpContext* that points at this struct (the
// client-common entry-point contract, same pattern as .work/freerdp-spike).
struct RdpAttemptContext {
  rdpClientContext common;
  const RdpHost* host;
  int generation;
  bool gdiReady;
  bool sawLogonErrorInfo;
};

RdpAttemptContext* attemptOf(rdpContext* context) {
  return reinterpret_cast<RdpAttemptContext*>(context);
}

bool hostStopped(const RdpAttemptContext* attempt) {
  return attempt->host->stopRequested(attempt->host->user);
}

// True when this attempt must no longer write into the session (closed, or a
// manual reconnect bumped the generation). Frame/cursor callbacks check this
// after taking the pixel lock, right before touching the mirror.
bool hostStale(const RdpAttemptContext* attempt) {
  return hostStopped(attempt) ||
         attempt->host->generation(attempt->host->user) != attempt->generation;
}

bool validDesktop(UINT32 width, UINT32 height) {
  return width > 0 && height > 0 && width <= static_cast<UINT32>(kMaxDesktopSide) &&
         height <= static_cast<UINT32>(kMaxDesktopSide);
}

// ---------------------------------------------------------------------------
// Error classification (the UI reuses its VNC errorCode semantics)
// ---------------------------------------------------------------------------

bool containsCi(const char* haystack, const char* needle) {
  if (haystack == nullptr || needle == nullptr || needle[0] == '\0') return false;
  const size_t needleLength = std::strlen(needle);
  for (size_t i = 0; haystack[i] != '\0'; ++i) {
    size_t j = 0;
    for (; j < needleLength; ++j) {
      char a = haystack[i + j];
      if (a == '\0') break;
      char b = needle[j];
      if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
      if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
      if (a != b) break;
    }
    if (j == needleLength) return true;
  }
  return false;
}

std::string singleLine(const char* text) {
  std::string out;
  if (text == nullptr) return out;
  for (const char* p = text; *p != '\0'; ++p) {
    const unsigned char c = static_cast<unsigned char>(*p);
    if (c == '\r' || c == '\n' || c == '\t') {
      out.push_back(' ');
    } else if (c >= 0x20 && c != 0x7f) {
      out.push_back(static_cast<char>(c));
    }
  }
  return out;
}

void replaceAll(std::string& text, const std::string& needle) {
  if (needle.empty()) return;
  const std::string replacement = "[redacted]";
  size_t at = 0;
  while ((at = text.find(needle, at)) != std::string::npos) {
    text.replace(at, needle.size(), replacement);
    at += replacement.size();
  }
}

// FreeRDP's static error strings never carry credentials, but an error path is
// exactly where a future upstream change could start embedding them. Redact
// every non-empty credential anyway so nothing private can reach statusJson or
// a log line.
std::string connectErrorText(UINT32 code, const RdpOptions& options) {
  const char* name = freerdp_get_last_error_name(code);
  const char* text = freerdp_get_last_error_string(code);
  std::string out;
  if (name != nullptr) out = singleLine(name);
  if (text != nullptr) {
    const std::string line = singleLine(text);
    if (!line.empty()) {
      if (!out.empty()) out += ": ";
      out += line;
    }
  }
  if (out.empty()) out = "RDP connection failed";
  replaceAll(out, options.password);
  replaceAll(out, options.username);
  replaceAll(out, options.domain);
  if (out.size() > kMaxErrorChars) {
    out.resize(kMaxErrorChars);
    out += "...";
  }
  return out;
}

enum RdpFailureClass {
  RdpFailureAuth,
  RdpFailureNetwork,
  RdpFailureProtocol,
};

RdpFailureClass classifyConnectFailure(UINT32 code, bool sawLogonErrorInfo) {
  switch (code) {
    // Credentials: the UI's existing auth-failed flow (password retry) applies.
    case FREERDP_ERROR_AUTHENTICATION_FAILED:
    case FREERDP_ERROR_CONNECT_PASSWORD_EXPIRED:
    case FREERDP_ERROR_CONNECT_PASSWORD_MUST_CHANGE:
    case FREERDP_ERROR_CONNECT_PASSWORD_CERTAINLY_EXPIRED:
    case FREERDP_ERROR_CONNECT_ACCOUNT_DISABLED:
    case FREERDP_ERROR_CONNECT_CLIENT_REVOKED:
    case FREERDP_ERROR_INSUFFICIENT_PRIVILEGES:
    case FREERDP_ERROR_CONNECT_KDC_UNREACHABLE:
    case FREERDP_ERROR_CONNECT_LOGON_FAILURE:
    case FREERDP_ERROR_CONNECT_WRONG_PASSWORD:
    case FREERDP_ERROR_CONNECT_ACCESS_DENIED:
    case FREERDP_ERROR_CONNECT_ACCOUNT_RESTRICTION:
    case FREERDP_ERROR_CONNECT_ACCOUNT_LOCKED_OUT:
    case FREERDP_ERROR_CONNECT_ACCOUNT_EXPIRED:
    case FREERDP_ERROR_CONNECT_LOGON_TYPE_NOT_GRANTED:
    case FREERDP_ERROR_CONNECT_NO_OR_MISSING_CREDENTIALS:
      return RdpFailureAuth;
    default:
      break;
  }
  // The server reported a logon error detail; that only happens after the
  // authentication exchange, whatever code the surrounding failure carries.
  if (sawLogonErrorInfo) return RdpFailureAuth;
  switch (code) {
    // Transport: same treatment as the VNC path's "reconnecting/network".
    case FREERDP_ERROR_DNS_ERROR:
    case FREERDP_ERROR_DNS_NAME_NOT_FOUND:
    case FREERDP_ERROR_CONNECT_FAILED:
    case FREERDP_ERROR_CONNECT_TRANSPORT_FAILED:
    case FREERDP_ERROR_CONNECT_CANCELLED:
    case FREERDP_ERROR_CONNECT_ACTIVATION_TIMEOUT:
      return RdpFailureNetwork;
    default:
      break;
  }
  switch (code) {
    // TLS negotiation / pre-connect / protocol: terminal until the user
    // reconnects, because retrying the same settings cannot fix it.
    case FREERDP_ERROR_PRE_CONNECT_FAILED:
    case FREERDP_ERROR_POST_CONNECT_FAILED:
    case FREERDP_ERROR_MCS_CONNECT_INITIAL_ERROR:
    case FREERDP_ERROR_TLS_CONNECT_FAILED:
    case FREERDP_ERROR_SECURITY_NEGO_CONNECT_FAILED:
    case FREERDP_ERROR_CONNECT_UNDEFINED:
      return RdpFailureProtocol;
    default:
      break;
  }
  if (GET_FREERDP_ERROR_CLASS(code) == FREERDP_ERROR_ERRINFO_CLASS) {
    switch (code) {
      case FREERDP_ERROR_SERVER_INSUFFICIENT_PRIVILEGES:
      case FREERDP_ERROR_SERVER_FRESH_CREDENTIALS_REQUIRED:
        return RdpFailureAuth;
      default:
        return RdpFailureProtocol;
    }
  }
  // Secondary signal used by the spike probe as well: some NLA paths surface
  // only the error name/string.
  const char* name = freerdp_get_last_error_name(code);
  const char* text = freerdp_get_last_error_string(code);
  if (containsCi(name, "LOGON") || containsCi(name, "AUTH") ||
      containsCi(name, "PASSWORD") || containsCi(name, "CREDENTIAL") ||
      containsCi(name, "ACCOUNT") || containsCi(text, "logon") ||
      containsCi(text, "authentication") || containsCi(text, "password") ||
      containsCi(text, "credential"))
    return RdpFailureAuth;
  // No error at all means the failure happened before FreeRDP classified it
  // (TCP refused/reset/timeout): retry with backoff.
  if (code == FREERDP_ERROR_SUCCESS || code == FREERDP_ERROR_NONE)
    return RdpFailureNetwork;
  return RdpFailureProtocol;
}

// ---------------------------------------------------------------------------
// Certificate handling
// ---------------------------------------------------------------------------

// v1 known limitation: the RDP server certificate is accepted for this session
// only and is NOT pinned, so the VNC-style trust-on-first-use store does not
// apply here yet. Returning 2 = "accept for this session, do not store".
DWORD verifyCertificate(freerdp* instance, const char* host, UINT16 port,
                        const char* commonName, const char* subject,
                        const char* issuer, const char* fingerprint, DWORD flags) {
  (void)instance;
  (void)host;
  (void)port;
  (void)commonName;
  (void)subject;
  (void)issuer;
  (void)fingerprint;
  (void)flags;
  return 2;
}

DWORD verifyChangedCertificate(freerdp* instance, const char* host, UINT16 port,
                               const char* commonName, const char* subject,
                               const char* issuer, const char* newFingerprint,
                               const char* oldSubject, const char* oldIssuer,
                               const char* oldFingerprint, DWORD flags) {
  (void)instance;
  (void)host;
  (void)port;
  (void)commonName;
  (void)subject;
  (void)issuer;
  (void)newFingerprint;
  (void)oldSubject;
  (void)oldIssuer;
  (void)oldFingerprint;
  (void)flags;
  return 2;
}

// ---------------------------------------------------------------------------
// Graphics: framebuffer, pointer shape
// ---------------------------------------------------------------------------

// gdi_init(instance, PIXEL_FORMAT_BGRA32) gives a BGRA primary buffer that is
// already the byte layout of HarmonyOS BGRA_8888. Every drawing operation gdi
// decodes (bitmap updates and primary orders alike) paints into that buffer and
// adds its rectangle to the primary DC's invalid region; EndPaint() below
// publishes that region once per server update.
struct RdpPointer {
  rdpPointer base;
  uint8_t* rgba;
  size_t rgbaBytes;
};

BOOL pointerNew(rdpContext* context, rdpPointer* pointer) {
  if (pointer == nullptr) return TRUE;
  RdpPointer* converted = reinterpret_cast<RdpPointer*>(pointer);
  converted->rgba = nullptr;
  converted->rgbaBytes = 0;
  if (context == nullptr || context->gdi == nullptr) return TRUE;
  if (pointer->width == 0 || pointer->height == 0 ||
      pointer->width > static_cast<UINT32>(kMaxCursorSide) ||
      pointer->height > static_cast<UINT32>(kMaxCursorSide))
    return TRUE;
  if (pointer->xorMaskData == nullptr || pointer->lengthXorMask == 0) return TRUE;
  const size_t bytes =
      static_cast<size_t>(pointer->width) * pointer->height * 4;
  uint8_t* rgba = new (std::nothrow) uint8_t[bytes];
  if (rgba == nullptr) return TRUE;
  if (!freerdp_image_copy_from_pointer_data(
          rgba, PIXEL_FORMAT_RGBA32, 0, 0, 0, pointer->width, pointer->height,
          pointer->xorMaskData, pointer->lengthXorMask, pointer->andMaskData,
          pointer->lengthAndMask, pointer->xorBpp, &context->gdi->palette)) {
    delete[] rgba;
    return TRUE;
  }
  converted->rgba = rgba;
  converted->rgbaBytes = bytes;
  return TRUE;
}

void pointerFree(rdpContext* context, rdpPointer* pointer) {
  (void)context;
  RdpPointer* converted = reinterpret_cast<RdpPointer*>(pointer);
  if (converted == nullptr) return;
  delete[] converted->rgba;
  converted->rgba = nullptr;
  converted->rgbaBytes = 0;
}

BOOL pointerSet(rdpContext* context, rdpPointer* pointer) {
  RdpAttemptContext* attempt = attemptOf(context);
  if (attempt == nullptr || attempt->host == nullptr || hostStale(attempt))
    return TRUE;
  RdpPointer* converted = reinterpret_cast<RdpPointer*>(pointer);
  if (converted == nullptr || converted->rgba == nullptr ||
      converted->rgbaBytes == 0)
    return TRUE;
  attempt->host->setCursor(
      attempt->host->user, attempt->generation,
      static_cast<int>(pointer->width), static_cast<int>(pointer->height),
      static_cast<int>(pointer->xPos), static_cast<int>(pointer->yPos),
      converted->rgba);
  return TRUE;
}

BOOL pointerSetNull(rdpContext* context) {
  RdpAttemptContext* attempt = attemptOf(context);
  if (attempt == nullptr || attempt->host == nullptr || hostStale(attempt))
    return TRUE;
  attempt->host->setCursorBlank(attempt->host->user, attempt->generation);
  return TRUE;
}

// Built-in cursor for SYSPTR_DEFAULT. Windows sends the "default" system
// pointer instead of a shape at session start, so without this the user would
// see no pointer until the first custom cursor arrives.
void buildDefaultCursor(std::vector<uint8_t>& rgba, int& width, int& height,
                        int& hotX, int& hotY) {
  static const char* const kPattern[] = {
      "X...........",
      "XX..........",
      "X+X.........",
      "X++X........",
      "X+++X.......",
      "X++++X......",
      "X+++++X.....",
      "X++++++X....",
      "X+++++++X...",
      "X++++++++X..",
      "X+++++++++X.",
      "X++++++XXXXX",
      "X+++X++X....",
      "X++X.X++X...",
      "X+X..X++X...",
      "XX...X++X...",
      "X.....X++X..",
      "......X++X..",
      ".......XX...",
  };
  width = 12;
  height = 19;
  hotX = 0;
  hotY = 0;
  rgba.assign(static_cast<size_t>(width) * height * 4, 0);
  for (int y = 0; y < height; ++y) {
    const char* row = kPattern[y];
    for (int x = 0; x < width; ++x) {
      uint8_t* pixel = &rgba[(static_cast<size_t>(y) * width + x) * 4];
      if (row[x] == 'X') {
        pixel[0] = 0;
        pixel[1] = 0;
        pixel[2] = 0;
        pixel[3] = 255;
      } else if (row[x] == '+') {
        pixel[0] = 255;
        pixel[1] = 255;
        pixel[2] = 255;
        pixel[3] = 255;
      }
    }
  }
}

BOOL pointerSetDefault(rdpContext* context) {
  RdpAttemptContext* attempt = attemptOf(context);
  if (attempt == nullptr || attempt->host == nullptr || hostStale(attempt))
    return TRUE;
  std::vector<uint8_t> rgba;
  int width = 0, height = 0, hotX = 0, hotY = 0;
  buildDefaultCursor(rgba, width, height, hotX, hotY);
  attempt->host->setCursor(attempt->host->user, attempt->generation, width,
                           height, hotX, hotY, rgba.data());
  return TRUE;
}

BOOL pointerSetPosition(rdpContext* context, UINT32 x, UINT32 y) {
  RdpAttemptContext* attempt = attemptOf(context);
  if (attempt == nullptr || attempt->host == nullptr || hostStale(attempt))
    return TRUE;
  attempt->host->setCursorPos(attempt->host->user, attempt->generation,
                              static_cast<int>(x), static_cast<int>(y));
  return TRUE;
}

void registerPointer(rdpGraphics* graphics) {
  if (graphics == nullptr || graphics->Pointer_Prototype == nullptr) return;
  RdpPointer prototype;
  std::memset(&prototype, 0, sizeof(prototype));
  // Pointer_Alloc() allocates Pointer_Prototype->size bytes and copies only the
  // base rdpPointer fields; the trailing rgba fields of the allocated object
  // are zeroed by calloc, which is what pointerNew/pointerFree expect.
  prototype.base.size = sizeof(RdpPointer);
  prototype.base.New = pointerNew;
  prototype.base.Free = pointerFree;
  prototype.base.Set = pointerSet;
  prototype.base.SetNull = pointerSetNull;
  prototype.base.SetDefault = pointerSetDefault;
  prototype.base.SetPosition = pointerSetPosition;
  graphics_register_pointer(graphics, &prototype.base);
}

BOOL endPaint(rdpContext* context) {
  RdpAttemptContext* attempt = attemptOf(context);
  if (attempt == nullptr || attempt->host == nullptr || hostStale(attempt))
    return TRUE;
  rdpGdi* gdi = context->gdi;
  if (gdi == nullptr || gdi->primary == nullptr || gdi->primary->hdc == nullptr)
    return TRUE;
  HGDI_DC hdc = gdi->primary->hdc;
  if (hdc->hwnd == nullptr || hdc->hwnd->invalid == nullptr) return TRUE;
  HGDI_RGN invalid = hdc->hwnd->invalid;
  const int width = gdi->width;
  const int height = gdi->height;
  const int stride = static_cast<int>(gdi->stride);
  if (!invalid->null && gdi->primary_buffer != nullptr && width > 0 &&
      height > 0 && stride >= width * 4) {
    // gdi keeps the union of everything this update drew. Clamp it to the
    // primary buffer: a server rectangle outside the desktop would otherwise
    // read past the buffer. The arithmetic is done in 64 bit because the
    // region comes from decoded server data.
    const long long rx = invalid->x;
    const long long ry = invalid->y;
    const long long rw = invalid->w;
    const long long rh = invalid->h;
    const int x = static_cast<int>(std::max(0LL, rx));
    const int y = static_cast<int>(std::max(0LL, ry));
    const int x1 = static_cast<int>(
        std::min<long long>(width, rx + rw));
    const int y1 = static_cast<int>(
        std::min<long long>(height, ry + rh));
    if (x1 > x && y1 > y) {
      RdpRect rect;
      rect.x = x;
      rect.y = y;
      rect.w = x1 - x;
      rect.h = y1 - y;
      rect.stride = stride;
      rect.pixels = gdi->primary_buffer + static_cast<size_t>(y) * stride +
                    static_cast<size_t>(x) * 4;
      // One commit per server update: the bridge merges this rectangle into
      // the delivery union and bumps the version once.
      attempt->host->commitFrame(attempt->host->user, attempt->generation,
                                 &rect, 1, width, height);
    }
  }
  // update_begin_paint() resets the region too, but consuming it here matches
  // every FreeRDP client and keeps a stale union from growing without bounds.
  invalid->null = TRUE;
  hdc->hwnd->ninvalid = 0;
  return TRUE;
}

BOOL desktopResize(rdpContext* context) {
  RdpAttemptContext* attempt = attemptOf(context);
  if (attempt == nullptr || attempt->host == nullptr || hostStale(attempt))
    return FALSE;
  if (context->gdi == nullptr) return FALSE;
  const UINT32 width =
      freerdp_settings_get_uint32(context->settings, FreeRDP_DesktopWidth);
  const UINT32 height =
      freerdp_settings_get_uint32(context->settings, FreeRDP_DesktopHeight);
  if (!validDesktop(width, height)) return FALSE;
  // Resize gdi first so subsequent bitmap updates are painted into a buffer of
  // the new geometry; only then rebuild the session mirror.
  if (!gdi_resize(context->gdi, width, height)) return FALSE;
  attempt->host->resizeFrame(attempt->host->user, attempt->generation,
                             static_cast<int>(width),
                             static_cast<int>(height));
  return TRUE;
}

// ---------------------------------------------------------------------------
// FreeRDP client callbacks
// ---------------------------------------------------------------------------

BOOL preConnect(freerdp* instance) {
  if (instance == nullptr || instance->context == nullptr ||
      instance->context->settings == nullptr)
    return FALSE;
  rdpSettings* settings = instance->context->settings;
  // This build contains only the drdynvc + rdpei client channels. client-common
  // otherwise forces rdpdr/cliprdr/an rdpsnd addin at pre-connect time and the
  // session fails with "Failed to load addins" (see .work/freerdp-spike).
  // None of them is needed: v1 has no clipboard channel, no audio and no drive
  // redirection.
  return freerdp_settings_set_bool(settings, FreeRDP_RedirectClipboard, FALSE) &&
         freerdp_settings_set_bool(settings, FreeRDP_NetworkAutoDetect, FALSE) &&
         freerdp_settings_set_bool(settings, FreeRDP_SupportHeartbeatPdu, FALSE) &&
         freerdp_settings_set_bool(settings, FreeRDP_SupportMultitransport, FALSE);
}

BOOL postConnect(freerdp* instance) {
  if (instance == nullptr || instance->context == nullptr ||
      instance->context->settings == nullptr)
    return FALSE;
  rdpContext* context = instance->context;
  RdpAttemptContext* attempt = attemptOf(context);
  if (attempt == nullptr || attempt->host == nullptr || hostStale(attempt))
    return FALSE;
  rdpSettings* settings = context->settings;
  // gdi_init must run before DeactivateClientDecoding is set: the flag makes
  // gdi_register_update_callbacks() a no-op, so the opposite order would leave
  // update->BitmapUpdate NULL. (Same order as the spike probe.)
  if (!gdi_init(instance, PIXEL_FORMAT_BGRA32)) return FALSE;
  attempt->gdiReady = true;
  if (!freerdp_settings_set_bool(settings, FreeRDP_DeactivateClientDecoding,
                                 TRUE))
    return FALSE;
  registerPointer(context->graphics);
  if (context->update == nullptr) return FALSE;
  // EndPaint is where gdi has finished painting one server update into its
  // primary buffer (bitmap updates and primary orders alike) and the invalid
  // region describes what changed. This is the same hook every FreeRDP client
  // uses; it must stay after gdi_init so the gdi callbacks are in place.
  context->update->EndPaint = endPaint;
  context->update->DesktopResize = desktopResize;
  // The pointer-position update is only forwarded while GrabMouse is set. The
  // app renders the cursor itself, so it is the cursor authority.
  if (!freerdp_settings_set_bool(settings, FreeRDP_GrabMouse, TRUE))
    return FALSE;
  const UINT32 width =
      freerdp_settings_get_uint32(settings, FreeRDP_DesktopWidth);
  const UINT32 height =
      freerdp_settings_get_uint32(settings, FreeRDP_DesktopHeight);
  if (!validDesktop(width, height)) return FALSE;
  attempt->host->resizeFrame(attempt->host->user, attempt->generation,
                             static_cast<int>(width),
                             static_cast<int>(height));
  attempt->host->setPhase(attempt->host->user, RdpPhaseConnected, "", "none", 0);
  return TRUE;
}

void postDisconnect(freerdp* instance) {
  if (instance == nullptr || instance->context == nullptr) return;
  RdpAttemptContext* attempt = attemptOf(instance->context);
  if (attempt != nullptr && attempt->gdiReady) {
    gdi_free(instance);
    attempt->gdiReady = false;
  }
}

int logonErrorInfo(freerdp* instance, UINT32 data, UINT32 type) {
  (void)data;
  (void)type;  // static FreeRDP strings only; never logged or forwarded
  if (instance != nullptr && instance->context != nullptr) {
    RdpAttemptContext* attempt = attemptOf(instance->context);
    if (attempt != nullptr) attempt->sawLogonErrorInfo = true;
  }
  return 1;
}

BOOL clientNew(freerdp* instance, rdpContext* context) {
  if (instance == nullptr || context == nullptr) return FALSE;
  instance->PreConnect = preConnect;
  instance->PostConnect = postConnect;
  instance->PostDisconnect = postDisconnect;
  instance->VerifyCertificateEx = verifyCertificate;
  instance->VerifyChangedCertificateEx = verifyChangedCertificate;
  instance->LogonErrorInfo = logonErrorInfo;
  return TRUE;
}

void clientFree(freerdp* instance, rdpContext* context) {
  (void)instance;
  (void)context;
}

int clientStart(rdpContext* context) {
  (void)context;
  return 0;
}

int clientStop(rdpContext* context) {
  (void)context;
  return 0;
}

// ---------------------------------------------------------------------------
// Input: keysym -> RDP scancode, pointer mask -> RDP pointer flags
// ---------------------------------------------------------------------------

// X11 keysyms for the keys the ArkTS layer can send. Printable keys use the
// ASCII code of the unshifted character; shifted symbols map to the same
// physical key because Shift itself is sent as a separate event.
UINT32 keysymToScancode(int keysym) {
  if (keysym >= 'a' && keysym <= 'z') {
    static const UINT8 kLetters[26] = {
        0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25,
        0x26, 0x32, 0x31, 0x18, 0x19, 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F,
        0x11, 0x2D, 0x15, 0x2C};
    return MAKE_RDP_SCANCODE(kLetters[keysym - 'a'], FALSE);
  }
  if (keysym >= 'A' && keysym <= 'Z') {
    static const UINT8 kLetters[26] = {
        0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25,
        0x26, 0x32, 0x31, 0x18, 0x19, 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F,
        0x11, 0x2D, 0x15, 0x2C};
    return MAKE_RDP_SCANCODE(kLetters[keysym - 'A'], FALSE);
  }
  if (keysym >= '0' && keysym <= '9') {
    static const UINT8 kDigits[10] = {0x0B, 0x02, 0x03, 0x04, 0x05,
                                      0x06, 0x07, 0x08, 0x09, 0x0A};
    return MAKE_RDP_SCANCODE(kDigits[keysym - '0'], FALSE);
  }
  switch (keysym) {
    // Unshifted punctuation.
    case ' ': return RDP_SCANCODE_SPACE;
    case '-': return RDP_SCANCODE_OEM_MINUS;
    case '=': return RDP_SCANCODE_OEM_PLUS;
    case '[': return RDP_SCANCODE_OEM_4;
    case ']': return RDP_SCANCODE_OEM_6;
    case '\\': return RDP_SCANCODE_OEM_5;
    case ';': return RDP_SCANCODE_OEM_1;
    case '\'': return RDP_SCANCODE_OEM_7;
    case '`': return RDP_SCANCODE_OEM_3;
    case ',': return RDP_SCANCODE_OEM_COMMA;
    case '.': return RDP_SCANCODE_OEM_PERIOD;
    case '/': return RDP_SCANCODE_OEM_2;
    // Shifted variants of the same physical keys.
    case '!': return MAKE_RDP_SCANCODE(0x02, FALSE);
    case '@': return MAKE_RDP_SCANCODE(0x03, FALSE);
    case '#': return MAKE_RDP_SCANCODE(0x04, FALSE);
    case '$': return MAKE_RDP_SCANCODE(0x05, FALSE);
    case '%': return MAKE_RDP_SCANCODE(0x06, FALSE);
    case '^': return MAKE_RDP_SCANCODE(0x07, FALSE);
    case '&': return MAKE_RDP_SCANCODE(0x08, FALSE);
    case '*': return MAKE_RDP_SCANCODE(0x09, FALSE);
    case '(': return MAKE_RDP_SCANCODE(0x0A, FALSE);
    case ')': return MAKE_RDP_SCANCODE(0x0B, FALSE);
    case '_': return MAKE_RDP_SCANCODE(0x0C, FALSE);
    case '+': return MAKE_RDP_SCANCODE(0x0D, FALSE);
    case '{': return MAKE_RDP_SCANCODE(0x1A, FALSE);
    case '}': return MAKE_RDP_SCANCODE(0x1B, FALSE);
    case '|': return MAKE_RDP_SCANCODE(0x2B, FALSE);
    case ':': return MAKE_RDP_SCANCODE(0x27, FALSE);
    case '"': return MAKE_RDP_SCANCODE(0x28, FALSE);
    case '~': return MAKE_RDP_SCANCODE(0x29, FALSE);
    case '<': return MAKE_RDP_SCANCODE(0x33, FALSE);
    case '>': return MAKE_RDP_SCANCODE(0x34, FALSE);
    case '?': return MAKE_RDP_SCANCODE(0x35, FALSE);
    // Editing / navigation.
    case 0xFF08: return RDP_SCANCODE_BACKSPACE;  // XK_BackSpace
    case 0xFF09: return RDP_SCANCODE_TAB;        // XK_Tab
    case 0xFF0D: return RDP_SCANCODE_RETURN;     // XK_Return
    case 0xFF1B: return RDP_SCANCODE_ESCAPE;     // XK_Escape
    case 0xFF50: return RDP_SCANCODE_HOME;       // XK_Home
    case 0xFF51: return RDP_SCANCODE_LEFT;       // XK_Left
    case 0xFF52: return RDP_SCANCODE_UP;         // XK_Up
    case 0xFF53: return RDP_SCANCODE_RIGHT;      // XK_Right
    case 0xFF54: return RDP_SCANCODE_DOWN;       // XK_Down
    case 0xFF55: return RDP_SCANCODE_PRIOR;      // XK_Page_Up
    case 0xFF56: return RDP_SCANCODE_NEXT;       // XK_Page_Down
    case 0xFF57: return RDP_SCANCODE_END;        // XK_End
    case 0xFF63: return RDP_SCANCODE_INSERT;     // XK_Insert
    case 0xFFFF: return RDP_SCANCODE_DELETE;     // XK_Delete
    case 0xFF67: return RDP_SCANCODE_APPS;       // XK_Menu
    // Modifiers.
    case 0xFFE1: return RDP_SCANCODE_LSHIFT;     // XK_Shift_L
    case 0xFFE2: return RDP_SCANCODE_RSHIFT;     // XK_Shift_R
    case 0xFFE3: return RDP_SCANCODE_LCONTROL;   // XK_Control_L
    case 0xFFE4: return RDP_SCANCODE_RCONTROL;   // XK_Control_R
    case 0xFFE5: return RDP_SCANCODE_CAPSLOCK;   // XK_Caps_Lock
    case 0xFFE7: return RDP_SCANCODE_LWIN;       // XK_Meta_L
    case 0xFFE8: return RDP_SCANCODE_RWIN;       // XK_Meta_R
    case 0xFFE9: return RDP_SCANCODE_LMENU;      // XK_Alt_L
    case 0xFFEA: return RDP_SCANCODE_RMENU;      // XK_Alt_R
    case 0xFFEB: return RDP_SCANCODE_LWIN;       // XK_Super_L
    case 0xFFEC: return RDP_SCANCODE_RWIN;       // XK_Super_R
    case 0xFF7F: return RDP_SCANCODE_NUMLOCK;    // XK_Num_Lock
    case 0xFF14: return RDP_SCANCODE_SCROLLLOCK; // XK_Scroll_Lock
    // Function keys.
    case 0xFFBE: return RDP_SCANCODE_F1;
    case 0xFFBF: return RDP_SCANCODE_F2;
    case 0xFFC0: return RDP_SCANCODE_F3;
    case 0xFFC1: return RDP_SCANCODE_F4;
    case 0xFFC2: return RDP_SCANCODE_F5;
    case 0xFFC3: return RDP_SCANCODE_F6;
    case 0xFFC4: return RDP_SCANCODE_F7;
    case 0xFFC5: return RDP_SCANCODE_F8;
    case 0xFFC6: return RDP_SCANCODE_F9;
    case 0xFFC7: return RDP_SCANCODE_F10;
    case 0xFFC8: return RDP_SCANCODE_F11;
    case 0xFFC9: return RDP_SCANCODE_F12;
    // Keypad.
    case 0xFF8D: return RDP_SCANCODE_RETURN_KP;  // XK_KP_Enter
    case 0xFFAA: return RDP_SCANCODE_MULTIPLY;   // XK_KP_Multiply
    case 0xFFAB: return RDP_SCANCODE_ADD;        // XK_KP_Add
    case 0xFFAC: return RDP_SCANCODE_DECIMAL;    // XK_KP_Separator
    case 0xFFAD: return RDP_SCANCODE_SUBTRACT;   // XK_KP_Subtract
    case 0xFFAE: return RDP_SCANCODE_DECIMAL;    // XK_KP_Decimal
    case 0xFFAF: return RDP_SCANCODE_DIVIDE;     // XK_KP_Divide
    case 0xFFB0: return RDP_SCANCODE_NUMPAD0;    // XK_KP_0
    case 0xFFB1: return RDP_SCANCODE_NUMPAD1;    // XK_KP_1
    case 0xFFB2: return RDP_SCANCODE_NUMPAD2;    // XK_KP_2
    case 0xFFB3: return RDP_SCANCODE_NUMPAD3;    // XK_KP_3
    case 0xFFB4: return RDP_SCANCODE_NUMPAD4;    // XK_KP_4
    case 0xFFB5: return RDP_SCANCODE_NUMPAD5;    // XK_KP_5
    case 0xFFB6: return RDP_SCANCODE_NUMPAD6;    // XK_KP_6
    case 0xFFB7: return RDP_SCANCODE_NUMPAD7;    // XK_KP_7
    case 0xFFB8: return RDP_SCANCODE_NUMPAD8;    // XK_KP_8
    case 0xFFB9: return RDP_SCANCODE_NUMPAD9;    // XK_KP_9
    default: return 0;
  }
}

struct InputBridge {
  RdpAttemptContext* attempt;
  std::map<int, UINT32> pressed;  // keysym -> scancode of the press
  int pointerMask;
  int lastX;
  int lastY;
  unsigned appliedRelease;
  size_t droppedClipboards;
  unsigned long long unmappedKeys;
};

void sinkPointer(void* context, int x, int y, int mask) {
  InputBridge* bridge = static_cast<InputBridge*>(context);
  if (bridge == nullptr || bridge->attempt == nullptr) return;
  RdpAttemptContext* attempt = bridge->attempt;
  rdpContext* context2 = &attempt->common.context;
  rdpInput* input = context2->input;
  if (input == nullptr) return;
  rdpGdi* gdi = context2->gdi;
  if (gdi != nullptr && gdi->width > 0 && gdi->height > 0) {
    x = std::max(0, std::min(x, gdi->width - 1));
    y = std::max(0, std::min(y, gdi->height - 1));
  }
  // RFB mask: bit 0 left, bit 1 middle, bit 2 right, bit 3 wheel up, bit 4
  // wheel down. RDP: BUTTON1 left, BUTTON2 right, BUTTON3 middle.
  const bool wheelUp = (mask & 8) != 0;
  const bool wheelDown = (mask & 16) != 0;
  // RDP pointer events are state changes (a button bit plus PTR_FLAGS_DOWN to
  // press, the bare button bit to release), while the RFB mask is the whole
  // held-button set. Diff it against the state already applied remotely.
  const unsigned previous = static_cast<unsigned>(bridge->pointerMask);
  const unsigned current = static_cast<unsigned>(mask & 7);
  const unsigned changed = previous ^ current;
  UINT16 buttonFlags = 0;
  if (changed & 1) buttonFlags |= PTR_FLAGS_BUTTON1 | ((current & 1) ? PTR_FLAGS_DOWN : 0);
  if (changed & 2) buttonFlags |= PTR_FLAGS_BUTTON3 | ((current & 2) ? PTR_FLAGS_DOWN : 0);
  if (changed & 4) buttonFlags |= PTR_FLAGS_BUTTON2 | ((current & 4) ? PTR_FLAGS_DOWN : 0);
  if (wheelUp || wheelDown) {
    // A wheel notch does not change the held-button state, but a button change
    // carried by the same mask must be applied first so that the tracked state
    // and the remote state agree.
    if (changed != 0) {
      freerdp_input_send_mouse_event(input, buttonFlags, static_cast<UINT16>(x),
                                     static_cast<UINT16>(y));
      bridge->pointerMask = static_cast<int>(current);
    }
    // One notch is 0x78 (120) in the 9-bit rotation field; the negative flag is
    // the sign bit, matching the X11 client's button mapping.
    UINT16 flags = PTR_FLAGS_WHEEL;
    if (wheelDown)
      flags |= PTR_FLAGS_WHEEL_NEGATIVE | static_cast<UINT16>(0x100 - 0x78);
    else
      flags |= 0x78;
    freerdp_input_send_mouse_event(input, flags, static_cast<UINT16>(x),
                                   static_cast<UINT16>(y));
  } else if (changed != 0) {
    // Button transitions carry only the button bits: PTRFLAGS_MOVE means
    // "pointer position update" and the FreeRDP reference clients (xf_event.c,
    // wf_event.c) omit it here. The position is already set by the pure-move
    // event sent for the previous pointer sample.
    freerdp_input_send_mouse_event(input, buttonFlags, static_cast<UINT16>(x),
                                   static_cast<UINT16>(y));
    bridge->pointerMask = static_cast<int>(current);
  } else {
    // Pure motion is the only case that sends PTRFLAGS_MOVE.
    freerdp_input_send_mouse_event(input, PTR_FLAGS_MOVE, static_cast<UINT16>(x),
                                   static_cast<UINT16>(y));
  }
  bridge->lastX = x;
  bridge->lastY = y;
  attempt->host->setCursorPos(attempt->host->user, attempt->generation, x, y);
}

void sinkKey(void* context, int keysym, int pressed) {
  InputBridge* bridge = static_cast<InputBridge*>(context);
  if (bridge == nullptr || bridge->attempt == nullptr) return;
  rdpInput* input = bridge->attempt->common.context.input;
  if (input == nullptr) return;
  if (pressed != 0) {
    const UINT32 scancode = keysymToScancode(keysym);
    if (scancode == 0) {
      ++bridge->unmappedKeys;
      return;
    }
    // A second down for a key that is already held is an auto-repeat: tell
    // FreeRDP to set KBD_FLAGS_DOWN instead of sending a fresh key-down.
    const bool repeated = bridge->pressed.find(keysym) != bridge->pressed.end();
    bridge->pressed[keysym] = scancode;
    freerdp_input_send_keyboard_event_ex(input, TRUE, repeated ? TRUE : FALSE,
                                         scancode);
  } else {
    std::map<int, UINT32>::iterator found = bridge->pressed.find(keysym);
    UINT32 scancode = 0;
    if (found != bridge->pressed.end()) {
      scancode = found->second;
      bridge->pressed.erase(found);
    } else {
      scancode = keysymToScancode(keysym);
    }
    if (scancode == 0) {
      ++bridge->unmappedKeys;
      return;
    }
    freerdp_input_send_keyboard_event_ex(input, FALSE, FALSE, scancode);
  }
}

void sinkRelease(void* context) {
  InputBridge* bridge = static_cast<InputBridge*>(context);
  if (bridge == nullptr || bridge->attempt == nullptr) return;
  rdpInput* input = bridge->attempt->common.context.input;
  if (input == nullptr) return;
  // Release exactly what this attempt pressed, in the same spirit as the VNC
  // loop's releaseSessionInput() handling.
  for (std::map<int, UINT32>::const_iterator it = bridge->pressed.begin();
       it != bridge->pressed.end(); ++it)
    freerdp_input_send_keyboard_event_ex(input, FALSE, FALSE, it->second);
  bridge->pressed.clear();
  if (bridge->pointerMask != 0) {
    // RDP is state-change semantics: release every still-held button by sending
    // its bare button bit without PTR_FLAGS_DOWN. Button events omit
    // PTRFLAGS_MOVE here as well, matching the reference clients; that flag is
    // only for pure motion.
    UINT16 flags = 0;
    if (bridge->pointerMask & 1) flags |= PTR_FLAGS_BUTTON1;
    if (bridge->pointerMask & 2) flags |= PTR_FLAGS_BUTTON3;
    if (bridge->pointerMask & 4) flags |= PTR_FLAGS_BUTTON2;
    if (flags != 0)
      freerdp_input_send_mouse_event(input, flags,
                                     static_cast<UINT16>(bridge->lastX),
                                     static_cast<UINT16>(bridge->lastY));
  }
  bridge->pointerMask = 0;
}

void sinkClipboard(void* context, const char* text) {
  (void)text;
  InputBridge* bridge = static_cast<InputBridge*>(context);
  if (bridge != nullptr) ++bridge->droppedClipboards;
}

// ---------------------------------------------------------------------------
// Connection attempt
// ---------------------------------------------------------------------------

struct Attempt {
  const RdpHost& host;
  const RdpOptions& options;
  int generation;

  rdpContext* context;
  freerdp* instance;
  RdpAttemptContext* attempt;
  bool connected;

  Attempt(const RdpHost& host, const RdpOptions& options, int generation)
      : host(host), options(options), generation(generation), context(nullptr),
        instance(nullptr), attempt(nullptr), connected(false) {}

  ~Attempt() { teardown(); }

  bool create() {
    RDP_CLIENT_ENTRY_POINTS entryPoints;
    std::memset(&entryPoints, 0, sizeof(entryPoints));
    entryPoints.Version = RDP_CLIENT_INTERFACE_VERSION;
    entryPoints.Size = sizeof(RDP_CLIENT_ENTRY_POINTS_V1);
    entryPoints.ContextSize = sizeof(RdpAttemptContext);
    entryPoints.ClientNew = clientNew;
    entryPoints.ClientFree = clientFree;
    entryPoints.ClientStart = clientStart;
    entryPoints.ClientStop = clientStop;
    context = freerdp_client_context_new(&entryPoints);
    if (context == nullptr) return false;
    instance = context->instance;
    if (instance == nullptr || context->settings == nullptr) return false;
    attempt = attemptOf(context);
    attempt->host = &host;
    attempt->generation = generation;
    attempt->gdiReady = false;
    attempt->sawLogonErrorInfo = false;
    return true;
  }

  bool configure() {
    if (context == nullptr || context->settings == nullptr) return false;
    rdpSettings* settings = context->settings;
    // Security stays at FreeRDP's defaults: NLA/TLS/RDP with negotiation.
    return freerdp_settings_set_string(settings, FreeRDP_ServerHostname,
                                       options.host.c_str()) &&
           freerdp_settings_set_uint32(
               settings, FreeRDP_ServerPort,
               static_cast<UINT32>(options.port)) &&
           freerdp_settings_set_string(settings, FreeRDP_Username,
                                       options.username.c_str()) &&
           freerdp_settings_set_string(settings, FreeRDP_Domain,
                                       options.domain.c_str()) &&
           freerdp_settings_set_string(settings, FreeRDP_Password,
                                       options.password.c_str()) &&
           freerdp_settings_set_uint32(settings, FreeRDP_DesktopWidth,
                                       static_cast<UINT32>(kRequestedWidth)) &&
           freerdp_settings_set_uint32(settings, FreeRDP_DesktopHeight,
                                       static_cast<UINT32>(kRequestedHeight));
  }

  // freerdp_connect() can block for seconds inside TCP/TLS/NLA. A watchdog
  // thread polls stop/generation every 10 ms and calls the abort API so
  // closeSession()'s reaper can join quickly; the watchdog is joined before the
  // context is used or freed again.
  bool connectPhase() {
    std::atomic<bool> finished(false);
    std::thread watchdog;
    try {
      watchdog = std::thread([this, &finished]() {
        while (!finished.load(std::memory_order_acquire)) {
          if (host.stopRequested(host.user) ||
              host.generation(host.user) != generation) {
            freerdp_abort_connect_context(context);
            return;
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(kConnectWatchdogMs));
        }
      });
    } catch (...) {
      // Without a watchdog, close still works; it just has to wait out the
      // FreeRDP connect timeout. Never fail the session over this.
    }
    const bool ok = freerdp_connect(instance) != FALSE;
    finished.store(true, std::memory_order_release);
    if (watchdog.joinable()) watchdog.join();
    if (ok) connected = true;
    return ok;
  }

  UINT32 lastError() const {
    return context != nullptr ? freerdp_get_last_error(context)
                              : static_cast<UINT32>(FREERDP_ERROR_NONE);
  }

  void eventLoop() {
    if (attempt == nullptr) return;
    InputBridge bridge;
    bridge.attempt = attempt;
    bridge.pointerMask = 0;
    bridge.lastX = 0;
    bridge.lastY = 0;
    bridge.droppedClipboards = 0;
    bridge.unmappedKeys = 0;
    // Drop pointer/key input queued for a previous connection and start with
    // nothing held (same reset as the VNC loop before it reads the socket).
    bridge.appliedRelease = host.prepareInput(host.user);
    RdpInputSink sink;
    sink.ctx = &bridge;
    sink.pointer = sinkPointer;
    sink.key = sinkKey;
    sink.release = sinkRelease;
    sink.clipboard = sinkClipboard;

    while (!host.stopRequested(host.user)) {
      if (host.generation(host.user) != generation) break;
      HANDLE handles[MAXIMUM_WAIT_OBJECTS] = {0};
      const DWORD count =
          freerdp_get_event_handles(context, handles, ARRAYSIZE(handles));
      if (count == 0) break;
      const DWORD wait = WaitForMultipleObjects(count, handles, FALSE, kLoopWaitMs);
      if (wait == WAIT_FAILED) break;
      if (!freerdp_check_event_handles(context)) break;
      // requestFullFrame(): treat the next update as a full-screen commit.
      host.consumeFullRequest(host.user);
      host.drainInput(host.user, &sink, &bridge.appliedRelease);
      if (freerdp_shall_disconnect_context(context)) break;
    }
    if (bridge.droppedClipboards > 0) {
      // No cliprdr channel in this build; the text is reported as delivered to
      // the caller, so say plainly that it could not be forwarded. Never log
      // the text itself.
      std::fprintf(stderr,
                   "tianguan: rdp dropped %zu clipboard item(s): clipboard "
                   "channel not built\n",
                   bridge.droppedClipboards);
      std::fflush(stderr);
    }
    if (bridge.unmappedKeys > 0) {
      std::fprintf(stderr,
                   "tianguan: rdp dropped %llu key event(s) with no scancode "
                   "mapping\n",
                   bridge.unmappedKeys);
      std::fflush(stderr);
    }
  }

  void teardown() {
    if (context == nullptr) return;
    if (connected) {
      connected = false;
      // Both calls are also safe on an already-closed connection; abort first
      // so no blocked transport call can outlive the teardown.
      freerdp_abort_connect_context(context);
      freerdp_disconnect(instance);
    } else if (attempt != nullptr && attempt->gdiReady) {
      // PostConnect ran (gdi_init succeeded) but freerdp_connect() failed after
      // that point and did not call PostDisconnect; release gdi here.
      gdi_free(instance);
      attempt->gdiReady = false;
    }
    // The credentials are copied into the settings by configure(); clear the
    // password copy before the settings are freed.
    if (context->settings != nullptr)
      freerdp_settings_set_string(context->settings, FreeRDP_Password, "");
    freerdp_client_context_free(context);
    context = nullptr;
    instance = nullptr;
    attempt = nullptr;
  }
};

void setPhase(const RdpHost& host, int phase, const std::string& error,
              const char* errorCode, int retryInMs) {
  host.setPhase(host.user, phase, error.c_str(), errorCode, retryInMs);
}

}  // namespace

void runRdpSession(const RdpHost& host, const RdpOptions& options) {
  if (host.user == nullptr || host.stopRequested == nullptr ||
      host.generation == nullptr || host.beginAttempt == nullptr ||
      host.setPhase == nullptr || host.sleepUnlessStopped == nullptr ||
      host.waitForReconnect == nullptr || host.resizeFrame == nullptr ||
      host.commitFrame == nullptr || host.setCursor == nullptr ||
      host.setCursorBlank == nullptr || host.setCursorPos == nullptr ||
      host.prepareInput == nullptr || host.drainInput == nullptr ||
      host.consumeFullRequest == nullptr) {
    return;  // bridge not fully wired: fail closed instead of crashing
  }
  int lastApplied = 0;
  int backoffMs = kInitialBackoffMs;
  while (!host.stopRequested(host.user)) {
    const int generation = host.beginAttempt(host.user, &lastApplied);
    setPhase(host, RdpPhaseConnecting, "", "none", 0);
    // closeSession() may have landed between the loop condition and here; do
    // not pay for a context/connect attempt that is already cancelled.
    if (host.stopRequested(host.user)) break;

    Attempt attempt(host, options, generation);
    if (!attempt.create() || !attempt.configure()) {
      attempt.teardown();
      if (host.stopRequested(host.user)) break;
      if (host.generation(host.user) != generation) continue;
      setPhase(host, RdpPhaseFailed, "RDP client initialization failed",
               "protocol", 0);
      host.waitForReconnect(host.user);
      continue;
    }
    if (host.stopRequested(host.user)) {
      attempt.teardown();
      break;
    }

    const bool connected = attempt.connectPhase();
    if (host.stopRequested(host.user)) break;
    if (host.generation(host.user) != generation) {
      // Manual reconnectSession() bumped the generation while connecting: tear
      // this attempt down and start the new one immediately (no backoff).
      attempt.teardown();
      continue;
    }

    if (!connected) {
      const UINT32 code = attempt.lastError();
      const bool sawLogonErrorInfo =
          attempt.attempt != nullptr && attempt.attempt->sawLogonErrorInfo;
      const RdpFailureClass failure = classifyConnectFailure(code, sawLogonErrorInfo);
      const std::string message = connectErrorText(code, options);
      attempt.teardown();
      if (failure == RdpFailureAuth) {
        // The UI's password-retry sheet keys off failed/auth-failed.
        setPhase(host, RdpPhaseFailed, message, "auth-failed", 0);
        host.waitForReconnect(host.user);
        continue;
      }
      if (failure == RdpFailureProtocol) {
        setPhase(host, RdpPhaseFailed, message, "protocol", 0);
        host.waitForReconnect(host.user);
        continue;
      }
      setPhase(host, RdpPhaseReconnecting, message, "network", backoffMs);
      if (!host.sleepUnlessStopped(host.user, backoffMs)) {
        if (host.stopRequested(host.user)) break;
        continue;  // manual reconnect interrupted the backoff
      }
      backoffMs = std::min(backoffMs * 2, kMaxBackoffMs);
      continue;
    }

    backoffMs = kInitialBackoffMs;
    attempt.eventLoop();
    const bool stopping = host.stopRequested(host.user);
    const bool superseded = host.generation(host.user) != generation;
    attempt.teardown();
    if (stopping) break;
    if (superseded) continue;
    // Same terminal wording as the VNC loop: a lost connection is a network
    // event that is retried with backoff, not a terminal failure.
    setPhase(host, RdpPhaseReconnecting, "connection closed", "network",
             backoffMs);
    if (!host.sleepUnlessStopped(host.user, backoffMs)) {
      if (host.stopRequested(host.user)) break;
      continue;
    }
    backoffMs = std::min(backoffMs * 2, kMaxBackoffMs);
  }
}

}  // namespace tianguan
