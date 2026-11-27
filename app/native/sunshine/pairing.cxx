// SPDX-License-Identifier: GPL-3.0-or-later
//
// Moonlight 配对协议实现 + 服务端证书指纹固定（TOFU）。
// 协议依据见 pairing.h 顶部注释与各阶段旁的 nvhttp.cpp 行号。
#include "pairing.h"
#include "xml_util.h"

#include <openssl/asn1.h>
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/x509.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <utility>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace tg {
namespace sunshine {
namespace {

using Bytes = std::vector<uint8_t>;
using Clock = std::chrono::steady_clock;
using X509Ptr = std::unique_ptr<X509, decltype(&X509_free)>;
using PkeyPtr = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;

constexpr size_t kAesKeySize = 16;
constexpr size_t kGen7HashLength = 32;  // moonlight-qt nvpairingmanager.cpp:246-259

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

std::string openssl_error_text(const std::string& what) {
    std::string message = what;
    char buffer[256];
    unsigned long code = 0;
    while ((code = ERR_get_error()) != 0) {
        ERR_error_string_n(code, buffer, sizeof(buffer));
        message += ": ";
        message += buffer;
    }
    return message;
}

Bytes bytes_of(const std::string& text) {
    return Bytes(text.begin(), text.end());
}

std::string string_of(const Bytes& data) {
    return std::string(data.begin(), data.end());
}

Bytes random_bytes(size_t count) {
    Bytes out(count);
    if (count != 0 && RAND_bytes(out.data(), static_cast<int>(count)) != 1) {
        fail(openssl_error_text("生成随机数失败"));
    }
    return out;
}

std::string to_hex_lower(const uint8_t* data, size_t length) {
    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.reserve(length * 2);
    for (size_t i = 0; i < length; ++i) {
        out.push_back(kHex[data[i] >> 4]);
        out.push_back(kHex[data[i] & 0x0F]);
    }
    return out;
}

std::string to_hex_lower(const Bytes& data) {
    return to_hex_lower(data.data(), data.size());
}

int hex_value(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

Bytes hex_to_bytes(const std::string& hex) {
    if (hex.size() % 2 != 0) {
        fail("十六进制字符串长度为奇数");
    }
    Bytes out;
    out.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2) {
        int high = hex_value(hex[i]);
        int low = hex_value(hex[i + 1]);
        if (high < 0 || low < 0) {
            fail("十六进制字符串包含非法字符");
        }
        out.push_back(static_cast<uint8_t>((high << 4) | low));
    }
    return out;
}

// 只保留十六进制数字并转大写，便于忽略冒号/空白/大小写比较指纹。
std::string normalize_fingerprint(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (unsigned char c : text) {
        if (std::isxdigit(c)) {
            out.push_back(static_cast<char>(std::toupper(c)));
        }
    }
    return out;
}

std::string format_digest(const Bytes& digest) {
    static const char* kHex = "0123456789ABCDEF";
    std::string out;
    out.reserve(digest.size() * 3);
    for (size_t i = 0; i < digest.size(); ++i) {
        if (i != 0) {
            out.push_back(':');
        }
        out.push_back(kHex[digest[i] >> 4]);
        out.push_back(kHex[digest[i] & 0x0F]);
    }
    return out;
}

std::string trim(const std::string& value) {
    const char* spaces = " \t\r\n";
    size_t begin = value.find_first_not_of(spaces);
    if (begin == std::string::npos) {
        return {};
    }
    size_t end = value.find_last_not_of(spaces);
    return value.substr(begin, end - begin + 1);
}

std::string percent_encode(const std::string& value) {
    static const char* kHex = "0123456789ABCDEF";
    std::string out;
    out.reserve(value.size());
    for (unsigned char c : value) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 0x0F]);
        }
    }
    return out;
}

Bytes digest_sha256(const Bytes& data) {
    Bytes digest(EVP_MAX_MD_SIZE);
    unsigned int length = 0;
    if (EVP_Digest(data.data(), data.size(), digest.data(), &length, EVP_sha256(), nullptr) != 1) {
        fail(openssl_error_text("计算 SHA-256 失败"));
    }
    digest.resize(length);
    return digest;
}

X509Ptr parse_cert_pem(const std::string& pem) {
    BIO* bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
    if (bio == nullptr) {
        fail(openssl_error_text("BIO_new 失败"));
    }
    X509* cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (cert == nullptr) {
        fail(openssl_error_text("解析 PEM 证书失败"));
    }
    return X509Ptr(cert, X509_free);
}

