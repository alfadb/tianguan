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

#include <freerdp/addin.h>
#include <freerdp/channels/channels.h>
#include <freerdp/channels/cliprdr.h>
#include <freerdp/channels/rdpgfx.h>
#include <freerdp/channels/rdpsnd.h>
#include <freerdp/client.h>
#include <freerdp/client/channels.h>
#include <freerdp/client/cliprdr.h>
#include <freerdp/client/cmdline.h>
#include <freerdp/client/rdpgfx.h>
#include <freerdp/client/rdpsnd.h>
#include <freerdp/codec/audio.h>
#include <freerdp/codec/dsp.h>
#include <freerdp/error.h>
#include <freerdp/event.h>
#include <freerdp/freerdp.h>
#include <freerdp/gdi/gdi.h>
// gdi/gfx.h uses RdpgfxClientContext without including the client header, so
// client/rdpgfx.h must come first (already included above).
#include <freerdp/gdi/gfx.h>
#include <freerdp/graphics.h>
#include <freerdp/input.h>
#include <freerdp/pointer.h>
#include <freerdp/scancode.h>
#include <freerdp/settings.h>
#include <freerdp/update.h>
#include <winpr/crt.h>
#include <winpr/input.h>
#include <winpr/string.h>
#include <winpr/synch.h>
#include <winpr/user.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <vector>

#if defined(__MUSL__)
#include <hilog/log.h>
#endif

// OHOS AudioRenderer NDK. Only the OHOS target compiles this translation unit
// (app/native/CMakeLists.txt keeps it out of the host target, like the rest of
// the FreeRDP backend); libohaudio.so is a platform system library.
#include <ohaudio/native_audiorenderer.h>
#include <ohaudio/native_audiostreambuilder.h>
#include <ohaudio/native_audiostream_base.h>

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
// GFX (EGFX / rdpgfx dynamic channel) takeover switch. true: advertise the
// graphics pipeline, let gdi decode surface commands and commit those frames.
// false: the old behaviour (plain GDI bitmap/orders updates only). Rebuild with
// this set to false to fall back if the EGFX picture is black or frozen.
const bool kEnableGfx = true;
// Same 1 MiB text bound as diagnostics.cxx's clipboard state (kClipboardLimit):
// the ArkTS setClipboard() already refuses larger text, this is the backend's
// own guard so a queued item can never exceed what the clip state accepts.
const size_t kClipboardLimit = 1024 * 1024;
// Refuse absurd CF_UNICODETEXT payloads before allocating a UTF-8 copy. Normal
// text clipboard data is far below this; the cap only bounds a misbehaving or
// hostile server.
const UINT32 kMaxClipboardWireBytes = 4u * 1024u * 1024u;

// Diagnostics line without any clipboard content: lengths and format ids only.
// On device it goes through the same hilog tag the rest of the native layer
// uses; the host build has no hilog and keeps the stderr fallback.
void diagWarn(const std::string& line) {
#if defined(__MUSL__)
  OH_LOG_Print(LOG_APP, LOG_WARN, 0, "TianguanDiag", "%{public}s", line.c_str());
#else
  std::fprintf(stderr, "W TianguanDiag %s\n", line.c_str());
  std::fflush(stderr);
#endif
}

// Periodic statistics lines use DEBUG: they are diagnostics, not errors, and
// must not spam the default log level. One-shot and error lines stay WARN.
void diagDebug(const std::string& line) {
#if defined(__MUSL__)
  OH_LOG_Print(LOG_APP, LOG_DEBUG, 0, "TianguanDiag", "%{public}s", line.c_str());
#else
  std::fprintf(stderr, "D TianguanDiag %s\n", line.c_str());
  std::fflush(stderr);
#endif
}

// One connection attempt. The head must be the FreeRDP client context: every
// FreeRDP callback receives an rdpContext* that points at this struct (the
// client-common entry-point contract, same pattern as .work/freerdp-spike).
struct GfxBridge;

