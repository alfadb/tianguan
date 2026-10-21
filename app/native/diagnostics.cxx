// SPDX-License-Identifier: GPL-3.0-or-later
// Uses TigerVNC v1.15.0 (GPL-2.0-or-later), without copying its implementation:
// https://github.com/TigerVNC/tigervnc/tree/v1.15.0
// Keep upstream copyright/license files and the build-time OHOS Thread patch
// with corresponding source when distributing the linked binary.
#include "diagnostics.h"
#include <rfb/CConnection.h>
#include <rfb/CMsgWriter.h>
#include <rfb/CSecurityRSAAES.h>
#include <rfb/Exception.h>
#include <rfb/PixelBuffer.h>
#include <network/Socket.h>
#include <network/TcpSocket.h>
#include <rdr/Exception.h>
#include <rdr/MemInStream.h>
#include <rdr/MemOutStream.h>
#include <os/Thread.h>
#include <os/Mutex.h>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <map>
#include <memory>
#include <netdb.h>
#include <poll.h>
#include <random>
#include <set>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <vector>
#include <jpeglib.h>
#include <atomic>
#include <deque>
#include <mutex>
#include <thread>

namespace {
std::string readSmallFile(const std::string& path, size_t limit, bool trim) {
  std::ifstream file(path.c_str(), std::ios::binary);
  if (!file) throw std::runtime_error("cannot open " + path);
  std::string data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  if (data.size() > limit) throw std::runtime_error("file too large: " + path);
  if (trim) {
    while (!data.empty() && (data.back() == '\n' || data.back() == '\r')) data.pop_back();
  }
  return data;
}
std::string jsonValue(const std::string& json, const std::string& key) {
  const std::string needle = "\"" + key + "\"";
  size_t at = json.find(needle);
  if (at == std::string::npos) throw std::runtime_error("missing " + key);
  at = json.find(':', at + needle.size());
  if (at == std::string::npos) throw std::runtime_error("bad " + key);
  ++at;
  while (at < json.size() && (json[at] == ' ' || json[at] == '\t')) ++at;
  if (at < json.size() && json[at] == '"') {
    // Escape-aware: session options carry a password that may contain quotes,
    // backslashes or control characters once the ArkTS side JSON-encodes it.
    // The old plain find('"') would silently truncate such a password.
    std::string out;
    size_t i = at + 1;
    while (i < json.size() && json[i] != '"') {
      const char c = json[i++];
      if (c != '\\') { out.push_back(c); continue; }
      if (i >= json.size()) throw std::runtime_error("bad " + key);
      const char escape = json[i++];
      switch (escape) {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'u': {
          if (i + 4 > json.size()) throw std::runtime_error("bad " + key);
          unsigned code = 0;
          for (int digit = 0; digit < 4; ++digit) {
            const char h = json[i++];
            code <<= 4;
            if (h >= '0' && h <= '9') code += static_cast<unsigned>(h - '0');
            else if (h >= 'a' && h <= 'f') code += static_cast<unsigned>(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F') code += static_cast<unsigned>(h - 'A' + 10);
            else throw std::runtime_error("bad " + key);
          }
          // A UTF-16 surrogate pair must become a single code point.
          if (code >= 0xd800 && code <= 0xdbff && i + 6 <= json.size() &&
              json[i] == '\\' && json[i + 1] == 'u') {
            unsigned low = 0;
            size_t j = i + 2;
            bool valid = true;
            for (int digit = 0; digit < 4; ++digit) {
              const char h = json[j++];
              low <<= 4;
              if (h >= '0' && h <= '9') low += static_cast<unsigned>(h - '0');
              else if (h >= 'a' && h <= 'f') low += static_cast<unsigned>(h - 'a' + 10);
              else if (h >= 'A' && h <= 'F') low += static_cast<unsigned>(h - 'A' + 10);
              else { valid = false; break; }
            }
            if (valid && low >= 0xdc00 && low <= 0xdfff) {
              code = 0x10000 + ((code - 0xd800) << 10) + (low - 0xdc00);
              i = j;
            }
          }
          if (code < 0x80) {
            out.push_back(static_cast<char>(code));
          } else if (code < 0x800) {
            out.push_back(static_cast<char>(0xc0 | (code >> 6)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
          } else if (code < 0x10000) {
            out.push_back(static_cast<char>(0xe0 | (code >> 12)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
          } else {
            out.push_back(static_cast<char>(0xf0 | (code >> 18)));
            out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
          }
          break;
        }
        default: throw std::runtime_error("bad " + key);
      }
    }
    if (i >= json.size()) throw std::runtime_error("bad " + key);
    return out;
  }
  size_t end = at;
  while (end < json.size() && (json[end] == '-' || json[end] == '.' ||
         (json[end] >= '0' && json[end] <= '9'))) ++end;
  if (end == at) throw std::runtime_error("bad " + key);
  return json.substr(at, end - at);
}
struct AddrInfoGuard {
  struct addrinfo* value;
  ~AddrInfoGuard() { if (value) freeaddrinfo(value); }
};
// TigerVNC installs SIGPIPE handling in network::initSockets() behind a plain
// static bool. Two session threads reaching it for the first time would race on
// that flag, so run it exactly once before any TcpSocket is constructed.
void initNetworkOnce() {
  static std::once_flag once;
  std::call_once(once, []() { network::initSockets(); });
}
void wipeString(std::string& value) {
  if (value.empty()) return;
  volatile char* data = &value[0];
  for (size_t i = 0; i < value.size(); ++i) data[i] = 0;
  value.clear();
}
struct WipeGuard {
  std::string& value;
  ~WipeGuard() { wipeString(value); }
};
std::string readPasswordFile(const std::string& path) {
  std::ifstream file(path.c_str(), std::ios::binary);
  if (!file) throw std::runtime_error("cannot read vnc-test.pass");
  std::string data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  if (data.size() > 514) throw std::runtime_error("vnc-test.pass must be 1..512 bytes with an optional trailing LF/CRLF");
  if (data.size() >= 2 && data[data.size() - 2] == '\r' && data[data.size() - 1] == '\n')
    data.resize(data.size() - 2);
  else if (!data.empty() && data.back() == '\n')
    data.resize(data.size() - 1);
  if (data.empty() || data.size() > 512)
    throw std::runtime_error("vnc-test.pass must be 1..512 bytes with an optional trailing LF/CRLF");
  if (data.find('\0') != std::string::npos || data.find('\r') != std::string::npos ||
      data.find('\n') != std::string::npos)
    throw std::runtime_error("vnc-test.pass must be a single line without NUL bytes");
  return data;
}
// A bounded connect keeps the whole network test inside its 20 second budget.
// The connected descriptor is still owned and driven by TigerVNC's TcpSocket.
int connectWithinDeadline(const std::string& host, int port,
                          std::chrono::steady_clock::time_point deadline) {
  struct addrinfo hints;
  std::memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  const std::string service = std::to_string(port);
  struct addrinfo* addresses = nullptr;
  const int resolved = getaddrinfo(host.c_str(), service.c_str(), &hints, &addresses);
  if (resolved != 0 || addresses == nullptr)
    throw std::runtime_error("cannot resolve host " + host);
  AddrInfoGuard guard{addresses};
  int lastError = 0;
  for (struct addrinfo* address = addresses; address != nullptr; address = address->ai_next) {
    const int fd = ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
    if (fd < 0) { lastError = errno; continue; }
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
      lastError = errno;
      ::close(fd);
      continue;
    }
    const int result = ::connect(fd, address->ai_addr, address->ai_addrlen);
    if (result == 0) {
      fcntl(fd, F_SETFL, flags);
      return fd;
    }
    if (errno != EINPROGRESS) {
      lastError = errno;
      ::close(fd);
      continue;
    }
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now()).count();
    if (remaining <= 0) {
      ::close(fd);
      throw std::runtime_error("TCP connect timed out after 20s");
    }
    struct pollfd pending;
    pending.fd = fd;
    pending.events = POLLOUT;
    pending.revents = 0;
    int ready;
    do { ready = ::poll(&pending, 1, static_cast<int>(remaining)); } while (ready < 0 && errno == EINTR);
    if (ready <= 0) {
      lastError = ETIMEDOUT;
      ::close(fd);
      continue;
    }
    int socketError = 0;
    socklen_t socketErrorLength = sizeof(socketError);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &socketError, &socketErrorLength) != 0)
      socketError = errno;
    if (socketError != 0) {
      lastError = socketError;
      ::close(fd);
      continue;
    }
    fcntl(fd, F_SETFL, flags);
    return fd;
  }
  throw std::runtime_error(std::string("TCP connect failed: ") +
                           (lastError ? std::strerror(lastError) : "no usable address"));
}
bool encodeJpegBuffer(rfb::ModifiablePixelBuffer* framebuffer, int width, int height,
                      const std::string& path, long& outSize, std::string& err) {
  outSize = 0;
  if (framebuffer == nullptr || width <= 0 || height <= 0) {
    err = "no decoded frame";
    return false;
  }
  std::vector<uint8_t> rgb(static_cast<size_t>(width) * height * 3);
  int stride = 0;
  const uint8_t* base = framebuffer->getBuffer(rfb::Rect(0, 0, width, height), &stride);
  if (base == nullptr) {
    err = "framebuffer not readable";
    return false;
  }
  const rfb::PixelFormat pf = framebuffer->getPF();
  // getBuffer() reports the stride in pixels, so convert it to bytes here.
  const size_t rowBytes = static_cast<size_t>(stride) * (pf.bpp / 8);
  for (int y = 0; y < height; ++y)
    pf.rgbFromBuffer(&rgb[static_cast<size_t>(y) * width * 3],
                     base + static_cast<size_t>(y) * rowBytes, width);

  FILE* out = std::fopen(path.c_str(), "wb");
  if (out == nullptr) {
    err = "cannot open snapshot file";
    return false;
  }
  jpeg_compress_struct info;
  jpeg_error_mgr errors;
  info.err = jpeg_std_error(&errors);
  jpeg_create_compress(&info);
  jpeg_stdio_dest(&info, out);
  info.image_width = width;
  info.image_height = height;
  info.input_components = 3;
  info.in_color_space = JCS_RGB;
  jpeg_set_defaults(&info);
  jpeg_set_quality(&info, 85, TRUE);
  jpeg_start_compress(&info, TRUE);
  while (info.next_scanline < info.image_height) {
    JSAMPROW row = &rgb[static_cast<size_t>(info.next_scanline) * width * 3];
    jpeg_write_scanlines(&info, &row, 1);
  }
  jpeg_finish_compress(&info);
  jpeg_destroy_compress(&info);
  std::fclose(out);
  std::ifstream check(path.c_str(), std::ios::binary | std::ios::ate);
  if (!check) {
    err = "snapshot not written";
    return false;
  }
  outSize = static_cast<long>(check.tellg());
  if (outSize <= 0) {
    err = "empty snapshot";
    return false;
  }
  return true;
}
class NetClient : public rfb::CConnection {
public:
  NetClient(const std::string& password, std::string& report,
            const std::string& typesText, const std::string& expectedPin)
    : password(password), report(report), expectedPin(expectedPin), authenticated(false),
      framed(false), updateHasArea(false), centreCovered(false), width(0), height(0),
      needsConfirmation(false), fingerprintMismatch(false) {
    auto types = rfb::parseSecTypes(typesText.c_str());
    if (types.empty()) throw std::runtime_error("no supported security type requested");
    security.SetSecTypes(types);
    setShared(true);
    setPreferredEncoding(0);
  }
  ~NetClient() override { wipeString(password); }
  bool authenticated;
  bool framed;
  int width, height;
  std::string centre;
  bool needsConfirmation;
  bool fingerprintMismatch;
  const std::string& serverPin() const { return fingerprint; }
  // Encode the decoded framebuffer as JPEG so the UI can display a real frame.
  bool saveJpeg(const std::string& path, long& outSize, std::string& err) {
    if (!framed) { outSize = 0; err = "no decoded frame"; return false; }
    return encodeJpegBuffer(getFramebuffer(), width, height, path, outSize, err);
  }
  // Copy the decoded framebuffer as packed RGB for pixel-level comparison.
  bool copyRgb(std::vector<uint8_t>& out) {
    if (!framed || width <= 0 || height <= 0 || getFramebuffer() == nullptr) return false;
    out.resize(static_cast<size_t>(width) * height * 3);
    int stride = 0;
    const uint8_t* base = getFramebuffer()->getBuffer(rfb::Rect(0, 0, width, height), &stride);
    if (base == nullptr) return false;
    const rfb::PixelFormat pf = getFramebuffer()->getPF();
    const size_t rowBytes = static_cast<size_t>(stride) * (pf.bpp / 8);
    for (int y = 0; y < height; ++y)
      pf.rgbFromBuffer(&out[static_cast<size_t>(y) * width * 3],
                       base + static_cast<size_t>(y) * rowBytes, width);
    return true;
  }
  void getUserPasswd(bool secure, std::string* user, std::string* pass) override {
    // Test mode: RA2_256 calls this with user == nullptr and secure == true,
    // while RA2/VncAuth pass a user slot and report secure == false. Accept all
    // of them, and report which shape was used so the weakness stays visible.
    if (pass == nullptr || password.empty())
      throw std::runtime_error("unexpected credential request");
    if (user != nullptr) *user = "";
    report += std::string("credential callback: userSlot=") + (user ? "yes" : "no") +
              (secure ? ", secure=yes" : ", secure=no") + "\n";
    *pass = password;
  }
  bool showMsgBox(rfb::MsgBoxFlags, const char* title, const char* text) override {
    // Identity confirmation: trust only the fingerprint the caller already
    // trusts. Without one we stop before authentication so the UI can ask.
    if (fingerprint.empty()) fingerprint = fingerprintLine(title, text);
    if (expectedPin.empty()) {
      needsConfirmation = true;
      return false;
    }
    if (fingerprint != expectedPin) {
      fingerprintMismatch = true;
      return false;
    }
    report += "fingerprint: " + fingerprint + " (trusted)\n";
    return true;
  }
  void authSuccess() override {
    rfb::CConnection::authSuccess();
    authenticated = true;
    const uint32_t type = csecurity ? csecurity->getType() : rfb::secTypeInvalid;
    // Upstream reports isSecure() only for RA2_256, so classify the negotiated
    // type explicitly instead of trusting that helper.
    const char* label;
    switch (type) {
      case rfb::secTypeRA256:   label = "AES-256 encrypted"; break;
      case rfb::secTypeRA2:     label = "AES-128 encrypted"; break;
      case rfb::secTypeRAne256: label = "NOT encrypted (RA2ne_256)"; break;
      case rfb::secTypeRA2ne:   label = "NOT encrypted (RA2ne)"; break;
      case rfb::secTypeVncAuth: label = "NOT encrypted (VncAuth)"; break;
      default:                  label = "unknown protection"; break;
    }
    report += std::string("PASS authentication complete with ") +
              rfb::secTypeName(type) + " (" + label + ")\n";
    wipeString(password);
  }
  void setCursor(int, int, const rfb::Point&, const uint8_t*) override {}
  void setCursorPos(const rfb::Point&) override {}
  void setColourMapEntries(int, int, uint16_t*) override {
    throw std::runtime_error("indexed colour unsupported");
  }
  void bell() override {}
  void initDone() override {
    if (!authenticated) throw std::runtime_error("initDone before authentication");
    width = server.width();
    height = server.height();
    if (width <= 0 || height <= 0 || width > 4096 || height > 4096)
      throw std::runtime_error("unexpected screen size");
    setFramebuffer(new rfb::ManagedPixelBuffer(server.pf(), width, height));
  }
  bool dataRect(const rfb::Rect& rect, int encoding) override {
    const bool result = rfb::CConnection::dataRect(rect, encoding);
    if (result && rect.area() > 0) {
      updateHasArea = true;
      const int centreX = width / 2;
      const int centreY = height / 2;
      if (rect.tl.x <= centreX && rect.br.x > centreX &&
          rect.tl.y <= centreY && rect.br.y > centreY)
        centreCovered = true;
    }
    return result;
  }
  void framebufferUpdateStart() override {
    rfb::CConnection::framebufferUpdateStart();
    updateHasArea = false;
    centreCovered = false;
  }
  void framebufferUpdateEnd() override {
    CConnection::framebufferUpdateEnd(); // Flush real DecodeManager workers.
    if (framed || !updateHasArea || !centreCovered) return;
    int stride = 0;
    const uint8_t* pixels = getFramebuffer()->getBuffer(
        rfb::Rect(width / 2, height / 2, width / 2 + 1, height / 2 + 1), &stride);
    if (pixels == nullptr) return;
    uint8_t rgb[3];
    getFramebuffer()->getPF().rgbFromBuffer(rgb, pixels, 1);
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "%02x%02x%02x", rgb[0], rgb[1], rgb[2]);
    centre = buffer;
    framed = true;
  }
private:
  static std::string fingerprintLine(const char* title, const char* text) {
    std::string line;
    if (text != nullptr) {
      const std::string prompt(text);
      const std::string label = "Fingerprint:";
      const size_t at = prompt.find(label);
      if (at != std::string::npos) {
        const size_t start = at + label.size();
        const size_t end = prompt.find('\n', start);
        line = prompt.substr(start, end == std::string::npos ? std::string::npos : end - start);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        while (!line.empty() && line.front() == ' ') line.erase(0, 1);
      }
    }
    if (line.empty() && title != nullptr) line = title;
    if (line.empty()) line = "server prompt carried no Fingerprint line";
    return line;
  }
  std::string password;
  std::string& report;
  std::string expectedPin;
  std::string fingerprint;
  bool updateHasArea;
  bool centreCovered;
};
class Client : public rfb::CConnection {
public:
  explicit Client(bool memoryFixture) : done(false), decoded(false) {
    auto types = rfb::parseSecTypes(memoryFixture ? "None" : "RA2_256");
    security.SetSecTypes(types);
    setShared(true);
    setPreferredEncoding(0);
  }
  bool done;
  bool decoded;
  void getUserPasswd(bool, std::string*, std::string*) override {
    throw std::runtime_error("Credential input disabled");
  }
  bool showMsgBox(rfb::MsgBoxFlags, const char*, const char*) override { return false; }
  void setCursor(int, int, const rfb::Point&, const uint8_t*) override {}
  void setCursorPos(const rfb::Point&) override {}
  void setColourMapEntries(int, int, uint16_t*) override {
    throw std::runtime_error("Unexpected indexed colour");
  }
  void bell() override {}
  void initDone() override {
    if (server.width() != 2 || server.height() != 1)
      throw std::runtime_error("Unexpected fixture dimensions");
    setFramebuffer(new rfb::ManagedPixelBuffer(server.pf(), 2, 1));
  }
  bool dataRect(const rfb::Rect& rect, int encoding) override {
    bool result = CConnection::dataRect(rect, encoding);
    if (result && encoding == 0 && rect == rfb::Rect(0, 0, 2, 1)) decoded = true;
    return result;
  }
  void framebufferUpdateEnd() override {
    CConnection::framebufferUpdateEnd(); // Flush real DecodeManager workers.
    if (!decoded) throw std::runtime_error("Raw rectangle missing");
    int stride;
    const uint8_t* pixels = getFramebuffer()->getBuffer(rfb::Rect(0, 0, 2, 1), &stride);
    uint8_t rgb[6];
    getFramebuffer()->getPF().rgbFromBuffer(rgb, pixels, 2);
    const uint8_t expected[] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    for (unsigned i = 0; i < sizeof(expected); ++i)
      if (rgb[i] != expected[i]) throw std::runtime_error("Raw pixel mismatch");
    done = true;
  }
};
void factory() {
  Client client(false);
  std::unique_ptr<rfb::CSecurity> security(client.security.GetCSecurity(&client, rfb::secTypeRA256));
  if (!dynamic_cast<rfb::CSecurityRSAAES*>(security.get()) || security->getType() != rfb::secTypeRA256)
    throw std::runtime_error("RA2_256 factory mismatch");
}
void raw() {
  // Full fixed server transcript: version, None offer, SecurityResult,
  // ServerInit (2x1, little-endian RGB), then one Raw framebuffer update.
  static const uint8_t transcript[] = {
    'R','F','B',' ','0','0','3','.','0','0','8','\n', 1,1, 0,0,0,0,
    0,2,0,1, 32,24,0,1, 0,255,0,255,0,255,16,8,0,0,0,0,
    0,0,0,7, 'f','i','x','t','u','r','e',
    0,0,0,1, 0,0,0,0,0,2,0,1,0,0,0,0,
    0x33,0x22,0x11,0,0x66,0x55,0x44,0
  };
  rdr::MemInStream input(transcript, sizeof(transcript));
  rdr::MemOutStream output;
  Client client(true);
  client.setStreams(&input, &output);
  client.initialiseProtocol();
  for (unsigned step = 0; !client.done && step < 32; ++step)
    if (!client.processMsg()) throw std::runtime_error("Incomplete memory fixture");
  if (!client.done) throw std::runtime_error("Memory fixture did not finish");
  client.close();
}
struct State {
  State() : condition(&mutex), entered(false), stop(false), exited(false) {}
  os::Mutex mutex;
  os::Condition condition;
  bool entered, stop, exited;
};
class Worker : public os::Thread {
public:
  explicit Worker(State& state) : state(state) {}
  void stop() {
    os::AutoMutex lock(&state.mutex);
    state.stop = true;
    state.condition.broadcast();
  }
protected:
  void worker() override {
    os::AutoMutex lock(&state.mutex);
    state.entered = true;
    state.condition.broadcast();
    while (!state.stop) state.condition.wait();
    state.exited = true;
  }
private:
  State& state;
};
void thread() {
  State state; // Outlives worker and its deletion.
  std::unique_ptr<Worker> worker(new Worker(state));
  worker->start();
  {
    os::AutoMutex lock(&state.mutex);
    while (!state.entered) state.condition.wait();
  }
  worker->stop();
  worker->wait();
  if (worker->isRunning()) throw std::runtime_error("Worker still running");
  {
    os::AutoMutex lock(&state.mutex);
    if (!state.exited) throw std::runtime_error("Worker exit not observed");
  }
  worker.reset();
}
void check(std::string& report, const char* name, void (*test)()) {
  try { test(); report += std::string("PASS ") + name + "\n"; }
  catch (const std::exception& error) {
    report += std::string("FAIL ") + name + ": " + error.what() + "\n";
  }
  catch (...) { report += std::string("FAIL ") + name + ": unknown exception\n"; }
}
}
std::string tianguan::runDiagnostics() {
  std::string report;
  check(report, "RA2_256 real CSecurityRSAAES factory (authentication not tested)", factory);
  check(report, "RFB 3.8 memory Raw 2x1 RGB=112233,445566", raw);
  check(report, "Thread entered -> cooperative stop -> wait -> !running -> delete; exit complete", thread);
  report += "Diagnostics only; no socket connection; device/real RA2 authentication not verified.\n";
  return report;
}
std::string tianguan::runNetworkTest(const std::string& dataDir) {
  std::string report;
  initNetworkOnce();
  try {
    std::string dir = dataDir;
    while (!dir.empty() && dir.back() == '/') dir.pop_back();
    if (dir.empty()) throw std::runtime_error("empty data directory");
    const std::string jsonPath = dir + "/vnc-test.json";
    const std::string passPath = dir + "/vnc-test.pass";
    if (access(jsonPath.c_str(), F_OK) != 0) {
      if (errno == ENOENT) return "SKIP network test: no vnc-test.json\n";
      throw std::runtime_error("cannot access vnc-test.json");
    }
    std::string json;
    try {
      json = readSmallFile(jsonPath, 4096, false);
    } catch (const std::exception& error) {
      throw std::runtime_error(std::string("cannot read vnc-test.json: ") + error.what());
    }
    const std::string host = jsonValue(json, "host");
    const std::string portText = jsonValue(json, "port");
    if (host.empty() || host.size() > 253) throw std::runtime_error("invalid host in vnc-test.json");
    size_t used = 0;
    int port = 0;
    try {
      port = std::stoi(portText, &used);
    } catch (...) {
      throw std::runtime_error("invalid port in vnc-test.json");
    }
    if (used != portText.size() || port < 1 || port > 65535)
      throw std::runtime_error("invalid port in vnc-test.json");

    if (access(passPath.c_str(), F_OK) != 0) {
      if (errno == ENOENT) return "SKIP network test: no vnc-test.pass\n";
      throw std::runtime_error("cannot access vnc-test.pass");
    }
    std::string password = readPasswordFile(passPath);
    WipeGuard passwordGuard{password};

    // Optional "security" field selects the offered types; default keeps the
    // encrypted RA2 family only. Weaker types are opt-in for interoperability tests.
    std::string typesText = "RA2_256,RA2";
    if (json.find("\"security\"") != std::string::npos) {
      typesText = jsonValue(json, "security");
      if (typesText.empty()) throw std::runtime_error("empty security field");
    }
    report += "requested security types: " + typesText + "\n";

    // One budget covers connect, authentication and the first frame.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    const int fd = connectWithinDeadline(host, port, deadline);
    network::TcpSocket socket(fd);
    struct timeval timeout;
    timeout.tv_sec = 20;
    timeout.tv_usec = 0;
    setsockopt(socket.getFd(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(socket.getFd(), SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    report += "PASS TCP connected " + host + ":" + std::to_string(port) + "\n";

    std::string expectedPin;
    if (json.find("\"expectedFingerprint\"") != std::string::npos)
      expectedPin = jsonValue(json, "expectedFingerprint");

    NetClient client(password, report, typesText, expectedPin);
    client.setServerName(host.c_str());
    client.setStreams(&socket.inStream(), &socket.outStream());
    client.initialiseProtocol();
    try {
      while (!client.framed) {
        if (std::chrono::steady_clock::now() >= deadline) {
          if (!client.authenticated)
            throw std::runtime_error("timed out after 20s before authentication completed");
          throw std::runtime_error("timed out after 20s waiting for first frame");
        }
        if (!client.processMsg()) {
          const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
              deadline - std::chrono::steady_clock::now()).count();
          if (remaining <= 0) continue;
          struct pollfd pending;
          pending.fd = socket.getFd();
          pending.events = POLLIN;
          pending.revents = 0;
          const int wait = remaining > 100 ? 100 : static_cast<int>(remaining);
          int ready;
          do { ready = poll(&pending, 1, wait); } while (ready < 0 && errno == EINTR);
          if (ready < 0) throw std::runtime_error("poll failed");
        }
      }
    } catch (const std::exception&) {
      // An unconfirmed or mismatched identity aborts the handshake on purpose;
      // report it as a decision, not as a generic protocol failure.
      if (client.needsConfirmation) {
        report += "NEEDS_CONFIRMATION: " + client.serverPin() + "\n";
        return report;
      }
      if (client.fingerprintMismatch) {
        report += "FAIL fingerprint mismatch: got " + client.serverPin() +
                  ", expected " + expectedPin + "\n";
        return report;
      }
      throw;
    }

    report += "PASS first frame " + std::to_string(client.width) + "x" +
              std::to_string(client.height) + "\n";
    report += "PASS center pixel " + client.centre + "\n";

    const bool wantSnapshot = json.find("\"snapshot\"") != std::string::npos &&
                              jsonValue(json, "snapshot") == "true";
    if (wantSnapshot) {
      long size = 0;
      std::string error;
      if (client.saveJpeg(dir + "/snapshot.jpg", size, error)) {
        report += "PASS snapshot snapshot.jpg (" + std::to_string(size) + " bytes)\n";
      } else {
        report += "FAIL snapshot: " + error + "\n";
      }
    }

    // Click the top-left corner, then look for a visual change: on a static
    // desktop that corner opens/closes the applications menu, so a changed
    // frame is evidence the click reached the server.
    std::vector<uint8_t> beforeRgb;
    if (wantSnapshot) client.copyRgb(beforeRgb);
    const auto clickAt = [](NetClient& c, int x, int y) {
      c.writer()->writePointerEvent(rfb::Point(x, y), 0);
      c.writer()->writePointerEvent(rfb::Point(x, y), 1);
      c.writer()->writePointerEvent(rfb::Point(x, y), 0);
      c.getOutStream()->flush();
    };
    if (wantSnapshot) {
      // Double-click the target: opening a window is an unambiguous visual
      // change, unlike a single click that may only toggle focus.
      const std::string cx = jsonValue(json, "clickX");
      const std::string cy = jsonValue(json, "clickY");
      const int targetX = cx.empty() ? 50 : std::stoi(cx);
      const int targetY = cy.empty() ? 55 : std::stoi(cy);
      const std::string clicksText = jsonValue(json, "clicks");
      const int clicks = clicksText.empty() ? 2 : std::stoi(clicksText);
      clickAt(client, targetX, targetY);
      for (int i = 1; i < clicks; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        clickAt(client, targetX, targetY);
      }
    }
    client.sendKeyPress(1, 0, 0x61);
    client.sendKeyRelease(1);
    client.writer()->writePointerEvent(rfb::Point(1, 1), 1);
    client.writer()->writePointerEvent(rfb::Point(1, 1), 0);
    client.getOutStream()->flush();
    report += "PASS input sent (key a press/release, pointer 1,1 button1 press/release)\n";

    if (wantSnapshot) {
      // Give the server time to render the click, draining any updates it sends.
      const auto settle = std::chrono::steady_clock::now() + std::chrono::milliseconds(900);
      while (std::chrono::steady_clock::now() < settle) {
        if (!client.processMsg()) {
          pollfd fd{socket.getFd(), POLLIN, 0};
          poll(&fd, 1, 50);
        }
      }
      std::vector<uint8_t> after;
      if (client.copyRgb(after) && after.size() == beforeRgb.size() && !after.empty()) {
        size_t changed = 0;
        int minX = client.width, minY = client.height, maxX = -1, maxY = -1;
        for (int y = 0; y < client.height; ++y) {
          for (int x = 0; x < client.width; ++x) {
            const size_t at = (static_cast<size_t>(y) * client.width + x) * 3;
            if (after[at] != beforeRgb[at] || after[at + 1] != beforeRgb[at + 1] ||
                after[at + 2] != beforeRgb[at + 2]) {
              ++changed;
              if (x < minX) minX = x;
              if (y < minY) minY = y;
              if (x > maxX) maxX = x;
              if (y > maxY) maxY = y;
            }
          }
        }
        if (changed == 0) {
          report += "FAIL input observed: no pixel changed after click\n";
        } else {
          report += "PASS input observed: " + std::to_string(changed) +
                    " pixels changed, bbox (" + std::to_string(minX) + "," +
                    std::to_string(minY) + ")-(" + std::to_string(maxX) + "," +
                    std::to_string(maxY) + ")\n";
        }
      } else {
        report += "FAIL input observation: framebuffer not readable after click\n";
      }
      // Keep the post-click frame as evidence next to the pre-click one.
      long evidenceSize = 0;
      std::string evidenceError;
      client.saveJpeg(dir + "/snapshot-after.jpg", evidenceSize, evidenceError);

      // Keyboard proof: open the applications menu, then move the selection
      // with Down. A changed frame means the key reached the server's X server.
      if (json.find("\"keyboard\"") != std::string::npos && jsonValue(json, "keyboard") == "true") {
        clickAt(client, 10, 10);
        const auto menuSettle = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        while (std::chrono::steady_clock::now() < menuSettle) {
          if (!client.processMsg()) {
            pollfd fd{socket.getFd(), POLLIN, 0};
            poll(&fd, 1, 50);
          }
        }
        std::vector<uint8_t> menuBefore;
        client.copyRgb(menuBefore);
        client.sendKeyPress(0xff54, 0, 0xff54);
        client.sendKeyRelease(0xff54);
        client.getOutStream()->flush();
        const auto keySettle = std::chrono::steady_clock::now() + std::chrono::milliseconds(600);
        while (std::chrono::steady_clock::now() < keySettle) {
          if (!client.processMsg()) {
            pollfd fd{socket.getFd(), POLLIN, 0};
            poll(&fd, 1, 50);
          }
        }
        std::vector<uint8_t> menuAfter;
        if (client.copyRgb(menuAfter) && menuAfter.size() == menuBefore.size() && !menuAfter.empty()) {
          size_t changed = 0;
          for (size_t i = 0; i < menuAfter.size(); i += 3)
            if (menuAfter[i] != menuBefore[i]) ++changed;
          if (changed > 0)
            report += "PASS keyboard observed: " + std::to_string(changed) +
                      " pixels changed after Down key\n";
          else
            report += "FAIL keyboard observed: no change after Down key\n";
        } else {
          report += "FAIL keyboard observed: framebuffer not readable\n";
        }
        long keySize = 0;
        std::string keyError;
        client.saveJpeg(dir + "/snapshot-key.jpg", keySize, keyError);
      }
    }
    client.close();
  } catch (const std::exception& error) {
    report += std::string("FAIL network test: ") + error.what() + "\n";
  } catch (...) {
    report += "FAIL network test: unknown exception\n";
  }
  return report;
}

// ---------------------------------------------------------------------------
// Multi-session live connections + clipboard.
//
// Every session owns its socket, TigerVNC CConnection, worker thread, input
// queue, framebuffer mirror, cursor state, version, generation and clipboard.
// The registry lock is only taken to find/insert/remove map entries. It is
// never held across connect, authentication, socket IO or thread join, so a
// session stuck on the network cannot stall the UI thread or another session.
//
// Only the session thread ever touches the socket: UI-side calls either read a
// protected snapshot or append to a queue, so no lock is ever held during a
// blocking read/write.
// ---------------------------------------------------------------------------
namespace {

// Clipboard text is capped so a hostile or buggy server cannot make one session
// consume arbitrary memory. The contract allows 1 MiB of plain text.
const size_t kClipboardLimit = 1024 * 1024;
const size_t kMaxQueuedInputs = 4096;
// Clipboard text that has not reached the server yet is already reported to the
// UI as delivered, so it must survive a connection that is not writable yet.
// Only the newest text is worth keeping (a newer copy supersedes the older one);
// a bigger bound would let one failed connection attempt add one entry each.
const size_t kMaxQueuedClipboards = 1;
// If a consumer that is otherwise up to date has not polled for this many
// framebuffer versions, the accumulated union is likely near the whole screen
// and a full frame is the cheaper (and always correct) answer.
const int kFrameVersionGapFull = 64;

enum SessionPhase {
  PhaseConnecting = 0,
  PhaseConnected,
  PhaseDisconnected,
  PhaseFailed,
  PhaseTrustRequired,
  PhaseReconnecting
};

const char* phaseName(int phase) {
  switch (phase) {
    case PhaseConnecting: return "connecting";
    case PhaseConnected: return "connected";
    case PhaseDisconnected: return "disconnected";
    case PhaseFailed: return "failed";
    case PhaseTrustRequired: return "trust-required";
    case PhaseReconnecting: return "reconnecting";
  }
  return "failed";
}

struct QueuedInput {
  // 0 = pointer (a=x, b=y, c=mask), 1 = key (a=keysym, b=pressed),
  // 2 = clipboard text (text).
  enum Kind { PointerKind = 0, KeyKind = 1, ClipboardKind = 2 };
  Kind kind;
  int a;
  int b;
  int c;
  std::string text;
};

std::string jsonEscape(const std::string& text) {
  std::string out;
  out.reserve(text.size() + 8);
  for (size_t i = 0; i < text.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buffer[8];
          std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
          out += buffer;
        } else {
          out.push_back(static_cast<char>(c));
        }
    }
  }
  return out;
}
std::string sanitizeLine(const std::string& text) {
  std::string out;
  out.reserve(text.size());
  for (size_t i = 0; i < text.size(); ++i)
    out.push_back((text[i] == '\n' || text[i] == '\r') ? ' ' : text[i]);
  return out;
}
// One session. The session thread is the only writer of the framebuffer mirror,
// region and cursor buffers. Every other thread takes the matching mutex for a
// short snapshot. Each lock protects one independent piece of state so status,
// frame taking and input never wait for the network.
struct SessionState {
  SessionState()
    : port(0), stop(false), forceReconnect(false), running(false), generation(1),
      phase(PhaseConnecting), releaseEpoch(0), needFullRequest(false), retryInMs(0),
      connected(false), errorCode("none"), fbWidth(0), fbHeight(0), version(0),
      frameGeneration(0), takenVersion(0), pixEpoch(0), takenEpoch(0),
      rectX(0), rectY(0), rectW(0), rectH(0),
      updAny(false), updX0(0), updY0(0), updX1(0), updY1(0),
      pendAny(false),
      pendX0(0), pendY0(0), pendX1(0), pendY1(0), regionFull(false),
      mirrorComplete(false), wantFull(true), cursorW(0), cursorH(0), cursorHotX(0),
      cursorHotY(0), cursorVersion(0), cursorBlank(false), cursorX(0), cursorY(0),
      clipVersion(0), clipTruncated(false), appliedRelease(0), pointerMask(0),
      lastPointerX(0), lastPointerY(0) {}

  std::string id;
  std::string profileName;
  std::string host;
  int port;
  std::string expectedPin;
  // Kept only so a reconnect can authenticate again; wiped by closeSession and
  // never written to a log or a status string.
  std::string password;
  // Security types offered to the server. Defaults to the encrypted family; a
  // "no password" connection passes "None", which is the only way to talk to a
  // server that advertises nothing but None (type 1).
  std::string securityTypes = "RA2_256,RA2";

  std::mutex meta;   // phase/error/retry/generation decisions
  std::mutex input;  // queued input + release epoch
  std::mutex pix;    // framebuffer mirror, region, cursor, versions
  std::mutex clip;   // clipboard text
  PerfStats perf;    // own mutex; timing summaries only

  std::atomic<bool> stop;
  std::atomic<bool> forceReconnect;
  std::atomic<bool> running;
  std::atomic<int> generation;
  std::atomic<int> phase;
  std::atomic<unsigned> releaseEpoch;
  std::atomic<bool> needFullRequest;
  std::atomic<int> retryInMs;

  bool connected;           // meta
  std::string error;        // meta, always a single line
  std::string errorCode;    // meta
  std::string fingerprint;  // meta

  std::deque<QueuedInput> inputs;  // input

  int fbWidth, fbHeight;          // pix
  int version;                    // pix
  int frameGeneration;            // pix: connection that produced `version`
  int takenVersion;               // pix: version of the last successful handoff
  // Geometry/layout epoch. Bumped by initDone() and resizeFramebuffer(); the
  // mirror is rebuilt on both, so a partial handoff from before the bump can
  // never be positioned on the current mirror.
  int pixEpoch;                   // pix
  int takenEpoch;                 // pix: pixEpoch of the last successful handoff
  int rectX, rectY, rectW, rectH; // pix (legacy takeRegion only)
  // Layer 1: rects decoded into the in-flight framebuffer update. Only the
  // session thread touches these; the commit in framebufferUpdateEnd() moves
  // them into the delivery layer and clears them.
  bool updAny;                    // pix
  int updX0, updY0, updX1, updY1; // pix
  // Layer 2: union of committed changes not handed to the consumer yet.
  // FramebufferUpdateEnd() only adds to it; only a successful takeFrame()
  // copy clears it.
  bool pendAny;                   // pix
  int pendX0, pendY0, pendX1, pendY1;  // pix
  bool regionFull;                // pix: pending region is the whole screen
  bool mirrorComplete;            // pix: at least one full frame was applied
  bool wantFull;                  // pix
  std::vector<uint8_t> full;      // pix, BGRA, fbWidth * fbHeight * 4
  std::vector<uint8_t> region;    // pix, BGRA, rectW * rectH * 4
  std::vector<uint8_t> cursor;    // pix, RGBA cursor image
  int cursorW, cursorH, cursorHotX, cursorHotY, cursorVersion;  // pix
  bool cursorBlank;               // pix
  int cursorX, cursorY;           // pix

  std::string clipText;    // clip
  int clipVersion;         // clip
  bool clipTruncated;      // clip

  // Owned by the session thread. releaseSessionInput() only bumps releaseEpoch,
  // so the thread can release exactly the keys it actually pressed.
  unsigned appliedRelease;
  std::set<int> pressedKeys;
  int pointerMask;
  int lastPointerX, lastPointerY;

  // shutdown() from closeSession wakes a read that is blocked waiting for the
  // peer. The slot mutex keeps the descriptor from being closed and reused
  // between the liveness check and the call. shutdown itself never waits.
  struct SocketSlot {
    SocketSlot() : fd(-1), live(false) {}
    std::mutex mutex;
    int fd;
    bool live;
    void attach(int value) {
      std::lock_guard<std::mutex> lock(mutex);
      fd = value;
      live = true;
    }
    void detach() {
      std::lock_guard<std::mutex> lock(mutex);
      live = false;
      fd = -1;
    }
    void interrupt() {
      std::lock_guard<std::mutex> lock(mutex);
      if (live && fd >= 0) ::shutdown(fd, SHUT_RDWR);
    }
  } sock;

  std::thread thread;
};

struct SocketSlotGuard {
  explicit SocketSlotGuard(SessionState::SocketSlot& slot) : slot(slot) {}
  ~SocketSlotGuard() { slot.detach(); }
  SessionState::SocketSlot& slot;
};

std::string pinFromPrompt(const char* title, const char* text) {
  std::string line;
  if (text != nullptr) {
    const std::string prompt(text);
    const std::string label = "Fingerprint:";
    const size_t at = prompt.find(label);
    if (at != std::string::npos) {
      const size_t start = at + label.size();
      const size_t end = prompt.find('\n', start);
      line = prompt.substr(start, end == std::string::npos ? std::string::npos : end - start);
      while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
      while (!line.empty() && line.front() == ' ') line.erase(0, 1);
    }
  }
  if (line.empty() && title != nullptr) line = title;
  if (line.empty()) line = "server prompt carried no Fingerprint line";
  return line;
}

void setPhase(SessionState& state, int phase, const std::string& error,
              const std::string& errorCode, int retryInMs) {
  std::lock_guard<std::mutex> lock(state.meta);
  state.phase = phase;
  state.error = sanitizeLine(error);
  state.errorCode = errorCode;
  state.retryInMs = retryInMs;
  state.connected = (phase == PhaseConnected);
  state.running = (phase == PhaseConnected);
}

// TigerVNC keeps one CConnection per client. This subclass only owns the
// session's rendering, identity checks and clipboard; all protocol work stays
// inside TigerVNC so every session behaves like a normal viewer.
class LiveClient : public rfb::CConnection {
public:
  LiveClient(const std::shared_ptr<SessionState>& state, int generation)
    : state(state), generation(generation), width(0), height(0) {
    {
      // Copy the credentials once so the session thread never re-reads them
      // while closeSession() is wiping the stored copy.
      std::lock_guard<std::mutex> lock(state->meta);
      secret = state->password;
      expectedPin = state->expectedPin;
    }
    auto types = rfb::parseSecTypes(state->securityTypes.c_str());
    security.SetSecTypes(types);
    setShared(true);
    // Raw encoding ships 9 MB per full frame and saturates the link; Tight
    // compresses it and keeps the session usable on a phone-sized link.
    setPreferredEncoding(rfb::encodingTight);
    setQualityLevel(6);
    setCompressLevel(6);
    // Server-drawn cursor for now: claiming local-cursor support without a
    // received shape made the pointer disappear entirely.
    supportsLocalCursor = true;
    supportsCursorPosition = true;
    // Without this the server treats a desktop resize as unsupported and
    // closes the connection ("Client does not support desktop size changes");
    // with it, resizeFramebuffer() below rebuilds the mirror instead.
    supportsDesktopResize = true;
  }
  ~LiveClient() override { wipeString(secret); }

  void getUserPasswd(bool, std::string* user, std::string* pass) override {
    if (pass == nullptr || secret.empty())
      throw std::runtime_error("credential unavailable");
    if (user != nullptr) *user = "";
    *pass = secret;
  }
  bool showMsgBox(rfb::MsgBoxFlags, const char* title, const char* text) override {
    const std::string pin = pinFromPrompt(title, text);
    if (expectedPin.empty())
      throw std::runtime_error("server not trusted yet: " + pin);
    if (pin != expectedPin) throw std::runtime_error("fingerprint mismatch: got " + pin);
    {
      std::lock_guard<std::mutex> lock(state->meta);
      state->fingerprint = pin;
    }
    return true;
  }
  void authSuccess() override { rfb::CConnection::authSuccess(); }
  // Like every regular viewer, the cursor shape and position are handled here
  // so moving the mouse never turns into a framebuffer update.
  void setCursor(int w, int h, const rfb::Point& hotspot, const uint8_t* data) override {
    if (!isCurrent()) return;
    std::lock_guard<std::mutex> lock(state->pix);
    // A 0x0 cursor is TigerVNC's emptyCursor: "this time I drew the cursor into
    // the framebuffer myself". Recording it (instead of ignoring it) is what
    // lets the UI hide its own cursor layer without ever losing the pointer.
    if (w <= 0 || h <= 0 || data == nullptr) {
      state->cursorBlank = true;
      state->cursorW = 0;
      state->cursorH = 0;
      state->cursorVersion++;
      return;
    }
    if (w > 1024 || h > 1024) return; // keep one bad server from ballooning memory
    state->cursor.assign(data, data + static_cast<size_t>(w) * h * 4);
    state->cursorBlank = false;
    state->cursorW = w;
    state->cursorH = h;
    state->cursorHotX = hotspot.x;
    state->cursorHotY = hotspot.y;
    state->cursorVersion++;
  }
  void setCursorPos(const rfb::Point& pos) override {
    if (!isCurrent()) return;
    std::lock_guard<std::mutex> lock(state->pix);
    state->cursorX = pos.x;
    state->cursorY = pos.y;
  }
  void setColourMapEntries(int, int, uint16_t*) override {
    throw std::runtime_error("indexed colour unsupported");
  }
  void bell() override {}
  void initDone() override {
    const int w = server.width();
    const int h = server.height();
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096)
      throw std::runtime_error("unexpected screen size");
    width = w;
    height = h;
    setFramebuffer(new rfb::ManagedPixelBuffer(server.pf(), w, h));
    {
      // A framebuffer mirror is kept in BGRA so takeFrame() can hand out a full
      // frame at any time without touching the decoder's pixel buffer.
      std::lock_guard<std::mutex> lock(state->pix);
      state->fbWidth = w;
      state->fbHeight = h;
      state->full.assign(static_cast<size_t>(w) * h * 4, 0);
      for (size_t i = 3; i < state->full.size(); i += 4) state->full[i] = 255;
      state->region.clear();
      state->updAny = false;
      state->pendAny = false;
      state->rectX = 0;
      state->rectY = 0;
      state->rectW = 0;
      state->rectH = 0;
      state->regionFull = false;
      state->mirrorComplete = false;
      state->wantFull = true;
      state->pixEpoch++;
      // Until the new connection produces pixels the mirror belongs to the
      // previous generation, but it is not handed out (see takeFrame()).
      state->frameGeneration = generation;
      state->takenVersion = state->version; // old pixels are gone
      state->takenEpoch = state->pixEpoch;
    }
    setPhase(*state, PhaseConnected, "", "none", 0);
  }
  // Called by TigerVNC when the server changes the framebuffer geometry
  // (SetDesktopSize / ExtendedDesktopSize). The base implementation is
  // assert(false), so leaving it unimplemented aborts a debug build on a
  // resize and silently keeps the old size in a release build.
  void resizeFramebuffer() override {
    const int w = server.width();
    const int h = server.height();
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096)
      throw std::runtime_error("unexpected screen size");
    // The server also announces the current size (reasonServer) on every
    // non-incremental update request; same geometry is not a resize and must
    // not throw away the mirror that is about to receive that full update.
    if (w == width && h == height && getFramebuffer() != nullptr) return;
    width = w;
    height = h;
    setFramebuffer(new rfb::ManagedPixelBuffer(server.pf(), w, h));
    {
      std::lock_guard<std::mutex> lock(state->pix);
      state->fbWidth = w;
      state->fbHeight = h;
      state->full.assign(static_cast<size_t>(w) * h * 4, 0);
      for (size_t i = 3; i < state->full.size(); i += 4) state->full[i] = 255;
      state->region.clear();
      // Both dirty layers and the consumer baseline belong to the old
      // geometry; nothing from them may be positioned on the new mirror.
      state->updAny = false;
      state->pendAny = false;
      state->rectX = 0;
      state->rectY = 0;
      state->rectW = 0;
      state->rectH = 0;
      state->regionFull = false;
      state->mirrorComplete = false;
      state->wantFull = true;
      state->version++;
      state->frameGeneration = generation;
      state->pixEpoch++;
      state->takenVersion = state->version;
      state->takenEpoch = state->pixEpoch;
    }
    // An idle desktop will not volunteer the new geometry as pixels: ask for
    // one non-incremental update once the session loop gets back to its poll.
    state->needFullRequest = true;
  }
  bool dataRect(const rfb::Rect& rect, int encoding) override {
    const bool result = rfb::CConnection::dataRect(rect, encoding);
    if (result && rect.area() > 0 && isCurrent()) {
      std::lock_guard<std::mutex> lock(state->pix);
      // Layer 1 only: this is decoded but not yet committed to the mirror.
      // It becomes part of the delivery layer in framebufferUpdateEnd().
      if (!state->updAny) {
        state->updX0 = rect.tl.x;
        state->updY0 = rect.tl.y;
        state->updX1 = rect.br.x;
        state->updY1 = rect.br.y;
        state->updAny = true;
      } else {
        state->updX0 = std::min(state->updX0, rect.tl.x);
        state->updY0 = std::min(state->updY0, rect.tl.y);
        state->updX1 = std::max(state->updX1, rect.br.x);
        state->updY1 = std::max(state->updY1, rect.br.y);
      }
    }
    return result;
  }
  void framebufferUpdateEnd() override {
    CConnection::framebufferUpdateEnd(); // Flush real DecodeManager workers.
    if (!isCurrent() || width <= 0 || height <= 0) {
      // A superseded connection's in-flight rects must not leak into the next
      // update of the current connection.
      std::lock_guard<std::mutex> lock(state->pix);
      state->updAny = false;
      return;
    }
    rfb::ModifiablePixelBuffer* fb = getFramebuffer();
    if (fb == nullptr) {
      std::lock_guard<std::mutex> lock(state->pix);
      state->updAny = false;
      return;
    }

    bool full = false;
    bool haveUpdate = false;
    int ux0 = 0, uy0 = 0, ux1 = 0, uy1 = 0;
    {
      std::lock_guard<std::mutex> lock(state->pix);
      full = state->wantFull;
      haveUpdate = state->updAny;
      ux0 = state->updX0;
      uy0 = state->updY0;
      ux1 = state->updX1;
      uy1 = state->updY1;
      // Layer 1 ends with this update: its rects are committed to the mirror
      // below (same lock section as the version bump) and merged into the
      // delivery layer, or dropped if this update carries no pixels.
      state->updAny = false;
      if (!haveUpdate) {
        // An update can carry only pseudo-rects (for example the DesktopSize
        // the server sends with every non-incremental request). Keep a pending
        // full-frame request for the update that actually has pixels, so a
        // blank just-resized framebuffer is never published as complete.
        return;
      }
      if (full) state->wantFull = false;
    }
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    if (full) {
      x1 = width;
      y1 = height;
    } else {
      x0 = std::max(0, ux0);
      y0 = std::max(0, uy0);
      x1 = std::min(width, ux1);
      y1 = std::min(height, uy1);
      if (x1 <= x0 || y1 <= y0) return;
    }

    const rfb::Rect target(x0, y0, x1, y1);
    int stride = 0;
    const uint8_t* base = fb->getBuffer(target, &stride);
    if (base == nullptr) {
      std::lock_guard<std::mutex> lock(state->pix);
      state->wantFull = true;
      return;
    }
    const rfb::PixelFormat pf = fb->getPF();
    const size_t rowBytes = static_cast<size_t>(stride) * (pf.bpp / 8);
    const int rw = target.width();
    const int rh = target.height();
    // Convert outside the pixel lock so a status/takeFrame caller is never
    // blocked by the RGB conversion of a large rectangle.
    std::vector<uint8_t> row(static_cast<size_t>(rw) * 3);
    std::vector<uint8_t> converted(static_cast<size_t>(rw) * rh * 4);
    for (int y = 0; y < rh; ++y) {
      pf.rgbFromBuffer(row.data(), base + static_cast<size_t>(y) * rowBytes, rw);
      uint8_t* dst = &converted[static_cast<size_t>(y) * rw * 4];
      for (int x = 0; x < rw; ++x) {
        // HarmonyOS writePixels takes BGRA_8888.
        dst[x * 4 + 0] = row[static_cast<size_t>(x) * 3 + 2];
        dst[x * 4 + 1] = row[static_cast<size_t>(x) * 3 + 1];
        dst[x * 4 + 2] = row[static_cast<size_t>(x) * 3 + 0];
        dst[x * 4 + 3] = 255;
      }
    }
    {
      std::lock_guard<std::mutex> lock(state->pix);
      // Commit-time check: the conversion ran without the lock, so the UI may
      // have started a reconnect (generation bumped) in the meantime. An old
      // connection must not publish pixels or a version into the new mirror.
      if (!isCurrent()) return;
      if (state->fbWidth != width || state->fbHeight != height) return;
      for (int y = 0; y < rh; ++y) {
        uint8_t* dstRow = &state->full[(static_cast<size_t>(y0 + y) * state->fbWidth + x0) * 4];
        std::memcpy(dstRow, &converted[static_cast<size_t>(y) * rw * 4],
                    static_cast<size_t>(rw) * 4);
      }
      state->region.swap(converted); // legacy takeRegion()/copyRegion()
      state->rectX = x0;
      state->rectY = y0;
      state->rectW = rw;
      state->rectH = rh;
      state->regionFull = full;
      if (full) {
        state->mirrorComplete = true;
        // A whole-screen update covers everything that had accumulated.
        state->pendAny = true;
        state->pendX0 = 0;
        state->pendY0 = 0;
        state->pendX1 = width;
        state->pendY1 = height;
      } else if (!state->pendAny) {
        state->pendX0 = x0;
        state->pendY0 = y0;
        state->pendX1 = x1;
        state->pendY1 = y1;
        state->pendAny = true;
      } else {
        state->pendX0 = std::min(state->pendX0, x0);
        state->pendY0 = std::min(state->pendY0, y0);
        state->pendX1 = std::max(state->pendX1, x1);
        state->pendY1 = std::max(state->pendY1, y1);
      }
      state->version++;
      state->frameGeneration = generation;
    }
  }

  // Classic RFB path: ServerCutText carries the text inline.
  void serverCutText(const char* str) override {
    if (str == nullptr) return;
    storeClipboard(str, std::strlen(str));
  }
  // Extended clipboard path (TigerVNC >= 1.10 negotiates it because
  // CConnection always advertises pseudoEncodingExtendedClipboard): the server
  // only announces a change and the data must be requested explicitly.
  void handleClipboardAnnounce(bool available) override {
    if (!available || !isCurrent()) return;
    requestClipboard();
    if (getOutStream() != nullptr && getOutStream()->length() > 0) getOutStream()->flush();
  }
  void handleClipboardData(const char* data) override {
    if (data == nullptr) return;
    storeClipboard(data, std::strlen(data));
  }

