// SPDX-License-Identifier: GPL-3.0-or-later
//
// Moonlight / Sunshine 的 HTTP 协议端点（nvhttp）。
//
// moonlight-common-c 不发任何 HTTP，因此下列端点都由本模块直接实现：
//   /serverinfo  /applist  /launch  /resume  /cancel  /unpair  /pair
// 参数与响应格式以 Sunshine 服务端源码为准（.work/ref/sunshine/src/nvhttp.cpp），
// 客户端意图参考 moonlight-qt（.work/ref/moonlight-qt/app/backend/nvhttp.cpp）。
// /pair 五阶段握手与证书指纹固定见 pairing.h。
#pragma once

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "identity.h"
#include "pairing.h"

namespace tg {
namespace sunshine {

// HTTP 状态码 + NV 协议 XML 中的 status_code 双重错误。
// transport 层失败时 http_status 为 0。
class NvError : public std::runtime_error {
public:
    NvError(const std::string& message, int http_status, int nv_status_code,
            const std::string& status_message, const std::string& response_body);
    ~NvError() override;

    int http_status() const { return http_status_; }
    int nv_status_code() const { return nv_status_code_; }
    const std::string& status_message() const { return status_message_; }
    const std::string& response_body() const { return response_body_; }

private:
    int http_status_;
    int nv_status_code_;
    std::string status_message_;
    std::string response_body_;
};

// 已固定的服务端证书指纹与本次 TLS 握手不一致：所有 TLS 请求在握手后、发请求前失败。
class PinMismatchError : public NvError {
public:
    PinMismatchError(const std::string& message, const std::string& expected_sha256,
                     const std::string& actual_sha256);
    ~PinMismatchError() override;

    const std::string& expected_sha256() const { return expected_sha256_; }
    const std::string& actual_sha256() const { return actual_sha256_; }

private:
    std::string expected_sha256_;
    std::string actual_sha256_;
};

// 原始响应，供上层打印/调试。
struct RawResponse {
    int http_status = 0;
    std::string body;
    std::string server_cert_sha256;  // TLS 握手后取到的服务端证书指纹（明文请求为空）
};

// /serverinfo 解析结果。字段名与 Sunshine nvhttp.cpp:684-791 一致。
struct ServerInfo {
    std::string hostname;
    std::string appversion;
    std::string gfe_version;
    std::string server_uniqueid;
    std::string mac;
    std::string local_ip;
    std::string external_ip;
    int https_port = 0;
    int external_port = 0;
    int pair_status = 0;   // Sunshine: 0 未配对 / 1 已配对（仅 HTTPS 下有意义）
    std::string state;     // SUNSHINE_SERVER_FREE / SUNSHINE_SERVER_BUSY
    int current_game = 0;
    uint32_t server_codec_mode_support = 0;
    bool has_max_luma_hevc = false;
    uint64_t max_luma_pixels_hevc = 0;
    bool has_max_luma_av1 = false;
    uint64_t max_luma_pixels_av1 = 0;

    bool paired() const { return pair_status == 1; }
};

// 同一次探测里两条路径（明文 HTTP / TLS+客户端证书）各自的结果。
// 未配对时 HTTPS 会在证书验证阶段被拒（Sunshine 返回 status_code=401 的 XML）。
struct ServerInfoProbe {
    bool http_ok = false;
    int http_status = 0;
    std::string http_error;
    std::string http_xml;
    ServerInfo http;

    bool https_ok = false;
    int https_status = 0;
    std::string https_error;
    std::string https_xml;
    ServerInfo https;
    std::string https_cert_sha256;

