// SPDX-License-Identifier: GPL-3.0-or-later
//
// 客户端身份：自签证书 + 私钥 + uniqueid 的生成与持久化。
//
// 生成参数对齐 moonlight-qt app/backend/identitymanager.cpp:29-109：
//   * RSA 2048（EVP_RSA_gen(2048)，OpenSSL 3 路径）
//   * X.509 v3（版本号 2），serial = 0
//   * notBefore = 现在，notAfter = 现在 + 20 年
//   * subject = issuer = CN=NVIDIA GameStream Client
//   * 自签，摘要 SHA-256
// uniqueid 对齐 identitymanager.cpp:139-153：RAND_bytes 取 64 位随机数，
// 以 base 16（小写、不补零）持久化。
#pragma once

#include <cstdint>
#include <string>

namespace tg {
namespace sunshine {

// 证书摘要信息。只读取证书，不接触私钥内容。
struct X509Summary {
    std::string subject;             // X509_NAME_oneline，如 "/CN=NVIDIA GameStream Client"
    std::string common_name;         // 仅 CN 段
    std::string serial_hex;          // 如 "0x0"
    long long serial_decimal = 0;
    std::string not_before;          // ASN1_TIME_print，如 "Nov 27 07:10:10 2026 GMT"
    std::string not_after;
    std::string sha256_fingerprint;  // "AA:BB:..."（大写、冒号分隔）
    int key_bits = 0;
};

// 一份可复用的客户端身份。cert_pem 是证书文本；私钥只以路径形式持有，
// 内容既不进结构体也不进日志/输出。
struct Identity {
    std::string state_dir;
    std::string cert_path;
    std::string key_path;
    std::string uniqueid_path;
    std::string cert_pem;
    std::string uniqueid;
    bool created_now = false;  // 本次调用是否新建了证书/私钥/uniqueid
};

// 在 state_dir 下加载身份；缺失或损坏时生成并落盘。
// 证书/私钥任一不可用（或互不匹配）时整体重新生成，与 moonlight-qt 行为一致。
// 失败抛 std::runtime_error（错误信息不含私钥内容）。
Identity load_or_create_identity(const std::string& state_dir);

// 解析 Identity::cert_pem 得到摘要信息。失败抛 std::runtime_error。
X509Summary summarize_identity(const Identity& identity);

// 只读检查私钥文件权限，返回八进制权限位（如 0600）；失败返回 -1。
int private_key_mode(const Identity& identity);

}  // namespace sunshine
}  // namespace tg
