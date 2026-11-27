// SPDX-License-Identifier: GPL-3.0-or-later
//
// tg-sunshine-stream: headless Moonlight pull-stream client used by the
// Sunshine spike. Every HTTP step (/serverinfo, /pair, /launch) is performed
// by another module; this binary only consumes the values obtained there.

#include <cctype>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "Limelight.h"
#include "stream_client.h"

namespace {

struct FormatAlias {
    const char* name;
    int mask;
};

const FormatAlias kFormatAliases[] = {
        {"h264", VIDEO_FORMAT_H264},
        {"h264-high8-444", VIDEO_FORMAT_H264_HIGH8_444},
        {"hevc", VIDEO_FORMAT_H265},
        {"h265", VIDEO_FORMAT_H265},
        {"hevc-main10", VIDEO_FORMAT_H265_MAIN10},
        {"hevc-rext8-444", VIDEO_FORMAT_H265_REXT8_444},
        {"hevc-rext10-444", VIDEO_FORMAT_H265_REXT10_444},
        {"av1", VIDEO_FORMAT_AV1_MAIN8},
        {"av1-main10", VIDEO_FORMAT_AV1_MAIN10},
        {"av1-high8-444", VIDEO_FORMAT_AV1_HIGH8_444},
        {"av1-high10-444", VIDEO_FORMAT_AV1_HIGH10_444},
        {"mpeg2", VIDEO_FORMAT_MPEG2},
        {"h263p", VIDEO_FORMAT_H263P},
};

void printUsage(const char* program) {
    fprintf(stderr,
            "usage: %s [options]\n"
            "\n"
            "Required values (obtained by the HTTP/pairing module):\n"
            "  --session-url URL      sessionUrl0 from /launch, e.g. rtsp://192.168.1.10:48010\n"
            "  --appversion VER       appversion from /serverinfo, e.g. 7.1.431.-1\n"
            "  --codec-support N      ServerCodecModeSupport from /serverinfo (decimal or 0x...)\n"
            "  --rikey HEX            rikey sent to /launch (32 hex characters)\n"
            "  --rikeyid HEX          rikeyid sent to /launch (hex, 0x prefix optional)\n"
            "\n"
            "Options:\n"
            "  --host HOST            host name or IP (default 127.0.0.1)\n"
            "  --rtsp-port PORT       RTSP port used when --session-url has none (default 48010)\n"
            "  --gfeversion VER       GfeVersion from /serverinfo (optional)\n"
            "  --width N              stream width (default 1280)\n"
            "  --height N             stream height (default 720)\n"
            "  --fps N                stream frame rate (default 60)\n"
            "  --bitrate KBPS         video bitrate in kbps (default 5000)\n"
            "  --formats LIST         comma separated codecs: h264,hevc,av1,... (default h264,hevc)\n"
            "  --seconds N            how long to stay connected (default 10)\n"
            "  --dump-video FILE      write the Annex B video stream to FILE\n"
            "  --input-smoke          send one round of mouse/keyboard/text/scroll events\n"
            "  --repeat N             run the complete pipeline N times in this process (self-check)\n"
            "  --help                 show this help\n",
            program);
}

bool parseDecimal(const std::string& text, int& value) {
    if (text.empty()) {
        return false;
    }
    char* end = nullptr;
    errno = 0;
    const long parsed = strtol(text.c_str(), &end, 10);
    if (errno != 0 || end == text.c_str() || *end != '\0' || parsed < INT_MIN || parsed > INT_MAX) {
        return false;
    }
    value = static_cast<int>(parsed);
    return true;
}

bool parseDecimalOrHex(const std::string& text, int& value) {
    if (text.empty()) {
        return false;
    }
    int base = 10;
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        base = 16;
    }
    char* end = nullptr;
    errno = 0;
    const long parsed = strtol(text.c_str(), &end, base);
    if (errno != 0 || end == text.c_str() || *end != '\0' || parsed < 0 || parsed > INT_MAX) {
        return false;
    }
    value = static_cast<int>(parsed);
    return true;
}

std::string trim(const std::string& text) {
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin])) != 0) {
        ++begin;
    }
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) {
        --end;
    }
    return text.substr(begin, end - begin);
}

std::string toLower(const std::string& text) {
    std::string result = text;
    for (char& c : result) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return result;
}

