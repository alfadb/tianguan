// SPDX-License-Identifier: GPL-3.0-or-later
//
// tg-sunshine-ctl：Sunshine / Moonlight 实验用命令行工具。
// 子命令：identity / serverinfo / applist / launch / resume / cancel / unpair / pair / selftest
#include "common/identity.h"
#include "common/nv_http.h"
#include "common/pairing.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include <termios.h>
#include <unistd.h>

namespace {

constexpr int kExitOk = 0;
constexpr int kExitFailure = 1;
constexpr int kExitUsage = 2;

struct GlobalOptions {
    std::string host = "127.0.0.1";
    int port = 47989;
    std::string state_dir = ".work/sunshine-spike";
};

struct LaunchArguments {
    int app_id = 0;
    int width = 0;
    int height = 0;
    int fps = 0;
    bool has_mode = false;
};

struct PairArguments {
    std::string device_name = "tianguan-spike";
    int pin_wait_seconds = 300;
};

void print_usage(std::ostream& out) {
    out << "用法: tg-sunshine-ctl [--host H] [--port N] [--state-dir DIR] <子命令> ...\n"
           "\n"
           "子命令:\n"
           "  identity                 打印客户端身份（证书主题、serial、有效期、SHA-256 指纹、uniqueid）\n"
           "  serverinfo [--raw]       同时走明文 HTTP 与 HTTPS（带客户端证书）取 /serverinfo，并显示已固定的服务端指纹\n"
           "  applist                  取 /applist\n"
           "  launch --appid N --mode WxHxFPS   取 /launch，输出 sessionUrl0 / rikey / rikeyid\n"
           "  resume --appid N --mode WxHxFPS   取 /resume，同上\n"
           "  cancel                   取 /cancel\n"
           "  unpair                   取 /unpair（Sunshine 未暴露该端点）\n"
           "  pair [--device-name NAME] [--pin-wait 秒]\n"
           "                           执行 /pair 五阶段配对并固定服务端证书指纹；\n"
           "                           PIN 从标准输入读一行 4 位数字（可管道输入），--pin-wait 默认 300 秒\n"
           "  selftest                 配对密码学自检（临时证书，不访问网络）\n"
           "\n"
           "公共参数:\n"
           "  --host HOST              服务端地址（默认 127.0.0.1）\n"
           "  --port N                 明文 HTTP 端口（默认 47989；HTTPS 端口从 HttpsPort 发现）\n"
           "  --state-dir DIR          身份状态目录（默认 .work/sunshine-spike）\n";
}

std::string one_line(const std::string& text, size_t limit = 600) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        out.push_back(c == '\n' || c == '\r' ? ' ' : c);
    }
    if (out.size() > limit) {
        out = out.substr(0, limit) + "...";
    }
    return out;
}

bool is_four_digit_pin(const std::string& value) {
    if (value.size() != 4) {
        return false;
    }
    for (char c : value) {
        if (c < '0' || c > '9') {
            return false;
        }
    }
    return true;
}

// 从标准输入读一行 PIN：不打印到任何输出，TTY 下临时关闭回显。
std::string read_pin_from_stdin() {
    std::string line;
    termios saved {};
    bool restore_echo = false;
    if (::isatty(STDIN_FILENO) != 0 && ::tcgetattr(STDIN_FILENO, &saved) == 0) {
        termios no_echo = saved;
        no_echo.c_lflag &= ~static_cast<tcflag_t>(ECHO);
        if (::tcsetattr(STDIN_FILENO, TCSANOW, &no_echo) == 0) {
            restore_echo = true;
            std::cerr << "请输入 4 位 PIN: " << std::flush;
        }
    }
    std::getline(std::cin, line);
    if (restore_echo) {
        ::tcsetattr(STDIN_FILENO, TCSANOW, &saved);
        std::cerr << "\n";
    }
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
        line.pop_back();
    }
    return line;
}

void print_nv_error(const tg::sunshine::NvError& error) {
    std::cerr << "错误: " << error.what() << "\n";
    if (error.http_status() != 0) {
        std::cerr << "  HTTP 状态码: " << error.http_status() << "\n";
    }
    if (error.nv_status_code() != 0) {
        std::cerr << "  NV 状态码: " << error.nv_status_code() << "\n";
    }
    if (!error.status_message().empty()) {
        std::cerr << "  状态信息: " << error.status_message() << "\n";
    }
    if (!error.response_body().empty()) {
        std::cerr << "  响应: " << one_line(error.response_body()) << "\n";
    }
}