    ServerInfo server;  // 优先 HTTPS，否则明文 HTTP
    int https_port_used = 0;
};

// /applist 中的应用条目。Sunshine 只返回 ID / AppTitle / IsHdrSupported
// （nvhttp.cpp:794-821），其余字段为兼容 GFE 而按可选处理。
struct AppInfo {
    int id = 0;
    std::string title;
    std::string app_name;
    bool hdr_supported = false;
    bool hidden = false;
    bool app_collector_game = false;
};

// /launch 与 /resume 的可选项。默认值对应 moonlight-qt 的常见配置。
struct LaunchOptions {
    int app_id = 0;
    int width = 1920;
    int height = 1080;
    int fps = 60;
    bool sops = true;            // 服务端游戏优化（Sunshine 用于是否调整显示模式）
    bool local_audio = false;    // 音频在主机播放
    bool hdr = false;            // 是否声明 10bit/HDR 能力
    uint32_t controller_bitmap = 0;
    uint32_t gcmap = 0;
    bool gcpersist = true;
    int surround_audio_info = 196610;  // 立体声：(mask 0x3 << 16) | 2
};

// /launch 与 /resume 的响应。rikey/rikeyid 由本模块生成，交给拉流客户端。
struct SessionLaunch {
    std::string session_url0;
    std::string session_url1;
    std::string rikey;  // 16 字节随机密钥的小写 hex（32 字符）
    uint32_t rikeyid = 0;
    std::string raw_xml;
};

class NvHttpClient {
public:
    struct Options {
        std::string host = "127.0.0.1";
        uint16_t http_port = 47989;
        uint16_t https_port = 0;  // 0 = 从 /serverinfo 的 HttpsPort 发现
        // 由调用方传入（应用沙箱目录的绝对路径），没有默认值：证书/私钥/uniqueid
        // 与 pin 全部落在该目录下。
        std::string state_dir;
        int connect_timeout_ms = 10000;
        int io_timeout_ms = 10000;
        // getservercert 阶段要等网页端提交 PIN；服务端此时挂起响应，所以单独给更长的时限。
        int pin_wait_ms = 300000;
    };

    explicit NvHttpClient(Options options);
    ~NvHttpClient();

    NvHttpClient(const NvHttpClient&) = delete;
    NvHttpClient& operator=(const NvHttpClient&) = delete;

    const Identity& identity() const;

    // 明文 HTTP /serverinfo 取 HttpsPort，再用 TLS + 客户端证书取一次。
    ServerInfoProbe get_server_info();

    // 重连快路径：跳过明文探测，直接用已知的 HTTPS 端口发一次 /serverinfo
    // （TLS + 客户端证书，走同一套证书指纹校验）。端口未知或请求失败时抛
    // NvError，调用方退回 get_server_info() 的完整探测。成功时把端口记进
    // discovered_https_port，供后续 /applist、/launch、/cancel 复用。
    ServerInfo get_server_info_https();

    std::vector<AppInfo> get_app_list();

    SessionLaunch launch(const LaunchOptions& options);
    SessionLaunch resume(const LaunchOptions& options);

    void cancel();
    void unpair();

    // 通用请求（HTTPS，自动附加 uniqueid/uuid 公共参数）。配对阶段可复用；
    // 协议端点本身全部使用 GET。
    RawResponse get(const std::string& path, const std::string& extra_query = "");
    RawResponse post(const std::string& path, const std::string& extra_query,
                     const std::string& body,
                     const std::string& content_type = "application/x-www-form-urlencoded");

    // ---- /pair 五阶段 ----
    // Sunshine 服务端顺序：getservercert -> clientchallenge -> serverchallengeresp
    // -> clientpairingsecret（前四步明文 HTTP，nvhttp.cpp:553-628）-> pairchallenge
    //（最后一步 HTTPS + 客户端证书，nvhttp.cpp:605-608）。
    // 成功后把服务端证书与 SHA-256 指纹固定到 state_dir；失败抛 PairError。
    PairResult pair(const std::string& pin, const std::string& device_name,
                    const PairProgress& progress = {});

private:
    struct Impl;

    void ensure_https_port();
    uint16_t https_port() const;
    // 只走明文 HTTP /serverinfo 取 HttpsPort（配对前不能依赖已授权证书）。
    uint16_t ensure_https_port_plaintext();
    SessionLaunch start_app(const char* verb, const LaunchOptions& options);

    std::unique_ptr<Impl> impl_;
};

}  // namespace sunshine
}  // namespace tg