bool parseFormatMask(const std::string& list, int& mask, std::string& error) {
    mask = 0;
    size_t start = 0;
    while (true) {
        const size_t comma = list.find(',', start);
        const std::string name = toLower(trim(list.substr(start, comma == std::string::npos ? std::string::npos
                                                                                          : comma - start)));
        if (name.empty()) {
            error = "--formats contains an empty codec name";
            return false;
        }
        bool matched = false;
        for (const FormatAlias& alias : kFormatAliases) {
            if (name == alias.name) {
                mask |= alias.mask;
                matched = true;
                break;
            }
        }
        if (!matched) {
            error = "unknown codec '" + name + "' in --formats (accepted: h264, hevc/h265, av1 and their profiles)";
            return false;
        }
        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }
    if (mask == 0) {
        error = "--formats must select at least one codec";
        return false;
    }
    return true;
}

bool takeValue(int argc, char** argv, int& index, const char* option, std::string& value, std::string& error) {
    if (index + 1 >= argc) {
        error = std::string("missing value for ") + option;
        return false;
    }
    value = argv[++index];
    return true;
}

void printSummary(const tg::sunshine::StreamStats& stats) {
    printf("\n=== summary ===\n");
    printf("negotiated format  : %s (0x%X)\n", tg::sunshine::videoFormatName(stats.negotiatedVideoFormat),
           stats.negotiatedVideoFormat);
    printf("resolution         : %dx%d @ %d fps\n", stats.videoWidth, stats.videoHeight, stats.videoFps);
    printf("video frames       : %llu (IDR %llu, P %llu)\n", static_cast<unsigned long long>(stats.videoFrames),
           static_cast<unsigned long long>(stats.idrFrames), static_cast<unsigned long long>(stats.pFrames));
    printf("video bytes        : %llu\n", static_cast<unsigned long long>(stats.videoBytes));
    printf("dump bytes         : %llu\n", static_cast<unsigned long long>(stats.dumpBytes));
    printf("bitstream checks   : start-code violations=%llu, IDR buffer-order violations=%llu\n",
           static_cast<unsigned long long>(stats.startCodeViolations),
           static_cast<unsigned long long>(stats.idrOrderViolations));
    if (stats.firstVideoDelayMs >= 0.0) {
        printf("first video frame  : %.1f ms after start\n", stats.firstVideoDelayMs);
    } else {
        printf("first video frame  : none\n");
    }
    printf("audio              : %llu packets, %llu bytes",
           static_cast<unsigned long long>(stats.audioPackets), static_cast<unsigned long long>(stats.audioBytes));
    if (stats.audioPackets > 0) {
        printf(" (%d Hz, %d channels, %d samples/frame)", stats.audioSampleRate, stats.audioChannels,
               stats.audioSamplesPerFrame);
    }
    printf("\n");
    if (stats.firstAudioDelayMs >= 0.0) {
        printf("first audio packet : %.1f ms after start\n", stats.firstAudioDelayMs);
    }
    printf("connection         : started=%s, stage failed=%s, terminated=%s, termination code=%d\n",
           stats.connectionStarted ? "yes" : "no", stats.stageFailed ? "yes" : "no",
           stats.terminated ? "yes" : "no", stats.terminationErrorCode);
    printf("poor status events : %d\n", stats.poorConnectionUpdates);
    if (stats.elapsedSeconds > 0.0) {
        printf("average frame rate : %.2f fps over %.2f s\n", stats.videoFrames / stats.elapsedSeconds,
               stats.elapsedSeconds);
    }
    printf("elapsed            : %.2f s\n", stats.elapsedSeconds);
    if (stats.failureReason.empty()) {
        printf("result             : OK\n");
    } else {
        printf("result             : FAILED: %s\n", stats.failureReason.c_str());
    }
    fflush(stdout);
}

