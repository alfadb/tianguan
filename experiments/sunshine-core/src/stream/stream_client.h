// SPDX-License-Identifier: GPL-3.0-or-later
//
// Headless Moonlight client for the Sunshine/Moonlight experiment.
//
// This client deliberately performs no HTTP. Every value the Moonlight
// protocol needs from HTTP (/serverinfo, /pair, /launch) is handed in by the
// caller through StreamConfig. The client only drives moonlight-common-c:
// LiStartConnection() -> callbacks -> LiStopConnection().

#pragma once

#include <cstdint>
#include <string>

#include "Limelight.h"

namespace tg::sunshine {

// Values obtained from the /launch and /serverinfo endpoints by the HTTP
// module. `rikey`/`rikeyid` must be the exact values that were sent in the
// /launch request, otherwise the host cannot decrypt remote input.
struct StreamConfig {
    // Host name or IP address that moonlight-common-c connects to.
    std::string host = "127.0.0.1";

    // RTSP port to append when `sessionUrl` does not carry an explicit port.
    // Real /launch responses normally include the port already.
    int rtspPort = 48010;

    // `sessionUrl0` from /launch (e.g. "rtsp://192.168.1.10:48010").
    std::string sessionUrl;

    // `appversion` from /serverinfo (e.g. "7.1.431.-1").
    std::string appVersion;

    // `GfeVersion` from /serverinfo (optional).
    std::string gfeVersion;

    // `ServerCodecModeSupport` from /serverinfo.
    int serverCodecModeSupport = 0;

    // 32 hex characters: the raw 16-byte key that was sent as `rikey`.
    std::string rikey;

    // Hex value that was sent as `rikeyid` (0x prefix optional).
    std::string rikeyid;

    int width = 1280;
    int height = 720;
    int fps = 60;
    int bitrateKbps = 5000;

    // Bitmask of VIDEO_FORMAT_* values offered to the host.
    int videoFormats = VIDEO_FORMAT_H264 | VIDEO_FORMAT_H265;

    // How long to keep the stream alive once the connection is established.
    int seconds = 10;

    // Send one round of mouse/keyboard/text/wheel events after connecting.
    bool inputSmoke = false;

    // If non-empty, the Annex B video elementary stream is written here.
    // The parent directory is created when missing.
    std::string dumpVideoPath;
};

struct StreamStats {
    // Connection outcome.
    bool connectionStarted = false;
    bool stageFailed = false;
    int failedStage = STAGE_NONE;
    int failedErrorCode = 0;
    bool terminated = false;
    int terminationErrorCode = 0;
    bool reachedDuration = false;

    // Negotiated video stream, filled by the decoder setup callback.
    int negotiatedVideoFormat = 0;
    int videoWidth = 0;
    int videoHeight = 0;
    int videoFps = 0;

    // Video counters.
    uint64_t videoFrames = 0;
    uint64_t idrFrames = 0;
    uint64_t pFrames = 0;
    uint64_t videoBytes = 0;
    uint64_t dumpBytes = 0;
    double firstVideoDelayMs = -1.0;
    uint64_t startCodeViolations = 0;
    uint64_t idrOrderViolations = 0;

    // Audio counters. Audio is never decoded: no libopus is available here.
    uint64_t audioPackets = 0;
    uint64_t audioBytes = 0;
    double firstAudioDelayMs = -1.0;
    int audioSampleRate = 0;
    int audioChannels = 0;
    int audioSamplesPerFrame = 0;

    // Connection quality callbacks.
    int lastConnectionStatus = CONN_STATUS_OKAY;
    int poorConnectionUpdates = 0;

    double elapsedSeconds = 0.0;

    // Human readable failure cause. Empty when run() succeeded.
    std::string failureReason;
};

// Short display name for a VIDEO_FORMAT_* value ("H.264", "HEVC", ...).
const char* videoFormatName(int videoFormat);

// Runs one connection attempt and returns true only when the stream ended
// normally (duration reached or the host ended it gracefully).
bool run(const StreamConfig& config, StreamStats& stats);

}  // namespace tg::sunshine
