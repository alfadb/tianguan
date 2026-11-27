// SPDX-License-Identifier: GPL-3.0-or-later
// Moonlight / Sunshine 的 nvhttp 端点：serverinfo / applist / launch / resume /
// cancel / unpair 与 /pair 五阶段配对（TLS 客户端证书 + 服务端指纹固定）。
#include "nv_http.h"
#include "xml_util.h"

#include <openssl/err.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <sstream>
#include <utility>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace tg {
namespace sunshine {
namespace {

using Clock = std::chrono::steady_clock;

class Deadline {
public:
    explicit Deadline(int timeout_ms)
        : end_(Clock::now() + std::chrono::milliseconds(timeout_ms > 0 ? timeout_ms : 1)) {}

    int remaining_ms() const {
        auto left = std::chrono::duration_cast<std::chrono::milliseconds>(end_ - Clock::now()).count();
        return left > 0 ? static_cast<int>(left) : 0;
    }

private:
    Clock::time_point end_;
};

std::string ssl_error_text() {
    std::string text;
    char buffer[256];
    unsigned long code = 0;
    while ((code = ERR_get_error()) != 0) {
        ERR_error_string_n(code, buffer, sizeof(buffer));
        if (!text.empty()) {
            text += "; ";
        }
        text += buffer;
    }
    if (text.empty()) {
        text = "未知 TLS 错误";
    }
    return text;
}

// SSL_ERROR_SYSCALL 时错误队列可能为空，补上 errno 的信息。
std::string ssl_failure_text(int ssl_error) {
    std::string text = ssl_error_text();
    if (ssl_error == SSL_ERROR_SYSCALL && text == "未知 TLS 错误") {
        text = errno != 0 ? std::string(std::strerror(errno)) : std::string("对端关闭连接");
    }
    return text;
}

NvError transport_error(const std::string& message) {
    return NvError(message, 0, 0, "", "");
}

void wait_fd(int fd, short events, const Deadline& deadline) {
    for (;;) {
        int timeout = deadline.remaining_ms();
        if (timeout <= 0) {
            throw transport_error("网络操作超时");
        }
        struct pollfd entry {};
        entry.fd = fd;
        entry.events = events;
        int ready = ::poll(&entry, 1, timeout);
        if (ready > 0) {
            return;
        }
        if (ready == 0) {
            throw transport_error("网络操作超时");
        }
        if (errno == EINTR) {
            continue;
        }
        throw transport_error(std::string("poll 失败: ") + std::strerror(errno));
    }
}

bool is_ip_literal(const std::string& host) {
    unsigned char storage[sizeof(struct in6_addr)];
    return ::inet_pton(AF_INET, host.c_str(), storage) == 1 ||
           ::inet_pton(AF_INET6, host.c_str(), storage) == 1;
}

std::string host_header(const std::string& host) {
    if (host.find(':') != std::string::npos && host.front() != '[') {
        return "[" + host + "]";  // IPv6 字面量
    }
    return host;
}

std::string hex_lower(const unsigned char* data, size_t length) {
    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.reserve(length * 2);
    for (size_t i = 0; i < length; ++i) {
        out.push_back(kHex[data[i] >> 4]);
        out.push_back(kHex[data[i] & 0x0F]);
    }
    return out;
}

std::string lowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
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

std::string certificate_fingerprint(X509* cert) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int length = 0;
    if (cert == nullptr || X509_digest(cert, EVP_sha256(), digest, &length) != 1) {
        return {};
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

// 一个 TCP（可选 TLS）连接，读写都带期限。
class Connection {
public:
    Connection(const std::string& host, uint16_t port, int connect_timeout_ms, int io_timeout_ms)
        : host_(host), port_(port), connect_timeout_ms_(connect_timeout_ms), io_timeout_ms_(io_timeout_ms) {
        fd_ = connect_tcp();
    }

    ~Connection() {
        if (ssl_ != nullptr) {
            SSL_free(ssl_);
        }
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    void start_tls(SSL_CTX* context) {
        ERR_clear_error();
        ssl_ = SSL_new(context);
        if (ssl_ == nullptr) {
            throw transport_error("SSL_new 失败: " + ssl_error_text());
        }
        if (SSL_set_fd(ssl_, fd_) != 1) {
            throw transport_error("SSL_set_fd 失败: " + ssl_error_text());
        }
        if (!is_ip_literal(host_) && SSL_set_tlsext_host_name(ssl_, host_.c_str()) != 1) {
            throw transport_error("设置 SNI 失败: " + ssl_error_text());
        }

        Deadline deadline(connect_timeout_ms_);
        for (;;) {
            int rc = SSL_connect(ssl_);
            if (rc == 1) {
                break;
            }
            int error = SSL_get_error(ssl_, rc);
            if (error == SSL_ERROR_WANT_READ) {
                wait_fd(fd_, POLLIN, deadline);
                continue;
            }
            if (error == SSL_ERROR_WANT_WRITE) {
                wait_fd(fd_, POLLOUT, deadline);
                continue;
            }
            throw transport_error("TLS 握手失败: " + ssl_failure_text(error));
        }

#if OPENSSL_VERSION_MAJOR >= 3
        X509* peer = SSL_get1_peer_certificate(ssl_);
#else
        X509* peer = SSL_get_peer_certificate(ssl_);
#endif
        if (peer != nullptr) {
            peer_fingerprint_ = certificate_fingerprint(peer);
            X509_free(peer);
        }
    }

    const std::string& peer_fingerprint() const { return peer_fingerprint_; }

    size_t read(void* buffer, size_t length) {
        Deadline deadline(io_timeout_ms_);
        for (;;) {
            if (ssl_ != nullptr) {
                int rc = SSL_read(ssl_, buffer, static_cast<int>(std::min<size_t>(length, 16384)));
                if (rc > 0) {
                    return static_cast<size_t>(rc);
                }
                int error = SSL_get_error(ssl_, rc);
                if (error == SSL_ERROR_WANT_READ) {
                    wait_fd(fd_, POLLIN, deadline);
                    continue;
                }
                if (error == SSL_ERROR_WANT_WRITE) {
                    wait_fd(fd_, POLLOUT, deadline);
                    continue;
                }
                if (error == SSL_ERROR_ZERO_RETURN) {
                    return 0;
                }
                throw transport_error("TLS 读取失败: " + ssl_failure_text(error));
            }

            ssize_t rc = ::recv(fd_, buffer, length, 0);
            if (rc > 0) {
                return static_cast<size_t>(rc);
            }
            if (rc == 0) {
                return 0;
            }
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                wait_fd(fd_, POLLIN, deadline);
                continue;
            }
            throw transport_error(std::string("socket 读取失败: ") + std::strerror(errno));
        }
    }

    void write(const void* buffer, size_t length) {
        const auto* data = static_cast<const unsigned char*>(buffer);
        size_t written = 0;
        Deadline deadline(io_timeout_ms_);
        while (written < length) {
            size_t chunk = std::min<size_t>(length - written, 16384);
            if (ssl_ != nullptr) {
                int rc = SSL_write(ssl_, data + written, static_cast<int>(chunk));
                if (rc > 0) {
                    written += static_cast<size_t>(rc);
                    continue;
                }
                int error = SSL_get_error(ssl_, rc);
                if (error == SSL_ERROR_WANT_READ) {
                    wait_fd(fd_, POLLIN, deadline);
                    continue;
                }
                if (error == SSL_ERROR_WANT_WRITE) {
                    wait_fd(fd_, POLLOUT, deadline);
                    continue;
                }
                throw transport_error("TLS 写入失败: " + ssl_failure_text(error));
            }

            ssize_t rc = ::send(fd_, data + written, chunk, MSG_NOSIGNAL);
            if (rc > 0) {
                written += static_cast<size_t>(rc);
                continue;
            }
            if (rc < 0 && errno == EINTR) {
                continue;
            }
            if (rc < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                wait_fd(fd_, POLLOUT, deadline);
                continue;
            }
            throw transport_error(std::string("socket 写入失败: ") + std::strerror(errno));
        }
    }

private:
    int connect_tcp() {
        struct addrinfo hints {};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;

        struct addrinfo* addresses = nullptr;
        std::string service = std::to_string(port_);
        int rc = ::getaddrinfo(host_.c_str(), service.c_str(), &hints, &addresses);
        if (rc != 0) {
            throw transport_error("解析主机 " + host_ + " 失败: " + ::gai_strerror(rc));
        }

        std::string last_error = "没有可用地址";
        int connected = -1;
        for (struct addrinfo* entry = addresses; entry != nullptr; entry = entry->ai_next) {
            int fd = ::socket(entry->ai_family, entry->ai_socktype | SOCK_CLOEXEC, entry->ai_protocol);
            if (fd < 0) {
                last_error = std::string("创建 socket 失败: ") + std::strerror(errno);
                continue;
            }
            int flags = ::fcntl(fd, F_GETFL, 0);
            if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
                last_error = std::string("设置非阻塞失败: ") + std::strerror(errno);
                ::close(fd);
                continue;
            }

            int result = ::connect(fd, entry->ai_addr, entry->ai_addrlen);
            if (result < 0 && errno != EINPROGRESS) {
                last_error = std::string("连接失败: ") + std::strerror(errno);
                ::close(fd);
                continue;
            }
            if (result < 0) {
                try {
                    Deadline deadline(connect_timeout_ms_);
                    wait_fd(fd, POLLOUT, deadline);
                } catch (...) {
                    ::close(fd);
                    throw;
                }
                int socket_error = 0;
                socklen_t length = sizeof(socket_error);
                if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &length) != 0 ||
                    socket_error != 0) {
                    last_error = std::string("连接失败: ") +
                                 std::strerror(socket_error != 0 ? socket_error : errno);
                    ::close(fd);
                    continue;
                }
            }
            connected = fd;
            break;
        }
        ::freeaddrinfo(addresses);
        if (connected < 0) {
            throw transport_error("无法连接 " + host_ + ":" + std::to_string(port_) + ": " + last_error);
        }
        return connected;
    }

