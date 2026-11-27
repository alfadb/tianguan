// SPDX-License-Identifier: GPL-3.0-or-later
#include "common/identity.h"

#include <openssl/asn1.h>
#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/x509.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace tg::sunshine {
namespace {

constexpr const char* kCertificateName = "identity.crt";
constexpr const char* kPrivateKeyName = "identity.key";
constexpr const char* kUniqueIdName = "uniqueid";
constexpr const char* kCommonName = "NVIDIA GameStream Client";
constexpr long kValiditySeconds = 60L * 60L * 24L * 365L * 20L;  // 20 年

std::string openssl_error(const std::string& what) {
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

std::string read_text_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {};
    }
    std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return data;
}

// 以指定权限位写文件（O_CREAT 的 mode 受 umask 影响，写完再 chmod 一次确保精确）。
void write_file_with_mode(const std::string& path, const std::string& data, mode_t mode) {
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
    if (fd < 0) {
        throw std::runtime_error("无法写入 " + path + ": " + std::strerror(errno));
    }
    size_t written = 0;
    while (written < data.size()) {
        ssize_t n = ::write(fd, data.data() + written, data.size() - written);
        if (n < 0) {
            int saved = errno;
            ::close(fd);
            throw std::runtime_error("写入 " + path + " 失败: " + std::strerror(saved));
        }
        written += static_cast<size_t>(n);
    }
    if (::fchmod(fd, mode) != 0) {
        int saved = errno;
        ::close(fd);
        throw std::runtime_error("无法设置 " + path + " 权限: " + std::strerror(saved));
    }
    ::close(fd);
}

std::string pem_from_x509(X509* cert) {
    BIO* bio = BIO_new(BIO_s_mem());
    if (bio == nullptr) {
        throw std::runtime_error(openssl_error("BIO_new 失败"));
    }
    if (PEM_write_bio_X509(bio, cert) != 1) {
        BIO_free(bio);
        throw std::runtime_error(openssl_error("证书 PEM 序列化失败"));
    }
    char* data = nullptr;
    long length = BIO_get_mem_data(bio, &data);
    std::string pem(data, static_cast<size_t>(length));
    BIO_free(bio);
    return pem;
}

std::string pem_from_pkey(EVP_PKEY* key) {
    BIO* bio = BIO_new(BIO_s_mem());
    if (bio == nullptr) {
        throw std::runtime_error(openssl_error("BIO_new 失败"));
    }
    if (PEM_write_bio_PrivateKey(bio, key, nullptr, nullptr, 0, nullptr, nullptr) != 1) {
        BIO_free(bio);
        throw std::runtime_error(openssl_error("私钥 PEM 序列化失败"));
    }
    char* data = nullptr;
    long length = BIO_get_mem_data(bio, &data);
    std::string pem(data, static_cast<size_t>(length));
    BIO_free(bio);
    return pem;
}

X509* parse_cert_pem(const std::string& pem) {
    BIO* bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
    if (bio == nullptr) {
        return nullptr;
    }
    X509* cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    return cert;
}

EVP_PKEY* parse_key_pem(const std::string& pem) {
    BIO* bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
    if (bio == nullptr) {
        return nullptr;
    }
    EVP_PKEY* key = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    return key;
}

// 生成自签证书与私钥（identitymanager.cpp:31-73）。
void generate_credentials(std::string& cert_pem, std::string& key_pem) {
    ERR_clear_error();

    X509* cert = X509_new();
    EVP_PKEY* key = EVP_RSA_gen(2048);  // OpenSSL 3 路径，identitymanager.cpp:34-36
    if (cert == nullptr || key == nullptr) {
        X509_free(cert);
        EVP_PKEY_free(key);
        throw std::runtime_error(openssl_error("生成 RSA 2048 密钥失败"));
    }

    bool ok = true;
    ok = ok && X509_set_version(cert, 2) == 1;                              // v3
    ok = ok && ASN1_INTEGER_set(X509_get_serialNumber(cert), 0) == 1;        // serial = 0
    ok = ok && X509_gmtime_adj(X509_getm_notBefore(cert), 0) != nullptr;     // 立即生效
    ok = ok && X509_gmtime_adj(X509_getm_notAfter(cert), kValiditySeconds) != nullptr;
    ok = ok && X509_set_pubkey(cert, key) == 1;

    X509_NAME* name = X509_NAME_new();
    if (name == nullptr) {
        ok = false;
    } else {
        ok = ok && X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                              reinterpret_cast<const unsigned char*>(kCommonName),
                                              -1, -1, 0) == 1;
        ok = ok && X509_set_subject_name(cert, name) == 1;
        ok = ok && X509_set_issuer_name(cert, name) == 1;
        X509_NAME_free(name);
    }
    ok = ok && X509_sign(cert, key, EVP_sha256()) > 0;

    if (!ok) {
        X509_free(cert);
        EVP_PKEY_free(key);
        throw std::runtime_error(openssl_error("构造客户端证书失败"));
    }

    cert_pem = pem_from_x509(cert);
    key_pem = pem_from_pkey(key);

    X509_free(cert);
    EVP_PKEY_free(key);
}