bool parse_int_value(const std::string& text, int& value) {
    char* end = nullptr;
    long parsed = std::strtol(text.c_str(), &end, 10);
    if (end == nullptr || *end != '\0' || parsed < 0 || parsed > 0x7FFFFFFF) {
        return false;
    }
    value = static_cast<int>(parsed);
    return true;
}

bool parse_mode(const std::string& text, LaunchArguments& arguments) {
    int width = 0;
    int height = 0;
    int fps = 0;
    if (std::sscanf(text.c_str(), "%dx%dx%d", &width, &height, &fps) != 3) {
        return false;
    }
    if (width <= 0 || height <= 0 || fps <= 0) {
        return false;
    }
    arguments.width = width;
    arguments.height = height;
    arguments.fps = fps;
    arguments.has_mode = true;
    return true;
}

void print_server_info_fields(const tg::sunshine::ServerInfo& info, bool hide_mac) {
    std::cout << "  hostname: " << info.hostname << "\n";
    std::cout << "  appversion: " << info.appversion << "\n";
    std::cout << "  GfeVersion: " << info.gfe_version << "\n";
    std::cout << "  HttpsPort: " << info.https_port << "\n";
    std::cout << "  ExternalPort: " << info.external_port << "\n";
    std::cout << "  PairStatus: " << info.pair_status << "\n";
    std::cout << "  state: " << info.state << "\n";
    std::cout << "  currentgame: " << info.current_game << "\n";
    std::cout << "  ServerCodecModeSupport: " << info.server_codec_mode_support << "\n";
    if (info.has_max_luma_hevc) {
        std::cout << "  MaxLumaPixelsHEVC: " << info.max_luma_pixels_hevc << "\n";
    } else {
        std::cout << "  MaxLumaPixelsHEVC: (未提供)\n";
    }
    if (info.has_max_luma_av1) {
        std::cout << "  MaxLumaPixelsAV1: " << info.max_luma_pixels_av1 << "\n";
    } else {
        std::cout << "  MaxLumaPixelsAV1: (未提供)\n";
    }
    std::cout << "  LocalIP: " << info.local_ip << "\n";
    std::cout << "  ExternalIP: " << (info.external_ip.empty() ? "(未提供)" : info.external_ip) << "\n";
    if (hide_mac) {
        std::cout << "  mac: (已隐藏，--raw 可查看原始响应)\n";
    } else {
        std::cout << "  mac: " << info.mac << "\n";
    }
}

void print_app_list(const std::vector<tg::sunshine::AppInfo>& apps) {
    if (apps.empty()) {
        std::cout << "（应用列表为空）\n";
        return;
    }
    for (const tg::sunshine::AppInfo& app : apps) {
        std::cout << "  [" << app.id << "] " << app.title;
        if (!app.app_name.empty()) {
            std::cout << " (AppName=" << app.app_name << ")";
        }
        std::cout << " HDR=" << (app.hdr_supported ? 1 : 0);
        if (app.hidden) {
            std::cout << " hidden=1";
        }
        if (app.app_collector_game) {
            std::cout << " collector=1";
        }
        std::cout << "\n";
    }
}

void print_launch_result(const tg::sunshine::SessionLaunch& launch) {
    std::cout << "  status: 200\n";
    std::cout << "sessionUrl0: " << launch.session_url0 << "\n";
    if (!launch.session_url1.empty()) {
        std::cout << "sessionUrl1: " << launch.session_url1 << "\n";
    }
    std::cout << "rikey: " << launch.rikey << "\n";
    std::cout << "rikeyid: " << launch.rikeyid << "\n";
}