    std::string host_;
    uint16_t port_;
    int connect_timeout_ms_;
    int io_timeout_ms_;
    int fd_ = -1;
    SSL* ssl_ = nullptr;
    std::string peer_fingerprint_;
};

// 在 Connection 上按行/按长度读响应。
class BufferedReader {
public:
    explicit BufferedReader(Connection& connection) : connection_(connection) {}

    bool read_line(std::string& line) {
        for (;;) {
            size_t newline = pending_.find('\n');
            if (newline != std::string::npos) {
                line = pending_.substr(0, newline);
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
                pending_.erase(0, newline + 1);
                return true;
            }
            if (!fill()) {
                if (pending_.empty()) {
                    return false;
                }
                line = pending_;
                pending_.clear();
                return true;
            }
        }
    }

    bool read_exact(size_t length, std::string& out) {
        while (pending_.size() < length) {
            if (!fill()) {
                return false;
            }
        }
        out.assign(pending_, 0, length);
        pending_.erase(0, length);
        return true;
    }

    void read_to_eof(std::string& out) {
        out.append(pending_);
        pending_.clear();
        char buffer[4096];
        for (;;) {
            size_t count = connection_.read(buffer, sizeof(buffer));
            if (count == 0) {
                return;
            }
            out.append(buffer, count);
        }
    }

private:
    bool fill() {
        char buffer[4096];
        size_t count = connection_.read(buffer, sizeof(buffer));
        if (count == 0) {
            return false;
        }
        pending_.append(buffer, count);
        return true;
    }