bool credentials_readable(const std::string& cert_pem, const std::string& key_pem) {
    X509* cert = parse_cert_pem(cert_pem);
    if (cert == nullptr) {
        return false;
    }
    EVP_PKEY* key = parse_key_pem(key_pem);
    if (key == nullptr) {
        X509_free(cert);
        return false;
    }
    // 证书与私钥必须匹配，否则 TLS 客户端认证必然失败。
    bool matches = X509_check_private_key(cert, key) == 1;
    EVP_PKEY_free(key);
    X509_free(cert);
    return matches;
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

// identitymanager.cpp:145-148：64 位随机数的 base 16 表示（小写、不补零）。
std::string generate_unique_id() {
    uint64_t value = 0;
    if (RAND_bytes(reinterpret_cast<unsigned char*>(&value), sizeof(value)) != 1) {
        throw std::runtime_error(openssl_error("生成 uniqueid 失败"));
    }
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%llx", static_cast<unsigned long long>(value));
    return buffer;
}

std::string asn1_time_to_string(const ASN1_TIME* time) {
    if (time == nullptr) {
        return {};
    }
    BIO* bio = BIO_new(BIO_s_mem());
    if (bio == nullptr) {
        return {};
    }
    ASN1_TIME_print(bio, time);
    char* data = nullptr;
    long length = BIO_get_mem_data(bio, &data);
    std::string text = length > 0 ? std::string(data, static_cast<size_t>(length)) : std::string();
    BIO_free(bio);
    return text;
}

std::string fingerprint_sha256(X509* cert) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int length = 0;
    if (X509_digest(cert, EVP_sha256(), digest, &length) != 1) {
        throw std::runtime_error(openssl_error("计算证书 SHA-256 指纹失败"));
    }
    static const char* kHex = "0123456789ABCDEF";
    std::string out;
    out.reserve(length * 3);
    for (unsigned int i = 0; i < length; ++i) {
        if (i != 0) {
            out.push_back(':');
        }
        out.push_back(kHex[digest[i] >> 4]);
        out.push_back(kHex[digest[i] & 0x0F]);
    }
    return out;
}

}  // namespace

Identity load_or_create_identity(const std::string& state_dir) {
    ERR_clear_error();

    std::filesystem::path dir(state_dir);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        throw std::runtime_error("无法创建状态目录 " + state_dir + ": " + ec.message());
    }

    Identity identity;
    identity.state_dir = state_dir;
    identity.cert_path = (dir / kCertificateName).string();
    identity.key_path = (dir / kPrivateKeyName).string();
    identity.uniqueid_path = (dir / kUniqueIdName).string();

    std::string cert_pem = read_text_file(identity.cert_path);
    std::string key_pem = read_text_file(identity.key_path);

    if (!cert_pem.empty() && !key_pem.empty() && credentials_readable(cert_pem, key_pem)) {
        identity.cert_pem = cert_pem;
    } else {
        generate_credentials(cert_pem, key_pem);
        write_file_with_mode(identity.cert_path, cert_pem, 0644);
        // 私钥 0600：内容不返回、不打印。
        write_file_with_mode(identity.key_path, key_pem, 0600);
        identity.cert_pem = cert_pem;
        identity.created_now = true;
    }

    std::string unique_id = trim(read_text_file(identity.uniqueid_path));
    if (unique_id.empty()) {
        unique_id = generate_unique_id();
        write_file_with_mode(identity.uniqueid_path, unique_id + "\n", 0644);
        identity.created_now = true;
    }
    identity.uniqueid = unique_id;

    return identity;
}

X509Summary summarize_identity(const Identity& identity) {
    ERR_clear_error();

    X509* cert = parse_cert_pem(identity.cert_pem);
    if (cert == nullptr) {
        throw std::runtime_error(openssl_error("无法解析客户端证书"));
    }

    X509Summary summary;
    char subject[512] = {0};
    X509_NAME_oneline(X509_get_subject_name(cert), subject, sizeof(subject));
    summary.subject = subject;

    char common_name[256] = {0};
    if (X509_NAME_get_text_by_NID(X509_get_subject_name(cert), NID_commonName, common_name,
                                  sizeof(common_name)) > 0) {
        summary.common_name = common_name;
    }

    const ASN1_INTEGER* serial = X509_get_serialNumber(cert);
    summary.serial_decimal = static_cast<long long>(ASN1_INTEGER_get(serial));
    BIGNUM* serial_bn = ASN1_INTEGER_to_BN(serial, nullptr);
    if (serial_bn != nullptr) {
        char* hex = BN_bn2hex(serial_bn);
        if (hex != nullptr) {
            // BN_bn2hex 对大数会输出前导零；serial=0 时输出 "0"。
            std::string text(hex);
            size_t first = text.find_first_not_of('0');
            if (first == std::string::npos) {
                text = "0";
            } else if (first > 0) {
                text = text.substr(first);
            }
            summary.serial_hex = "0x" + text;
            OPENSSL_free(hex);
        }
        BN_free(serial_bn);
    }
    if (summary.serial_hex.empty()) {
        summary.serial_hex = "0x0";
    }

    summary.not_before = asn1_time_to_string(X509_get0_notBefore(cert));
    summary.not_after = asn1_time_to_string(X509_get0_notAfter(cert));
    summary.sha256_fingerprint = fingerprint_sha256(cert);

    EVP_PKEY* public_key = X509_get_pubkey(cert);
    if (public_key != nullptr) {
        summary.key_bits = EVP_PKEY_bits(public_key);
        EVP_PKEY_free(public_key);
    }

    X509_free(cert);
    return summary;
}

int private_key_mode(const Identity& identity) {
    struct stat info {};
    if (::stat(identity.key_path.c_str(), &info) != 0) {
        return -1;
    }
    return static_cast<int>(info.st_mode & 07777);
}

}  // namespace tg::sunshine