private:
  // A frame from an older connection (reconnect happened, generation moved on)
  // must never reach the buffers of the current one.
  bool isCurrent() const {
    return !state->stop && state->generation.load() == generation;
  }
  // Drop a UTF-8 sequence that the 1 MiB cut split in half.
  static void trimIncompleteUtf8(std::string& text) {
    size_t start = text.size();
    while (start > 0 && (static_cast<unsigned char>(text[start - 1]) & 0xc0) == 0x80)
      --start;
    if (start == 0) return;
    const unsigned char lead = static_cast<unsigned char>(text[start - 1]);
    size_t needed = 0;
    if ((lead & 0x80) == 0) return;
    if ((lead & 0xe0) == 0xc0) needed = 1;
    else if ((lead & 0xf0) == 0xe0) needed = 2;
    else if ((lead & 0xf8) == 0xf0) needed = 3;
    else {
      text.resize(start - 1);
      return;
    }
    if (text.size() - start < needed) text.resize(start - 1);
  }
  void storeClipboard(const char* data, size_t length) {
    if (!isCurrent()) return;
    bool truncated = false;
    if (length > kClipboardLimit) {
      length = kClipboardLimit;
      truncated = true;
    }
    std::string normalized;
    normalized.reserve(length);
    for (size_t i = 0; i < length; ++i) {
      const char c = data[i];
      if (c == '\0') continue;
      // Contract: "\r\n" and "\n" both become "\n".
      if (c == '\r' && i + 1 < length && data[i + 1] == '\n') continue;
      normalized.push_back(c);
    }
    if (truncated) trimIncompleteUtf8(normalized);
    std::lock_guard<std::mutex> lock(state->clip);
    state->clipText.swap(normalized);
    state->clipVersion++;
    state->clipTruncated = truncated;
  }

  std::shared_ptr<SessionState> state;
  int generation;
  int width;
  int height;
  std::string secret;
  std::string expectedPin;
};