    Connection& connection_;
    std::string pending_;
};

int parse_http_status(const std::string& status_line) {
    size_t space = status_line.find(' ');
    if (space == std::string::npos) {
        return 0;
    }
    return std::atoi(status_line.c_str() + space + 1);
}

void read_chunked_body(BufferedReader& reader, std::string& body) {
    for (;;) {
        std::string line;
        if (!reader.read_line(line)) {
            throw transport_error("chunked 响应被截断（缺少分块长度行）");
        }
        line = trim(line);
        if (line.empty()) {
            continue;  // 容忍分块之间的空行
        }
        size_t semicolon = line.find(';');
        std::string hex = semicolon == std::string::npos ? line : line.substr(0, semicolon);
        char* end = nullptr;
        unsigned long long length = std::strtoull(hex.c_str(), &end, 16);
        if (end == nullptr || *end != '\0' || hex.empty()) {
            throw transport_error("chunked 响应的分块长度非法: " + hex);
        }
        if (length == 0) {
            // 尾随 header，直到空行。
            while (reader.read_line(line) && !line.empty()) {
            }
            return;
        }
        std::string chunk;
        if (!reader.read_exact(static_cast<size_t>(length), chunk)) {
            throw transport_error("chunked 响应体被截断");
        }
        body += chunk;
        std::string crlf;
        if (!reader.read_exact(2, crlf)) {
            throw transport_error("chunked 响应缺少分块结束符");
        }
    }
}