int command_identity(const GlobalOptions& global) {
    tg::sunshine::Identity identity = tg::sunshine::load_or_create_identity(global.state_dir);
    tg::sunshine::X509Summary summary = tg::sunshine::summarize_identity(identity);

    std::cout << "state_dir: " << identity.state_dir << "\n";
    std::cout << "identity_source: " << (identity.created_now ? "generated" : "reused") << "\n";
    std::cout << "certificate: " << identity.cert_path << "\n";
    std::cout << "private_key: " << identity.key_path << "\n";

    int mode = tg::sunshine::private_key_mode(identity);
    if (mode >= 0) {
        char buffer[16];
        std::snprintf(buffer, sizeof(buffer), "%04o", mode);
        std::cout << "private_key_mode: " << buffer << "\n";
    } else {
        std::cout << "private_key_mode: (无法读取)\n";
    }

    std::cout << "subject: " << summary.subject << "\n";
    std::cout << "common_name: " << summary.common_name << "\n";
    std::cout << "serial: " << summary.serial_decimal << " (" << summary.serial_hex << ")\n";
    std::cout << "not_before: " << summary.not_before << "\n";
    std::cout << "not_after: " << summary.not_after << "\n";
    std::cout << "key_bits: " << summary.key_bits << "\n";
    std::cout << "sha256: " << summary.sha256_fingerprint << "\n";
    std::cout << "uniqueid: " << identity.uniqueid << "\n";
    return kExitOk;
}

int command_server_info(const GlobalOptions& global, bool raw) {
    tg::sunshine::NvHttpClient::Options options;
    options.host = global.host;
    options.http_port = static_cast<uint16_t>(global.port);
    options.state_dir = global.state_dir;
    tg::sunshine::NvHttpClient client(options);

    tg::sunshine::ServerInfoProbe probe = client.get_server_info();

    std::cout << "== 明文 HTTP /serverinfo (" << global.host << ":" << global.port << ") ==\n";
    if (probe.http_ok) {
        std::cout << "  http_status: " << probe.http_status << "\n";
        print_server_info_fields(probe.http, true);
    } else {
        std::cout << "  失败: " << probe.http_error << "\n";
    }

    std::cout << "\n== HTTPS /serverinfo (" << global.host << ":" << probe.https_port_used
              << "，TLS1.2+ 且带客户端证书) ==\n";
    if (!probe.https_cert_sha256.empty()) {
        std::cout << "  server_cert_sha256: " << probe.https_cert_sha256 << "\n";
    }
    if (probe.https_ok) {
        std::cout << "  http_status: " << probe.https_status << "\n";
        print_server_info_fields(probe.https, true);
    } else {
        std::cout << "  未取得已授权的 serverinfo: " << probe.https_error << "\n";
        if (probe.https_error.find("401") != std::string::npos ||
            probe.https_error.find("authorized") != std::string::npos) {
            std::cout << "  （未配对时这是预期行为：服务端拒绝未授权客户端证书）\n";
        }
    }

    std::cout << "\n== 汇总 ==\n";
    std::cout << "  http: " << (probe.http_ok ? "ok" : "failed") << "\n";
    std::cout << "  https: " << (probe.https_ok ? "ok" : "rejected") << "\n";
    std::cout << "  paired: " << (probe.server.paired() ? 1 : 0) << "\n";
    std::cout << "  ServerCodecModeSupport: " << probe.server.server_codec_mode_support << "\n";

    // 该主机是否已有固定的服务端证书指纹（只打印指纹，不打印证书全文）。
    tg::sunshine::ServerPin pin = tg::sunshine::load_server_pin(
        global.state_dir, global.host, static_cast<uint16_t>(probe.https_port_used));
    if (pin.exists()) {
        std::cout << "  server_pin: " << pin.sha256 << "\n";
        std::cout << "  server_pin_file: " << pin.sha256_path << "\n";
    } else {
        std::cout << "  server_pin: (无，尚未配对或未固定)\n";
    }

    if (raw) {
        std::cout << "\n--- HTTP 原始响应 ---\n";
        std::cout << probe.http_xml << "\n";
        std::cout << "--- HTTPS 原始响应 ---\n";
        std::cout << probe.https_xml << "\n";
    }
    return (probe.http_ok || probe.https_ok) ? kExitOk : kExitFailure;
}

int command_app_list(const GlobalOptions& global) {
    tg::sunshine::NvHttpClient::Options options;
    options.host = global.host;
    options.http_port = static_cast<uint16_t>(global.port);
    options.state_dir = global.state_dir;
    tg::sunshine::NvHttpClient client(options);

    std::cout << "== /applist ==\n" << std::flush;
    std::vector<tg::sunshine::AppInfo> apps = client.get_app_list();
    print_app_list(apps);
    return kExitOk;
}
int command_launch(const GlobalOptions& global, const char* verb,
                   const LaunchArguments& arguments) {
    tg::sunshine::NvHttpClient::Options options;
    options.host = global.host;
    options.http_port = static_cast<uint16_t>(global.port);
    options.state_dir = global.state_dir;
    tg::sunshine::NvHttpClient client(options);

    tg::sunshine::LaunchOptions launch_options;
    launch_options.app_id = arguments.app_id;
    launch_options.width = arguments.width;
    launch_options.height = arguments.height;
    launch_options.fps = arguments.fps;

    std::cout << "== /" << verb << " ==\n" << std::flush;
    tg::sunshine::SessionLaunch launch =
        std::strcmp(verb, "resume") == 0 ? client.resume(launch_options) : client.launch(launch_options);
    print_launch_result(launch);
    return kExitOk;
}