// crypto.cpp:388-395：X509 签名取 signature BIT STRING 的原始字节。
Bytes cert_signature(X509* cert) {
    const ASN1_BIT_STRING* signature = nullptr;
    X509_get0_signature(&signature, nullptr, cert);
    if (signature == nullptr || signature->data == nullptr || signature->length <= 0) {
        fail("证书缺少签名");
    }
    return Bytes(signature->data, signature->data + signature->length);
}

std::string cert_fingerprint(X509* cert) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int length = 0;
    if (X509_digest(cert, EVP_sha256(), digest, &length) != 1) {
        fail(openssl_error_text("计算证书 SHA-256 指纹失败"));
    }
    return format_digest(Bytes(digest, digest + length));
}

bool verify_signature_sha256(X509* cert, const Bytes& data, const Bytes& signature) {
    EVP_PKEY* public_key = X509_get0_pubkey(cert);
    if (public_key == nullptr) {
        ERR_clear_error();
        return false;
    }
    EVP_MD_CTX* context = EVP_MD_CTX_new();
    if (context == nullptr) {
        ERR_clear_error();
        return false;
    }
    bool ok = EVP_DigestVerifyInit(context, nullptr, EVP_sha256(), nullptr, public_key) == 1 &&
              EVP_DigestVerifyUpdate(context, data.data(), data.size()) == 1 &&
              EVP_DigestVerifyFinal(context, signature.data(), signature.size()) == 1;
    EVP_MD_CTX_free(context);
    // 验签失败会往错误队列塞内容，清掉以免污染后续 TLS 错误信息。
    ERR_clear_error();
    return ok;
}

bool verify_self_signed(X509* cert) {
    EVP_PKEY* public_key = X509_get0_pubkey(cert);
    if (public_key == nullptr) {
        ERR_clear_error();
        return false;
    }
    int result = X509_verify(cert, public_key);
    ERR_clear_error();
    return result == 1;
}

PkeyPtr load_private_key(const std::string& path) {
    FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        fail("无法打开客户端私钥 " + path + ": " + std::strerror(errno));
    }
    EVP_PKEY* key = PEM_read_PrivateKey(file, nullptr, nullptr, nullptr);
    std::fclose(file);
    if (key == nullptr) {
        fail(openssl_error_text("解析客户端私钥失败: " + path));
    }
    return PkeyPtr(key, EVP_PKEY_free);
}

Bytes sign_sha256(EVP_PKEY* key, const Bytes& data) {
    EVP_MD_CTX* context = EVP_MD_CTX_new();
    if (context == nullptr) {
        fail(openssl_error_text("EVP_MD_CTX_new 失败"));
    }
    size_t length = 0;
    bool ok = EVP_DigestSignInit(context, nullptr, EVP_sha256(), nullptr, key) == 1 &&
              EVP_DigestSignUpdate(context, data.data(), data.size()) == 1 &&
              EVP_DigestSignFinal(context, nullptr, &length) == 1 && length > 0;
    Bytes signature(length);
    ok = ok && EVP_DigestSignFinal(context, signature.data(), &length) == 1;
    EVP_MD_CTX_free(context);
    if (!ok) {
        fail(openssl_error_text("RSA-SHA256 签名失败"));
    }
    signature.resize(length);
    return signature;
}

std::string read_text_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {};
    }
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

bool file_exists(const std::string& path) {
    struct stat info {};
    return ::stat(path.c_str(), &info) == 0;
}

void write_file_atomic(const std::string& path, const std::string& data, mode_t mode) {
    const std::string temp = path + ".tmp";
    int fd = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
    if (fd < 0) {
        fail("无法写入 " + temp + ": " + std::strerror(errno));
    }
    size_t written = 0;
    while (written < data.size()) {
        ssize_t count = ::write(fd, data.data() + written, data.size() - written);
        if (count < 0) {
            int saved = errno;
            ::close(fd);
            ::unlink(temp.c_str());
            fail("写入 " + temp + " 失败: " + std::strerror(saved));
        }
        written += static_cast<size_t>(count);
    }
    // O_CREAT 的 mode 受 umask 影响，显式 chmod 保证 0644。
    if (::fchmod(fd, mode) != 0) {
        int saved = errno;
        ::close(fd);
        ::unlink(temp.c_str());
        fail("设置 " + temp + " 权限失败: " + std::strerror(saved));
    }
    ::close(fd);
    if (::rename(temp.c_str(), path.c_str()) != 0) {
        int saved = errno;
        ::unlink(temp.c_str());
        fail("重命名 " + temp + " 失败: " + std::strerror(saved));
    }
}