std::string xml_first_text(const XmlNode& root, const std::string& name) {
    if (const XmlNode* direct = root.child(name)) {
        return direct->text;
    }
    if (const XmlNode* anywhere = root.find_first(name)) {
        return anywhere->text;
    }
    return {};
}

int parse_int(const std::string& text, int fallback = 0) {
    if (text.empty()) {
        return fallback;
    }
    return static_cast<int>(std::strtol(text.c_str(), nullptr, 10));
}

uint64_t parse_uint64(const std::string& text, bool& present) {
    present = !text.empty();
    if (!present) {
        return 0;
    }
    return std::strtoull(text.c_str(), nullptr, 10);
}

int parse_nv_status_code(const std::string& text) {
    // GFE 3.20.3 偶尔返回 0xFFFFFFFF，用 unsigned 解析再转 int（moonlight-qt 同样处理）。
    char* end = nullptr;
    long long value = std::strtoll(text.c_str(), &end, 10);
    if (end == nullptr || *end != '\0') {
        return -1;
    }
    return static_cast<int>(value);
}

}  // namespace

NvError::NvError(const std::string& message, int http_status, int nv_status_code,
                 const std::string& status_message, const std::string& response_body)
    : std::runtime_error(message),
      http_status_(http_status),
      nv_status_code_(nv_status_code),
      status_message_(status_message),
      response_body_(response_body) {}

NvError::~NvError() = default;

PinMismatchError::PinMismatchError(const std::string& message, const std::string& expected_sha256,
                                   const std::string& actual_sha256)
    : NvError(message, 0, 0, "", ""),
      expected_sha256_(expected_sha256),
      actual_sha256_(actual_sha256) {}

PinMismatchError::~PinMismatchError() = default;

struct NvHttpClient::Impl {
    Options options;
    Identity identity;
    SSL_CTX* ssl_ctx = nullptr;
    uint16_t discovered_https_port = 0;

    ~Impl() {
        if (ssl_ctx != nullptr) {
            SSL_CTX_free(ssl_ctx);
        }
    }

    // 存在已固定 pin 的主机：TLS 握手后、发出任何请求字节之前比对服务端证书指纹。
    void enforce_server_pin(uint16_t port, const std::string& peer_fingerprint) {
        ServerPin pin = load_server_pin(options.state_dir, options.host, port);
        if (!pin.exists()) {
            return;  // 未配对本来就会被服务端 401 拒绝，不做校验
        }
        const std::string mismatch = check_server_pin(pin, peer_fingerprint);
        if (mismatch.empty()) {
            return;
        }
        throw PinMismatchError(mismatch, pin.sha256, peer_fingerprint);
    }

