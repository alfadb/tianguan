// SPDX-License-Identifier: GPL-3.0-or-later
#include "stream_client.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

namespace tg::sunshine {
namespace {

using Clock = std::chrono::steady_clock;

// moonlight-common-c has no callback context pointer, so the active run's
// counters live in this process-lifetime object. A terminated connection is
// reported from a detached thread; keeping the storage static means such a
// late callback can never touch a destroyed stack object.
struct CallbackState {
    std::mutex mutex;
    std::atomic<bool> active{false};
    std::atomic<bool> terminated{false};

    StreamStats stats;
    std::string dumpVideoPath;
    std::ofstream dump;
    bool dumpOpen = false;
    bool dumpFailed = false;

    Clock::time_point startTime;
    Clock::time_point lastRateTime;
    uint64_t lastRateFrames = 0;
    uint64_t lastRateBytes = 0;

    int startCodeIssueLogs = 0;
    int idrOrderIssueLogs = 0;
};

CallbackState g_state;
std::mutex g_logMutex;
// common-c sometimes logs partial lines ("Initializing platform...", then
// "done\n"); track whether the terminal is mid-line so our own messages do not
// get glued onto those fragments.
bool g_lineStart = true;

void logLine(const char* format, ...) {
    char message[4096];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    std::lock_guard<std::mutex> lock(g_logMutex);
    if (!g_lineStart) {
        fputc('\n', stderr);
    }
    fputs("[stream] ", stderr);
    fputs(message, stderr);
    fputc('\n', stderr);
    fflush(stderr);
    g_lineStart = true;
}

CallbackState* activeState() {
    return g_state.active.load(std::memory_order_acquire) ? &g_state : nullptr;
}

double secondsSince(Clock::time_point start) {
    return std::chrono::duration<double>(Clock::now() - start).count();
}

const char* frameTypeName(int frameType) {
    return frameType == FRAME_TYPE_IDR ? "IDR" : "P";
}

std::string firstBytesHex(const char* data, int length, int maxBytes) {
    std::string text;
    const int count = length < maxBytes ? length : maxBytes;
    char byteText[8];
    for (int i = 0; i < count; ++i) {
        snprintf(byteText, sizeof(byteText), "%s%02x", i == 0 ? "" : " ", static_cast<unsigned char>(data[i]));
        text += byteText;
    }
    return text;
}

std::string bufferTypesText(PLENTRY bufferList, int maxEntries) {
    std::string text;
    int count = 0;
    for (PLENTRY entry = bufferList; entry != nullptr && count < maxEntries; entry = entry->next, ++count) {
        char typeText[8];
        snprintf(typeText, sizeof(typeText), "%s%02x", count == 0 ? "" : ",", entry->bufferType);
        text += typeText;
    }
    return text;
}

bool isAnnexBCodec(int videoFormat) {
    return (videoFormat & (VIDEO_FORMAT_MASK_H264 | VIDEO_FORMAT_MASK_H265)) != 0;
}

// Mirrors getAnnexBStartSequence() in VideoDepacketizer.c: a 3-byte start
// sequence needs at least one following NAL header byte, a 4-byte one needs
// five bytes in total.
bool hasAnnexBStartCode(const char* data, int length) {
    if (data == nullptr) {
        return false;
    }
    if (length >= 4 && static_cast<unsigned char>(data[0]) == 0 && static_cast<unsigned char>(data[1]) == 0 &&
        static_cast<unsigned char>(data[2]) == 1) {
        return true;
    }
    if (length >= 5 && static_cast<unsigned char>(data[0]) == 0 && static_cast<unsigned char>(data[1]) == 0 &&
        static_cast<unsigned char>(data[2]) == 0 && static_cast<unsigned char>(data[3]) == 1) {
        return true;
    }
    return false;
}

// Same expectations as validateDecodeUnitForPlayback() in VideoDepacketizer.c:
// H.264 IDR: SPS -> PPS -> PICDATA, HEVC IDR: VPS -> SPS -> PPS -> PICDATA.
bool hasExpectedIdrBufferOrder(PLENTRY bufferList, int videoFormat) {
    int types[4] = {-1, -1, -1, -1};
    int count = 0;
    for (PLENTRY entry = bufferList; entry != nullptr && count < 4; entry = entry->next) {
        types[count++] = entry->bufferType;
    }
    if (videoFormat & VIDEO_FORMAT_MASK_H264) {
        return count >= 3 && types[0] == BUFFER_TYPE_SPS && types[1] == BUFFER_TYPE_PPS &&
               types[2] == BUFFER_TYPE_PICDATA;
    }
    return count >= 4 && types[0] == BUFFER_TYPE_VPS && types[1] == BUFFER_TYPE_SPS && types[2] == BUFFER_TYPE_PPS &&
           types[3] == BUFFER_TYPE_PICDATA;
}

const char* terminationReason(int errorCode) {
    switch (errorCode) {
        case ML_ERROR_GRACEFUL_TERMINATION:
            return "graceful termination (host ended the stream)";
        case ML_ERROR_NO_VIDEO_TRAFFIC:
            return "no video traffic received (check UDP 47998/48000)";
        case ML_ERROR_NO_VIDEO_FRAME:
            return "no complete video frame received (network too unstable or bitrate too high)";
        case ML_ERROR_UNEXPECTED_EARLY_TERMINATION:
            return "host reported an unexpected early termination";
        case ML_ERROR_PROTECTED_CONTENT:
            return "host reported protected content (DRM)";
        case ML_ERROR_FRAME_CONVERSION:
            return "host reported a frame conversion error";
        default:
            return "unknown termination error";
    }
}

// ---------------------------------------------------------------------------
// Video callbacks
// ---------------------------------------------------------------------------

int drSetup(int videoFormat, int width, int height, int redrawRate, void* /*context*/, int /*drFlags*/) {
    CallbackState* state = activeState();
    if (state == nullptr) {
        return 0;
    }

    std::lock_guard<std::mutex> lock(state->mutex);
    StreamStats& stats = state->stats;
    stats.negotiatedVideoFormat = videoFormat;
    stats.videoWidth = width;
    stats.videoHeight = height;
    stats.videoFps = redrawRate;

    logLine("video setup: format=%s (0x%X), %dx%d @ %d fps", videoFormatName(videoFormat), videoFormat, width, height,
            redrawRate);

    if (!state->dumpVideoPath.empty()) {
        state->dump.open(state->dumpVideoPath, std::ios::binary | std::ios::trunc);
        if (!state->dump.is_open()) {
            stats.failureReason = "failed to open video dump file: " + state->dumpVideoPath;
            logLine("failed to open video dump file '%s'", state->dumpVideoPath.c_str());
            return -1;
        }
        state->dumpOpen = true;
        logLine("video dump: %s", state->dumpVideoPath.c_str());
    }
    return 0;
}

int drSubmitDecodeUnit(PDECODE_UNIT decodeUnit) {
    CallbackState* state = activeState();
    if (state == nullptr || decodeUnit == nullptr) {
        return DR_OK;
    }

    std::lock_guard<std::mutex> lock(state->mutex);
    StreamStats& stats = state->stats;
    const bool annexB = isAnnexBCodec(stats.negotiatedVideoFormat);

    // Basic bitstream sanity checks. Only the first buffer of a decode unit
    // carries the start sequence: later buffers are continuation fragments of
    // the same NAL unit (see queueFragment() in VideoDepacketizer.c).
    if (annexB) {
        PLENTRY first = decodeUnit->bufferList;
        if (first == nullptr || !hasAnnexBStartCode(first->data, first->length)) {
            ++stats.startCodeViolations;
            if (state->startCodeIssueLogs < 3) {
                ++state->startCodeIssueLogs;
                logLine("decode unit %d (%s): first buffer has no Annex B start code (length=%d, first bytes=[%s])",
                        decodeUnit->frameNumber, frameTypeName(decodeUnit->frameType),
                        first != nullptr ? first->length : 0,
                        first != nullptr ? firstBytesHex(first->data, first->length, 6).c_str() : "");
            }
        }
        if (decodeUnit->frameType == FRAME_TYPE_IDR && !hasExpectedIdrBufferOrder(decodeUnit->bufferList,
                                                                                stats.negotiatedVideoFormat)) {
            ++stats.idrOrderViolations;
            if (state->idrOrderIssueLogs < 3) {
                ++state->idrOrderIssueLogs;
                logLine("decode unit %d (IDR): unexpected buffer order [%s] (expected %s)", decodeUnit->frameNumber,
                        bufferTypesText(decodeUnit->bufferList, 8).c_str(),
                        (stats.negotiatedVideoFormat & VIDEO_FORMAT_MASK_H264) ? "SPS,PPS,PICDATA"
                                                                              : "VPS,SPS,PPS,PICDATA");
            }
        }
    }

    // Write the buffer chain in order. For H.264/HEVC these buffers form an
    // Annex B elementary stream (the common-c decoder contract).
    if (state->dumpOpen) {
        uint64_t written = 0;
        for (PLENTRY entry = decodeUnit->bufferList; entry != nullptr; entry = entry->next) {
            state->dump.write(entry->data, entry->length);
            written += static_cast<uint64_t>(entry->length);
            if (!state->dump.good()) {
                state->dumpOpen = false;
                state->dumpFailed = true;
                logLine("video dump write failed after %llu bytes; dump disabled",
                        static_cast<unsigned long long>(stats.dumpBytes + written));
                break;
            }
        }
        stats.dumpBytes += written;
    }

    stats.videoBytes += static_cast<uint64_t>(decodeUnit->fullLength);
    ++stats.videoFrames;
    if (decodeUnit->frameType == FRAME_TYPE_IDR) {
        ++stats.idrFrames;
    } else {
        ++stats.pFrames;
    }

    if (stats.firstVideoDelayMs < 0.0) {
        stats.firstVideoDelayMs = secondsSince(state->startTime) * 1000.0;
        logLine("first video frame: %s frame %d, %d bytes, %.1f ms after start", frameTypeName(decodeUnit->frameType),
                decodeUnit->frameNumber, decodeUnit->fullLength, stats.firstVideoDelayMs);
    }

    const auto now = Clock::now();
    if (now - state->lastRateTime >= std::chrono::seconds(5)) {
        const double windowSeconds = std::chrono::duration<double>(now - state->lastRateTime).count();
        const uint64_t windowFrames = stats.videoFrames - state->lastRateFrames;
        const uint64_t windowBytes = stats.videoBytes - state->lastRateBytes;
        logLine("t=%.1fs video frames=%llu (+%llu in %.1fs => %.1f fps), bytes=%llu (+%.1f KiB/s), idr=%llu, "
                "audio packets=%llu",
                secondsSince(state->startTime), static_cast<unsigned long long>(stats.videoFrames),
                static_cast<unsigned long long>(windowFrames), windowSeconds,
                windowSeconds > 0 ? static_cast<double>(windowFrames) / windowSeconds : 0.0,
                static_cast<unsigned long long>(stats.videoBytes),
                windowSeconds > 0 ? static_cast<double>(windowBytes) / 1024.0 / windowSeconds : 0.0,
                static_cast<unsigned long long>(stats.idrFrames),
                static_cast<unsigned long long>(stats.audioPackets));
        state->lastRateTime = now;
        state->lastRateFrames = stats.videoFrames;
        state->lastRateBytes = stats.videoBytes;
    }

    return DR_OK;
}

// ---------------------------------------------------------------------------
// Audio callbacks (statistics only; no Opus decoder is available)
// ---------------------------------------------------------------------------

int arInit(int audioConfiguration, const POPUS_MULTISTREAM_CONFIGURATION opusConfig, void* /*context*/,
           int /*arFlags*/) {
    CallbackState* state = activeState();
    if (state == nullptr) {
        return 0;
    }

    std::lock_guard<std::mutex> lock(state->mutex);
    StreamStats& stats = state->stats;
    stats.audioSampleRate = opusConfig->sampleRate;
    stats.audioChannels = opusConfig->channelCount;
    stats.audioSamplesPerFrame = opusConfig->samplesPerFrame;

    char mapping[64] = {};
    for (int i = 0; i < opusConfig->channelCount && i < AUDIO_CONFIGURATION_MAX_CHANNEL_COUNT; ++i) {
        char item[8];
        snprintf(item, sizeof(item), "%s%u", i == 0 ? "" : ",", opusConfig->mapping[i]);
        strncat(mapping, item, sizeof(mapping) - strlen(mapping) - 1);
    }

    logLine("audio setup: %d Hz, %d channels, streams=%d, coupledStreams=%d, samplesPerFrame=%d, mapping=[%s], "
            "audioConfiguration=0x%X (channels=%d, mask=0x%X)",
             opusConfig->sampleRate, opusConfig->channelCount, opusConfig->streams, opusConfig->coupledStreams,
             opusConfig->samplesPerFrame, mapping, audioConfiguration,
             CHANNEL_COUNT_FROM_AUDIO_CONFIGURATION(audioConfiguration),
             CHANNEL_MASK_FROM_AUDIO_CONFIGURATION(audioConfiguration));
    return 0;
}

void arDecodeAndPlaySample(char* /*sampleData*/, int sampleLength) {
    CallbackState* state = activeState();
    if (state == nullptr) {
        return;
    }

    std::lock_guard<std::mutex> lock(state->mutex);
    ++state->stats.audioPackets;
    state->stats.audioBytes += static_cast<uint64_t>(sampleLength);
    if (state->stats.firstAudioDelayMs < 0.0) {
        state->stats.firstAudioDelayMs = secondsSince(state->startTime) * 1000.0;
    }
}

// ---------------------------------------------------------------------------
// Connection callbacks
// ---------------------------------------------------------------------------

void clStageStarting(int stage) {
    if (activeState() == nullptr) {
        return;
    }
    logLine("stage starting: %s (stage %d)", LiGetStageName(stage), stage);
}

void clStageComplete(int stage) {
    if (activeState() == nullptr) {
        return;
    }
    logLine("stage complete: %s", LiGetStageName(stage));
}

void clStageFailed(int stage, int errorCode) {
    CallbackState* state = activeState();
    if (state == nullptr) {
        return;
    }

    unsigned int portFlags = LiGetPortFlagsFromStage(stage);
    char ports[128] = {};
    LiStringifyPortFlags(portFlags, ", ", ports, sizeof(ports));

    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->stats.stageFailed = true;
        state->stats.failedStage = stage;
        state->stats.failedErrorCode = errorCode;
    }