// Bounded, stop-aware TCP connect: a blocking connect() to an unreachable host
// can take minutes, which would make closeSession() wait just as long.
int connectForSession(SessionState& state, bool& stopped) {
  stopped = false;
  initNetworkOnce();
  struct addrinfo hints;
  std::memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  const std::string service = std::to_string(state.port);
  struct addrinfo* addresses = nullptr;
  // getaddrinfo() itself cannot be interrupted; this is the one remaining
  // blocking call in the connect path and normally returns immediately.
  const int resolved = getaddrinfo(state.host.c_str(), service.c_str(), &hints, &addresses);
  if (resolved != 0 || addresses == nullptr)
    throw std::runtime_error("cannot resolve host " + state.host);
  AddrInfoGuard guard{addresses};
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
  int lastError = 0;
  for (struct addrinfo* address = addresses; address != nullptr; address = address->ai_next) {
    const int fd = ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
    if (fd < 0) { lastError = errno; continue; }
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
      lastError = errno;
      ::close(fd);
      continue;
    }
    const int result = ::connect(fd, address->ai_addr, address->ai_addrlen);
    if (result == 0) {
      fcntl(fd, F_SETFL, flags);
      return fd;
    }
    if (errno != EINPROGRESS) {
      lastError = errno;
      ::close(fd);
      continue;
    }
    bool usable = false;
    while (true) {
      if (state.stop) {
        ::close(fd);
        stopped = true;
        return -1;
      }
      const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
          deadline - std::chrono::steady_clock::now()).count();
      if (remaining <= 0) {
        lastError = ETIMEDOUT;
        ::close(fd);
        break;
      }
      struct pollfd pending;
      pending.fd = fd;
      pending.events = POLLOUT;
      pending.revents = 0;
      const int wait = remaining > 100 ? 100 : static_cast<int>(remaining);
      int ready;
      do { ready = ::poll(&pending, 1, wait); } while (ready < 0 && errno == EINTR);
      if (ready < 0) {
        lastError = errno;
        ::close(fd);
        break;
      }
      if (ready == 0) continue; // checked stop again
      int socketError = 0;
      socklen_t socketErrorLength = sizeof(socketError);
      if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &socketError, &socketErrorLength) != 0)
        socketError = errno;
      if (socketError != 0) {
        lastError = socketError;
        ::close(fd);
        break;
      }
      fcntl(fd, F_SETFL, flags);
      usable = true;
      break;
    }
    if (usable) return fd;
  }
  if (stopped) return -1;
  throw std::runtime_error(std::string("TCP connect failed: ") +
                           (lastError ? std::strerror(lastError) : "no usable address"));
}