    RawResponse request(const char* method, bool tls, uint16_t port, const std::string& path,
                        const std::string& query, const std::string& body,
                        const std::string& content_type, int io_timeout_ms = 0) {
        std::string target = path;
        if (!query.empty()) {
            target += "?";
            target += query;
        }

        std::string head;
        head += method;
        head += " ";
        head += target;
        head += " HTTP/1.1\r\n";
        head += "Host: " + host_header(options.host) + ":" + std::to_string(port) + "\r\n";
        head += "User-Agent: tianguan-sunshine-core/0.1\r\n";
        head += "Accept: */*\r\n";
        head += "Connection: close\r\n";
        if (!body.empty() || std::strcmp(method, "POST") == 0) {
            head += "Content-Type: ";
            head += content_type.empty() ? "application/x-www-form-urlencoded" : content_type;
            head += "\r\n";
            head += "Content-Length: " + std::to_string(body.size()) + "\r\n";
        }
        head += "\r\n";
        head += body;

        Connection connection(options.host, port, options.connect_timeout_ms,
                              io_timeout_ms > 0 ? io_timeout_ms : options.io_timeout_ms);
        if (tls) {
            if (ssl_ctx == nullptr) {
                throw transport_error("TLS 上下文未初始化");
            }
            connection.start_tls(ssl_ctx);
            enforce_server_pin(port, connection.peer_fingerprint());
        }
        connection.write(head.data(), head.size());

        RawResponse response;
        response.server_cert_sha256 = connection.peer_fingerprint();

        BufferedReader reader(connection);
        std::string status_line;
        if (!reader.read_line(status_line)) {
            throw transport_error("服务端没有返回 HTTP 响应");
        }
        response.http_status = parse_http_status(status_line);

        std::map<std::string, std::string> headers;
        for (;;) {
            std::string line;
            if (!reader.read_line(line)) {
                throw transport_error("HTTP 响应头被截断");
            }
            if (line.empty()) {
                break;
            }
            size_t colon = line.find(':');
            if (colon == std::string::npos) {
                continue;
            }
            headers[lowercase(trim(line.substr(0, colon)))] = trim(line.substr(colon + 1));
        }

        auto transfer_encoding = headers.find("transfer-encoding");
        if (transfer_encoding != headers.end() &&
            lowercase(transfer_encoding->second).find("chunked") != std::string::npos) {
            read_chunked_body(reader, response.body);
        } else {
            auto content_length = headers.find("content-length");
            if (content_length != headers.end()) {
                size_t length = static_cast<size_t>(std::strtoull(content_length->second.c_str(), nullptr, 10));
                if (!reader.read_exact(length, response.body)) {
                    throw transport_error("HTTP 响应体被截断（Content-Length 不足）");
                }
            } else {
                reader.read_to_eof(response.body);
            }
        }

        return response;
    }

    std::string common_query() const {
        unsigned char uuid[16] = {0};
        if (RAND_bytes(uuid, sizeof(uuid)) != 1) {
            throw transport_error("生成 uuid 失败");
        }
        // moonlight-qt: uniqueid=<uid>&uuid=<32 hex>（nvhttp.cpp:495-498）
        return "uniqueid=" + percent_encode(identity.uniqueid) + "&uuid=" + hex_lower(uuid, sizeof(uuid));
    }
};

namespace {

void check_nv_status(const RawResponse& response) {
    XmlDocument document = parse_xml(response.body);
    if (!document.ok) {
        throw NvError("响应不是合法 XML: " + document.error, response.http_status, -1, "",
                      response.body);
    }
    std::string status_code_text = document.root.attribute("status_code", "200");
    int status_code = parse_nv_status_code(status_code_text);
    if (status_code == 200) {
        return;
    }
    std::string status_message = document.root.attribute("status_message");
    std::string message = "服务端返回 status_code=" + std::to_string(status_code);
    if (!status_message.empty()) {
        message += ": " + status_message;
    }
    throw NvError(message, response.http_status, status_code, status_message, response.body);
}

ServerInfo parse_server_info(const std::string& xml) {
    XmlDocument document = parse_xml(xml);
    if (!document.ok) {
        throw NvError("serverinfo 响应不是合法 XML: " + document.error, 0, -1, "", xml);
    }
    const XmlNode& root = document.root;

    ServerInfo info;
    info.hostname = root.child_text("hostname");
    info.appversion = root.child_text("appversion");
    info.gfe_version = root.child_text("GfeVersion");
    info.server_uniqueid = root.child_text("uniqueid");
    info.mac = root.child_text("mac");
    info.local_ip = root.child_text("LocalIP");
    info.external_ip = root.child_text("ExternalIP");
    info.https_port = parse_int(root.child_text("HttpsPort"));
    info.external_port = parse_int(root.child_text("ExternalPort"));
    info.pair_status = parse_int(root.child_text("PairStatus"));
    info.state = root.child_text("state");
    info.current_game = parse_int(root.child_text("currentgame"));
    bool present = false;
    info.server_codec_mode_support =
        static_cast<uint32_t>(parse_uint64(root.child_text("ServerCodecModeSupport"), present));
    info.max_luma_pixels_hevc = parse_uint64(root.child_text("MaxLumaPixelsHEVC"), info.has_max_luma_hevc);
    info.max_luma_pixels_av1 = parse_uint64(root.child_text("MaxLumaPixelsAV1"), info.has_max_luma_av1);
    return info;
}

std::string build_launch_query(const std::string& common, const LaunchOptions& options,
                               std::string& rikey, uint32_t& rikeyid) {
    unsigned char key[16] = {0};
    unsigned char iv[4] = {0};
    ERR_clear_error();
    if (RAND_bytes(key, sizeof(key)) != 1 || RAND_bytes(iv, sizeof(iv)) != 1) {
        throw transport_error("生成 rikey 失败: " + ssl_error_text());
    }
    // moonlight-qt session.cpp:700-705 生成 16 字节 key + 4 字节 IV；
    // nvhttp.cpp:210-213 把 IV 前 4 字节按大端解释成 rikeyid。
    rikey = hex_lower(key, sizeof(key));
    rikeyid = (static_cast<uint32_t>(iv[0]) << 24) | (static_cast<uint32_t>(iv[1]) << 16) |
              (static_cast<uint32_t>(iv[2]) << 8) | static_cast<uint32_t>(iv[3]);

    // 参数顺序与 moonlight-qt nvhttp.cpp:218-237 一致。
    std::string query = common;
    query += "&appid=" + std::to_string(options.app_id);
    query += "&mode=" + std::to_string(options.width) + "x" + std::to_string(options.height) + "x" +
             std::to_string(options.fps);
    query += "&additionalStates=1";
    query += std::string("&sops=") + (options.sops ? "1" : "0");
    query += "&rikey=" + rikey;
    query += "&rikeyid=" + std::to_string(rikeyid);
    if (options.hdr) {
        query += "&hdrMode=1&clientHdrCapVersion=0&clientHdrCapSupportedFlagsInUint32=0"
                 "&clientHdrCapMetaDataId=NV_STATIC_METADATA_TYPE_1"
                 "&clientHdrCapDisplayData=0x0x0x0x0x0x0x0x0x0x0";
    }
    query += std::string("&localAudioPlayMode=") + (options.local_audio ? "1" : "0");
    query += "&surroundAudioInfo=" + std::to_string(options.surround_audio_info);
    query += "&remoteControllersBitmap=" + std::to_string(options.controller_bitmap);
    query += "&gcmap=" + std::to_string(options.gcmap);
    query += std::string("&gcpersist=") + (options.gcpersist ? "1" : "0");
    query += "&corever=1";  // LiGetLaunchUrlQueryParameters()，moonlight-common-c Connection.c:554
    return query;
}

}  // namespace