    logLine("stage failed: %s (stage %d), error %d%s%s", LiGetStageName(stage), stage, errorCode,
            ports[0] != '\0' ? ", likely ports: " : "", ports);
}

void clConnectionStarted(void) {
    CallbackState* state = activeState();
    if (state == nullptr) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->stats.connectionStarted = true;
    }
    logLine("connection established");
}

void clConnectionTerminated(int errorCode) {
    CallbackState* state = activeState();
    if (state == nullptr) {
        return;
    }

    unsigned int portFlags = LiGetPortFlagsFromTerminationErrorCode(errorCode);
    char ports[128] = {};
    LiStringifyPortFlags(portFlags, ", ", ports, sizeof(ports));

    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->stats.terminated = true;
        state->stats.terminationErrorCode = errorCode;
    }
    state->terminated.store(true, std::memory_order_release);

    logLine("connection terminated: error %d (0x%X, %s)%s%s", errorCode, static_cast<unsigned int>(errorCode),
            terminationReason(errorCode), ports[0] != '\0' ? ", likely ports: " : "", ports);
}

void clConnectionStatusUpdate(int connectionStatus) {
    CallbackState* state = activeState();
    if (state == nullptr) {
        return;
    }

    RTP_VIDEO_STATS videoStats = {};
    RTP_AUDIO_STATS audioStats = {};
    LiGetRTPVideoStats(&videoStats);
    LiGetRTPAudioStats(&audioStats);

    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->stats.lastConnectionStatus = connectionStatus;
        if (connectionStatus == CONN_STATUS_POOR) {
            ++state->stats.poorConnectionUpdates;
        }
    }

    logLine("connection status: %s; video packets=%u, fec=%u, fec recovered=%u, fec failed=%u, oos=%u, invalid=%u; "
            "audio packets=%u, fec failed=%u, oos=%u, invalid=%u",
            connectionStatus == CONN_STATUS_POOR ? "POOR" : "OKAY", videoStats.packetCountVideo,
            videoStats.packetCountFec, videoStats.packetCountFecRecovered, videoStats.packetCountFecFailed,
            videoStats.packetCountOOS, videoStats.packetCountInvalid, audioStats.packetCountAudio,
            audioStats.packetCountFecFailed, audioStats.packetCountOOS, audioStats.packetCountInvalid);
}