bool sleepUnlessStopped(SessionState& state, int milliseconds) {
  const auto until = std::chrono::steady_clock::now() +
                     std::chrono::milliseconds(milliseconds);
  while (std::chrono::steady_clock::now() < until) {
    // A stop or a manual reconnect must interrupt the retry backoff.
    if (state.stop) return false;
    if (state.forceReconnect.exchange(false)) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return true;
}

void waitForReconnect(SessionState& state) {
  while (!state.stop) {
    if (state.forceReconnect.exchange(false)) return;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
}

// Returns the generation this attempt will run under. Manual reconnectSession()
// already bumped it; only the automatic retry path bumps it here, so exactly one
// increment happens per new connection attempt.
int beginAttempt(SessionState& state, int& lastApplied) {
  std::lock_guard<std::mutex> lock(state.meta);
  if (state.generation.load() == lastApplied) state.generation++;
  const int generation = state.generation.load();
  lastApplied = generation;
  state.retryInMs = 0;
  return generation;
}

// Same policy as keepQueuedInputOnRelease(), named for the connection case:
// clipboard text was already reported to the caller as delivered and is not
// tied to a socket, so it may outlive a connection. Pointer and key input is
// tied to the connection (or to buttons the user already released) and must not
// leak into the next one.
inline bool keepQueuedInputOnReconnect(int kind) {
  return tianguan::keepQueuedInputOnRelease(kind);
}

// Caller holds state.input. Drops the oldest queued clipboard items beyond the
// bound so a socket that keeps failing cannot accumulate one text per attempt.
// Each dropped text is a real loss of text the UI believes was delivered, so it
// is logged instead of vanishing silently.
void trimQueuedClipboardItemsLocked(SessionState& state) {
  size_t count = 0;
  for (std::deque<QueuedInput>::const_iterator it = state.inputs.begin();
       it != state.inputs.end(); ++it) {
    if (keepQueuedInputOnReconnect(static_cast<int>(it->kind))) ++count;
  }
  for (std::deque<QueuedInput>::iterator it = state.inputs.begin();
       it != state.inputs.end() && count > kMaxQueuedClipboards;) {
    if (keepQueuedInputOnReconnect(static_cast<int>(it->kind))) {
      std::fprintf(stderr,
                   "tianguan: session %s dropped oldest queued clipboard item "
                   "(%zu > %zu)\n",
                   state.id.c_str(), count, kMaxQueuedClipboards);
      std::fflush(stderr);
      it = state.inputs.erase(it);
      --count;
    } else {
      ++it;
    }
  }
}

// A new connection must start with no keys held, but clipboard text is not
// tied to the old socket: it stays queued until the new connection reaches
// RFBSTATE_NORMAL. Pointer/key items from the old connection are dropped.
void clearQueuedInputForReconnect(SessionState& state) {
  std::lock_guard<std::mutex> lock(state.input);
  std::deque<QueuedInput> kept;
  for (std::deque<QueuedInput>::const_iterator it = state.inputs.begin();
       it != state.inputs.end(); ++it) {
    if (keepQueuedInputOnReconnect(static_cast<int>(it->kind)))
      kept.push_back(*it);
  }
  state.inputs.swap(kept);
  trimQueuedClipboardItemsLocked(state);
}

// Put the clipboard items swapped out by drainSessionInput() back at the front
// of the queue (relative order preserved) so the next connection sends them.
// Pointer/key items are intentionally not requeued: sending them on a
// connection that is not writable yet would be meaningless.
void requeueClipboardInput(SessionState& state, std::deque<QueuedInput>& pending) {
  std::deque<QueuedInput> kept;
  for (std::deque<QueuedInput>::const_iterator it = pending.begin();
       it != pending.end(); ++it) {
    if (keepQueuedInputOnReconnect(static_cast<int>(it->kind)))
      kept.push_back(*it);
  }
  if (kept.empty()) return;
  std::lock_guard<std::mutex> lock(state.input);
  for (std::deque<QueuedInput>::reverse_iterator it = kept.rbegin();
       it != kept.rend(); ++it)
    state.inputs.push_front(*it);
  trimQueuedClipboardItemsLocked(state);
}

void drainSessionInput(SessionState& state, LiveClient& client) {
  std::deque<QueuedInput> pending;
  const unsigned targetRelease = state.releaseEpoch.load();
  {
    std::lock_guard<std::mutex> lock(state.input);
    if (state.inputs.empty() && targetRelease == state.appliedRelease) return;
    pending.swap(state.inputs);
  }
  // The session pointer member hides CConnection::state(), so qualify the call.
  if (static_cast<rfb::CConnection&>(client).state() != rfb::CConnection::RFBSTATE_NORMAL ||
      client.writer() == nullptr || client.getOutStream() == nullptr) {
    // Not writable yet (handshake or reconnect). Pointer/key input would be
    // meaningless now, but clipboard text was already reported to the caller as
    // delivered: keep it queued (bounded) so the next writable connection sends
    // it instead of losing it forever.
    requeueClipboardInput(state, pending);
    return;
  }
  for (std::deque<QueuedInput>::const_iterator it = pending.begin(); it != pending.end(); ++it) {
    if (it->kind == QueuedInput::PointerKind) {
      client.writer()->writePointerEvent(rfb::Point(it->a, it->b),
                                         static_cast<uint16_t>(it->c));
      state.pointerMask = it->c;
      state.lastPointerX = it->a;
      state.lastPointerY = it->b;
    } else if (it->kind == QueuedInput::KeyKind) {
      if (it->b != 0) {
        client.sendKeyPress(0, 0, static_cast<uint32_t>(it->a));
        state.pressedKeys.insert(it->a);
      } else {
        client.sendKeyRelease(it->a);
        state.pressedKeys.erase(it->a);
      }
    } else {
      // Announce before providing: an extended-clipboard server only accepts
      // data from the client that currently owns the clipboard, and that
      // ownership is established by the clipboard notify. announceClipboard()
      // also falls back cleanly to the classic ClientCutText path.
      client.announceClipboard(true);
      client.sendClipboardData(it->text.c_str());
    }
  }
  if (targetRelease != state.appliedRelease) {
    // releaseSessionInput(): release exactly what this session pressed.
    for (std::set<int>::const_iterator it = state.pressedKeys.begin();
         it != state.pressedKeys.end(); ++it)
      client.sendKeyRelease(*it);
    state.pressedKeys.clear();
    if (state.pointerMask != 0)
      client.writer()->writePointerEvent(rfb::Point(state.lastPointerX, state.lastPointerY), 0);
    state.pointerMask = 0;
    state.appliedRelease = targetRelease;
  }
  client.getOutStream()->flush();
}

void sessionMain(std::shared_ptr<SessionState> state) {
  int lastApplied = 0;
  int backoffMs = 1000;
  while (!state->stop) {
    const int generation = beginAttempt(*state, lastApplied);
    setPhase(*state, PhaseConnecting, "", "none", 0);
    try {
      bool stopped = false;
      const int fd = connectForSession(*state, stopped);
      if (stopped) break;
      network::TcpSocket socket(fd);
      struct timeval timeout;
      timeout.tv_sec = 10;
      timeout.tv_usec = 0;
      setsockopt(socket.getFd(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
      setsockopt(socket.getFd(), SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
      state->sock.attach(fd);
      bool manualReconnect = false;
      {
        SocketSlotGuard slotGuard(state->sock);
        LiveClient client(state, generation);
        client.setServerName(state->host.c_str());
        client.setStreams(&socket.inStream(), &socket.outStream());
        client.initialiseProtocol();
        backoffMs = 1000;
        {
          // Drop pointer/key input queued for the previous connection and start
          // with no keys held: a press from the old socket must not leak into
          // this one. Clipboard text is connection-independent and was already
          // reported to the UI as delivered, so it stays queued for this
          // connection (see drainSessionInput()).
          clearQueuedInputForReconnect(*state);
        }
        state->appliedRelease = state->releaseEpoch.load();
        state->pressedKeys.clear();
        state->pointerMask = 0;
        while (!state->stop) {
          if (state->forceReconnect.exchange(false)) {
            // reconnectSession() already bumped the generation; reconnect now
            // instead of sleeping through the retry backoff.
            manualReconnect = true;
            break;
          }
          const bool more = client.processMsg();
          // Flush protocol replies produced inside processMsg (for example the
          // extended-clipboard capability answer) without waiting for input.
          if (client.getOutStream() != nullptr && client.getOutStream()->length() > 0)
            client.getOutStream()->flush();
          drainSessionInput(*state, client);
          // A fresh view needs a non-incremental request; otherwise an idle
          // desktop never sends a full frame and the view stays mostly black.
          if (state->needFullRequest.exchange(false)) {
            int w = 0, h = 0;
            {
              std::lock_guard<std::mutex> lock(state->pix);
              w = state->fbWidth;
              h = state->fbHeight;
            }
            // Qualified for the same reason as in drainSessionInput().
            if (static_cast<rfb::CConnection&>(client).state() ==
                    rfb::CConnection::RFBSTATE_NORMAL &&
                client.writer() != nullptr && w > 0 && h > 0) {
              client.writer()->writeFramebufferUpdateRequest(rfb::Rect(0, 0, w, h), false);
              client.getOutStream()->flush();
              std::lock_guard<std::mutex> lock(state->pix);
              state->wantFull = true;
            }
          }
          if (!more) {
            struct pollfd pending;
            pending.fd = socket.getFd();
            pending.events = POLLIN;
            pending.revents = 0;
            // Short timeout: queued input must reach the server promptly even
            // when the desktop is idle and sends nothing.
            if (::poll(&pending, 1, 5) < 0 && errno != EINTR)
              throw std::runtime_error("socket poll failed");
          }
        }
        {
          std::lock_guard<std::mutex> lock(state->meta);
          state->connected = false;
          state->running = false;
        }
        client.close();
      }
      state->sock.detach();
      if (state->stop) break;
      if (manualReconnect) continue;
      setPhase(*state, PhaseReconnecting, "connection closed", "network", backoffMs);
      if (!sleepUnlessStopped(*state, backoffMs)) {
        if (state->stop) break;
        continue; // manual reconnect: retry immediately
      }
      backoffMs = std::min(backoffMs * 2, 4000);
    } catch (const rfb::auth_error& error) {
      // Wrong credentials will not fix themselves; wait for reconnectSession().
      setPhase(*state, PhaseFailed, error.what(), "auth-failed", 0);
      waitForReconnect(*state);
    } catch (const std::exception& error) {
      const std::string message = sanitizeLine(error.what());
      const std::string trustPrefix = "server not trusted yet: ";
      if (message.compare(0, trustPrefix.size(), trustPrefix) == 0) {
        {
          std::lock_guard<std::mutex> lock(state->meta);
          state->phase = PhaseTrustRequired;
          state->error = message;
          state->errorCode = "fingerprint-mismatch";
          state->fingerprint = message.substr(trustPrefix.size());
          state->retryInMs = 0;
          state->connected = false;
          state->running = false;
        }
        // Contract: the UI confirms the fingerprint, saves it, and calls
        // reconnectSession(); retrying before that would only spin.
        waitForReconnect(*state);
        continue;
      }
      if (message.compare(0, 19, "fingerprint mismatch") == 0) {
        setPhase(*state, PhaseFailed, message, "fingerprint-mismatch", 0);
        waitForReconnect(*state);
        continue;
      }
      const bool protocolError = dynamic_cast<const rfb::protocol_error*>(&error) != nullptr ||
                                 message.find("unexpected screen size") != std::string::npos ||
                                 message.find("indexed colour") != std::string::npos;
      if (protocolError) {
        setPhase(*state, PhaseFailed, message, "protocol", 0);
        waitForReconnect(*state);
        continue;
      }
      setPhase(*state, PhaseReconnecting, message, "network", backoffMs);
      if (!sleepUnlessStopped(*state, backoffMs)) {
        if (state->stop) break;
        continue;
      }
      backoffMs = std::min(backoffMs * 2, 4000);
    } catch (...) {
      setPhase(*state, PhaseReconnecting, "unknown session error", "network", backoffMs);
      if (!sleepUnlessStopped(*state, backoffMs) && state->stop) break;
      backoffMs = std::min(backoffMs * 2, 4000);
    }
  }
  {
    std::lock_guard<std::mutex> lock(state->meta);
    state->connected = false;
    state->running = false;
    state->retryInMs = 0;
    if (state->phase != PhaseFailed && state->phase != PhaseTrustRequired)
      state->phase = PhaseDisconnected;
  }
}

// The registry lock only guards map lookups/inserts/removes. Every caller takes
// a shared_ptr copy and releases the lock before doing anything else, so no
// network IO, authentication or join can run while it is held.
struct SessionRegistry {
  SessionRegistry() : nextSeq(0) {}
  std::mutex mutex;
  std::map<std::string, std::shared_ptr<SessionState> > sessions;
  std::string defaultId;
  unsigned nextSeq;

  std::shared_ptr<SessionState> find(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex);
    std::map<std::string, std::shared_ptr<SessionState> >::iterator it = sessions.find(id);
    if (it == sessions.end()) return std::shared_ptr<SessionState>();
    return it->second;
  }
  std::shared_ptr<SessionState> findDefault() {
    std::lock_guard<std::mutex> lock(mutex);
    if (defaultId.empty()) return std::shared_ptr<SessionState>();
    std::map<std::string, std::shared_ptr<SessionState> >::iterator it = sessions.find(defaultId);
    if (it == sessions.end()) return std::shared_ptr<SessionState>();
    return it->second;
  }
  std::string insert(const std::shared_ptr<SessionState>& state, bool makeDefault) {
    std::lock_guard<std::mutex> lock(mutex);
    if (makeDefault && !defaultId.empty()) return std::string();
    std::string id;
    do {
      id = makeSessionId(++nextSeq);
    } while (sessions.find(id) != sessions.end());
    state->id = id;
    sessions[id] = state;
    if (makeDefault) defaultId = id;
    return id;
  }
  std::shared_ptr<SessionState> take(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex);
    std::map<std::string, std::shared_ptr<SessionState> >::iterator it = sessions.find(id);
    if (it == sessions.end()) return std::shared_ptr<SessionState>();
    std::shared_ptr<SessionState> state = it->second;
    sessions.erase(it);
    if (defaultId == id) defaultId.clear();
    return state;
  }
  std::vector<std::pair<std::string, std::shared_ptr<SessionState> > > snapshot() {
    std::lock_guard<std::mutex> lock(mutex);
    std::vector<std::pair<std::string, std::shared_ptr<SessionState> > > out;
    out.reserve(sessions.size());
    for (std::map<std::string, std::shared_ptr<SessionState> >::const_iterator it =
             sessions.begin(); it != sessions.end(); ++it)
      out.push_back(*it);
    return out;
  }

private:
  static unsigned randomSuffix() {
    static std::mutex lock;
    static std::mt19937 generator(seed());
    std::lock_guard<std::mutex> guard(lock);
    return generator() & 0xffffu;
  }
  static unsigned seed() {
    unsigned value = 0;
    try {
      std::random_device device;
      value = device();
    } catch (...) {
      value = 0;
    }
    if (value == 0) {
      value = static_cast<unsigned>(
          std::chrono::steady_clock::now().time_since_epoch().count());
      value ^= static_cast<unsigned>(reinterpret_cast<uintptr_t>(&value));
    }
    return value;
  }
  static std::string makeSessionId(unsigned sequence) {
    char suffix[8];
    std::snprintf(suffix, sizeof(suffix), "%04x", randomSuffix());
    return "s" + std::to_string(sequence) + "-" + suffix;
  }
};
SessionRegistry g_sessions;

struct SessionOptions {
  SessionOptions() : port(0) {}
  std::string host;
  int port;
  std::string password;
  std::string expectedPin;
  std::string name;
  std::string securityTypes = "RA2_256,RA2";
};

bool parseSessionOptions(const std::string& json, SessionOptions& options,
                         std::string& error) {
  try {
    options.host = jsonValue(json, "host");
    const std::string portText = jsonValue(json, "port");
    size_t used = 0;
    int port = 0;
    try {
      port = std::stoi(portText, &used);
    } catch (...) {
      error = "invalid port";
      return false;
    }
    if (used != portText.size() || port < 1 || port > 65535) {
      error = "invalid port";
      return false;
    }
    options.port = port;
    if (json.find("\"password\"") != std::string::npos)
      options.password = jsonValue(json, "password");
    if (json.find("\"expectedFingerprint\"") != std::string::npos)
      options.expectedPin = jsonValue(json, "expectedFingerprint");
    if (json.find("\"name\"") != std::string::npos)
      options.name = jsonValue(json, "name");
    if (json.find("\"security\"") != std::string::npos) {
      const std::string requested = jsonValue(json, "security");
      if (!requested.empty()) options.securityTypes = requested;
    }
  } catch (const std::exception& failure) {
    error = sanitizeLine(failure.what());
    return false;
  }
  if (options.host.empty() || options.host.size() > 253) {
    error = "invalid host";
    return false;
  }
  if (options.password.size() > 512) {
    error = "password too long";
    return false;
  }
  if (options.password.find('\0') != std::string::npos) {
    error = "password contains NUL";
    return false;
  }
  if (options.expectedPin.size() > 256) {
    error = "fingerprint too long";
    return false;
  }
  if (options.name.size() > 128) {
    error = "name too long";
    return false;
  }
  if (options.securityTypes.size() > 256) {
    error = "security types too long";
    return false;
  }
  return true;
}

std::shared_ptr<SessionState> launchSession(const SessionOptions& options,
                                            bool makeDefault,
                                            std::string& error) {
  std::shared_ptr<SessionState> state(new SessionState);
  state->host = options.host;
  state->port = options.port;
  state->password = options.password;
  state->expectedPin = options.expectedPin;
  state->profileName = options.name.empty() ? "session" : options.name;
  state->securityTypes = options.securityTypes;
  // Start the thread before publishing the session so closeSession() can never
  // find a session whose thread is still not joinable.
  try {
    state->thread = std::thread(sessionMain, state);
  } catch (...) {
    error = "cannot start session thread";
    wipeString(state->password);
    return std::shared_ptr<SessionState>();
  }
  const std::string id = g_sessions.insert(state, makeDefault);
  if (id.empty()) {
    // The legacy default slot was taken between the check and the insert.
    state->stop = true;
    state->sock.interrupt();
    if (state->thread.joinable()) state->thread.join();
    wipeString(state->password);
    error = "already running";
    return std::shared_ptr<SessionState>();
  }
  return state;
}

std::string statusJson(const std::shared_ptr<SessionState>& state) {
  int phase = PhaseFailed;
  int retryInMs = 0;
  std::string error, errorCode, fingerprint;
  {
    std::lock_guard<std::mutex> lock(state->meta);
    phase = state->phase.load();
    retryInMs = state->retryInMs.load();
    error = state->error;
    errorCode = state->errorCode;
    fingerprint = state->fingerprint;
  }
  int width = 0, height = 0, version = 0;
  int cursorW = 0, cursorH = 0, cursorHotX = 0, cursorHotY = 0, cursorVersion = 0;
  int cursorX = 0, cursorY = 0;
  bool cursorBlank = true;
  {
    std::lock_guard<std::mutex> lock(state->pix);
    width = state->fbWidth;
    height = state->fbHeight;
    version = state->version;
    cursorW = state->cursorW;
    cursorH = state->cursorH;
    cursorHotX = state->cursorHotX;
    cursorHotY = state->cursorHotY;
    cursorVersion = state->cursorVersion;
    cursorBlank = state->cursorBlank;
    cursorX = state->cursorX;
    cursorY = state->cursorY;
  }
  int clipVersion = 0;
  bool clipTruncated = false;
  {
    std::lock_guard<std::mutex> lock(state->clip);
    clipVersion = state->clipVersion;
    clipTruncated = state->clipTruncated;
  }
  const int generation = state->generation.load();
  std::string out = "{\"id\":\"" + jsonEscape(state->id) + "\",\"state\":\"" +
                    phaseName(phase) + "\",\"error\":\"" + jsonEscape(error) +
                    "\",\"errorCode\":\"" + jsonEscape(errorCode) + "\"";
  out += ",\"width\":" + std::to_string(width) + ",\"height\":" + std::to_string(height);
  out += ",\"generation\":" + std::to_string(generation) +
         ",\"version\":" + std::to_string(version);
  out += ",\"cursor\":{\"w\":" + std::to_string(cursorW) +
         ",\"h\":" + std::to_string(cursorH) +
         ",\"hotX\":" + std::to_string(cursorHotX) +
         ",\"hotY\":" + std::to_string(cursorHotY) +
         ",\"blank\":" + (cursorBlank ? "true" : "false") +
         ",\"version\":" + std::to_string(cursorVersion) +
         ",\"x\":" + std::to_string(cursorX) +
         ",\"y\":" + std::to_string(cursorY) + "}";
  out += ",\"clipboard\":{\"version\":" + std::to_string(clipVersion);
  if (clipTruncated) out += ",\"truncated\":true";
  out += "},\"retryInMs\":" + std::to_string(retryInMs);
  out += ",\"fingerprint\":\"" + jsonEscape(fingerprint) + "\"";
  out += ",\"pixelBytes\":" +
         std::to_string(static_cast<long long>(width) * height * 4) + "}";
  return out;
}

std::string legacyStatusText() {
  std::shared_ptr<SessionState> state = g_sessions.findDefault();
  if (!state) {
    return "running=0 version=0 size=0x0 rect=0,0,0,0 cursor=0x0,0,0,0 blank=1 cpos=0,0";
  }
  bool running = state->running.load();
  int width = 0, height = 0, version = 0;
  int rectX = 0, rectY = 0, rectW = 0, rectH = 0;
  int cursorW = 0, cursorH = 0, cursorHotX = 0, cursorHotY = 0, cursorVersion = 0;
  int cursorX = 0, cursorY = 0;
  bool cursorBlank = true;
  {
    std::lock_guard<std::mutex> lock(state->pix);
    width = state->fbWidth;
    height = state->fbHeight;
    version = state->version;
    rectX = state->rectX;
    rectY = state->rectY;
    rectW = state->rectW;
    rectH = state->rectH;
    cursorW = state->cursorW;
    cursorH = state->cursorH;
    cursorHotX = state->cursorHotX;
    cursorHotY = state->cursorHotY;
    cursorVersion = state->cursorVersion;
    cursorBlank = state->cursorBlank;
    cursorX = state->cursorX;
    cursorY = state->cursorY;
  }
  std::string error;
  {
    std::lock_guard<std::mutex> lock(state->meta);
    error = state->error;
  }
  std::string text = "running=" + std::string(running ? "1" : "0");
  text += " version=" + std::to_string(version);
  text += " size=" + std::to_string(width) + "x" + std::to_string(height);
  text += " rect=" + std::to_string(rectX) + "," + std::to_string(rectY) + "," +
          std::to_string(rectW) + "," + std::to_string(rectH);
  text += " cursor=" + std::to_string(cursorW) + "x" + std::to_string(cursorH) + "," +
          std::to_string(cursorHotX) + "," + std::to_string(cursorHotY) + "," +
          std::to_string(cursorVersion);
  text += std::string(" blank=") + (cursorBlank ? "1" : "0");
  text += " cpos=" + std::to_string(cursorX) + "," + std::to_string(cursorY);
  if (!error.empty()) text += " error=" + error;
  return text;
}

void trimInputQueue(SessionState& state) {
  // Only motion may be dropped: losing a press or a release would leave the
  // remote mouse/keyboard stuck. A newer motion event always follows.
  if (state.inputs.size() <= kMaxQueuedInputs) return;
  for (std::deque<QueuedInput>::iterator it = state.inputs.begin();
       it != state.inputs.end(); ++it) {
    if (it->kind == QueuedInput::PointerKind && it->c == 0) {
      state.inputs.erase(it);
      return;
    }
  }
}

// closeSession() hands the thread join to this detached reaper so the NAPI
// call (and therefore the UI event thread) never waits for the session thread.
// The counter is the only state shared with a caller; polling it never blocks.
// A process that exits while a reaper is still blocked in join() is safe: the
// reaper holds only its own SessionState and this process-lifetime atomic (no
// registry lock, no static destructor), so process teardown just ends the
// thread.
std::atomic<unsigned> g_pendingCloses(0);

void reapClosedSession(std::shared_ptr<SessionState> state) {
  // getaddrinfo() cannot be cancelled, so this may take as long as the
  // resolver does. It runs on the reaper thread, never on the caller.
  if (state->thread.joinable()) state->thread.join();
  state->sock.detach();
  {
    std::lock_guard<std::mutex> lock(state->meta);
    state->connected = false;
    state->running = false;
    if (state->phase != PhaseFailed && state->phase != PhaseTrustRequired)
      state->phase = PhaseDisconnected;
  }
  // Only after the join can no thread read the stored password any more.
  wipeString(state->password);
  g_pendingCloses.fetch_sub(1, std::memory_order_acq_rel);
}

} // namespace

// ---------------------------------------------------------------------------
// Multi-session API
// ---------------------------------------------------------------------------
std::string tianguan::createSession(const std::string& optionsJson) {
  SessionOptions options;
  std::string error;
  if (!parseSessionOptions(optionsJson, options, error))
    return "ERROR:" + (error.empty() ? std::string("invalid options") : error);
  WipeGuard passwordGuard{options.password};
  const std::shared_ptr<SessionState> state = launchSession(options, false, error);
  if (!state) return "ERROR:" + (error.empty() ? std::string("session failed") : error);
  return state->id;
}

std::string tianguan::closeSession(const std::string& sessionId) {
  // Remove first: no new API call can reach this session afterwards. The
  // thread join is not done here: a detached reaper owns it, so this call
  // returns even when the session thread is stuck in the uncancellable
  // getaddrinfo() (F7).
  const std::shared_ptr<SessionState> state = g_sessions.take(sessionId);
  if (!state) return "INVALID_SESSION";
  state->stop = true;
  // Wake a thread blocked in read(); the connect loop checks stop between
  // short polls, and the reaper waits out anything that cannot be interrupted.
  state->sock.interrupt();
  {
    std::lock_guard<std::mutex> lock(state->input);
    state->inputs.clear();
  }
  g_pendingCloses.fetch_add(1, std::memory_order_acq_rel);
  try {
    std::thread(&reapClosedSession, state).detach();
  } catch (...) {
    // Creating the reaper thread can only fail when the process is out of
    // resources. Reap on this thread as a last resort: the close still
    // completes and keeps the lifetime rules intact, at the cost of blocking
    // in that emergency.
    reapClosedSession(state);
  }
  return "CLOSED";
}

size_t tianguan::pendingCloses() {
  return static_cast<size_t>(g_pendingCloses.load(std::memory_order_acquire));
}

std::string tianguan::listSessions() {
  const std::vector<std::pair<std::string, std::shared_ptr<SessionState> > > snapshot =
      g_sessions.snapshot();
  std::string out = "[";
  for (size_t i = 0; i < snapshot.size(); ++i) {
    if (i != 0) out += ",";
    const std::shared_ptr<SessionState>& state = snapshot[i].second;
    int phase = PhaseFailed, width = 0, height = 0;
    {
      std::lock_guard<std::mutex> lock(state->meta);
      phase = state->phase.load();
    }
    {
      std::lock_guard<std::mutex> lock(state->pix);
      width = state->fbWidth;
      height = state->fbHeight;
    }
    out += "{\"id\":\"" + jsonEscape(snapshot[i].first) + "\",\"state\":\"" +
           phaseName(phase) + "\",\"width\":" + std::to_string(width) +
           ",\"height\":" + std::to_string(height) +
           ",\"generation\":" + std::to_string(state->generation.load()) +
           ",\"profileName\":\"" + jsonEscape(state->profileName) +
           "\",\"host\":\"" + jsonEscape(state->host) +
           "\",\"port\":" + std::to_string(state->port) + "}";
  }
  return out + "]";
}

std::string tianguan::sessionStatus(const std::string& sessionId) {
  const std::shared_ptr<SessionState> state = g_sessions.find(sessionId);
  if (!state) {
    return "{\"id\":\"" + jsonEscape(sessionId) +
           "\",\"state\":\"failed\",\"error\":\"unknown session\","
           "\"errorCode\":\"none\",\"width\":0,\"height\":0,\"generation\":0,"
           "\"version\":0,\"cursor\":{\"w\":0,\"h\":0,\"hotX\":0,\"hotY\":0,"
           "\"blank\":true,\"version\":0},\"clipboard\":{\"version\":0},"
           "\"retryInMs\":0,\"fingerprint\":\"\",\"pixelBytes\":0}";
  }
  return statusJson(state);
}

std::string tianguan::reconnectSession(const std::string& sessionId) {
  const std::shared_ptr<SessionState> state = g_sessions.find(sessionId);
  if (!state) return "INVALID_SESSION";
  {
    std::lock_guard<std::mutex> lock(state->meta);
    // Contract flow: the UI showed the fingerprint reported by trust-required,
    // the user confirmed it and the UI saved it to its trust store. The only
    // trust anchor the one-argument reconnectSession() can carry is this
    // explicit reconnect, so pin the fingerprint we already observed.
    if (state->phase.load() == PhaseTrustRequired && !state->fingerprint.empty()) {
      state->expectedPin = state->fingerprint;
      state->error.clear();
      state->errorCode = "none";
    }
    // One connection attempt per generation: bumping it here makes the next
    // attempt use a fresh generation, and beginAttempt() will not bump again.
    state->generation++;
    state->retryInMs = 0;
  }
  state->forceReconnect = true;
  return "OK";
}

std::string tianguan::takeFrame(const std::string& sessionId, void* dst,
                                size_t capacity, int afterVersion, bool forceFull) {
  const std::shared_ptr<SessionState> state = g_sessions.find(sessionId);
  if (!state) return "INVALID_SESSION";
  std::lock_guard<std::mutex> lock(state->pix);
  const int width = state->fbWidth;
  const int height = state->fbHeight;
  const int current = state->version;
  if (width <= 0 || height <= 0 || current <= 0) return "NONE";
  // NONE means "the caller already has the newest version".
  if (!forceFull && afterVersion == current) return "NONE";
  // A mirror produced by a superseded connection is never handed out: the
  // session thread may still be tearing the old connection down.
  if (state->frameGeneration != state->generation.load()) return "NONE";
  // The only safe partial handoff is from a known baseline: afterVersion must
  // be the version this session actually delivered last, on the current mirror
  // geometry. Anything else (missed handoff, reconnect, resize, first call)
  // needs the whole mirror.
  const bool baselineKnown =
      afterVersion == state->takenVersion && state->pixEpoch == state->takenEpoch;
  bool full = forceFull || !baselineKnown;
  if (!full && current - afterVersion > kFrameVersionGapFull) full = true;
  if (full) {
    // Without at least one complete frame applied, the mirror can still have
    // unfilled areas; a full copy would show black. Ask the caller to retry.
    if (!state->mirrorComplete) return "NONE";
    const size_t need = static_cast<size_t>(width) * height * 4;
    if (state->full.size() < need) return "NONE";
    if (dst == nullptr || capacity < need) return "ERROR:BUFFER_TOO_SMALL";
    std::memcpy(dst, state->full.data(), need);
    state->takenVersion = current;
    state->takenEpoch = state->pixEpoch;
    state->pendAny = false;
    return "{\"x\":0,\"y\":0,\"w\":" + std::to_string(width) +
           ",\"h\":" + std::to_string(height) +
           ",\"version\":" + std::to_string(current) +
           ",\"generation\":" + std::to_string(state->frameGeneration) +
           ",\"full\":true}";
  }
  // Partial handoff: the delivery layer is the union of every committed change
  // since the last successful handoff, so it covers afterVersion+1..current.
  if (!state->pendAny) return "NONE";
  int x = std::max(0, state->pendX0);
  int y = std::max(0, state->pendY0);
  int x1 = std::min(width, state->pendX1);
  int y1 = std::min(height, state->pendY1);
  if (x1 <= x || y1 <= y) return "NONE";
  const int w = x1 - x;
  const int h = y1 - y;
  const size_t need = static_cast<size_t>(w) * h * 4;
  if (dst == nullptr || capacity < need) return "ERROR:BUFFER_TOO_SMALL";
  // Copy the union straight out of the full BGRA mirror (row by row, because
  // the mirror stride is the whole screen).
  uint8_t* out = static_cast<uint8_t*>(dst);
  for (int row = 0; row < h; ++row) {
    std::memcpy(out + static_cast<size_t>(row) * w * 4,
                state->full.data() +
                    (static_cast<size_t>(y + row) * width + x) * 4,
                static_cast<size_t>(w) * 4);
  }
  // Only a successful copy consumes the delivery layer and moves the baseline.
  state->takenVersion = current;
  state->takenEpoch = state->pixEpoch;
  state->pendAny = false;
  const bool fullAnswer = (x == 0 && y == 0 && w == width && h == height);
  return "{\"x\":" + std::to_string(x) + ",\"y\":" + std::to_string(y) +
         ",\"w\":" + std::to_string(w) + ",\"h\":" + std::to_string(h) +
         ",\"version\":" + std::to_string(current) +
         ",\"generation\":" + std::to_string(state->frameGeneration) +
         ",\"full\":" + (fullAnswer ? "true" : "false") + "}";
}

size_t tianguan::copyCursorFor(const std::string& sessionId, void* dst, size_t capacity) {
  const std::shared_ptr<SessionState> state = g_sessions.find(sessionId);
  if (!state) return 0;
  std::lock_guard<std::mutex> lock(state->pix);
  const size_t n = std::min(capacity, state->cursor.size());
  if (n > 0 && dst != nullptr) std::memcpy(dst, state->cursor.data(), n);
  return n;
}

std::string tianguan::sendPointer(const std::string& sessionId, int x, int y, int mask) {
  const std::shared_ptr<SessionState> state = g_sessions.find(sessionId);
  if (!state) return "INVALID_SESSION";
  {
    std::lock_guard<std::mutex> lock(state->meta);
    if (!state->connected) return "NOT_CONNECTED";
  }
  std::lock_guard<std::mutex> lock(state->input);
  // Coalesce motion like noVNC does: only the newest pointer position matters,
  // and queuing every move made the remote pointer lag behind the real one.
  if (mask == 0 && !state->inputs.empty() &&
      state->inputs.back().kind == QueuedInput::PointerKind &&
      state->inputs.back().c == 0) {
    state->inputs.back().a = x;
    state->inputs.back().b = y;
    return "COALESCED";
  }
  QueuedInput item;
  item.kind = QueuedInput::PointerKind;
  item.a = x;
  item.b = y;
  item.c = mask;
  state->inputs.push_back(item);
  trimInputQueue(*state);
  return "QUEUED";
}

std::string tianguan::sendKey(const std::string& sessionId, int keysym, int pressed) {
  if (keysym <= 0 || keysym > 0x10ffff) return "FAIL key: invalid keysym";
  const std::shared_ptr<SessionState> state = g_sessions.find(sessionId);
  if (!state) return "INVALID_SESSION";
  {
    std::lock_guard<std::mutex> lock(state->meta);
    if (!state->connected) return "NOT_CONNECTED";
  }
  std::lock_guard<std::mutex> lock(state->input);
  QueuedInput item;
  item.kind = QueuedInput::KeyKind;
  item.a = keysym;
  item.b = pressed;
  item.c = 0;
  state->inputs.push_back(item);
  return "QUEUED";
}

std::string tianguan::releaseSessionInput(const std::string& sessionId) {
  const std::shared_ptr<SessionState> state = g_sessions.find(sessionId);
  if (!state) return "INVALID_SESSION";
  // Drop queued pointer/key input and flag the release. Queued clipboard text
  // is kept: it is independent of releasing buttons and was already reported to
  // the caller as delivered, so clearing it would lose that text with no retry.
  // The session thread knows which keys it actually pressed, so it can
  // synthesize the matching releases.
  std::lock_guard<std::mutex> lock(state->input);
  for (std::deque<QueuedInput>::iterator it = state->inputs.begin();
       it != state->inputs.end();) {
    if (tianguan::keepQueuedInputOnRelease(static_cast<int>(it->kind))) {
      ++it;
    } else {
      it = state->inputs.erase(it);
    }
  }
  state->releaseEpoch++;
  return "OK";
}

std::string tianguan::setClipboard(const std::string& sessionId, const std::string& text) {
  if (text.size() > kClipboardLimit) return "ERROR:TOO_LARGE";
  const std::shared_ptr<SessionState> state = g_sessions.find(sessionId);
  if (!state) return "INVALID_SESSION";
  {
    std::lock_guard<std::mutex> lock(state->meta);
    if (!state->connected) return "NOT_CONNECTED";
  }
  std::string filtered;
  filtered.reserve(text.size());
  for (size_t i = 0; i < text.size(); ++i)
    if (text[i] != '\0') filtered.push_back(text[i]);
  QueuedInput item;
  item.kind = QueuedInput::ClipboardKind;
  item.a = 0;
  item.b = 0;
  item.c = 0;
  item.text = filtered;
  std::lock_guard<std::mutex> lock(state->input);
  state->inputs.push_back(item);
  // Keep at most kMaxQueuedClipboards texts: a newer copy supersedes an older
  // one that has not been sent yet, and this also bounds what drainSessionInput
  // can put back while the connection is not writable.
  trimQueuedClipboardItemsLocked(*state);
  return "QUEUED";
}

std::string tianguan::takeClipboard(const std::string& sessionId) {
  const std::shared_ptr<SessionState> state = g_sessions.find(sessionId);
  if (!state) return "";
  std::lock_guard<std::mutex> lock(state->clip);
  std::string text;
  text.swap(state->clipText);
  state->clipTruncated = false;
  return text;
}

// ---------------------------------------------------------------------------
// Legacy single-session wrappers: they all resolve the default session so the
// existing ArkTS pages keep compiling and working until they move to the
// explicit-session API.
// ---------------------------------------------------------------------------
std::string tianguan::requestFullFrame(const std::string& sessionId) {
  const std::shared_ptr<SessionState> state = g_sessions.find(sessionId);
  if (!state) return "INVALID_SESSION";
  {
    std::lock_guard<std::mutex> lock(state->pix);
    state->wantFull = true;
  }
  state->needFullRequest = true;
  return "OK";
}

std::string tianguan::startSession(const std::string& dataDir) {
  if (g_sessions.findDefault()) return "ALREADY_RUNNING";
  std::string dir = dataDir;
  while (!dir.empty() && dir.back() == '/') dir.pop_back();
  std::string json;
  try {
    json = readSmallFile(dir + "/vnc-test.json", 4096, false);
  } catch (...) {
    return "FAIL session: no vnc-test.json";
  }
  SessionOptions options;
  try {
    options.host = jsonValue(json, "host");
    const std::string portText = jsonValue(json, "port");
    size_t used = 0;
    options.port = std::stoi(portText, &used);
    if (used != portText.size() || options.port < 1 || options.port > 65535)
      throw std::runtime_error("invalid port in vnc-test.json");
    if (json.find("\"expectedFingerprint\"") != std::string::npos)
      options.expectedPin = jsonValue(json, "expectedFingerprint");
    if (json.find("\"security\"") != std::string::npos) {
      const std::string requested = jsonValue(json, "security");
      if (!requested.empty()) options.securityTypes = requested;
    }
  } catch (const std::exception& error) {
    return std::string("FAIL session: ") + error.what();
  }
  try {
    options.password = readPasswordFile(dir + "/vnc-test.pass");
  } catch (...) {
    return "FAIL session: no vnc-test.pass";
  }
  WipeGuard passwordGuard{options.password};
  options.name = "default";
  std::string error;
  const std::shared_ptr<SessionState> state = launchSession(options, true, error);
  if (!state) {
    if (error == "already running") return "ALREADY_RUNNING";
    return std::string("FAIL session: ") + error;
  }
  return "STARTED";
}

std::string tianguan::stopSession() {
  const std::shared_ptr<SessionState> state = g_sessions.findDefault();
  if (state) closeSession(state->id);
  return "STOPPED";
}

std::string tianguan::sessionStatus() {
  return legacyStatusText();
}

std::string tianguan::sendPointer(int x, int y, int mask, bool) {
  const std::shared_ptr<SessionState> state = g_sessions.findDefault();
  if (!state) return "NOT_CONNECTED";
  return sendPointer(state->id, x, y, mask);
}

std::string tianguan::sendKey(int keysym, int pressed, bool) {
  const std::shared_ptr<SessionState> state = g_sessions.findDefault();
  if (!state) return "NOT_CONNECTED";
  return sendKey(state->id, keysym, pressed);
}

std::string tianguan::requestFullFrame() {
  const std::shared_ptr<SessionState> state = g_sessions.findDefault();
  if (state) requestFullFrame(state->id);
  return "OK";
}

size_t tianguan::copyRegion(void* dst, size_t capacity) {
  const std::shared_ptr<SessionState> state = g_sessions.findDefault();
  if (!state) return 0;
  std::lock_guard<std::mutex> lock(state->pix);
  const size_t n = std::min(capacity, state->region.size());
  if (n > 0 && dst != nullptr) std::memcpy(dst, state->region.data(), n);
  return n;
}

size_t tianguan::copyCursor(void* dst, size_t capacity) {
  const std::shared_ptr<SessionState> state = g_sessions.findDefault();
  if (!state) return 0;
  std::lock_guard<std::mutex> lock(state->pix);
  const size_t n = std::min(capacity, state->cursor.size());
  if (n > 0 && dst != nullptr) std::memcpy(dst, state->cursor.data(), n);
  return n;
}

std::string tianguan::takeRegion(void* dst, size_t capacity) {
  const std::shared_ptr<SessionState> state = g_sessions.findDefault();
  if (!state) return "NONE";
  std::lock_guard<std::mutex> lock(state->pix);
  const int current = state->version;
  if (current == state->takenVersion) return "NONE";
  const size_t need = state->region.size();
  if (need == 0 || dst == nullptr || need > capacity) return "NONE";
  std::memcpy(dst, state->region.data(), need);
  state->takenVersion = current;
  state->takenEpoch = state->pixEpoch;
  // Legacy path: takeFrame() owns the delivery union; do not consume it here.
  return std::to_string(state->rectX) + "," + std::to_string(state->rectY) + "," +
         std::to_string(state->rectW) + "," + std::to_string(state->rectH);
}