NvHttpClient::NvHttpClient(Options options) : impl_(new Impl()) {
    // 状态目录必须由调用方给出（应用沙箱绝对路径）；没有默认值，
    // 否则证书/私钥会落到进程当前目录。
    if (options.state_dir.empty()) {
        throw std::runtime_error("Sunshine 状态目录不能为空（须传应用沙箱绝对路径）");
    }
    impl_->options = std::move(options);
    impl_->identity = load_or_create_identity(impl_->options.state_dir);

    ERR_clear_error();
    impl_->ssl_ctx = SSL_CTX_new(TLS_client_method());
    if (impl_->ssl_ctx == nullptr) {
        throw transport_error("SSL_CTX_new 失败: " + ssl_error_text());
    }
    SSL_CTX_set_min_proto_version(impl_->ssl_ctx, TLS1_2_VERSION);
    // 服务端证书是自签的；配对阶段靠 PIN，之后靠指纹固定，这里不校验链。
    SSL_CTX_set_verify(impl_->ssl_ctx, SSL_VERIFY_NONE, nullptr);

    if (SSL_CTX_use_certificate_chain_file(impl_->ssl_ctx, impl_->identity.cert_path.c_str()) != 1) {
        throw transport_error("加载客户端证书失败: " + ssl_error_text());
    }
    if (SSL_CTX_use_PrivateKey_file(impl_->ssl_ctx, impl_->identity.key_path.c_str(),
                                    SSL_FILETYPE_PEM) != 1) {
        throw transport_error("加载客户端私钥失败: " + ssl_error_text());
    }
    if (SSL_CTX_check_private_key(impl_->ssl_ctx) != 1) {
        throw transport_error("客户端证书与私钥不匹配: " + ssl_error_text());
    }
}

NvHttpClient::~NvHttpClient() = default;

const Identity& NvHttpClient::identity() const {
    return impl_->identity;
}

void NvHttpClient::ensure_https_port() {
    if (impl_->options.https_port != 0 || impl_->discovered_https_port != 0) {
        return;
    }
    ServerInfoProbe probe = get_server_info();
    impl_->discovered_https_port = probe.https_port_used != 0 ? probe.https_port_used : 47984;
}