void clSetHdrMode(bool hdrEnabled) {
    if (activeState() == nullptr) {
        return;
    }
    logLine("host HDR mode: %s", hdrEnabled ? "enabled" : "disabled");
}

void clLogMessage(const char* format, ...) {
    char message[4096];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    std::lock_guard<std::mutex> lock(g_logMutex);
    if (g_lineStart) {
        fputs("[moonlight] ", stderr);
    }
    fputs(message, stderr);
    fflush(stderr);
    const size_t length = strlen(message);
    g_lineStart = length != 0 && message[length - 1] == '\n';
}

// ---------------------------------------------------------------------------
// Configuration helpers
// ---------------------------------------------------------------------------

bool parseHexValue(char c, uint8_t& value) {
    if (c >= '0' && c <= '9') {
        value = static_cast<uint8_t>(c - '0');
        return true;
    }
    if (c >= 'a' && c <= 'f') {
        value = static_cast<uint8_t>(c - 'a' + 10);
        return true;
    }
    if (c >= 'A' && c <= 'F') {
        value = static_cast<uint8_t>(c - 'A' + 10);
        return true;
    }
    return false;
}

std::string stripHexPrefix(const std::string& text) {
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        return text.substr(2);
    }
    return text;
}

bool parseHexBytes(const std::string& text, uint8_t* output, size_t outputLength) {
    const std::string digits = stripHexPrefix(text);
    if (digits.size() != outputLength * 2) {
        return false;
    }
    for (size_t i = 0; i < outputLength; ++i) {
        uint8_t high = 0;
        uint8_t low = 0;
        if (!parseHexValue(digits[i * 2], high) || !parseHexValue(digits[i * 2 + 1], low)) {
            return false;
        }
        output[i] = static_cast<uint8_t>((high << 4) | low);
    }
    return true;
}