int command_simple(const GlobalOptions& global, const char* command) {
    tg::sunshine::NvHttpClient::Options options;
    options.host = global.host;
    options.http_port = static_cast<uint16_t>(global.port);
    options.state_dir = global.state_dir;
    tg::sunshine::NvHttpClient client(options);

    std::cout << "== /" << command << " ==\n";
    if (std::strcmp(command, "cancel") == 0) {
        client.cancel();
    } else {
        client.unpair();
    }
    std::cout << "  status: 200\n";
    return kExitOk;
}

int command_pair(const GlobalOptions& global, const PairArguments& arguments,
                 const std::string& pin) {
    tg::sunshine::NvHttpClient::Options options;
    options.host = global.host;
    options.http_port = static_cast<uint16_t>(global.port);
    options.state_dir = global.state_dir;
    options.pin_wait_ms = arguments.pin_wait_seconds * 1000;
    tg::sunshine::NvHttpClient client(options);

    std::cout << "== /pair（" << global.host << ":" << global.port
              << "，devicename=" << arguments.device_name << "，逐阶段）==\n"
              << std::flush;
    std::cerr << "等待网页端提交 PIN（最多 " << arguments.pin_wait_seconds << " 秒）…\n"
              << std::flush;
    // 只打印阶段名/协议/耗时/服务端消息，不打印 PIN、密钥或证书全文。
    tg::sunshine::PairProgress progress = [](const tg::sunshine::PairStageRecord& stage) {
        std::cout << "  [" << (stage.ok ? "ok" : "fail") << "] " << stage.name
                  << (stage.tls ? "（HTTPS）" : "（HTTP）") << "  " << stage.elapsed_ms << " ms";
        if (!stage.detail.empty()) {
            std::cout << "  " << one_line(stage.detail);
        }
        std::cout << "\n" << std::flush;
    };

    tg::sunshine::PairResult result = client.pair(pin, arguments.device_name, progress);
    std::cout << "paired: " << (result.paired ? 1 : 0) << "\n";
    std::cout << "server_cert_sha256: " << result.server_cert_sha256 << "\n";
    std::cout << "pin_cert: " << result.pin_cert_path << "\n";
    std::cout << "pin_sha256: " << result.pin_sha256_path << "\n";
    return kExitOk;
}

int command_selftest() {
    std::cout << "== 配对密码学自检（临时证书，不访问网络，不写状态目录里的身份）==\n";
    const std::vector<tg::sunshine::SelfTestResult> results = tg::sunshine::run_pairing_selftest();
    size_t passed = 0;
    for (const tg::sunshine::SelfTestResult& result : results) {
        std::cout << "  [" << (result.passed ? "PASS" : "FAIL") << "] " << result.name << "\n";
        if (!result.detail.empty()) {
            std::cout << "         " << one_line(result.detail) << "\n";
        }
        if (result.passed) {
            ++passed;
        }
    }
    std::cout << "结果: " << passed << "/" << results.size() << " 通过\n";
    return passed == results.size() ? kExitOk : kExitFailure;
}