uint16_t NvHttpClient::https_port() const {
    if (impl_->options.https_port != 0) {
        return impl_->options.https_port;
    }
    return impl_->discovered_https_port != 0 ? impl_->discovered_https_port : 47984;
}

uint16_t NvHttpClient::ensure_https_port_plaintext() {
    if (impl_->options.https_port != 0) {
        return impl_->options.https_port;
    }
    if (impl_->discovered_https_port != 0) {
        return impl_->discovered_https_port;
    }
    // 只走明文 /serverinfo：配对前客户端证书一定未授权，走 TLS 只会拿到 401。
    RawResponse response =
        impl_->request("GET", false, impl_->options.http_port, "/serverinfo", impl_->common_query(),
                       "", "");
    check_nv_status(response);
    ServerInfo info = parse_server_info(response.body);
    impl_->discovered_https_port =
        info.https_port != 0 ? static_cast<uint16_t>(info.https_port) : 47984;
    return impl_->discovered_https_port;
}

ServerInfoProbe NvHttpClient::get_server_info() {
    ServerInfoProbe probe;

    // 1) 明文 HTTP：未配对时也能拿到 HttpsPort 等基本信息（moonlight-qt nvhttp.cpp:172-187）。
    uint16_t http_port = impl_->options.http_port;
    try {
        RawResponse response =
            impl_->request("GET", false, http_port, "/serverinfo", impl_->common_query(), "", "");
        probe.http_status = response.http_status;
        probe.http_xml = response.body;
        probe.http = parse_server_info(response.body);
        probe.http_ok = true;
    } catch (const NvError& error) {
        probe.http_error = error.what();
    }

    // 2) HTTPS + 客户端证书：Sunshine 只在证书通过校验时给出真实的 PairStatus/mac。
    uint16_t port = impl_->options.https_port;
    if (port == 0) {
        port = probe.http_ok && probe.http.https_port != 0 ? static_cast<uint16_t>(probe.http.https_port)
                                                          : 47984;
    }
    probe.https_port_used = port;
    try {
        RawResponse response =
            impl_->request("GET", true, port, "/serverinfo", impl_->common_query(), "", "");
        probe.https_status = response.http_status;
        probe.https_xml = response.body;
        probe.https_cert_sha256 = response.server_cert_sha256;
        try {
            check_nv_status(response);
            probe.https = parse_server_info(response.body);
            probe.https_ok = true;
        } catch (const NvError& error) {
            probe.https_error = error.what();
        }
    } catch (const PinMismatchError&) {
        // 指纹不一致必须让上层命令失败（退出码非 0），不能降级成“HTTPS 不可用”。
        throw;
    } catch (const NvError& error) {
        probe.https_error = error.what();
    }

    if (probe.https_ok) {
        impl_->discovered_https_port = port;
    }
    probe.server = probe.https_ok ? probe.https : probe.http;
    return probe;
}

std::vector<AppInfo> NvHttpClient::get_app_list() {
    ensure_https_port();
    RawResponse response =
        impl_->request("GET", true, https_port(), "/applist", impl_->common_query(), "", "");
    check_nv_status(response);

    XmlDocument document = parse_xml(response.body);
    if (!document.ok) {
        throw NvError("applist 响应不是合法 XML: " + document.error, response.http_status, -1, "",
                      response.body);
    }

    std::vector<AppInfo> apps;
    for (const XmlNode* node : document.root.children_named("App")) {
        AppInfo app;
        app.id = parse_int(node->child_text("ID"));
        app.title = node->child_text("AppTitle");
        app.app_name = node->child_text("AppName");
        app.hdr_supported = node->child_text("IsHdrSupported") == "1";
        app.hidden = node->child_text("Hidden") == "1";
        app.app_collector_game = node->child_text("IsAppCollectorGame") == "1";
        apps.push_back(std::move(app));
    }
    return apps;
}