bool parseHexU32(const std::string& text, uint32_t& value) {
    const std::string digits = stripHexPrefix(text);
    if (digits.empty() || digits.size() > 8) {
        return false;
    }
    uint32_t parsed = 0;
    for (char c : digits) {
        uint8_t digit = 0;
        if (!parseHexValue(c, digit)) {
            return false;
        }
        parsed = (parsed << 4) | digit;
    }
    value = parsed;
    return true;
}

bool isVersionQuad(const std::string& text) {
    size_t position = 0;
    for (int part = 0; part < 4; ++part) {
        const size_t dot = text.find('.', position);
        const std::string component = text.substr(position, dot == std::string::npos ? std::string::npos : dot - position);
        if (component.empty()) {
            return false;
        }
        char* end = nullptr;
        errno = 0;
        strtol(component.c_str(), &end, 10);
        if (errno != 0 || end == component.c_str() || *end != '\0') {
            return false;
        }
        if (dot == std::string::npos) {
            return part == 3;
        }
        position = dot + 1;
    }
    return false;
}

// common-c derives the RTSP port from the session URL and falls back to 48010
// when it cannot parse one, so append the configured port when it is absent.
std::string sessionUrlWithPort(const std::string& sessionUrl, int rtspPort) {
    const size_t schemeEnd = sessionUrl.find("://");
    const size_t hostStart = schemeEnd == std::string::npos ? 0 : schemeEnd + 3;

    // Skip over a bracketed IPv6 literal before looking for the port colon.
    size_t portSearchStart = hostStart;
    if (hostStart < sessionUrl.size() && sessionUrl[hostStart] == '[') {
        const size_t bracket = sessionUrl.find(']', hostStart);
        portSearchStart = bracket == std::string::npos ? sessionUrl.size() : bracket + 1;
    }

    const size_t pathStart = sessionUrl.find('/', portSearchStart);
    const size_t authorityEnd = pathStart == std::string::npos ? sessionUrl.size() : pathStart;
    const size_t colon = sessionUrl.find(':', portSearchStart);
    if (colon != std::string::npos && colon < authorityEnd) {
        return sessionUrl;
    }
    return sessionUrl.substr(0, authorityEnd) + ":" + std::to_string(rtspPort) + sessionUrl.substr(authorityEnd);
}