int run(int argc, char** argv) {
    GlobalOptions global;
    std::vector<std::string> rest;

    for (int i = 1; i < argc; ++i) {
        std::string argument = argv[i];
        if (argument == "--host" || argument == "--port" || argument == "--state-dir") {
            if (i + 1 >= argc) {
                std::cerr << "错误: " << argument << " 缺少参数值\n";
                return kExitUsage;
            }
            std::string value = argv[++i];
            if (argument == "--host") {
                global.host = value;
            } else if (argument == "--state-dir") {
                global.state_dir = value;
            } else {
                int port = 0;
                if (!parse_int_value(value, port) || port <= 0 || port > 65535) {
                    std::cerr << "错误: --port 需要 1..65535 的整数\n";
                    return kExitUsage;
                }
                global.port = port;
            }
        } else {
            rest.push_back(argument);
        }
    }

    if (rest.empty()) {
        print_usage(std::cerr);
        return kExitUsage;
    }

    const std::string command = rest.front();
    rest.erase(rest.begin());
    if (command == "--help" || command == "-h" || command == "help") {
        print_usage(std::cout);
        return kExitOk;
    }

    bool raw = false;
    LaunchArguments arguments;
    PairArguments pair_arguments;
    for (size_t i = 0; i < rest.size(); ++i) {
        const std::string& argument = rest[i];
        if (argument == "--raw") {
            raw = true;
            continue;
        }
        if (argument == "--device-name" || argument == "--pin-wait") {
            if (i + 1 >= rest.size()) {
                std::cerr << "错误: " << argument << " 缺少参数值\n";
                return kExitUsage;
            }
            std::string value = rest[++i];
            if (argument == "--device-name") {
                pair_arguments.device_name = value;
            } else {
                int seconds = 0;
                if (!parse_int_value(value, seconds) || seconds <= 0 || seconds > 86400) {
                    std::cerr << "错误: --pin-wait 需要 1..86400 的整数秒\n";
                    return kExitUsage;
                }
                pair_arguments.pin_wait_seconds = seconds;
            }
            continue;
        }
        if (argument == "--appid" || argument == "--mode") {
            if (i + 1 >= rest.size()) {
                std::cerr << "错误: " << argument << " 缺少参数值\n";
                return kExitUsage;
            }
            std::string value = rest[++i];
            if (argument == "--appid") {
                if (!parse_int_value(value, arguments.app_id) || arguments.app_id <= 0) {
                    std::cerr << "错误: --appid 需要正整数\n";
                    return kExitUsage;
                }
            } else if (!parse_mode(value, arguments)) {
                std::cerr << "错误: --mode 需要 WxHxFPS 形式（如 1920x1080x60）\n";
                return kExitUsage;
            }
            continue;
        }
        std::cerr << "错误: 无法识别的参数 " << argument << "\n";
        print_usage(std::cerr);
        return kExitUsage;
    }

    if (command == "identity") {
        return command_identity(global);
    }
    if (command == "serverinfo") {
        return command_server_info(global, raw);
    }
    if (command == "applist") {
        return command_app_list(global);
    }
    if (command == "launch" || command == "resume") {
        if (arguments.app_id <= 0 || !arguments.has_mode) {
            std::cerr << "错误: " << command << " 需要 --appid N 与 --mode WxHxFPS\n";
            return kExitUsage;
        }
        return command_launch(global, command.c_str(), arguments);
    }
    if (command == "cancel" || command == "unpair") {
        return command_simple(global, command.c_str());
    }
    if (command == "pair") {
        // PIN 只从标准输入读：先校验格式，不合格直接退出 2，不创建客户端、不发起网络请求。
        const std::string pin = read_pin_from_stdin();
        if (!is_four_digit_pin(pin)) {
            std::cerr << "错误: 从标准输入读取的 PIN 必须是 4 位纯数字\n";
            return kExitUsage;
        }
        return command_pair(global, pair_arguments, pin);
    }
    if (command == "selftest") {
        return command_selftest();
    }

    std::cerr << "错误: 未知子命令 " << command << "\n";
    print_usage(std::cerr);
    return kExitUsage;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const tg::sunshine::NvError& error) {
        print_nv_error(error);
        return kExitFailure;
    } catch (const tg::sunshine::PairError& error) {
        std::cerr << "配对失败（阶段: " << error.stage() << "）: " << error.what() << "\n";
        if (error.http_status() != 0) {
            std::cerr << "  HTTP 状态码: " << error.http_status() << "\n";
        }
        if (error.nv_status_code() != 0) {
            std::cerr << "  NV 状态码: " << error.nv_status_code() << "\n";
        }
        if (!error.status_message().empty()) {
            std::cerr << "  服务端消息: " << error.status_message() << "\n";
        }
        if (!error.response_body().empty()) {
            std::cerr << "  响应: " << one_line(error.response_body()) << "\n";
        }
        return kExitFailure;
    } catch (const std::exception& error) {
        std::cerr << "错误: " << error.what() << "\n";
        return kExitFailure;
    }
}