SessionLaunch NvHttpClient::start_app(const char* verb, const LaunchOptions& options) {
    ensure_https_port();

    std::string rikey;
    uint32_t rikeyid = 0;
    std::string query = build_launch_query(impl_->common_query(), options, rikey, rikeyid);

    RawResponse response =
        impl_->request("GET", true, https_port(), std::string("/") + verb, query, "", "");
    check_nv_status(response);

    XmlDocument document = parse_xml(response.body);
    if (!document.ok) {
        throw NvError(std::string(verb) + " 响应不是合法 XML: " + document.error,
                      response.http_status, -1, "", response.body);
    }

    SessionLaunch launch;
    launch.raw_xml = response.body;
    launch.rikey = rikey;
    launch.rikeyid = rikeyid;
    launch.session_url0 = xml_first_text(document.root, "sessionUrl0");
    launch.session_url1 = xml_first_text(document.root, "sessionUrl1");
    if (launch.session_url0.empty()) {
        throw NvError(std::string(verb) + " 响应缺少 sessionUrl0",
                      response.http_status, -1, "", response.body);
    }
    return launch;
}

SessionLaunch NvHttpClient::launch(const LaunchOptions& options) {
    return start_app("launch", options);
}

SessionLaunch NvHttpClient::resume(const LaunchOptions& options) {
    return start_app("resume", options);
}

void NvHttpClient::cancel() {
    ensure_https_port();
    RawResponse response =
        impl_->request("GET", true, https_port(), "/cancel", impl_->common_query(), "", "");
    check_nv_status(response);
}

void NvHttpClient::unpair() {
    // moonlight-qt 在明文 HTTP 上调用 /unpair（nvpairingmanager.cpp:279-397）。
    RawResponse response = impl_->request("GET", false, impl_->options.http_port, "/unpair",
                                         impl_->common_query(), "", "");
    check_nv_status(response);
}

RawResponse NvHttpClient::get(const std::string& path, const std::string& extra_query) {
    ensure_https_port();
    std::string query = impl_->common_query();
    if (!extra_query.empty()) {
        query += "&";
        query += extra_query;
    }
    return impl_->request("GET", true, https_port(), path, query, "", "");
}

RawResponse NvHttpClient::post(const std::string& path, const std::string& extra_query,
                               const std::string& body, const std::string& content_type) {
    ensure_https_port();
    std::string query = impl_->common_query();
    if (!extra_query.empty()) {
        query += "&";
        query += extra_query;
    }
    return impl_->request("POST", true, https_port(), path, query, body, content_type);
}

PairResult NvHttpClient::pair(const std::string& pin, const std::string& device_name,
                              const PairProgress& progress) {
    if (pin.size() != 4 ||
        !std::all_of(pin.begin(), pin.end(),
                     [](unsigned char c) { return std::isdigit(c) != 0; })) {
        // Sunshine 的 pin() 只接受 4 位数字（nvhttp.cpp:644-660）。
        throw std::runtime_error("PIN 必须是 4 位数字（Sunshine 服务端要求）");
    }

    // 配对前只走明文 /serverinfo：HttpsPort 用于最后阶段，appversion 主版本决定哈希算法。
    RawResponse info_response = impl_->request("GET", false, impl_->options.http_port,
                                               "/serverinfo", impl_->common_query(), "", "");
    check_nv_status(info_response);
    const ServerInfo info = parse_server_info(info_response.body);
    const uint16_t port =
        impl_->options.https_port != 0
            ? impl_->options.https_port
            : (info.https_port != 0 ? static_cast<uint16_t>(info.https_port) : 47984);
    impl_->discovered_https_port = port;

    PairOptions options;
    options.pin = pin;
    options.device_name = device_name;
    options.host = impl_->options.host;
    options.https_port = port;
    options.server_generation = parse_int(info.appversion);
    options.pin_wait_ms = impl_->options.pin_wait_ms;

    // 前四阶段走明文 HTTP，最后阶段走 HTTPS；TLS 请求统一经过 Impl::request 的 pin 校验。
    PairTransport transport = [this, port](const PairRequest& request) {
        std::string query = impl_->common_query();
        query += "&";
        query += request.query;
        RawResponse raw =
            impl_->request("GET", request.tls, request.tls ? port : impl_->options.http_port,
                           "/pair", query, "", "", request.timeout_ms);
        PairResponse response;
        response.http_status = raw.http_status;
        response.body = std::move(raw.body);
        response.server_cert_sha256 = std::move(raw.server_cert_sha256);
        return response;
    };

    return run_pairing(impl_->identity, options, transport, progress);
}

}  // namespace sunshine
}  // namespace tg