bool buildConnectionParameters(const StreamConfig& config, uint8_t key[16], char iv[16], std::string& sessionUrl,
                               std::string& error) {
    if (config.host.empty()) {
        error = "host is empty";
        return false;
    }
    if (config.sessionUrl.empty()) {
        error = "sessionUrl is required (sessionUrl0 from /launch); pass --session-url";
        return false;
    }
    if (config.sessionUrl.rfind("rtsp://", 0) != 0 && config.sessionUrl.rfind("rtspenc://", 0) != 0) {
        error = "sessionUrl must start with rtsp:// or rtspenc:// (got '" + config.sessionUrl + "')";
        return false;
    }
    if (!isVersionQuad(config.appVersion)) {
        error = "appVersion must be a four-component version like 7.1.431.-1 (got '" + config.appVersion + "')";
        return false;
    }
    if (config.serverCodecModeSupport == 0) {
        error = "serverCodecModeSupport is required (ServerCodecModeSupport from /serverinfo)";
        return false;
    }
    if (!parseHexBytes(config.rikey, key, 16)) {
        error = "rikey must be exactly 32 hex characters (16-byte AES key sent to /launch)";
        return false;
    }
    uint32_t rikeyId = 0;
    if (!parseHexU32(config.rikeyid, rikeyId)) {
        error = "rikeyid must be a hex value of up to 8 hex digits (as sent to /launch)";
        return false;
    }
    if (config.width <= 0 || config.height <= 0) {
        error = "width and height must be positive";
        return false;
    }
    if (config.fps <= 0 || config.fps > 480) {
        error = "fps must be in 1..480";
        return false;
    }
    if (config.bitrateKbps <= 0) {
        error = "bitrate must be positive";
        return false;
    }
    if (config.videoFormats == 0) {
        error = "videoFormats mask is empty";
        return false;
    }
    if (config.seconds <= 0) {
        error = "seconds must be positive";
        return false;
    }
    if (config.rtspPort <= 0 || config.rtspPort > 65535) {
        error = "rtspPort must be in 1..65535";
        return false;
    }

    // The host reads rikeyid as a big-endian 32-bit value from the first four
    // IV bytes (Sunshine nvhttp.cpp prepends big<uint32_t>(rikeyid)); the rest
    // of the IV stays zero exactly like moonlight-qt's RAND_bytes(iv, 4).
    std::memset(iv, 0, 16);
    iv[0] = static_cast<char>((rikeyId >> 24) & 0xFF);
    iv[1] = static_cast<char>((rikeyId >> 16) & 0xFF);
    iv[2] = static_cast<char>((rikeyId >> 8) & 0xFF);
    iv[3] = static_cast<char>(rikeyId & 0xFF);

    sessionUrl = sessionUrlWithPort(config.sessionUrl, config.rtspPort);
    return true;
}