std::string sanitize_path_component(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (unsigned char c : value) {
        if (std::isalnum(c) || c == '.' || c == '-' || c == '_') {
            out.push_back(static_cast<char>(c));
        } else {
            // IPv6 冒号、路径分隔符等一律替换，避免越出状态目录。
            out.push_back('_');
        }
    }
    return out.empty() ? "_" : out;
}

std::string pin_base_path(const std::string& state_dir, const std::string& host,
                          uint16_t https_port) {
    std::string name =
        "server-" + sanitize_path_component(host) + "-" + std::to_string(https_port);
    // 本目标编译选项是 gnu++11，没有 <filesystem>，按目录 + 文件名拼接。
    if (state_dir.empty()) {
        return name;
    }
    if (state_dir.back() == '/') {
        return state_dir + name;
    }
    return state_dir + "/" + name;
}

// 解析 /pair 响应：status_code 必须为 200 且 paired 必须为 1（moonlight-qt
// nvpairingmanager.cpp 的 verifyResponseStatus + getXmlString("paired") 组合）。
XmlDocument require_ok(const PairResponse& response, const std::string& stage) {
    XmlDocument document = parse_xml(response.body);
    if (!document.ok) {
        throw PairError(stage, "响应不是合法 XML: " + document.error, response.http_status, -1, "",
                        response.body);
    }
    const std::string status_code_text = document.root.attribute("status_code");
    if (status_code_text.empty()) {
        throw PairError(stage, "响应缺少 status_code 属性", response.http_status, -1, "",
                        response.body);
    }
    const int status_code = static_cast<int>(std::strtol(status_code_text.c_str(), nullptr, 10));
    const std::string status_message = document.root.attribute("status_message");
    if (status_code != 200) {
        std::string message = "服务端返回 status_code=" + std::to_string(status_code);
        if (!status_message.empty()) {
            message += ": " + status_message;
        }
        throw PairError(stage, message, response.http_status, status_code, status_message,
                        response.body);
    }
    const std::string paired = document.root.child_text("paired");
    if (paired != "1") {
        std::string message =
            "服务端未确认配对（paired=" + (paired.empty() ? std::string("(缺失)") : paired) + "）";
        if (!status_message.empty()) {
            message += ": " + status_message;
        }
        throw PairError(stage, message, response.http_status, status_code, status_message,
                        response.body);
    }
    return document;
}

// 取响应里的十六进制字段并解码（plaincert / challengeresponse / pairingsecret）。
Bytes response_field_bytes(const PairResponse& response, const XmlDocument& document,
                           const std::string& stage, const std::string& field) {
    const std::string hex = trim(document.root.child_text(field));
    if (hex.empty()) {
        throw PairError(stage, "响应缺少 " + field + " 字段", response.http_status, 200, "",
                        response.body);
    }
    try {
        return hex_to_bytes(hex);
    } catch (const std::exception& error) {
        throw PairError(stage, field + " 不是合法十六进制: " + error.what(), response.http_status,
                        200, "", response.body);
    }
}