bool parseArguments(int argc, char** argv, tg::sunshine::StreamConfig& config, int& repeat, bool& help,
                    std::string& error) {
    for (int index = 1; index < argc; ++index) {
        const std::string arg = argv[index];
        std::string value;

        if (arg == "--help" || arg == "-h") {
            help = true;
            return true;
        } else if (arg == "--input-smoke") {
            config.inputSmoke = true;
            continue;
        }

        if (arg == "--host") {
            if (!takeValue(argc, argv, index, "--host", value, error)) return false;
            config.host = value;
        } else if (arg == "--session-url") {
            if (!takeValue(argc, argv, index, "--session-url", value, error)) return false;
            config.sessionUrl = value;
        } else if (arg == "--appversion") {
            if (!takeValue(argc, argv, index, "--appversion", value, error)) return false;
            config.appVersion = value;
        } else if (arg == "--gfeversion") {
            if (!takeValue(argc, argv, index, "--gfeversion", value, error)) return false;
            config.gfeVersion = value;
        } else if (arg == "--rikey") {
            if (!takeValue(argc, argv, index, "--rikey", value, error)) return false;
            config.rikey = value;
        } else if (arg == "--rikeyid") {
            if (!takeValue(argc, argv, index, "--rikeyid", value, error)) return false;
            config.rikeyid = value;
        } else if (arg == "--dump-video") {
            if (!takeValue(argc, argv, index, "--dump-video", value, error)) return false;
            config.dumpVideoPath = value;
        } else if (arg == "--codec-support") {
            if (!takeValue(argc, argv, index, "--codec-support", value, error)) return false;
            if (!parseDecimalOrHex(value, config.serverCodecModeSupport)) {
                error = "invalid value for --codec-support: '" + value + "'";
                return false;
            }
        } else if (arg == "--rtsp-port") {
            if (!takeValue(argc, argv, index, "--rtsp-port", value, error)) return false;
            if (!parseDecimal(value, config.rtspPort)) {
                error = "invalid value for --rtsp-port: '" + value + "'";
                return false;
            }
        } else if (arg == "--width") {
            if (!takeValue(argc, argv, index, "--width", value, error)) return false;
            if (!parseDecimal(value, config.width)) {
                error = "invalid value for --width: '" + value + "'";
                return false;
            }
        } else if (arg == "--height") {
            if (!takeValue(argc, argv, index, "--height", value, error)) return false;
            if (!parseDecimal(value, config.height)) {
                error = "invalid value for --height: '" + value + "'";
                return false;
            }
        } else if (arg == "--fps") {
            if (!takeValue(argc, argv, index, "--fps", value, error)) return false;
            if (!parseDecimal(value, config.fps)) {
                error = "invalid value for --fps: '" + value + "'";
                return false;
            }
        } else if (arg == "--bitrate") {
            if (!takeValue(argc, argv, index, "--bitrate", value, error)) return false;
            if (!parseDecimal(value, config.bitrateKbps)) {
                error = "invalid value for --bitrate: '" + value + "'";
                return false;
            }
        } else if (arg == "--seconds") {
            if (!takeValue(argc, argv, index, "--seconds", value, error)) return false;
            if (!parseDecimal(value, config.seconds)) {
                error = "invalid value for --seconds: '" + value + "'";
                return false;
            }
        } else if (arg == "--formats") {
            if (!takeValue(argc, argv, index, "--formats", value, error)) return false;
            if (!parseFormatMask(value, config.videoFormats, error)) {
                return false;
            }
        } else if (arg == "--repeat") {
            if (!takeValue(argc, argv, index, "--repeat", value, error)) return false;
            if (!parseDecimal(value, repeat) || repeat <= 0) {
                error = "invalid value for --repeat: '" + value + "'";
                return false;
            }
        } else {
            error = "unknown option: '" + arg + "'";
            return false;
        }
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    tg::sunshine::StreamConfig config;
    int repeat = 1;
    bool help = false;
    std::string error;

    if (!parseArguments(argc, argv, config, repeat, help, error)) {
        fprintf(stderr, "error: %s\n\n", error.c_str());
        printUsage(argv[0]);
        return 2;
    }
    if (help) {
        printUsage(argv[0]);
        return 0;
    }

    bool allOk = true;
    for (int attempt = 0; attempt < repeat; ++attempt) {
        if (repeat > 1) {
            printf("=== run %d/%d ===\n", attempt + 1, repeat);
            fflush(stdout);
        }
        tg::sunshine::StreamStats stats;
        const bool ok = tg::sunshine::run(config, stats);
        printSummary(stats);
        allOk = allOk && ok;
        if (repeat > 1 && attempt + 1 < repeat) {
            printf("\n");
        }
    }
    return allOk ? 0 : 1;
}