std::string videoFormatMaskName(int mask) {
    std::string text;
    const int formats[] = {VIDEO_FORMAT_H264, VIDEO_FORMAT_H264_HIGH8_444, VIDEO_FORMAT_H265,
                           VIDEO_FORMAT_H265_MAIN10, VIDEO_FORMAT_H265_REXT8_444, VIDEO_FORMAT_H265_REXT10_444,
                           VIDEO_FORMAT_AV1_MAIN8, VIDEO_FORMAT_AV1_MAIN10, VIDEO_FORMAT_AV1_HIGH8_444,
                           VIDEO_FORMAT_AV1_HIGH10_444, VIDEO_FORMAT_MPEG2, VIDEO_FORMAT_H263P};
    for (int format : formats) {
        if ((mask & format) == 0) {
            continue;
        }
        if (!text.empty()) {
            text += "|";
        }
        text += videoFormatName(format);
    }
    if (text.empty()) {
        text = "none";
    }
    return text;
}

void sendInputSmoke(const StreamConfig& config) {
    const short width = static_cast<short>(config.width);
    const short height = static_cast<short>(config.height);
    const short centerX = static_cast<short>(config.width / 2);
    const short centerY = static_cast<short>(config.height / 2);

    int result = LiSendMousePositionEvent(centerX, centerY, width, height);
    logLine("input: LiSendMousePositionEvent(%d, %d, %d, %d) -> %d", centerX, centerY, width, height, result);

    result = LiSendMouseButtonEvent(BUTTON_ACTION_PRESS, BUTTON_LEFT);
    logLine("input: LiSendMouseButtonEvent(PRESS, LEFT) -> %d", result);
    result = LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, BUTTON_LEFT);
    logLine("input: LiSendMouseButtonEvent(RELEASE, LEFT) -> %d", result);

    // 0x41 is the Win32 virtual-key code for 'A'.
    result = LiSendKeyboardEvent(0x41, KEY_ACTION_DOWN, 0);
    logLine("input: LiSendKeyboardEvent(VK 'A', DOWN, 0) -> %d", result);
    result = LiSendKeyboardEvent(0x41, KEY_ACTION_UP, 0);
    logLine("input: LiSendKeyboardEvent(VK 'A', UP, 0) -> %d", result);

    const char text[] = "tianguan spike\n";
    result = LiSendUtf8TextEvent(text, static_cast<unsigned int>(sizeof(text) - 1));
    logLine("input: LiSendUtf8TextEvent(\"tianguan spike\\n\") -> %d", result);

    result = LiSendScrollEvent(1);
    logLine("input: LiSendScrollEvent(1) -> %d", result);
}

}  // namespace

const char* videoFormatName(int videoFormat) {
    switch (videoFormat) {
        case VIDEO_FORMAT_H264:
            return "H.264";
        case VIDEO_FORMAT_H264_HIGH8_444:
            return "H.264 High 4:4:4 8-bit";
        case VIDEO_FORMAT_H265:
            return "HEVC";
        case VIDEO_FORMAT_H265_MAIN10:
            return "HEVC Main10";
        case VIDEO_FORMAT_H265_REXT8_444:
            return "HEVC RExt 4:4:4 8-bit";
        case VIDEO_FORMAT_H265_REXT10_444:
            return "HEVC RExt 4:4:4 10-bit";
        case VIDEO_FORMAT_AV1_MAIN8:
            return "AV1";
        case VIDEO_FORMAT_AV1_MAIN10:
            return "AV1 Main10";
        case VIDEO_FORMAT_AV1_HIGH8_444:
            return "AV1 High 4:4:4 8-bit";
        case VIDEO_FORMAT_AV1_HIGH10_444:
            return "AV1 High 4:4:4 10-bit";
        case VIDEO_FORMAT_MPEG2:
            return "MPEG-2";
        case VIDEO_FORMAT_H263P:
            return "H.263+";
        case 0:
            return "none";
        default:
            return "unknown";
    }
}