PairResponse run_stage(const std::string& stage, bool tls, const std::string& query, int timeout_ms,
                       const PairTransport& transport, const PairProgress& progress,
                       std::vector<PairStageRecord>& stages) {
    const auto begin = Clock::now();
    auto elapsed_ms = [&begin]() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - begin).count();
    };
    // gnu++11 里带默认成员初始化的 struct 不是聚合体，不能花括号整体初始化，
    // 因此逐字段赋值（行为与实验代码的聚合初始化一致）。
    PairRequest request;
    request.tls = tls;
    request.query = query;
    request.timeout_ms = timeout_ms;
    try {
        PairResponse response = transport(request);
        PairStageRecord record;
        record.name = stage;
        record.tls = tls;
        record.elapsed_ms = elapsed_ms();
        record.ok = true;
        stages.push_back(record);
        if (progress) {
            progress(record);
        }
        return response;
    } catch (const std::exception& error) {
        PairStageRecord record;
        record.name = stage;
        record.tls = tls;
        record.elapsed_ms = elapsed_ms();
        record.ok = false;
        record.detail = error.what();
        stages.push_back(record);
        if (progress) {
            progress(record);
        }
        throw PairError(stage, std::string("请求失败: ") + error.what(), 0, 0, "", "");
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// 密码学原语
// ---------------------------------------------------------------------------

std::vector<uint8_t> pairing_key(const std::vector<uint8_t>& salt, const std::string& pin) {
    // crypto.cpp:324-338：SHA-256(salt16 || PIN) 取前 16 字节。
    if (salt.size() != kAesKeySize) {
        fail("配对 salt 长度必须为 16 字节");
    }
    Bytes salted = salt;
    salted.insert(salted.end(), pin.begin(), pin.end());
    Bytes hash = digest_sha256(salted);
    hash.resize(kAesKeySize);
    return hash;
}

std::vector<uint8_t> aes128_ecb(const std::vector<uint8_t>& key,
                                const std::vector<uint8_t>& data, bool encrypt) {
    // crypto.cpp:222-273：AES-128-ECB，EVP_CIPHER_CTX_set_padding(0)。
    if (key.size() != kAesKeySize) {
        fail("AES 密钥长度必须为 16 字节");
    }
    if (data.size() % 16 != 0) {
        fail("AES-ECB 数据长度必须是 16 的倍数");
    }
    EVP_CIPHER_CTX* context = EVP_CIPHER_CTX_new();
    if (context == nullptr) {
        fail(openssl_error_text("EVP_CIPHER_CTX_new 失败"));
    }
    Bytes out(data.size() + 16);
    int update_length = 0;
    int final_length = 0;
    int init = encrypt ? EVP_EncryptInit_ex(context, EVP_aes_128_ecb(), nullptr, key.data(), nullptr)
                       : EVP_DecryptInit_ex(context, EVP_aes_128_ecb(), nullptr, key.data(), nullptr);
    bool ok = init == 1 && EVP_CIPHER_CTX_set_padding(context, 0) == 1;
    if (ok) {
        int result = encrypt ? EVP_EncryptUpdate(context, out.data(), &update_length, data.data(),
                                                 static_cast<int>(data.size()))
                             : EVP_DecryptUpdate(context, out.data(), &update_length, data.data(),
                                                 static_cast<int>(data.size()));
        ok = result == 1;
    }
    if (ok) {
        int result = encrypt
                         ? EVP_EncryptFinal_ex(context, out.data() + update_length, &final_length)
                         : EVP_DecryptFinal_ex(context, out.data() + update_length, &final_length);
        ok = result == 1;
    }
    EVP_CIPHER_CTX_free(context);
    if (!ok) {
        fail(openssl_error_text("AES-128-ECB 加解密失败"));
    }
    out.resize(static_cast<size_t>(update_length + final_length));
    return out;
}

// ---------------------------------------------------------------------------
// 服务端证书指纹固定
// ---------------------------------------------------------------------------

ServerPin load_server_pin(const std::string& state_dir, const std::string& host,
                          uint16_t https_port) {
    ServerPin pin;
    pin.host = host;
    pin.https_port = https_port;
    const std::string base = pin_base_path(state_dir, host, https_port);
    pin.cert_path = base + ".pem";
    pin.sha256_path = base + ".sha256";

    const bool cert_exists = file_exists(pin.cert_path);
    const bool fingerprint_exists = file_exists(pin.sha256_path);
    if (!cert_exists && !fingerprint_exists) {
        return pin;  // 没有 pin
    }
    if (cert_exists != fingerprint_exists) {
        fail("pin 文件不完整（" + pin.cert_path + " 与 " + pin.sha256_path +
             " 必须同时存在）；删除残缺文件后可重新配对");
    }

    pin.cert_pem = read_text_file(pin.cert_path);
    if (pin.cert_pem.empty()) {
        fail("无法读取 pin 证书文件 " + pin.cert_path);
    }
    const std::string stored = read_text_file(pin.sha256_path);
    const std::string stored_normalized = normalize_fingerprint(stored);
    if (stored_normalized.size() != 64) {
        fail("pin 指纹文件内容不是 SHA-256（" + pin.sha256_path + "）");
    }

    X509Ptr cert = parse_cert_pem(pin.cert_pem);
    pin.sha256 = cert_fingerprint(cert.get());
    if (normalize_fingerprint(pin.sha256) != stored_normalized) {
        fail("pin 文件损坏：证书 " + pin.cert_path + " 的指纹与 " + pin.sha256_path +
             " 不一致；删除这两个文件后可重新配对");
    }
    return pin;
}

std::string check_server_pin(const ServerPin& pin, const std::string& peer_sha256) {
    if (!pin.exists()) {
        return {};
    }
    const std::string expected = normalize_fingerprint(pin.sha256);
    const std::string actual = normalize_fingerprint(peer_sha256);
    if (actual.empty()) {
        return "TLS 握手未取得服务端证书，无法校验已固定的指纹";
    }
    if (expected != actual) {
        return "服务端证书指纹变化，疑似中间人或主机重装；请重新配对。已固定 " + pin.sha256 +
               "（" + pin.host + ":" + std::to_string(pin.https_port) + "），本次握手 " +
               peer_sha256;
    }
    return {};
}

void store_server_pin(ServerPin& pin, const std::string& cert_pem, const std::string& sha256) {
    const std::string normalized = normalize_fingerprint(sha256);
    if (normalized.size() != 64) {
        fail("拒绝写入非法 SHA-256 指纹");
    }
    // 只有配对成功才会走到这里；写坏了也宁可让下次启动报错，而不是静默放行。
    write_file_atomic(pin.cert_path, cert_pem, 0644);
    write_file_atomic(pin.sha256_path, sha256 + "\n", 0644);
    pin.cert_pem = cert_pem;
    pin.sha256 = sha256;
}

// ---------------------------------------------------------------------------
// 配对流程
// ---------------------------------------------------------------------------

PairError::PairError(const std::string& stage, const std::string& message, int http_status,
                     int nv_status_code, const std::string& status_message,
                     const std::string& response_body)
    : std::runtime_error(message),
      stage_(stage),
      http_status_(http_status),
      nv_status_code_(nv_status_code),
      status_message_(status_message),
      response_body_(response_body) {}

PairError::~PairError() = default;

PairResult run_pairing(const Identity& identity, const PairOptions& options,
                       const PairTransport& transport, const PairProgress& progress) {
    if (!transport) {
        throw std::runtime_error("配对传输回调为空");
    }
    if (options.server_generation < 7) {
        // moonlight-qt nvpairingmanager.cpp:243-259：Gen 7+ 才是 SHA-256；本实现不做 SHA-1。
        throw PairError("preflight",
                        "仅支持 Gen 7+ 配对（SHA-256）；服务端 appversion 主版本为 " +
                            std::to_string(options.server_generation),
                        0, 0, "", "");
    }

    PairResult result;
    X509Ptr client_cert(nullptr, X509_free);
    Bytes client_cert_signature;
    PkeyPtr client_key(nullptr, EVP_PKEY_free);
    try {
        client_cert = parse_cert_pem(identity.cert_pem);
        client_cert_signature = cert_signature(client_cert.get());
        client_key = load_private_key(identity.key_path);
    } catch (const std::exception& error) {
        throw PairError("preflight", std::string("客户端身份不可用: ") + error.what(), 0, 0, "",
                        "");
    }

    // 阶段 1 之前生成的随机量：salt 与客户端 challenge 各 16 字节
    // （moonlight-qt nvpairingmanager.cpp:258-259、296）。
    const Bytes salt = random_bytes(16);
    const Bytes key = pairing_key(salt, options.pin);
    const Bytes client_challenge = random_bytes(16);

    const std::string device =
        percent_encode(options.device_name.empty() ? "roth" : options.device_name);
    const std::string common = "devicename=" + device + "&updateState=1";
    // 服务端会把 getservercert 请求挂起，直到网页端提交 PIN（nvhttp.cpp:588-601）；
    // 该阶段单独用 pin_wait_ms，其余阶段用传输层默认 IO 超时。
    const int pin_wait_ms = options.pin_wait_ms > 0 ? options.pin_wait_ms : 300000;
    const int pin_wait_seconds = (pin_wait_ms + 999) / 1000;

    // ---- 阶段 1：getservercert（明文 HTTP，nvhttp.cpp:354-376 / 578-601）----
    X509Ptr server_cert(nullptr, X509_free);
    {
        const std::string query = common + "&phrase=getservercert&salt=" + to_hex_lower(salt) +
                                  "&clientcert=" + to_hex_lower(bytes_of(identity.cert_pem));
        PairResponse response;
        const auto wait_begin = Clock::now();
        try {
            response = run_stage("getservercert", false, query, pin_wait_ms, transport, progress,
                                 result.stages);
        } catch (const PairError& error) {
            const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    Clock::now() - wait_begin)
                                    .count();
            // 传输层用同一个时限做读超时：请求耗时达到 pin_wait_ms 即判定为“未提交 PIN”。
            if (error.http_status() == 0 && error.nv_status_code() == 0 &&
                waited + 500 >= pin_wait_ms) {
                throw PairError("getservercert",
                                "网页端未在 " + std::to_string(pin_wait_seconds) +
                                    " 秒内提交 PIN（服务端会把 getservercert 请求挂起，"
                                    "直到网页端 POST /api/pin 提交同一个 PIN）",
                                0, 0, "", "");
            }
            throw;
        }
        const XmlDocument document = require_ok(response, "getservercert");
        const Bytes plaincert = response_field_bytes(response, document, "getservercert", "plaincert");
        result.server_cert_pem = string_of(plaincert);
        try {
            server_cert = parse_cert_pem(result.server_cert_pem);
            result.server_cert_sha256 = cert_fingerprint(server_cert.get());
        } catch (const std::exception& error) {
            throw PairError("getservercert", std::string("plaincert 解析失败: ") + error.what(),
                            response.http_status, 200, "", response.body);
        }
        if (!verify_self_signed(server_cert.get())) {
            throw PairError("getservercert",
                            "plaincert 不是有效的自签证书（Sunshine 的服务端证书为自签）；"
                            "拒绝继续配对，避免配到假主机",
                            response.http_status, 200, "", response.body);
        }
        result.raw_xml = response.body;
    }

    // 阶段 3 的 hash 输入要用服务端证书的签名原文（moonlight-qt:322-328）。
    const Bytes server_cert_signature = cert_signature(server_cert.get());

    // ---- 阶段 2：clientchallenge（明文 HTTP，nvhttp.cpp:378-419 / 620-623）----
    Bytes server_response_hash;
    Bytes server_challenge;
    {
        const Bytes encrypted_challenge = aes128_ecb(key, client_challenge, true);
        const std::string query =
            common + "&clientchallenge=" + to_hex_lower(encrypted_challenge);
        PairResponse response =
            run_stage("clientchallenge", false, query, 0, transport, progress, result.stages);
        const XmlDocument document = require_ok(response, "clientchallenge");
        const Bytes encrypted_response =
            response_field_bytes(response, document, "clientchallenge", "challengeresponse");
        Bytes plaintext;
        try {
            plaintext = aes128_ecb(key, encrypted_response, false);
        } catch (const std::exception& error) {
            throw PairError("clientchallenge",
                            std::string("challengeresponse 解密失败: ") + error.what(),
                            response.http_status, 200, "", response.body);
        }
        // 服务端回的是 SHA-256(hash32 + serverchallenge16)，共 48 字节（nvhttp.cpp:396-411）。
        if (plaintext.size() < kGen7HashLength + 16) {
            throw PairError("clientchallenge",
                            "challengeresponse 解密后长度不足（" +
                                std::to_string(plaintext.size()) + " 字节，期望 >= 48）",
                            response.http_status, 200, "", response.body);
        }
        server_response_hash.assign(plaintext.begin(), plaintext.begin() + kGen7HashLength);
        server_challenge.assign(plaintext.begin() + kGen7HashLength,
                                plaintext.begin() + kGen7HashLength + 16);
    }

    // ---- 阶段 3：serverchallengeresp（明文 HTTP，nvhttp.cpp:421-448 / 624-625）----
    const Bytes client_secret = random_bytes(16);
    {
        // crypto.cpp:340-343 + moonlight:322-328：SHA-256(serverChallenge||客户端证书签名||clientSecret)，
        // Gen 7 恰好 32 字节，无需再补零。
        Bytes hash_input = server_challenge;
        hash_input.insert(hash_input.end(), client_cert_signature.begin(),
                          client_cert_signature.end());
        hash_input.insert(hash_input.end(), client_secret.begin(), client_secret.end());
        const Bytes padded_hash = digest_sha256(hash_input);
        const Bytes encrypted_hash = aes128_ecb(key, padded_hash, true);
        const std::string query =
            common + "&serverchallengeresp=" + to_hex_lower(encrypted_hash);
        PairResponse response =
            run_stage("serverchallengeresp", false, query, 0, transport, progress, result.stages);
        const XmlDocument document = require_ok(response, "serverchallengeresp");
        const Bytes pairing_secret =
            response_field_bytes(response, document, "serverchallengeresp", "pairingsecret");
        if (pairing_secret.size() <= 16) {
            throw PairError("serverchallengeresp",
                            "pairingsecret 过短（" + std::to_string(pairing_secret.size()) +
                                " 字节，期望 > 16）",
                            response.http_status, 200, "", response.body);
        }
        const Bytes server_secret(pairing_secret.begin(), pairing_secret.begin() + 16);
        const Bytes server_signature(pairing_secret.begin() + 16, pairing_secret.end());

        // crypto.cpp:497-499 / moonlight:352-357：pairingsecret 必须由 plaincert 的私钥签署，
        // 否则说明对方拿不到该证书对应的私钥（中间人）。
        if (!verify_signature_sha256(server_cert.get(), server_secret, server_signature)) {
            throw PairError("serverchallengeresp",
                            "服务端配对签名校验失败：plaincert 的私钥未签署 pairingsecret"
                            "（疑似中间人或证书不匹配）",
                            response.http_status, 200, "", response.body);
        }

        // moonlight:361-369：用同一把 PIN 派生的密钥重算 hash，验证服务端确实解出了 challenge。
        Bytes expected_input = client_challenge;
        expected_input.insert(expected_input.end(), server_cert_signature.begin(),
                              server_cert_signature.end());
        expected_input.insert(expected_input.end(), server_secret.begin(), server_secret.end());
        if (digest_sha256(expected_input) != server_response_hash) {
            throw PairError("serverchallengeresp",
                            "PIN 错误：服务端返回的 challenge hash 与本地计算不一致"
                            "（双方 PIN 不同，AES 密钥不同）",
                            response.http_status, 200, "", response.body);
        }
    }

    // ---- 阶段 4：clientpairingsecret（明文 HTTP，nvhttp.cpp:450-500 / 626-628）----
    {
        Bytes client_pairing_secret = client_secret;
        const Bytes client_signature = sign_sha256(client_key.get(), client_secret);
        client_pairing_secret.insert(client_pairing_secret.end(), client_signature.begin(),
                                     client_signature.end());
        const std::string query =
            common + "&clientpairingsecret=" + to_hex_lower(client_pairing_secret);
        PairResponse response =
            run_stage("clientpairingsecret", false, query, 0, transport, progress, result.stages);
        // 该阶段服务端总是返回 status_code=200；paired=0 表示 hash/签名校验失败。
        require_ok(response, "clientpairingsecret");
        result.raw_xml = response.body;
    }

    // ---- 阶段 5：pairchallenge（HTTPS + 客户端证书，nvhttp.cpp:605-608）----
    {
        PairResponse response =
            run_stage("pairchallenge", true, common + "&phrase=pairchallenge", 0, transport,
                      progress, result.stages);
        require_ok(response, "pairchallenge");
        // TOFU：明文阶段拿到的 plaincert 必须与 HTTPS 握手出示的证书一致，否则不落 pin。
        if (normalize_fingerprint(response.server_cert_sha256) !=
            normalize_fingerprint(result.server_cert_sha256)) {
            throw PairError("pairchallenge",
                            "TLS 握手证书与 plaincert 不一致（疑似中间人）：plaincert " +
                                result.server_cert_sha256 + "，本次握手 " +
                                (response.server_cert_sha256.empty()
                                     ? std::string("(未取得)")
                                     : response.server_cert_sha256),
                            response.http_status, 200, "", response.body);
        }
    }

    // 只有五阶段全部成功才写入/覆盖 pin（配对失败不改 pin）。
    try {
        ServerPin pin = load_server_pin(identity.state_dir, options.host, options.https_port);
        if (pin.exists()) {
            const std::string mismatch = check_server_pin(pin, result.server_cert_sha256);
            if (!mismatch.empty()) {
                throw PairError("pairchallenge", mismatch, 0, 0, "", "");
            }
        }
        store_server_pin(pin, result.server_cert_pem, result.server_cert_sha256);
        result.pin_cert_path = pin.cert_path;
        result.pin_sha256_path = pin.sha256_path;
    } catch (const PairError&) {
        throw;
    } catch (const std::exception& error) {
        throw PairError("pairchallenge", std::string("写入 pin 失败: ") + error.what(), 0, 0, "",
                        "");
    }

    result.paired = true;
    return result;
}

}  // namespace sunshine
}  // namespace tg