struct RdpAttemptContext {
  rdpClientContext common;
  const RdpHost* host;
  int generation;
  bool gdiReady;
  bool sawLogonErrorInfo;
  // Set by Attempt::create() when kEnableGfx is on; owned by the Attempt.
  GfxBridge* gfx;
  // gdi update callbacks saved by counted* wrappers (see the GFX section), so
  // the wrappers can forward unchanged and count what actually paints.
  pBitmapUpdate bitmapUpdate;
  pSurfaceBits surfaceBits;
  // Certificate pinning outcome of this attempt. Set by the FreeRDP
  // certificate callbacks and read by runRdpSession() after a failed connect to
  // decide how the attempt is reported instead of guessing from the error code.
  bool trustRequired;
  bool fingerprintMismatch;
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

// ---------------------------------------------------------------------------
// Graphics pipeline (EGFX / rdpgfx dynamic channel)
//
// rdpgfx is a dynamic channel: after the connection is up the server opens
// "Microsoft::Windows::RDS::Graphics" inside drdynvc, FreeRDP publishes
// ChannelConnected on the context pubSub, and gdi_graphics_pipeline_init()
// installs the gdi surface decoders. Decoded surfaces are painted into the gdi
// primary buffer and gdi_OutputUpdate() invalidates the copied rectangles, so
// the EndPaint hook above commits them to the session mirror exactly like a
// bitmap update. That is also why GFX must be able to use the client codecs:
// with DeactivateClientDecoding set, gdi_graphics_pipeline_init() clears
// SurfaceCommand/UpdateSurfaces and no frame would ever be decoded.
//
// This FreeRDP prefix is built with WITH_FFMPEG/WITH_SWSCALE, which puts
// WITH_GFX_H264 into config.h: libavcodec supplies the H.264 decoder and
// libswscale the YUV->RGB conversion for AVC420/AVC444(x) surfaces. preConnect()
// therefore advertises those codecs in the 8.1/10.x capability sets so the
// server may select them; ClearCodec / planar / RemoteFX / progressive stay at
// their defaults. The "gfx: ... last codec=<id>" stats line reports which codec
// the server actually picked (11=AVC420, 14=AVC444, 15=AVC444v2, see
// freerdp/channels/rdpgfx.h).
// ---------------------------------------------------------------------------

struct GfxBridge {
  RdpgfxClientContext* context = nullptr;
  rdpGdi* gdi = nullptr;
  // gdi_SurfaceCommand/EndFrame, installed by gdi_graphics_pipeline_init(); the
  // wrappers below count calls and forward to them unchanged.
  pcRdpgfxSurfaceCommand surfaceCommand = nullptr;
  pcRdpgfxEndFrame endFrame = nullptr;
  std::atomic<UINT64> commands{0};
  std::atomic<UINT32> lastCodec{0};
  std::atomic<UINT64> lastStatsMs{0};
  // Frame-source accounting for the 5 s stats line: endFrames are EGFX frames,
  // commits are frames actually published to the session mirror, bitmap/surface
  // bits are the fastpath (GDI) paths. If endFrames grows while bitmap/surface
  // bits stay zero, the picture can only have come from GFX.
  std::atomic<UINT64> endFrames{0};
  std::atomic<UINT64> commits{0};
  std::atomic<UINT64> bitmapUpdates{0};
  std::atomic<UINT64> surfaceBits{0};
};

// SurfaceCommand only receives the RdpgfxClientContext*, so the per-attempt
// bridge is looked up by that pointer. An entry lives for exactly one channel
// binding. drdynvc runs on its own thread by default (async dynamic channels),
// hence the mutex; teardown/unbind also happens on that same code path or after
// the drdynvc thread has been joined, so a returned bridge cannot be freed
// while a command is being handled.
std::mutex g_gfxMutex;
std::map<RdpgfxClientContext*, GfxBridge*> g_gfxBridges;

UINT64 monotonicMillis() {
  return static_cast<UINT64>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

GfxBridge* findGfxBridge(RdpgfxClientContext* context) {
  std::lock_guard<std::mutex> lock(g_gfxMutex);
  const auto it = g_gfxBridges.find(context);
  return it == g_gfxBridges.end() ? nullptr : it->second;
}

// Wraps gdi_SurfaceCommand: first command is logged once, then at most one
// counter line every kGfxStatsIntervalMs. The original implementation always
// runs, so wrapping cannot change what is painted.
const UINT64 kGfxStatsIntervalMs = 5000;

UINT gfxSurfaceCommand(RdpgfxClientContext* context,
                       const RDPGFX_SURFACE_COMMAND* cmd) {
  GfxBridge* bridge = findGfxBridge(context);
  if (bridge == nullptr || bridge->surfaceCommand == nullptr)
    return CHANNEL_RC_BAD_CHANNEL_HANDLE;
  const UINT64 count = bridge->commands.fetch_add(1) + 1;
  const UINT32 codec = cmd != nullptr ? cmd->codecId : 0;
  if (cmd != nullptr) bridge->lastCodec.store(codec);
  if (count == 1) {
    char line[160];
    std::snprintf(line, sizeof(line),
                  "gfx: first surface command codec=%u w=%u h=%u",
                  static_cast<unsigned>(codec),
                  cmd != nullptr ? static_cast<unsigned>(cmd->width) : 0u,
                  cmd != nullptr ? static_cast<unsigned>(cmd->height) : 0u);
    diagWarn(line);
  } else {
    const UINT64 now = monotonicMillis();
    UINT64 previous = bridge->lastStatsMs.load();
    if (now >= previous + kGfxStatsIntervalMs &&
        bridge->lastStatsMs.compare_exchange_strong(previous, now)) {
      char line[240];
      std::snprintf(line, sizeof(line),
                    "gfx: %llu surface commands, last codec=%u, endframes=%llu, "
                    "commits=%llu, bitmap=%llu, surfbits=%llu",
                    static_cast<unsigned long long>(count),
                    static_cast<unsigned>(bridge->lastCodec.load()),
                    static_cast<unsigned long long>(bridge->endFrames.load()),
                    static_cast<unsigned long long>(bridge->commits.load()),
                    static_cast<unsigned long long>(bridge->bitmapUpdates.load()),
                    static_cast<unsigned long long>(bridge->surfaceBits.load()));
      diagDebug(line);
    }
  }
  return bridge->surfaceCommand(context, cmd);
}

UINT gfxEndFrame(RdpgfxClientContext* context,
                 const RDPGFX_END_FRAME_PDU* endFrame) {
  GfxBridge* bridge = findGfxBridge(context);
  if (bridge == nullptr) return CHANNEL_RC_BAD_CHANNEL_HANDLE;
  bridge->endFrames.fetch_add(1);
  if (bridge->endFrame == nullptr) return CHANNEL_RC_BAD_CHANNEL_HANDLE;
  return bridge->endFrame(context, endFrame);
}

// Fastpath (GDI) frame sources. Wrapping update->BitmapUpdate and
// update->SurfaceBits lets the stats line show whether the picture is also
// being painted outside GFX; the wrappers themselves only count and forward.
BOOL countedBitmapUpdate(rdpContext* context, const BITMAP_UPDATE* bitmap) {
  RdpAttemptContext* attempt = attemptOf(context);
  if (attempt == nullptr || attempt->bitmapUpdate == nullptr) return TRUE;
  if (attempt->gfx != nullptr) attempt->gfx->bitmapUpdates.fetch_add(1);
  return attempt->bitmapUpdate(context, bitmap);
}

BOOL countedSurfaceBits(rdpContext* context,
                        const SURFACE_BITS_COMMAND* surfaceBitsCommand) {
  RdpAttemptContext* attempt = attemptOf(context);
  if (attempt == nullptr || attempt->surfaceBits == nullptr) return TRUE;
  if (attempt->gfx != nullptr) attempt->gfx->surfaceBits.fetch_add(1);
  return attempt->surfaceBits(context, surfaceBitsCommand);
}

bool isGfxChannelName(const char* name) {
  // The settings-side dynamic channel is named "rdpgfx", but the server opens
  // it under its DVC name; the ChannelConnected event carries the latter.
  return name != nullptr &&
         (std::strcmp(name, RDPGFX_CHANNEL_NAME) == 0 ||
          std::strcmp(name, RDPGFX_DVC_CHANNEL_NAME) == 0);
}

void gfxUnbind(RdpAttemptContext* attempt) {
  if (attempt == nullptr) return;
  GfxBridge* bridge = attempt->gfx;
  if (bridge == nullptr || bridge->context == nullptr) return;
  RdpgfxClientContext* gfx = bridge->context;
  {
    std::lock_guard<std::mutex> lock(g_gfxMutex);
    g_gfxBridges.erase(gfx);
  }
  if (bridge->surfaceCommand != nullptr)
    gfx->SurfaceCommand = bridge->surfaceCommand;
  if (bridge->endFrame != nullptr) gfx->EndFrame = bridge->endFrame;
  // gdi_graphics_pipeline_uninit() only releases what gdi_graphics_pipeline_init
  // allocated (gfx->custom points at the gdi and is cleared there).
  if (bridge->gdi != nullptr) gdi_graphics_pipeline_uninit(bridge->gdi, gfx);
  bridge->context = nullptr;
  bridge->gdi = nullptr;
  bridge->surfaceCommand = nullptr;
  bridge->endFrame = nullptr;
  diagWarn("rdpgfx: graphics pipeline detached");
}

void gfxChannelConnected(void* context, const ChannelConnectedEventArgs* e) {
  if (context == nullptr || e == nullptr || !isGfxChannelName(e->name)) return;
  RdpAttemptContext* attempt = attemptOf(static_cast<rdpContext*>(context));
  GfxBridge* bridge = attempt->gfx;
  if (bridge == nullptr || e->pInterface == nullptr) return;
  // One binding per attempt: a repeated event must not install the wrapper
  // twice (and this is what keeps the "channel connected" line single).
  if (bridge->context != nullptr) return;
  rdpGdi* gdi = attempt->common.context.gdi;
  if (!attempt->gdiReady || gdi == nullptr) {
    diagWarn("rdpgfx: channel connected before gdi_init, pipeline skipped");
    return;
  }
  RdpgfxClientContext* gfx = static_cast<RdpgfxClientContext*>(e->pInterface);
  if (!gdi_graphics_pipeline_init(gdi, gfx)) {
    diagWarn("rdpgfx: gdi_graphics_pipeline_init failed");
    return;
  }
  bridge->context = gfx;
  bridge->gdi = gdi;
  bridge->surfaceCommand = gfx->SurfaceCommand;
  bridge->endFrame = gfx->EndFrame;
  bridge->commands.store(0);
  bridge->lastCodec.store(0);
  bridge->lastStatsMs.store(monotonicMillis());
  bridge->endFrames.store(0);
  bridge->commits.store(0);
  bridge->bitmapUpdates.store(0);
  bridge->surfaceBits.store(0);
  {
    std::lock_guard<std::mutex> lock(g_gfxMutex);
    g_gfxBridges[gfx] = bridge;
  }
  gfx->SurfaceCommand = gfxSurfaceCommand;
  gfx->EndFrame = gfxEndFrame;
  // The channel name is included for the record; the "rdpgfx channel
  // connected" prefix is the marker the acceptance log grep uses.
  diagWarn(std::string("rdpgfx channel connected (") + e->name +
           "): gdi graphics pipeline active");
}

void gfxChannelDisconnected(void* context,
                            const ChannelDisconnectedEventArgs* e) {
  if (context == nullptr || e == nullptr || !isGfxChannelName(e->name)) return;
  RdpAttemptContext* attempt = attemptOf(static_cast<rdpContext*>(context));
  GfxBridge* bridge = attempt->gfx;
  if (bridge == nullptr || bridge->context == nullptr) return;
  if (static_cast<void*>(bridge->context) != e->pInterface) return;
  gfxUnbind(attempt);
}

// ---------------------------------------------------------------------------
// Audio playback (rdpsnd -> OHOS AudioRenderer)
//
// FreeRDP 3.9 has no client-facing rdpsnd callback interface: the channel
// resolves a "device plugin" through freerdp_load_channel_addin_entry() and
// this prefix only contains the built-in "fake" null sink. The backend
// therefore registers a static addin provider (installRdpsndAddinProvider)
// that answers the channel's "rdpsnd" + "tianguan" subsystem lookup with the
// device plugin below. preConnect() adds the channel as
// {"rdpsnd","sys:tianguan","channel:2"} to both the static and the dynamic
// channel collection: the arguments select that subsystem and pin rdpsnd's
// fixed_format to stereo only. "format" is deliberately not pinned to PCM
// anymore (it used to be "format:1"): that filter would reject every
// compressed format before this device ever saw it, and FreeRDP is now built
// WITH_FFMPEG (WITH_DSP_FFMPEG), so AAC/AAC-LATM can be decoded. Modern
// servers (Windows 8.1+) carry the audio over the AUDIO_PLAYBACK_DVC dynamic
// channels opened by drdynvc, whose instances parse the same arguments; if
// only the static collection had them, those instances would silently fall
// back to the built-in fake sink and no audio would ever be negotiated.
//
// Call flow: ServerFormatAnnounce() sees the server's format list,
// FormatSupported() accepts PCM 16-bit stereo plus compressed formats that
// freerdp_dsp_supports_format() can decode (and that the renderer can handle:
// 2 channels, 8k..192k), Open() receives the format the server selected, Play()
// decodes compressed data to PCM through freerdp_dsp_decode() (PCM passes
// through unchanged) and pushes it into a ring buffer, Close() stops the
// renderer. The decoder output is configured (freerdp_dsp_context_reset) with
// the same AUDIO_FORMAT that Open() got, so the PCM it produces always matches
// the renderer's rate/channels; libswresample does the decoder-side rate/
// channel conversion when the bitstream differs from the declared format.
// The OHOS NDK has no blocking OH_AudioRenderer_Write; playback is pulled by
// OH_AudioRenderer_OnWriteDataCallback, which fills the buffer from the ring
// and writes silence when the server is not sending audio, so a starved stream
// never stalls. Audio is strictly a side path: no failure in here is turned
// into a channel error, so it cannot break the picture or input.
// Note (checked against the dependencies): libavcodec is built with the AAC,
// AAC-LATM and ADPCM decoders, but FreeRDP's dsp_ffmpeg.c filters ADPCM/GSM/
// A-law/u-law out unless WITH_DSP_EXPERIMENTAL is defined, which this FreeRDP
// build leaves off. Of the compressed tags only WAVE_FORMAT_AAC_MS (and OPUS if
// the server offered it) actually reaches a decoder; the rest are rejected at
// negotiation and reported by the format announce line.
//
// Threading: rdpsnd dispatches its channel data on its own thread by default
// (FreeRDP_SynchronousStaticChannels is FALSE), while Close/Free arrive on the
// session thread during freerdp_disconnect(). The callbacks therefore never
// dereference the device object: it is only used as a map key into
// gRdpsndPlayers, and the shared player below owns every mutable field (one
// small lock for the ring, one for renderer start/stop).
// ---------------------------------------------------------------------------

// The addin provider below maps RDPSND_CHANNEL_NAME + this subsystem name to
// the device plugin, bypassing the dlopen() path client-common would take for
// an unknown subsystem.
const char kRdpsndSubsystem[] = "tianguan";
// PCM formats the renderer is allowed to open. The server normally offers
// 44.1 or 48 kHz 16-bit stereo PCM; other rates are still valid for
// OH_AudioRenderer, so accept the whole common range instead of narrowing the
// negotiation. The renderer always runs at the exact rate/channels of the
// format rdpsnd selected; for compressed formats the decoder is reset with that
// same format, so libswresample produces PCM in the renderer's layout.
const int kAudioMinRate = 8000;
const int kAudioMaxRate = 192000;
// Ring capacity = half a second of PCM, clamped. Dropping the oldest bytes when
// the ring is full keeps playback close to real time instead of accumulating
// latency when the server bursts (older Windows servers do not throttle).
const size_t kAudioRingMinBytes = 32 * 1024;
const size_t kAudioRingMaxBytes = 256 * 1024;
// "audio: N bytes played, underrun=X" is logged at most once per 5 s and at
// most 10 times per connection (plus one final line when audio did play).
const UINT64 kAudioStatsIntervalMs = 5000;
const int kAudioStatsMaxLines = 10;

struct RdpsndPlayer;

OH_AudioData_Callback_Result audioWriteData(OH_AudioRenderer* renderer,
                                            void* userData, void* audioData,
                                            int32_t audioDataSize);

// One negotiated PCM renderer (16-bit stereo) and the ring between the rdpsnd
// channel and the OHOS audio thread. Owned by gRdpsndPlayers through a
// shared_ptr: the write callback copies the shared_ptr from the registry into
// its call frame, so releasing the device can never free memory a callback is
// still using.
struct RdpsndPlayer {
  RdpsndPlayer()
      : readPos(0),
        writePos(0),
        used(0),
        capacity(0),
        rate(0),
        channels(0),
        renderer(nullptr),
        started(false),
        bytesPlayed(0),
        underruns(0),
        dropped(0),
        lastStatsMs(0),
        lastStatsBytes(0),
        statsLines(0),
        dsp(nullptr),
        dspOut(nullptr),
        sourceTag(WAVE_FORMAT_PCM),
        dspDecoded(false),
        dspLogged(false),
        decodedBytes(0),
        decodeFailures(0),
        discarded(0),
        lastDspLogMs(0) {}

  ~RdpsndPlayer() {
    stop();
    // No callback can be inside this object here: every callback takes a
    // shared_ptr from gRdpsndPlayers first, and Free() removed the map entry
    // before dropping the last reference.
    if (dsp != nullptr) freerdp_dsp_context_free(dsp);
    if (dspOut != nullptr) Stream_Free(dspOut, TRUE);
  }

  // Open a renderer for the format rdpsnd selected. For a compressed format the
  // decoder is (re)configured to produce PCM in that same rate/channel layout.
  // Replaces any previous renderer/decoder.
  void start(const AUDIO_FORMAT& format);
  // Stop and release the renderer and drop buffered audio. Idempotent.
  void stop() {
    std::lock_guard<std::mutex> lifecycleLock(lifecycle);
    stopLocked();
  }
  // rdpsnd Play: append PCM, decoding a compressed payload first, without ever
  // blocking the channel thread for more than the ring/decode locks.
  void push(const uint8_t* data, size_t size);
  // OHOS write callback: fill out[0,size) with PCM, silence when starved.
  size_t fill(uint8_t* out, size_t size);

 private:
  void startRenderer();
  void pushPcm(const uint8_t* data, size_t size);
  void stopLocked();
  void logStats(bool force);

  // Serializes renderer creation against Stop()/Free() on another thread.
  std::mutex lifecycle;
  // Guards the ring and the renderer fields the write callback reads.
  std::mutex lock;
  std::vector<uint8_t> ring;
  size_t readPos;
  size_t writePos;
  size_t used;
  size_t capacity;
  int rate;
  int channels;
  OH_AudioRenderer* renderer;
  bool started;
  std::atomic<uint64_t> bytesPlayed;
  std::atomic<uint64_t> underruns;
  std::atomic<uint64_t> dropped;
  uint64_t lastStatsMs;
  uint64_t lastStatsBytes;
  int statsLines;
  // Guards the decoder state below. Play runs on the rdpsnd channel thread,
  // start()/stop() on the session thread, so a reset must not race a decode.
  std::mutex decodeLock;
  FREERDP_DSP_CONTEXT* dsp;
  wStream* dspOut;
  AUDIO_FORMAT sourceFormat;  // format of the stream Play() currently receives
  UINT16 sourceTag;
  bool dspDecoded;  // source is compressed and the decoder is ready
  bool dspLogged;   // the "dsp: ... -> PCM ok" line was already printed
  std::atomic<uint64_t> decodedBytes;
  std::atomic<uint64_t> decodeFailures;
  std::atomic<uint64_t> discarded;  // PCM dropped because no renderer is open
  uint64_t lastDspLogMs;
};

OH_AudioData_Callback_Result audioWriteData(OH_AudioRenderer* renderer,
                                            void* userData, void* audioData,
                                            int32_t audioDataSize) {
  (void)renderer;
  RdpsndPlayer* player = static_cast<RdpsndPlayer*>(userData);
  if (player == nullptr || audioData == nullptr || audioDataSize <= 0)
    return AUDIO_DATA_CALLBACK_RESULT_INVALID;
  player->fill(static_cast<uint8_t*>(audioData),
               static_cast<size_t>(audioDataSize));
  // Silence is valid data: never report AUDIO_DATA_CALLBACK_RESULT_INVALID,
  // which would make the framework skip the buffer and log its own errors.
  return AUDIO_DATA_CALLBACK_RESULT_VALID;
}

void RdpsndPlayer::start(const AUDIO_FORMAT& format) {
  std::lock_guard<std::mutex> lifecycleLock(lifecycle);
  // A format change (or a re-open after SNDC_CLOSE) always restarts cleanly.
  stopLocked();
  const int sampleRate = static_cast<int>(format.nSamplesPerSec);
  const int channelCount = static_cast<int>(format.nChannels);
  char line[240];
  {
    // Reset the decode state first: if the format is unusable below, Play must
    // not keep decoding the previous stream into a dead ring.
    std::lock_guard<std::mutex> guard(decodeLock);
    sourceFormat = format;
    sourceTag = format.wFormatTag;
    dspDecoded = false;
    dspLogged = false;
    decodedBytes.store(0);
    decodeFailures.store(0);
    discarded.store(0);
    lastDspLogMs = 0;
  }
  if (sampleRate < kAudioMinRate || sampleRate > kAudioMaxRate ||
      channelCount != 2) {
    std::snprintf(line, sizeof(line),
                  "audio: unsupported format %dHz %dch, stereo 8k..192k only",
                  sampleRate, channelCount);
    diagWarn(line);
    return;
  }
  if (format.wFormatTag != WAVE_FORMAT_PCM) {
    // Compressed payload: let FreeRDP's DSP (dsp_ffmpeg.c) decode it. The
    // context is reset with the very format rdpsnd selected, so the decoder
    // output is S16LE at this same rate/channels; libswresample handles a
    // bitstream whose real layout differs. If the decoder cannot be opened the
    // player stays in "drop" mode and the session keeps running silently.
    bool ready = false;
    {
      std::lock_guard<std::mutex> guard(decodeLock);
      if (dsp == nullptr) dsp = freerdp_dsp_context_new(FALSE);
      if (dspOut == nullptr) dspOut = Stream_New(nullptr, 4096);
      if (dsp != nullptr && dspOut != nullptr &&
          freerdp_dsp_context_reset(dsp, &format, 0)) {
        dspDecoded = true;
        ready = true;
      }
    }
    if (!ready) {
      std::snprintf(line, sizeof(line),
                    "dsp: no decoder for tag=%u (0x%04x), staying silent",
                    static_cast<unsigned>(format.wFormatTag),
                    static_cast<unsigned>(format.wFormatTag));
      diagWarn(line);
    }
  }
  size_t ringBytes = static_cast<size_t>(sampleRate) *
                     static_cast<size_t>(channelCount) * 2u / 2u;
  ringBytes = std::max<size_t>(kAudioRingMinBytes,
                               std::min<size_t>(kAudioRingMaxBytes, ringBytes));
  ringBytes &= ~static_cast<size_t>(3);
  {
    std::lock_guard<std::mutex> guard(lock);
    rate = sampleRate;
    channels = channelCount;
    capacity = ringBytes;
    ring.assign(capacity, 0);
    readPos = 0;
    writePos = 0;
    used = 0;
    lastStatsMs = monotonicMillis();
    lastStatsBytes = bytesPlayed.load();
  }
  startRenderer();
}

void RdpsndPlayer::stopLocked() {
  OH_AudioRenderer* previous = nullptr;
  {
    std::lock_guard<std::mutex> guard(lock);
    started = false;
    previous = renderer;
    renderer = nullptr;
  }
  if (previous != nullptr) {
    char line[200];
    OH_AudioStream_Result result = OH_AudioRenderer_Stop(previous);
    if (result != AUDIOSTREAM_SUCCESS) {
      std::snprintf(line, sizeof(line),
                    "audio: OH_AudioRenderer_Stop failed [%d]",
                    static_cast<int>(result));
      diagWarn(line);
    }
    result = OH_AudioRenderer_Release(previous);
    if (result != AUDIOSTREAM_SUCCESS) {
      std::snprintf(line, sizeof(line),
                    "audio: OH_AudioRenderer_Release failed [%d]",
                    static_cast<int>(result));
      diagWarn(line);
    }
    // Release() ends the write callback; taking the data lock once waits for a
    // callback that was already running when the renderer went away.
    {
      std::lock_guard<std::mutex> guard(lock);
    }
  }
  {
    std::lock_guard<std::mutex> guard(lock);
    ring.clear();
    capacity = 0;
    readPos = 0;
    writePos = 0;
    used = 0;
  }
  logStats(true);
}

void RdpsndPlayer::push(const uint8_t* data, size_t size) {
  if (data == nullptr || size == 0) return;
  std::lock_guard<std::mutex> guard(decodeLock);
  if (!dspDecoded) {
    // Uncompressed PCM: the ring is fed exactly like before.
    pushPcm(data, size);
    return;
  }
  if (dsp == nullptr || dspOut == nullptr) {
    discarded.fetch_add(size);
    return;
  }
  char line[220];
  Stream_SetPosition(dspOut, 0);
  if (!freerdp_dsp_decode(dsp, &sourceFormat, data, size, dspOut)) {
    // A decode failure only costs this packet. Count it and log at most once
    // per kAudioStatsIntervalMs so a broken stream cannot flood hilog.
    const uint64_t failures = decodeFailures.fetch_add(1) + 1;
    const UINT64 now = monotonicMillis();
    if (now >= lastDspLogMs + kAudioStatsIntervalMs) {
      lastDspLogMs = now;
      std::snprintf(line, sizeof(line),
                    "dsp: decode failed tag=%u (0x%04x), failures=%llu, "
                    "%zu compressed bytes",
                    static_cast<unsigned>(sourceTag),
                    static_cast<unsigned>(sourceTag),
                    static_cast<unsigned long long>(failures), size);
      diagWarn(line);
    }
    return;
  }
  const size_t decoded = Stream_GetPosition(dspOut);
  // An AAC packet can decode to zero samples while the decoder primes; that is
  // not a failure and there is nothing to enqueue yet.
  if (decoded == 0) return;
  decodedBytes.fetch_add(decoded);
  if (!dspLogged) {
    // First successful decode of this connection: the acceptance log line.
    dspLogged = true;
    std::snprintf(line, sizeof(line), "dsp: %u (0x%04x) -> PCM ok, %zu bytes",
                  static_cast<unsigned>(sourceTag),
                  static_cast<unsigned>(sourceTag), decoded);
    diagWarn(line);
  }
  // The decoder was reset with the Open() format, so this is S16LE at the
  // renderer's rate/channels by construction; push it like server PCM.
  pushPcm(Stream_Buffer(dspOut), decoded);
}

void RdpsndPlayer::pushPcm(const uint8_t* data, size_t size) {
  if (data == nullptr || size == 0) return;
  std::lock_guard<std::mutex> guard(lock);
  if (!started || capacity == 0) {
    // No renderer (should not happen for an accepted format): count the audio
    // that had to be thrown away instead of pretending it played.
    discarded.fetch_add(size);
    return;
  }
  if (size > capacity) {
    // A single block larger than the ring: keep the newest PCM only.
    const size_t excess = size - capacity;
    dropped.fetch_add(excess);
    data += excess;
    size = capacity;
  }
  const size_t freeBytes = capacity - used;
  if (size > freeBytes) {
    // Drop the oldest buffered audio so playback stays close to real time.
    const size_t dropFront = size - freeBytes;
    dropped.fetch_add(dropFront);
    readPos = (readPos + dropFront) % capacity;
    used -= dropFront;
  }
  const size_t first = std::min(size, capacity - writePos);
  std::memcpy(&ring[writePos], data, first);
  if (size > first) std::memcpy(&ring[0], data + first, size - first);
  writePos = (writePos + size) % capacity;
  used += size;
}

size_t RdpsndPlayer::fill(uint8_t* out, size_t size) {
  if (out == nullptr || size == 0) return 0;
  size_t real = 0;
  {
    std::lock_guard<std::mutex> guard(lock);
    if (started && used > 0) {
      real = std::min(size, used);
      const size_t first = std::min(real, capacity - readPos);
      std::memcpy(out, &ring[readPos], first);
      if (real > first) std::memcpy(out + first, &ring[0], real - first);
      readPos = (readPos + real) % capacity;
      used -= real;
      bytesPlayed.fetch_add(real);
    }
    if (real < size) std::memset(out + real, 0, size - real);
  }
  if (real < size) underruns.fetch_add(1);
  logStats(false);
  return real;
}

void RdpsndPlayer::startRenderer() {
  int sampleRate = 0;
  int channelCount = 0;
  {
    std::lock_guard<std::mutex> guard(lock);
    sampleRate = rate;
    channelCount = channels;
  }
  char line[240];
  OH_AudioStreamBuilder* builder = nullptr;
  OH_AudioStream_Result result =
      OH_AudioStreamBuilder_Create(&builder, AUDIOSTREAM_TYPE_RENDERER);
  if (result != AUDIOSTREAM_SUCCESS || builder == nullptr) {
    std::snprintf(line, sizeof(line),
                  "audio: OH_AudioStreamBuilder_Create failed [%d]",
                  static_cast<int>(result));
    diagWarn(line);
    return;
  }
  // The exact NDK call sequence of the renderer: rate, channels, S16LE, raw
  // PCM, latency mode, usage, then the pull callback that owns the audio
  // thread. Every step is checked and logged; a failure only costs audio.
  struct Step {
    const char* name;
    OH_AudioStream_Result result;
  };
  const Step steps[] = {
      {"SetSamplingRate",
       OH_AudioStreamBuilder_SetSamplingRate(builder, sampleRate)},
      {"SetChannelCount",
       OH_AudioStreamBuilder_SetChannelCount(builder, channelCount)},
      {"SetSampleFormat",
       OH_AudioStreamBuilder_SetSampleFormat(builder, AUDIOSTREAM_SAMPLE_S16LE)},
      {"SetEncodingType", OH_AudioStreamBuilder_SetEncodingType(
                              builder, AUDIOSTREAM_ENCODING_TYPE_RAW)},
      {"SetLatencyMode", OH_AudioStreamBuilder_SetLatencyMode(
                             builder, AUDIOSTREAM_LATENCY_MODE_NORMAL)},
      {"SetRendererInfo",
       OH_AudioStreamBuilder_SetRendererInfo(builder, AUDIOSTREAM_USAGE_MUSIC)},
      {"SetRendererWriteDataCallback",
       OH_AudioStreamBuilder_SetRendererWriteDataCallback(builder,
                                                         audioWriteData, this)},
  };
  for (const Step& step : steps) {
    if (step.result == AUDIOSTREAM_SUCCESS) continue;
    std::snprintf(line, sizeof(line), "audio: OH_AudioStreamBuilder_%s failed [%d]",
                  step.name, static_cast<int>(step.result));
    diagWarn(line);
    (void)OH_AudioStreamBuilder_Destroy(builder);
    return;
  }
  OH_AudioRenderer* created = nullptr;
  result = OH_AudioStreamBuilder_GenerateRenderer(builder, &created);
  (void)OH_AudioStreamBuilder_Destroy(builder);
  if (result != AUDIOSTREAM_SUCCESS || created == nullptr) {
    std::snprintf(line, sizeof(line),
                  "audio: OH_AudioStreamBuilder_GenerateRenderer failed [%d]",
                  static_cast<int>(result));
    diagWarn(line);
    if (created != nullptr) (void)OH_AudioRenderer_Release(created);
    return;
  }
  result = OH_AudioRenderer_Start(created);
  if (result != AUDIOSTREAM_SUCCESS) {
    std::snprintf(line, sizeof(line), "audio: OH_AudioRenderer_Start failed [%d]",
                  static_cast<int>(result));
    diagWarn(line);
    (void)OH_AudioRenderer_Release(created);
    return;
  }
  {
    std::lock_guard<std::mutex> guard(lock);
    renderer = created;
    started = true;
    lastStatsMs = monotonicMillis();
    lastStatsBytes = bytesPlayed.load();
  }
  std::snprintf(line, sizeof(line), "audio: renderer started %dHz %dch S16LE",
                sampleRate, channelCount);
  diagWarn(line);
}

void RdpsndPlayer::logStats(bool force) {
  const uint64_t now = monotonicMillis();
  char line[240];
  {
    std::lock_guard<std::mutex> guard(lock);
    if (!force && statsLines >= kAudioStatsMaxLines) return;
    if (!force && now < lastStatsMs + kAudioStatsIntervalMs) return;
    const uint64_t played = bytesPlayed.load();
    // Nothing was ever pulled by the renderer: no stats line yet.
    if (force && played == 0) return;
    const uint64_t delta = played - lastStatsBytes;
    const uint64_t elapsed = now > lastStatsMs ? now - lastStatsMs : 0;
    const uint64_t ratePerSecond = elapsed > 0 ? delta * 1000u / elapsed : 0;
    const uint64_t under = underruns.load();
    const uint64_t lost = dropped.load();
    const uint64_t decoded = decodedBytes.load();
    const uint64_t dspFailures = decodeFailures.load();
    const uint64_t lostDiscarded = discarded.load();
    const unsigned sampleRate = static_cast<unsigned>(rate);
    const unsigned channelCount = static_cast<unsigned>(channels);
    lastStatsMs = now;
    lastStatsBytes = played;
    ++statsLines;
    std::snprintf(
        line, sizeof(line),
        "audio: %llu bytes played (rate=%llu B/s), underrun=%llu, dropped=%llu, "
        "%uHz %uch, decoded=%llu, dspfail=%llu, discarded=%llu",
        static_cast<unsigned long long>(played),
        static_cast<unsigned long long>(ratePerSecond),
        static_cast<unsigned long long>(under),
        static_cast<unsigned long long>(lost), sampleRate, channelCount,
        static_cast<unsigned long long>(decoded),
        static_cast<unsigned long long>(dspFailures),
        static_cast<unsigned long long>(lostDiscarded));
  }
  diagDebug(line);
}

// The device object itself carries no state: it is the key the rdpsnd channel
// hands back to the callbacks. All state lives in the shared player, so a Play
// racing Free() only ever looks up a key (it never dereferences the freed
// struct) and finds nothing.
struct RdpsndDevice {
  rdpsndDevicePlugin base;
};

static_assert(offsetof(RdpsndDevice, base) == 0,
              "RdpsndDevice.base must stay first: Free() casts it back");

std::mutex gRdpsndLock;
std::map<rdpsndDevicePlugin*, std::shared_ptr<RdpsndPlayer>> gRdpsndPlayers;

std::shared_ptr<RdpsndPlayer> rdpsndPlayerFor(rdpsndDevicePlugin* device) {
  if (device == nullptr) return nullptr;
  std::lock_guard<std::mutex> guard(gRdpsndLock);
  const auto it = gRdpsndPlayers.find(device);
  if (it == gRdpsndPlayers.end()) return nullptr;
  return it->second;
}

BOOL rdpsndFormatSupported(rdpsndDevicePlugin* device, const AUDIO_FORMAT* format) {
  (void)device;
  if (format == nullptr) return FALSE;
  // The renderer can only open 2-channel streams in this rate range; anything
  // else is left out of the reply so the server never selects it.
  if (format->nChannels != 2 ||
      format->nSamplesPerSec < static_cast<UINT32>(kAudioMinRate) ||
      format->nSamplesPerSec > static_cast<UINT32>(kAudioMaxRate))
    return FALSE;
  if (format->wFormatTag == WAVE_FORMAT_PCM) {
    // Uncompressed: the renderer plays S16LE directly, so only 16-bit PCM.
    return format->wBitsPerSample == 16 ? TRUE : FALSE;
  }
  // Compressed: accept exactly what this FreeRDP build's DSP can decode with
  // libavcodec (AAC/AAC-LATM reach a decoder; ADPCM/GSM/A-law/u-law are
  // filtered out by dsp_ffmpeg.c because WITH_DSP_EXPERIMENTAL is off). Play()
  // then decodes each packet with freerdp_dsp_decode().
  return freerdp_dsp_supports_format(format, FALSE) ? TRUE : FALSE;
}

UINT32 rdpsndGetVolume(rdpsndDevicePlugin* device) {
  (void)device;
  return 0xFFFFFFFFu;  // TSSNDCAPS_VOLUME capabilities: full volume
}

BOOL rdpsndSetVolume(rdpsndDevicePlugin* device, UINT32 value) {
  (void)device;
  (void)value;
  return TRUE;  // the OHOS stream follows the system volume
}

BOOL rdpsndOpen(rdpsndDevicePlugin* device, const AUDIO_FORMAT* format,
                UINT32 latency) {
  (void)latency;
  if (format == nullptr) return FALSE;
  char line[240];
  // "rdpsnd format selected: ..." is the acceptance marker for negotiation.
  // tag + decode mode are appended so the log shows whether audio arrived as
  // PCM or as a compressed payload that Play() hands to the DSP.
  std::snprintf(line, sizeof(line),
                "rdpsnd format selected: %uHz %uch %ubit tag=%u (0x%04x), %s",
                static_cast<unsigned>(format->nSamplesPerSec),
                static_cast<unsigned>(format->nChannels),
                static_cast<unsigned>(format->wBitsPerSample),
                static_cast<unsigned>(format->wFormatTag),
                static_cast<unsigned>(format->wFormatTag),
                format->wFormatTag == WAVE_FORMAT_PCM ? "PCM passthrough"
                                                      : "DSP decode");
  diagWarn(line);
  std::shared_ptr<RdpsndPlayer> player = rdpsndPlayerFor(device);
  if (player != nullptr) player->start(*format);
  // Audio is a side path: a renderer failure must not fail the channel, so
  // Open always reports success and the session keeps running silently.
  return TRUE;
}

UINT rdpsndPlay(rdpsndDevicePlugin* device, const BYTE* data, size_t size) {
  std::shared_ptr<RdpsndPlayer> player = rdpsndPlayerFor(device);
  if (player != nullptr)
    player->push(reinterpret_cast<const uint8_t*>(data), size);
  return CHANNEL_RC_OK;
}

void rdpsndClose(rdpsndDevicePlugin* device) {
  std::shared_ptr<RdpsndPlayer> player = rdpsndPlayerFor(device);
  if (player != nullptr) player->stop();
}

void rdpsndFree(rdpsndDevicePlugin* device) {
  if (device == nullptr) return;
  std::shared_ptr<RdpsndPlayer> player;
  {
    std::lock_guard<std::mutex> guard(gRdpsndLock);
    const auto it = gRdpsndPlayers.find(device);
    if (it != gRdpsndPlayers.end()) {
      player = it->second;
      gRdpsndPlayers.erase(it);
    }
  }
  if (player != nullptr) {
    player->stop();
    player.reset();  // destroyed here, after the renderer is gone
  }
  delete reinterpret_cast<RdpsndDevice*>(device);
}

BOOL rdpsndDefaultFormat(rdpsndDevicePlugin* device, const AUDIO_FORMAT* desired,
                         AUDIO_FORMAT* defaultFormat) {
  (void)device;
  (void)desired;
  (void)defaultFormat;
  // No fallback conversion: a format that FormatSupported() rejected is also
  // left out of the client's format list, so rdpsnd can never select it and
  // this callback stays unused. Returning FALSE keeps FreeRDP's PCM default.
  return FALSE;
}

UINT rdpsndServerFormatAnnounce(rdpsndDevicePlugin* device,
                                const AUDIO_FORMAT* formats, size_t count) {
  (void)device;
  size_t usablePcm = 0;
  size_t usableCompressed = 0;
  if (formats != nullptr) {
    for (size_t index = 0; index < count; ++index) {
      if (rdpsndFormatSupported(nullptr, &formats[index]) != TRUE) continue;
      if (formats[index].wFormatTag == WAVE_FORMAT_PCM)
        ++usablePcm;
      else
        ++usableCompressed;
    }
  }
  const size_t usable = usablePcm + usableCompressed;
  char line[240];
  if (count == 0 || formats == nullptr) {
    std::snprintf(line, sizeof(line),
                  "rdpsnd: server offered no audio formats, staying silent");
  } else if (usable == 0) {
    // Kept behaviour: nothing playable -> stay silent and say why.
    std::snprintf(line, sizeof(line),
                  "rdpsnd: no usable format offered (%zu format(s); first tag=%u "
                  "%uHz %uch %ubit), staying silent",
                  count, static_cast<unsigned>(formats[0].wFormatTag),
                  static_cast<unsigned>(formats[0].nSamplesPerSec),
                  static_cast<unsigned>(formats[0].nChannels),
                  static_cast<unsigned>(formats[0].wBitsPerSample));
  } else {
    std::snprintf(line, sizeof(line),
                  "rdpsnd: server offered %zu format(s), %zu usable PCM 16-bit "
                  "stereo + %zu decodable compressed (2ch)",
                  count, usablePcm, usableCompressed);
  }
  diagWarn(line);
  return CHANNEL_RC_OK;
}

// rdpsnd device subsystem entry, returned by the addin provider below. The
// channel calls this while handling CHANNEL_EVENT_CONNECTED; from here on the
// channel owns the device pointer and calls the callbacks above.
extern "C" UINT VCAPITYPE tianguanRdpsndDeviceEntry(
    PFREERDP_RDPSND_DEVICE_ENTRY_POINTS pEntryPoints) {
  if (pEntryPoints == nullptr || pEntryPoints->pRegisterRdpsndDevice == nullptr)
    return CHANNEL_RC_INITIALIZATION_ERROR;
  RdpsndDevice* device = new (std::nothrow) RdpsndDevice();
  if (device == nullptr) return CHANNEL_RC_NO_MEMORY;
  std::memset(&device->base, 0, sizeof(device->base));
  device->base.FormatSupported = rdpsndFormatSupported;
  device->base.Open = rdpsndOpen;
  device->base.GetVolume = rdpsndGetVolume;
  device->base.SetVolume = rdpsndSetVolume;
  device->base.Play = rdpsndPlay;
  device->base.Close = rdpsndClose;
  device->base.Free = rdpsndFree;
  device->base.DefaultFormat = rdpsndDefaultFormat;
  device->base.ServerFormatAnnounce = rdpsndServerFormatAnnounce;
  std::shared_ptr<RdpsndPlayer> player(new (std::nothrow) RdpsndPlayer());
  if (player == nullptr) {
    delete device;
    return CHANNEL_RC_NO_MEMORY;
  }
  {
    std::lock_guard<std::mutex> guard(gRdpsndLock);
    gRdpsndPlayers[&device->base] = player;
  }
  pEntryPoints->pRegisterRdpsndDevice(pEntryPoints->rdpsnd, &device->base);
  diagWarn("rdpsnd channel connected");
  return CHANNEL_RC_OK;
}

// Static addin provider: client-common installs its resolver
// (freerdp_channels_load_static_addin_entry) at every client context creation.
// Replacing it with this thin wrapper lets the rdpsnd channel resolve the
// "tianguan" device subsystem while every other channel lookup (including
// rdpsnd itself and the rdpdr addin it drags in) is delegated unchanged.
PVIRTUALCHANNELENTRY tianguanAddinProvider(LPCSTR name, LPCSTR subsystem,
                                           LPCSTR type, DWORD flags) {
  if (name != nullptr && subsystem != nullptr &&
      std::strcmp(name, RDPSND_CHANNEL_NAME) == 0 &&
      std::strcmp(subsystem, kRdpsndSubsystem) == 0) {
    return WINPR_FUNC_PTR_CAST(&tianguanRdpsndDeviceEntry, PVIRTUALCHANNELENTRY);
  }
  return freerdp_channels_load_static_addin_entry(name, subsystem, type, flags);
}

void installRdpsndAddinProvider() {
  freerdp_register_addin_provider(tianguanAddinProvider, 0);
}

// ---------------------------------------------------------------------------
// Clipboard redirection (cliprdr, CF_UNICODETEXT only)
//
// The channel is a static addin; client-common loads it at pre-connect when
// RedirectClipboard is TRUE and opens it during freerdp_connect(). The
// CliprdrClientContext* is the addin's pInterface, retrievable through the
// public channel manager API after the connect call returned. Its Client*
// function pointers (assigned by the addin itself) send PDUs to the server;
// the Server*/MonitorReady fields below are the callbacks the client owns.
// Every callback runs on the session thread inside
// freerdp_check_event_handles(), so no extra locking is needed around the
// bridge state.
// ---------------------------------------------------------------------------

struct CliprdrBridge {
  CliprdrClientContext* context = nullptr;
  RdpAttemptContext* attempt = nullptr;
  // Latest local clipboard text (UTF-8), already stripped/capped. It is what
  // the next ServerFormatDataRequest answers with; it is never filled from the
  // remote side, so a remote copy is not echoed back.
  std::string localText;
  // MonitorReady received: the channel now accepts client -> server PDUs.
  bool ready = false;
};

// Drop a UTF-8 sequence that the 1 MiB cut split in half (same rule as the VNC
// path's storeClipboard, which also has this helper).
void trimIncompleteUtf8(std::string& text) {
  size_t start = text.size();
  while (start > 0 && (static_cast<unsigned char>(text[start - 1]) & 0xc0) == 0x80)
    --start;
  if (start == 0) return;
  const unsigned char lead = static_cast<unsigned char>(text[start - 1]);
  size_t needed = 0;
  if ((lead & 0x80) == 0) return;
  if ((lead & 0xe0) == 0xc0) needed = 1;
  else if ((lead & 0xf0) == 0xe0) needed = 2;
  else if ((lead & 0xf8) == 0xf0) needed = 3;
  else {
    text.resize(start - 1);
    return;
  }
  if (text.size() - start < needed) text.resize(start - 1);
}

// Copy one outbound text item into the backend's local clipboard slot. The
// queued text is NUL-terminated (setClipboard already removed embedded NULs)
// and bounded by the same 1 MiB limit as the VNC clipboard state.
void setLocalClipboardText(const char* text, std::string& out) {
  out.clear();
  if (text == nullptr) return;
  size_t length = std::strlen(text);
  bool truncated = false;
  if (length > kClipboardLimit) {
    length = kClipboardLimit;
    truncated = true;
  }
  out.assign(text, length);
  if (truncated) trimIncompleteUtf8(out);
}

UINT cliprdrSendCapabilities(CliprdrBridge& bridge) {
  CliprdrClientContext* context = bridge.context;
  if (context == nullptr || context->ClientCapabilities == nullptr)
    return CHANNEL_RC_OK;
  CLIPRDR_CAPABILITIES capabilities;
  CLIPRDR_GENERAL_CAPABILITY_SET general;
  std::memset(&general, 0, sizeof(general));
  std::memset(&capabilities, 0, sizeof(capabilities));
  capabilities.cCapabilitiesSets = 1;
  capabilities.capabilitySets =
      reinterpret_cast<CLIPRDR_CAPABILITY_SET*>(&general);
  general.capabilitySetType = CB_CAPSTYPE_GENERAL;
  general.capabilitySetLength = CB_CAPSTYPE_GENERAL_LEN;
  general.version = CB_CAPS_VERSION_2;
  general.generalFlags = 0;  // no file clip, no long format names
  return context->ClientCapabilities(context, &capabilities);
}

UINT cliprdrSendFormatList(CliprdrBridge& bridge) {
  CliprdrClientContext* context = bridge.context;
  if (context == nullptr || context->ClientFormatList == nullptr)
    return CHANNEL_RC_OK;
  CLIPRDR_FORMAT format;
  CLIPRDR_FORMAT_LIST formatList;
  std::memset(&format, 0, sizeof(format));
  std::memset(&formatList, 0, sizeof(formatList));
  format.formatId = CF_UNICODETEXT;
  format.formatName = nullptr;
  formatList.numFormats = 1;
  formatList.formats = &format;
  return context->ClientFormatList(context, &formatList);
}

UINT cliprdrSendLocalText(CliprdrBridge& bridge, UINT32 formatId) {
  CliprdrClientContext* context = bridge.context;
  if (context == nullptr || context->ClientFormatDataResponse == nullptr)
    return CHANNEL_RC_OK;
  CLIPRDR_FORMAT_DATA_RESPONSE response;
  std::memset(&response, 0, sizeof(response));
  response.common.msgType = CB_FORMAT_DATA_RESPONSE;
  WCHAR* wide = nullptr;
  size_t width = 0;
  if (formatId == CF_UNICODETEXT && !bridge.localText.empty())
    wide = ConvertUtf8NToWCharAlloc(bridge.localText.c_str(),
                                    bridge.localText.size(), &width);
  if (wide == nullptr || width == 0) {
    std::free(wide);
    // Nothing to paste: an explicit failure response is what the reference
    // clients send when the requested format is not available.
    response.common.msgFlags = CB_RESPONSE_FAIL;
    response.common.dataLen = 0;
    response.requestedFormatData = nullptr;
    const UINT error = context->ClientFormatDataResponse(context, &response);
    if (error != CHANNEL_RC_OK)
      diagWarn("cliprdr: empty CF_UNICODETEXT response failed [" +
               std::to_string(error) + "]");
    else if (formatId != CF_UNICODETEXT)
      diagWarn("cliprdr: refused unsupported format id " +
               std::to_string(formatId));
    return CHANNEL_RC_OK;
  }
  // CF_UNICODETEXT is a NUL-terminated UTF-16LE string, so the trailing 0x0000
  // is part of the payload.
  const size_t bytes = (width + 1) * sizeof(WCHAR);
  if (bytes > 0xffffffffu) {
    std::free(wide);
    diagWarn("cliprdr: local text exceeds the cliprdr payload limit");
    return CHANNEL_RC_OK;
  }
  response.common.msgFlags = CB_RESPONSE_OK;
  response.common.dataLen = static_cast<UINT32>(bytes);
  response.requestedFormatData = reinterpret_cast<const BYTE*>(wide);
  const UINT error = context->ClientFormatDataResponse(context, &response);
  if (error != CHANNEL_RC_OK)
    diagWarn("cliprdr: CF_UNICODETEXT response failed [" +
             std::to_string(error) + "], " + std::to_string(bytes) + " bytes");
  else
    diagWarn("cliprdr: sent CF_UNICODETEXT (" + std::to_string(bytes) +
             " bytes)");
  std::free(wide);
  return error;
}

UINT cliprdrMonitorReady(CliprdrClientContext* context,
                         const CLIPRDR_MONITOR_READY* monitorReady) {
  (void)monitorReady;
  if (context == nullptr) return CHANNEL_RC_OK;
  CliprdrBridge* bridge = static_cast<CliprdrBridge*>(context->custom);
  if (bridge == nullptr) return CHANNEL_RC_OK;
  bridge->ready = true;
  UINT error = cliprdrSendCapabilities(*bridge);
  if (error != CHANNEL_RC_OK) {
    diagWarn("cliprdr: sending client capabilities failed [" +
             std::to_string(error) + "]");
    return error;
  }
  error = cliprdrSendFormatList(*bridge);
  if (error != CHANNEL_RC_OK) {
    diagWarn("cliprdr: sending CF_UNICODETEXT format list failed [" +
             std::to_string(error) + "]");
    return error;
  }
  diagWarn("cliprdr: monitor ready, advertised CF_UNICODETEXT (local bytes " +
           std::to_string(bridge->localText.size()) + ")");
  return CHANNEL_RC_OK;
}

UINT cliprdrServerFormatList(CliprdrClientContext* context,
                             const CLIPRDR_FORMAT_LIST* formatList) {
  if (context == nullptr || formatList == nullptr) return CHANNEL_RC_OK;
  CliprdrBridge* bridge = static_cast<CliprdrBridge*>(context->custom);
  if (bridge == nullptr) return CHANNEL_RC_OK;
  // Acknowledge the list even when every format is ignored: the server waits
  // for this response before it treats the clipboard as handed over.
  if (context->ClientFormatListResponse != nullptr) {
    CLIPRDR_FORMAT_LIST_RESPONSE response;
    std::memset(&response, 0, sizeof(response));
    response.common.msgType = CB_FORMAT_LIST_RESPONSE;
    response.common.msgFlags = CB_RESPONSE_OK;
    const UINT error = context->ClientFormatListResponse(context, &response);
    if (error != CHANNEL_RC_OK)
      diagWarn("cliprdr: format list response failed [" +
               std::to_string(error) + "]");
  }
  bool hasText = false;
  if (formatList->formats != nullptr) {
    for (UINT32 i = 0; i < formatList->numFormats; ++i) {
      if (formatList->formats[i].formatId == CF_UNICODETEXT) {
        hasText = true;
        break;
      }
    }
  }
  if (!hasText || context->ClientFormatDataRequest == nullptr)
    return CHANNEL_RC_OK;
  if (bridge->attempt == nullptr || hostStale(bridge->attempt))
    return CHANNEL_RC_OK;
  CLIPRDR_FORMAT_DATA_REQUEST request;
  std::memset(&request, 0, sizeof(request));
  request.common.msgType = CB_FORMAT_DATA_REQUEST;
  request.requestedFormatId = CF_UNICODETEXT;
  const UINT error = context->ClientFormatDataRequest(context, &request);
  if (error != CHANNEL_RC_OK) {
    diagWarn("cliprdr: CF_UNICODETEXT data request failed [" +
             std::to_string(error) + "]");
    return error;
  }
  diagWarn("cliprdr: server format list has CF_UNICODETEXT (" +
           std::to_string(formatList->numFormats) + " formats), data requested");
  return CHANNEL_RC_OK;
}

UINT cliprdrServerFormatDataRequest(
    CliprdrClientContext* context,
    const CLIPRDR_FORMAT_DATA_REQUEST* formatDataRequest) {
  if (context == nullptr || formatDataRequest == nullptr)
    return CHANNEL_RC_OK;
  CliprdrBridge* bridge = static_cast<CliprdrBridge*>(context->custom);
  if (bridge == nullptr) return CHANNEL_RC_OK;
  if (bridge->attempt == nullptr || hostStale(bridge->attempt))
    return CHANNEL_RC_OK;
  return cliprdrSendLocalText(*bridge, formatDataRequest->requestedFormatId);
}

UINT cliprdrServerFormatDataResponse(
    CliprdrClientContext* context,
    const CLIPRDR_FORMAT_DATA_RESPONSE* formatDataResponse) {
  if (context == nullptr || formatDataResponse == nullptr)
    return CHANNEL_RC_OK;
  CliprdrBridge* bridge = static_cast<CliprdrBridge*>(context->custom);
  if (bridge == nullptr) return CHANNEL_RC_OK;
  const UINT32 dataLen = formatDataResponse->common.dataLen;
  if (formatDataResponse->common.msgFlags != CB_RESPONSE_OK) {
    diagWarn("cliprdr: remote text response failed, flags " +
             std::to_string(formatDataResponse->common.msgFlags));
    return CHANNEL_RC_OK;
  }
  if (formatDataResponse->requestedFormatData == nullptr || dataLen == 0)
    return CHANNEL_RC_OK;
  if (dataLen > kMaxClipboardWireBytes) {
    diagWarn("cliprdr: remote text too large (" + std::to_string(dataLen) +
             " bytes), dropped");
    return CHANNEL_RC_OK;
  }
  if ((dataLen & 1u) != 0) {
    diagWarn("cliprdr: remote text has an odd byte length (" +
             std::to_string(dataLen) + "), dropped");
    return CHANNEL_RC_OK;
  }
  if (bridge->attempt == nullptr || hostStale(bridge->attempt))
    return CHANNEL_RC_OK;
  // Copy before converting: the payload arrives as a BYTE buffer and WinPR's
  // converter reads WCHAR units; the nothrow buffer keeps an out-of-memory
  // condition from throwing through FreeRDP's C callback frames.
  const size_t units = dataLen / sizeof(WCHAR);
  WCHAR* wide = new (std::nothrow) WCHAR[units];
  if (wide == nullptr) {
    diagWarn("cliprdr: no memory for remote text (" + std::to_string(dataLen) +
             " bytes), dropped");
    return CHANNEL_RC_OK;
  }
  std::memcpy(wide, formatDataResponse->requestedFormatData,
              units * sizeof(WCHAR));
  size_t utf8Size = 0;
  char* utf8 = ConvertWCharNToUtf8Alloc(wide, units, &utf8Size);
  delete[] wide;
  if (utf8 == nullptr) {
    diagWarn("cliprdr: UTF-16 to UTF-8 conversion failed (" +
             std::to_string(dataLen) + " bytes)");
    return CHANNEL_RC_OK;
  }
  const RdpHost* host = bridge->attempt->host;
  if (host != nullptr && host->sinkClipboard != nullptr) {
    host->sinkClipboard(host->user, bridge->attempt->generation, utf8,
                        utf8Size);
    diagWarn("cliprdr: remote text received (" + std::to_string(dataLen) +
             " wire bytes, " + std::to_string(utf8Size) + " UTF-8 bytes)");
  }
  std::free(utf8);
  return CHANNEL_RC_OK;
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
// Certificate handling (trust on first use, same model as the VNC path)
// ---------------------------------------------------------------------------
//
// FreeRDP hands over the SHA-256 certificate fingerprint as lowercase hex bytes
// separated by ':'. The app's trust store (vnc-pins.json) uses lowercase bytes
// separated by '-', so the conversion happens here: the state JSON, the UI and
// the store only ever see the normalized form.

// Hex digits only (lowercase), so both fingerprint styles can be compared
// without caring about case or separators.
std::string fingerprintHex(const char* fingerprint) {
  std::string hex;
  if (fingerprint == nullptr) return hex;
  for (const char* p = fingerprint; *p != '\0'; ++p) {
    const char c = *p;
    if (c >= '0' && c <= '9')
      hex.push_back(c);
    else if (c >= 'a' && c <= 'f')
      hex.push_back(c);
    else if (c >= 'A' && c <= 'F')
      hex.push_back(static_cast<char>(c - 'A' + 'a'));
  }
  return hex;
}

// "AB:CD:0d" -> "ab-cd-0d". An empty result means the server supplied no
// usable fingerprint (or a malformed one); callers fail closed on it.
std::string normalizeFingerprint(const char* fingerprint) {
  const std::string hex = fingerprintHex(fingerprint);
  if (hex.empty() || hex.size() % 2 != 0) return std::string();
  std::string out;
  out.reserve(hex.size() + hex.size() / 2);
  for (size_t i = 0; i < hex.size(); i += 2) {
    if (i != 0) out.push_back('-');
    out.push_back(hex[i]);
    out.push_back(hex[i + 1]);
  }
  return out;
}

bool fingerprintsEqual(const std::string& pinned, const std::string& observed) {
  const std::string a = fingerprintHex(pinned.c_str());
  const std::string b = fingerprintHex(observed.c_str());
  return !a.empty() && a == b;
}

// One decision for both the first-contact and the changed-certificate callback:
// FreeRDP passes the fingerprint of the certificate being presented, and the
// pin (if any) comes fresh from the session so a confirmation that happened
// between attempts is honored. Returning 0 rejects the attempt; the flags on
// the attempt tell runRdpSession() how to report it.
DWORD decideCertificate(RdpAttemptContext* attempt, const char* fingerprint) {
  if (attempt == nullptr || attempt->host == nullptr ||
      attempt->host->currentPin == nullptr ||
      attempt->host->setTrustRequired == nullptr) {
    return 0;  // bridge not fully wired: fail closed, never accept blindly
  }
  const std::string observed = normalizeFingerprint(fingerprint);
  if (observed.empty()) {
    // No identity to show and no identity to pin: refuse instead of offering a
    // blank "confirm this" prompt.
    attempt->fingerprintMismatch = true;
    return 0;
  }
  std::string pinned;
  attempt->host->currentPin(attempt->host->user, &pinned);
  if (pinned.empty()) {
    // First connection to this endpoint: publish the fingerprint as
    // "trust required" and refuse this attempt. The UI confirms it, saves the
    // pin and calls reconnectSession(); retrying before that would only spin.
    attempt->host->setTrustRequired(attempt->host->user, attempt->generation,
                                    observed.c_str());
    attempt->trustRequired = true;
    return 0;
  }
  if (fingerprintsEqual(pinned, observed))
    return 2;  // accept for this session only; 1 would store it in FreeRDP's own
               // known_hosts, which this app deliberately does not use
  attempt->fingerprintMismatch = true;
  diagWarn("rdp: server certificate fingerprint differs from the trusted value");
  return 0;
}

DWORD verifyCertificate(freerdp* instance, const char* host, UINT16 port,
                        const char* commonName, const char* subject,
                        const char* issuer, const char* fingerprint, DWORD flags) {
  (void)host;
  (void)port;
  (void)commonName;
  (void)subject;
  (void)issuer;
  (void)flags;
  if (instance == nullptr || instance->context == nullptr) return 0;
  return decideCertificate(attemptOf(instance->context), fingerprint);
}

DWORD verifyChangedCertificate(freerdp* instance, const char* host, UINT16 port,
                               const char* commonName, const char* subject,
                               const char* issuer, const char* newFingerprint,
                               const char* oldSubject, const char* oldIssuer,
                               const char* oldFingerprint, DWORD flags) {
  (void)host;
  (void)port;
  (void)commonName;
  (void)subject;
  (void)issuer;
  (void)oldSubject;
  (void)oldIssuer;
  (void)oldFingerprint;
  (void)flags;
  // A changed certificate is held to exactly the same pin as a first contact:
  // it may only be accepted when it matches the trusted fingerprint. This
  // callback must never be an unconditional acceptance path.
  if (instance == nullptr || instance->context == nullptr) return 0;
  return decideCertificate(attemptOf(instance->context), newFingerprint);
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
      if (attempt->gfx != nullptr) attempt->gfx->commits.fetch_add(1);
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
  // This build contains drdynvc + rdpei + cliprdr + rdpsnd + rdpdr client
  // channels. Clipboard redirection is wanted (the cliprdr addin is in
  // CLIENT_STATIC_ADDIN_TABLE), so client-common loads it at pre-connect.
  if (!freerdp_settings_set_bool(settings, FreeRDP_RedirectClipboard, TRUE) ||
      !freerdp_settings_set_bool(settings, FreeRDP_NetworkAutoDetect, FALSE) ||
      !freerdp_settings_set_bool(settings, FreeRDP_SupportHeartbeatPdu, FALSE) ||
      !freerdp_settings_set_bool(settings, FreeRDP_SupportMultitransport, FALSE))
    return FALSE;
  // Audio playback (rdpsnd): add the channel *here*, before client-common's
  // freerdp_client_load_addins() runs right after PreConnect. Several things
  // depend on that ordering:
  //  - client-common sees "rdpsnd" in the static channel collection, forces
  //    DeviceRedirection=TRUE and registers the rdpdr addin (rdpsnd cannot be
  //    loaded without it). rdpdr is only registered: every device service
  //    redirection below stays FALSE, so it never announces a drive/printer/
  //    smartcard to the server. The RDP8 features that also need rdpdr
  //    (NetworkAutoDetect / heartbeat / multitransport) stay disabled as
  //    before.
  //  - the same arguments are registered for the dynamic channel collection,
  //    exactly like the reference clients' /sound option does. Modern servers
  //    (Windows 8.1+) play audio over the AUDIO_PLAYBACK_DVC dynamic channels
  //    that drdynvc opens, not over the static rdpsnd channel, and the DVC
  //    instances parse their device backend from these same arguments. Without
  //    this the dynamic instances fall back to the built-in "fake" null sink,
  //    which accepts every format and silently discards all audio.
  //  - the arguments select the OHOS renderer device subsystem registered by
  //    installRdpsndAddinProvider() ("sys:tianguan"), and pin rdpsnd's
  //    fixed_format to stereo ("channel:2") so the client never advertises a
  //    layout this renderer cannot open. "format:1" (PCM only) is gone on
  //    purpose: it filtered every compressed format out before the device saw
  //    it, and this FreeRDP build now has WITH_DSP_FFMPEG, so AAC/AAC-LATM can
  //    be decoded by the device plugin (freerdp_dsp_decode).
  static const char* kSoundChannel[] = {
      RDPSND_CHANNEL_NAME, "sys:tianguan", "channel:2"};
  if (!freerdp_client_add_static_channel(settings, ARRAYSIZE(kSoundChannel),
                                         kSoundChannel) ||
      !freerdp_client_add_dynamic_channel(settings, ARRAYSIZE(kSoundChannel),
                                          kSoundChannel))
    return FALSE;
  if (!freerdp_settings_set_bool(settings, FreeRDP_AudioPlayback, TRUE) ||
      !freerdp_settings_set_bool(settings, FreeRDP_AudioCapture, FALSE) ||
      !freerdp_settings_set_bool(settings, FreeRDP_RedirectDrives, FALSE) ||
      !freerdp_settings_set_bool(settings, FreeRDP_RedirectHomeDrive, FALSE) ||
      !freerdp_settings_set_bool(settings, FreeRDP_RedirectSerialPorts, FALSE) ||
      !freerdp_settings_set_bool(settings, FreeRDP_RedirectSmartCards, FALSE) ||
      !freerdp_settings_set_bool(settings, FreeRDP_RedirectPrinters, FALSE))
    return FALSE;
  if (!kEnableGfx) return TRUE;
  // SupportGraphicsPipeline makes client-common register the "rdpgfx" dynamic
  // channel (freerdp_client_load_addins) before drdynvc is opened.
  if (!freerdp_settings_set_bool(settings, FreeRDP_SupportGraphicsPipeline, TRUE))
    return FALSE;
  // This prefix is built WITH_FFMPEG/WITH_SWSCALE, so config.h defines
  // WITH_GFX_H264 and libavcodec/libswscale decode AVC420/AVC444(x). Advertise
  // all three AVC flavours instead of the previous explicit disable:
  // FreeRDP_GfxH264 adds RDPGFX_CAPS_FLAG_AVC420_ENABLED to the 8.1 caps set,
  // and GfxAVC444/GfxAVC444v2 clear RDPGFX_CAPS_FLAG_AVC_DISABLED on 10.x.
  // The still-supported codecs (RemoteFX / progressive / planar / clear) stay
  // at their defaults. If the server never selects AVC the stats line keeps
  // reporting whatever codec id it did select (8=clear, 10=planar, ...).
  if (!freerdp_settings_set_bool(settings, FreeRDP_GfxH264, TRUE) ||
      !freerdp_settings_set_bool(settings, FreeRDP_GfxAVC444, TRUE) ||
      !freerdp_settings_set_bool(settings, FreeRDP_GfxAVC444v2, TRUE))
    return FALSE;
  return TRUE;
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
  // DeactivateClientDecoding is FreeRDP's "do not decode on the client" flag.
  // GFX needs the client decoders: gdi_graphics_pipeline_init() checks this
  // flag and clears SurfaceCommand/UpdateSurfaces when it is set, so with TRUE
  // the rdpgfx channel would connect but never paint. With GFX enabled the flag
  // stays at FreeRDP's default (FALSE), exactly like every reference client.
  // The plain GDI path is unaffected either way: gdi_init() above already
  // registered the bitmap/order callbacks while the flag was FALSE, so the old
  // fallback still works when the server never sends GFX. When GFX is switched
  // off, keep the historical value so the pre-GFX behaviour is bit-for-bit.
  if (!freerdp_settings_set_bool(settings, FreeRDP_DeactivateClientDecoding,
                                 kEnableGfx ? FALSE : TRUE))
    return FALSE;
  registerPointer(context->graphics);
  if (context->update == nullptr) return FALSE;
  // EndPaint is where gdi has finished painting one server update into its
  // primary buffer (bitmap updates and primary orders alike) and the invalid
  // region describes what changed. This is the same hook every FreeRDP client
  // uses; it must stay after gdi_init so the gdi callbacks are in place.
  context->update->EndPaint = endPaint;
  context->update->DesktopResize = desktopResize;
  // Count the fastpath (GDI) frame sources. gdi_init() installed these above
  // because DeactivateClientDecoding was still FALSE; wrapping them only adds
  // counters, the gdi implementation still runs. The counts end up in the GFX
  // stats line so "GFX took the picture" can be told apart from "GDI painted".
  attempt->bitmapUpdate = context->update->BitmapUpdate;
  attempt->surfaceBits = context->update->SurfaceBits;
  if (kEnableGfx && attempt->bitmapUpdate != nullptr)
    context->update->BitmapUpdate = countedBitmapUpdate;
  if (kEnableGfx && attempt->surfaceBits != nullptr)
    context->update->SurfaceBits = countedSurfaceBits;
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
  if (attempt == nullptr) return;
  // Safety net: freerdp_disconnect() stops drdynvc and closes its channels
  // before PostDisconnect, so the ChannelDisconnected handler has normally
  // released the pipeline already; this keeps a gfx->custom from outliving the
  // gdi if some disconnect path skips that event. gfxUnbind() is idempotent.
  gfxUnbind(attempt);
  if (attempt->gdiReady) {
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
  CliprdrBridge* cliprdr;
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
  InputBridge* bridge = static_cast<InputBridge*>(context);
  if (bridge == nullptr || bridge->attempt == nullptr || text == nullptr)
    return;
  RdpAttemptContext* attempt = bridge->attempt;
  if (hostStale(attempt)) return;
  CliprdrBridge* cliprdr = bridge->cliprdr;
  if (cliprdr == nullptr) return;
  // Keep the newest local text for the next ServerFormatDataRequest even when
  // the channel is not ready yet: MonitorReady announces whatever is current.
  setLocalClipboardText(text, cliprdr->localText);
  if (cliprdr->localText.empty()) return;  // never announce an empty clipboard
  if (cliprdr->context == nullptr) {
    // No cliprdr channel on this connection: count the drop so the event loop
    // can report it once, exactly like the pre-channel build did.
    ++bridge->droppedClipboards;
    return;
  }
  if (!cliprdr->ready) return;
  const UINT error = cliprdrSendFormatList(*cliprdr);
  if (error != CHANNEL_RC_OK) {
    diagWarn("cliprdr: local CF_UNICODETEXT format list failed [" +
             std::to_string(error) + "]");
    return;
  }
  diagWarn("cliprdr: local text set (" + std::to_string(cliprdr->localText.size()) +
           " bytes), format list sent");
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
  CliprdrBridge clipboard;
  GfxBridge gfx;
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
    attempt->trustRequired = false;
    attempt->fingerprintMismatch = false;
    attempt->bitmapUpdate = nullptr;
    attempt->surfaceBits = nullptr;
    // freerdp_client_context_new() registers client-common's static addin
    // resolver as the process-wide provider, so (re)install ours afterwards:
    // it answers only the rdpsnd "tianguan" subsystem lookup and delegates
    // everything else. Must be in place before freerdp_connect() loads the
    // channels.
    installRdpsndAddinProvider();
    // Subscribe before connect: the rdpgfx channel is opened after PostConnect
    // (gdi_init) and its ChannelConnected event is what installs the gdi
    // graphics pipeline. The reference clients (xfreerdp/SDL) subscribe at the
    // same point. context->pubSub exists as soon as the client context is
    // created.
    if (kEnableGfx && context->pubSub != nullptr) {
      attempt->gfx = &gfx;
      if (PubSub_SubscribeChannelConnected(context->pubSub,
                                           gfxChannelConnected) < 0 ||
          PubSub_SubscribeChannelDisconnected(context->pubSub,
                                              gfxChannelDisconnected) < 0)
        return false;
    } else {
      attempt->gfx = nullptr;
    }
    // A fresh connection starts with no clipboard ownership; the text queued
    // by ArkTS is drained again by eventLoop() and re-announced.
    clipboard.context = nullptr;
    clipboard.attempt = attempt;
    clipboard.localText.clear();
    clipboard.ready = false;
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

  // Hook the cliprdr callbacks once the channel has been opened by
  // freerdp_connect() (client-common opens static channels during
  // freerdp_channels_post_connect, after PostConnect). The addin's interface is
  // published in the channel-open data, which the public channel-manager API
  // returns by channel name; that pointer is the CliprdrClientContext whose
  // Client* fields are the already-installed send functions.
  void attachClipboard() {
    if (context == nullptr || context->channels == nullptr ||
        attempt == nullptr)
      return;
    void* iface = freerdp_channels_get_static_channel_interface(
        context->channels, CLIPRDR_SVC_CHANNEL_NAME);
    CliprdrClientContext* cliprdr =
        static_cast<CliprdrClientContext*>(iface);
    if (cliprdr == nullptr) return;
    clipboard.context = cliprdr;
    clipboard.attempt = attempt;
    clipboard.ready = false;
    cliprdr->custom = &clipboard;
    cliprdr->MonitorReady = cliprdrMonitorReady;
    cliprdr->ServerFormatList = cliprdrServerFormatList;
    cliprdr->ServerFormatDataRequest = cliprdrServerFormatDataRequest;
    cliprdr->ServerFormatDataResponse = cliprdrServerFormatDataResponse;
  }

  // Unhook before the channel/context are torn down. Idempotent; called from
  // teardown() so every path (including a failed connect after attach) clears
  // the back-pointer to this stack object.
  void detachClipboard() {
    if (clipboard.context != nullptr) {
      if (clipboard.context->custom == &clipboard)
        clipboard.context->custom = nullptr;
      clipboard.context->MonitorReady = nullptr;
      clipboard.context->ServerFormatList = nullptr;
      clipboard.context->ServerFormatDataRequest = nullptr;
      clipboard.context->ServerFormatDataResponse = nullptr;
    }
    clipboard.context = nullptr;
    clipboard.attempt = nullptr;
    clipboard.ready = false;
  }

  void eventLoop() {
    if (attempt == nullptr) return;
    InputBridge bridge;
    bridge.attempt = attempt;
    bridge.cliprdr = &clipboard;
    bridge.pointerMask = 0;
    bridge.lastX = 0;
    bridge.lastY = 0;
    bridge.droppedClipboards = 0;
    bridge.unmappedKeys = 0;
    // Attach before pumping the channel message queue: a MonitorReady that
    // already arrived is dispatched by the first freerdp_check_event_handles()
    // call below, with these callbacks in place.
    attachClipboard();
    if (clipboard.context != nullptr)
      diagWarn("cliprdr: channel interface attached");
    else
      diagWarn("cliprdr: channel not available on this connection");
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
      // The text is reported as delivered to the caller, so say plainly that
      // it could not be forwarded (cliprdr channel missing or not ready).
      // Never log the text itself.
      std::fprintf(stderr,
                   "tianguan: rdp dropped %zu clipboard item(s): clipboard "
                   "channel unavailable\n",
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
    detachClipboard();
    if (connected) {
      connected = false;
      // Both calls are also safe on an already-closed connection; abort first
      // so no blocked transport call can outlive the teardown.
      freerdp_abort_connect_context(context);
      freerdp_disconnect(instance);
    } else if (attempt != nullptr && attempt->gdiReady) {
      // PostConnect ran (gdi_init succeeded) but freerdp_connect() failed after
      // that point and did not call PostDisconnect; release gdi here.
      gfxUnbind(attempt);
      gdi_free(instance);
      attempt->gdiReady = false;
    }
    // Idempotent safety net: normally the ChannelDisconnected handler (emitted
    // by drdynvc while freerdp_disconnect() stops it) already unbound.
    gfxUnbind(attempt);
    if (attempt != nullptr) attempt->gfx = nullptr;
    if (kEnableGfx && context->pubSub != nullptr) {
      PubSub_UnsubscribeChannelConnected(context->pubSub, gfxChannelConnected);
      PubSub_UnsubscribeChannelDisconnected(context->pubSub,
                                            gfxChannelDisconnected);
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
      host.consumeFullRequest == nullptr || host.sinkClipboard == nullptr ||
      host.currentPin == nullptr || host.setTrustRequired == nullptr) {
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
      // Certificate pinning outcomes are known exactly, so they are handled
      // before the error-code classification: a rejected certificate must not
      // be mistaken for a network failure and retried automatically.
      const bool trustRequired =
          attempt.attempt != nullptr && attempt.attempt->trustRequired;
      const bool fingerprintMismatch =
          attempt.attempt != nullptr && attempt.attempt->fingerprintMismatch;
      const RdpFailureClass failure = classifyConnectFailure(code, sawLogonErrorInfo);
      const std::string message = connectErrorText(code, options);
      attempt.teardown();
      if (trustRequired) {
        // setTrustRequired() already published phase=trust-required with the
        // fingerprint; wait for the UI to confirm and call reconnectSession().
        host.waitForReconnect(host.user);
        continue;
      }
      if (fingerprintMismatch) {
        setPhase(host, RdpPhaseFailed,
                 "服务端指纹与已信任值不一致，已拒绝连接", "fingerprint-mismatch", 0);
        host.waitForReconnect(host.user);
        continue;
      }
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