bool run(const StreamConfig& config, StreamStats& stats) {
    stats = StreamStats();

    uint8_t key[16] = {};
    char iv[16] = {};
    std::string sessionUrl;
    std::string error;
    if (!buildConnectionParameters(config, key, iv, sessionUrl, error)) {
        stats.failureReason = error;
        logLine("error: %s", error.c_str());
        return false;
    }

    if (!config.dumpVideoPath.empty()) {
        std::error_code directoryError;
        const std::filesystem::path dumpPath(config.dumpVideoPath);
        if (dumpPath.has_parent_path()) {
            std::filesystem::create_directories(dumpPath.parent_path(), directoryError);
        }
        if (directoryError) {
            stats.failureReason = "cannot create video dump directory '" + dumpPath.parent_path().string() +
                                  "': " + directoryError.message();
            logLine("error: %s", stats.failureReason.c_str());
            return false;
        }
    }

    {
        std::lock_guard<std::mutex> lock(g_state.mutex);
        g_state.stats = StreamStats();
        g_state.dumpVideoPath = config.dumpVideoPath;
        g_state.dumpFailed = false;
        g_state.dumpOpen = false;
        if (g_state.dump.is_open()) {
            g_state.dump.close();
        }
        g_state.startTime = Clock::now();
        g_state.lastRateTime = g_state.startTime;
        g_state.lastRateFrames = 0;
        g_state.lastRateBytes = 0;
        g_state.startCodeIssueLogs = 0;
        g_state.idrOrderIssueLogs = 0;
    }
    g_state.terminated.store(false, std::memory_order_release);
    g_state.active.store(true, std::memory_order_release);

    logLine("connecting: host=%s, session URL=%s (effective %s), appversion=%s, gfeversion=%s, codec support=0x%X, "
            "formats=%s, %dx%d@%d, %d kbps, duration=%ds, input smoke=%s, dump=%s",
            config.host.c_str(), config.sessionUrl.c_str(), sessionUrl.c_str(), config.appVersion.c_str(),
            config.gfeVersion.empty() ? "(none)" : config.gfeVersion.c_str(), config.serverCodecModeSupport,
            videoFormatMaskName(config.videoFormats).c_str(), config.width, config.height, config.fps,
            config.bitrateKbps, config.seconds, config.inputSmoke ? "on" : "off",
            config.dumpVideoPath.empty() ? "(none)" : config.dumpVideoPath.c_str());

    SERVER_INFORMATION serverInfo;
    LiInitializeServerInformation(&serverInfo);
    serverInfo.address = config.host.c_str();
    serverInfo.serverInfoAppVersion = config.appVersion.c_str();
    serverInfo.serverInfoGfeVersion = config.gfeVersion.empty() ? nullptr : config.gfeVersion.c_str();
    serverInfo.rtspSessionUrl = sessionUrl.c_str();
    serverInfo.serverCodecModeSupport = config.serverCodecModeSupport;

    STREAM_CONFIGURATION streamConfig;
    LiInitializeStreamConfiguration(&streamConfig);
    streamConfig.width = config.width;
    streamConfig.height = config.height;
    streamConfig.fps = config.fps;
    streamConfig.bitrate = config.bitrateKbps;
    // moonlight-qt: 1392-byte video packets with common-c deciding local/remote
    // (session.cpp:1674-1700).
    streamConfig.packetSize = 1392;
    streamConfig.streamingRemotely = STREAM_CFG_AUTO;
    streamConfig.audioConfiguration = AUDIO_CONFIGURATION_STEREO;
    streamConfig.supportedVideoFormats = config.videoFormats;
    // moonlight-qt leaves this zeroed (LiInitializeStreamConfiguration only);
    // SdpGenerator.c still emits the attribute with value 0.
    streamConfig.clientRefreshRateX100 = 0;
    // No decoder exists here, so pick the SDR defaults moonlight-qt would get
    // from a typical 8-bit decoder (session.cpp:546/555).
    streamConfig.colorSpace = COLORSPACE_REC_709;
    streamConfig.colorRange = COLOR_RANGE_LIMITED;
    streamConfig.encryptionFlags = ENCFLG_NONE;
    std::memcpy(streamConfig.remoteInputAesKey, key, sizeof(streamConfig.remoteInputAesKey));
    std::memcpy(streamConfig.remoteInputAesIv, iv, sizeof(streamConfig.remoteInputAesIv));

    DECODER_RENDERER_CALLBACKS videoCallbacks;
    LiInitializeVideoCallbacks(&videoCallbacks);
    videoCallbacks.setup = drSetup;
    videoCallbacks.submitDecodeUnit = drSubmitDecodeUnit;

    AUDIO_RENDERER_CALLBACKS audioCallbacks;
    LiInitializeAudioCallbacks(&audioCallbacks);
    audioCallbacks.init = arInit;
    audioCallbacks.decodeAndPlaySample = arDecodeAndPlaySample;

    CONNECTION_LISTENER_CALLBACKS connectionCallbacks;
    LiInitializeConnectionCallbacks(&connectionCallbacks);
    connectionCallbacks.stageStarting = clStageStarting;
    connectionCallbacks.stageComplete = clStageComplete;
    connectionCallbacks.stageFailed = clStageFailed;
    connectionCallbacks.connectionStarted = clConnectionStarted;
    connectionCallbacks.connectionTerminated = clConnectionTerminated;
    connectionCallbacks.logMessage = clLogMessage;
    connectionCallbacks.connectionStatusUpdate = clConnectionStatusUpdate;
    connectionCallbacks.setHdrMode = clSetHdrMode;

    // LiStartConnection() blocks until the stream is established or the
    // attempt fails. Callbacks for the initial stages are invoked on this
    // thread; runtime callbacks come from common-c's own threads.
    const int startResult =
            LiStartConnection(&serverInfo, &streamConfig, &connectionCallbacks, &videoCallbacks, &audioCallbacks,
                              nullptr, 0, nullptr, 0);
    if (startResult != 0) {
        logLine("LiStartConnection failed: %d", startResult);
    }

    if (startResult == 0 && config.inputSmoke) {
        sendInputSmoke(config);
        // Give the input send thread a moment to flush before teardown.
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    bool reachedDuration = false;
    if (startResult == 0) {
        const auto deadline = Clock::now() + std::chrono::seconds(config.seconds);
        while (!g_state.terminated.load(std::memory_order_acquire) && Clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        reachedDuration = !g_state.terminated.load(std::memory_order_acquire);
    }

    // Always tear down through common-c, including after a failed start (it
    // already ran its own cleanup, which LiStopConnection() handles fine).
    LiStopConnection();

    // A termination notification is delivered from a detached thread; stop
    // accepting callback writes and give that thread a moment to finish before
    // copying the counters out.
    g_state.active.store(false, std::memory_order_release);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    bool dumpFailed = false;
    {
        std::lock_guard<std::mutex> lock(g_state.mutex);
        stats = g_state.stats;
        dumpFailed = g_state.dumpFailed;
        if (g_state.dump.is_open()) {
            g_state.dump.flush();
            g_state.dump.close();
        }
        g_state.dumpOpen = false;
        stats.reachedDuration = reachedDuration;
        stats.elapsedSeconds = secondsSince(g_state.startTime);
    }

    if (!stats.failureReason.empty()) {
        // Already explained (for example a dump file that could not be opened).
    } else if (startResult != 0) {
        if (stats.stageFailed) {
            stats.failureReason = std::string("connection failed at stage '") + LiGetStageName(stats.failedStage) +
                                  "' (stage " + std::to_string(stats.failedStage) + ") with error code " +
                                  std::to_string(stats.failedErrorCode);
        } else {
            stats.failureReason = "LiStartConnection returned " + std::to_string(startResult);
        }
    } else if (!stats.connectionStarted) {
        stats.failureReason = "LiStartConnection returned success but connectionStarted never fired";
    } else if (stats.terminated && stats.terminationErrorCode != ML_ERROR_GRACEFUL_TERMINATION) {
        stats.failureReason = "connection terminated with error code " +
                              std::to_string(stats.terminationErrorCode) + " (" +
                              terminationReason(stats.terminationErrorCode) + ")";
    } else if (stats.videoFrames == 0) {
        stats.failureReason = "no video frames were received during the session";
    } else if (dumpFailed) {
        stats.failureReason = "video dump write failed";
    }

    const bool okay = stats.failureReason.empty();
    logLine("stopped: elapsed=%.2fs, frames=%llu (IDR %llu), video bytes=%llu, audio packets=%llu, result=%s",
            stats.elapsedSeconds, static_cast<unsigned long long>(stats.videoFrames),
            static_cast<unsigned long long>(stats.idrFrames), static_cast<unsigned long long>(stats.videoBytes),
            static_cast<unsigned long long>(stats.audioPackets), okay ? "OK" : "FAILED");
    if (!okay) {
        logLine("failure: %s", stats.failureReason.c_str());
    }
    return okay;
}

}  // namespace tg::sunshine
