// SPDX-License-Identifier: GPL-3.0-or-later
//
// Moonlight 配对协议（/pair 多阶段握手）、服务端证书指纹固定（TOFU：配对即信任）
// 以及不依赖真实服务端的配对自检。
//
// 服务端依据（本机 Sunshine 2026.516.143833 / commit 14ffa6f，与 .work/ref/sunshine 同版本）：
//   nvhttp.cpp:553-640    /pair 路由、uniqueid 校验与 phrase/参数分派
//   nvhttp.cpp:354-376    getservercert：注册会话、由 salt+PIN 派生 AES 密钥、返回 plaincert
//   nvhttp.cpp:378-419    clientchallenge：解出 16 字节 challenge，回 SHA-256 与 serverchallenge
//   nvhttp.cpp:421-448    serverchallengeresp：解出 clienthash，回 serversecret + 服务端签名
//   nvhttp.cpp:450-500    clientpairingsecret：比对 hash 与客户端签名，登记客户端证书并 paired=1
//   nvhttp.cpp:605-608    phrase=pairchallenge（走 HTTPS）：直接 paired=1
//   crypto.cpp:324-338    gen_aes_key = SHA-256(salt16 || PIN) 取前 16 字节
//   crypto.cpp:222-273    AES-128-ECB，无填充（crypto::cipher::ecb_t(key, false)）
//   crypto.cpp:388-395    X509 签名取 signature BIT STRING 原始字节（RSA-2048 = 256 字节）
//   crypto.cpp:473-499    sign256/verify256 = RSA PKCS#1 v1.5 + SHA-256
// 客户端意图：
//   moonlight-qt app/backend/nvpairingmanager.cpp:238-402
//   （Gen 7+ 用 SHA-256，hashLength=32；Gen 6 及更早用 SHA-1——本实现只支持 Gen 7+）
#pragma once

#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

#include "common/identity.h"

namespace tg::sunshine {

// 单个配对阶段的进度记录（供 CLI 逐阶段打印；不含密钥/PIN）。
struct PairStageRecord {
    std::string name;  // 服务端可识别的阶段名，如 "getservercert"
    bool tls = false;  // 本阶段走明文 HTTP 还是 HTTPS
    long long elapsed_ms = 0;
    bool ok = false;
    std::string detail;  // 失败原因；成功为空
};

// 一次 /pair 请求。query 不含公共参数 uniqueid/uuid（由传输层补）。
struct PairRequest {
    bool tls = false;
    std::string query;
    int timeout_ms = 0;  // 0 = 传输层默认；getservercert 阶段用 pin_wait_ms
};

struct PairResponse {
    int http_status = 0;
    std::string body;
    std::string server_cert_sha256;  // TLS 时握手得到的服务端证书指纹（"AA:BB:..."）
};

using PairTransport = std::function<PairResponse(const PairRequest&)>;
using PairProgress = std::function<void(const PairStageRecord&)>;

struct PairOptions {
    std::string pin;          // 4 位数字
    std::string device_name;  // devicename 参数（Sunshine 忽略其内容）
    std::string host;         // 用于 pin 文件名与错误信息
    uint16_t https_port = 0;  // 最后阶段（pairchallenge）的 HTTPS 端口
    int server_generation = 7;  // appversion 主版本；Sunshine 报 7
    int pin_wait_ms = 300000;   // getservercert 等待网页端提交 PIN 的时限
};

struct PairResult {
    bool paired = false;
    std::string raw_xml;             // clientpairingsecret 阶段的原始响应
    std::string server_cert_pem;     // plaincert 解出的服务端证书
    std::string server_cert_sha256;  // 上述证书的 SHA-256 指纹
    std::string pin_cert_path;       // 成功后写入的证书文件路径
    std::string pin_sha256_path;     // 成功后写入的指纹文件路径
    std::vector<PairStageRecord> stages;
};

// 配对失败：带阶段名与服务端原始 status_code/status_message。
class PairError : public std::runtime_error {
public:
    PairError(const std::string& stage, const std::string& message, int http_status,
              int nv_status_code, const std::string& status_message,
              const std::string& response_body);
    ~PairError() override;

    const std::string& stage() const { return stage_; }
    int http_status() const { return http_status_; }
    int nv_status_code() const { return nv_status_code_; }
    const std::string& status_message() const { return status_message_; }
    const std::string& response_body() const { return response_body_; }

private:
    std::string stage_;
    int http_status_;
    int nv_status_code_;
    std::string status_message_;
    std::string response_body_;
};

// ---- 服务端证书指纹固定（TOFU）----
// 文件：<state_dir>/server-<host>-<httpsPort>.pem 与同名 .sha256，权限均 0644。
// .sha256 内容为大写、冒号分隔的 SHA-256 指纹加换行；加载时从 PEM 重新计算并与其比对，
// 两者不一致说明文件被改动或损坏，直接报错而不是静默放行。
struct ServerPin {
    std::string host;
    uint16_t https_port = 0;
    std::string cert_path;
    std::string sha256_path;
    std::string cert_pem;  // 仅在 exists() 时有效
    std::string sha256;    // 仅 exists() 时有效；大写冒号分隔
    bool exists() const { return !sha256.empty(); }
};

// 读取该主机的 pin；不存在时返回 exists()==false。文件不完整/损坏抛 std::runtime_error。
ServerPin load_server_pin(const std::string& state_dir, const std::string& host,
                          uint16_t https_port);

// 比对握手得到的服务端证书指纹。返回空串表示通过，否则为错误说明。
// 没有 pin 时永远通过（未配对时 TLS 本来就会被服务端以 401 拒绝）。
std::string check_server_pin(const ServerPin& pin, const std::string& peer_sha256);

// 配对成功后写入 pin（cert_pem + sha256，0644）。失败抛 std::runtime_error。
void store_server_pin(ServerPin& pin, const std::string& cert_pem, const std::string& sha256);

// ---- 密码学原语（配对流程与 --selftest 共用）----
// AES-128 密钥 = SHA-256(salt16 || pin) 的前 16 字节（crypto.cpp:324-338）。
std::vector<uint8_t> pairing_key(const std::vector<uint8_t>& salt, const std::string& pin);

// AES-128-ECB，无填充；data 长度必须是 16 的倍数（crypto.cpp:222-273）。
std::vector<uint8_t> aes128_ecb(const std::vector<uint8_t>& key,
                                const std::vector<uint8_t>& data, bool encrypt);

// ---- 配对流程 ----
// 依次执行 getservercert -> clientchallenge -> serverchallengeresp ->
// clientpairingsecret（明文）-> pairchallenge（TLS），失败抛 PairError。
PairResult run_pairing(const Identity& identity, const PairOptions& options,
                       const PairTransport& transport, const PairProgress& progress = {});

// ---- 自检（临时证书，不访问网络，不触碰状态目录里的身份）----
struct SelfTestResult {
    std::string name;
    bool passed = false;
    std::string detail;
};

std::vector<SelfTestResult> run_pairing_selftest();

}  // namespace tg::sunshine
