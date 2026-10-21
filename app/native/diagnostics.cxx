// SPDX-License-Identifier: GPL-3.0-or-later
// Uses TigerVNC v1.15.0 (GPL-2.0-or-later), without copying its implementation:
// https://github.com/TigerVNC/tigervnc/tree/v1.15.0
// Keep upstream copyright/license files and the build-time OHOS Thread patch
// with corresponding source when distributing the linked binary.
#include "diagnostics.h"
#include <rfb/CConnection.h>
#include <rfb/CMsgWriter.h>
#include <rfb/CSecurityRSAAES.h>
#include <rfb/PixelBuffer.h>
#include <network/TcpSocket.h>
#include <rdr/MemInStream.h>
#include <rdr/MemOutStream.h>
#include <os/Thread.h>
#include <os/Mutex.h>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <memory>
#include <netdb.h>
#include <poll.h>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <vector>
#include <jpeglib.h>
#include <atomic>
#include <condition_variable>
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
    size_t end = json.find('"', at + 1);
    if (end == std::string::npos) throw std::runtime_error("bad " + key);
    return json.substr(at + 1, end - at - 1);
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
// Live session: one persistent connection, frames rewritten to snapshot.jpg,
// and pointer events forwarded from the UI.
// ---------------------------------------------------------------------------
namespace {
// Input is queued instead of written from the UI thread: the session thread can
// hold the io mutex for a whole blocking read, and dropping events (or blocking
// the UI) breaks clicks because the release then never reaches the server.
struct QueuedInput {
  int kind;  // 0 = pointer (a=x, b=y, c=mask); 1 = key (a=keysym, b=pressed)
  int a;
  int b;
  int c;
};
struct SessionState {
  std::mutex io;
  std::mutex meta;
  std::mutex input;
  std::deque<QueuedInput> inputs;
  // Pixels for the view are handed over as raw RGBA, like a normal VNC viewer:
  // the framebuffer stays authoritative and the UI applies changed rectangles.
  std::mutex pix;
  std::vector<uint8_t> region;
  std::vector<uint8_t> cursor;
  std::atomic<bool> running{false};
  std::atomic<bool> stop{false};
  std::atomic<int> version{0};
  std::atomic<int> width{0};
  std::atomic<int> height{0};
  std::atomic<int> rectX{0};
  std::atomic<int> rectY{0};
  std::atomic<int> rectW{0};
  std::atomic<int> rectH{0};
  std::atomic<int> cursorW{0};
  std::atomic<int> cursorH{0};
  std::atomic<int> cursorHotX{0};
  std::atomic<int> cursorHotY{0};
  std::atomic<int> cursorVersion{0};
  // Server asked us to hide the local cursor because it drew the pointer into
  // the framebuffer for this period.
  std::atomic<bool> cursorBlank{false};
  std::atomic<int> cursorX{0};
  std::atomic<int> cursorY{0};
  // Version already handed to the view; taking a region and its rectangle under
  // one lock prevents size mismatches when a new frame lands in between.
  std::atomic<int> takenVersion{0};
  // Changed area accumulated since the view last took a region. Batching here
  // avoids dropping intermediate updates, which left ghost cursors behind.
  bool pendAny = false;
  int pendX0 = 0;
  int pendY0 = 0;
  int pendX1 = 0;
  int pendY1 = 0;
  // Set when the view needs a complete frame again (first paint, resize).
  std::atomic<bool> wantFull{true};
  // A full frame only arrives if the client asks for a non-incremental update;
  // an idle desktop would otherwise never send one.
  std::atomic<bool> needFullRequest{false};
  std::string error;
  std::string dir;
  std::thread thread;
  rfb::CConnection* client = nullptr;
};
SessionState g_session;

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

class LiveClient : public rfb::CConnection {
public:
  LiveClient(const std::string& password, const std::string& expectedPin)
    : password(password), expectedPin(expectedPin) {
    auto types = rfb::parseSecTypes("RA2_256,RA2");
    security.SetSecTypes(types);
    setShared(true);
    // Raw encoding ships 9 MB per full frame and saturates the link, which is
    // what made the live session feel extremely laggy. Tight compresses it.
    setPreferredEncoding(rfb::encodingTight);
    setQualityLevel(6);
    setCompressLevel(6);
    // Server-drawn cursor for now: claiming local-cursor support without a
    // received shape made the pointer disappear entirely.
    supportsLocalCursor = true;
    supportsCursorPosition = true;
  }
  ~LiveClient() override { wipeString(password); }
  void getUserPasswd(bool, std::string* user, std::string* pass) override {
    if (pass == nullptr || password.empty())
      throw std::runtime_error("credential unavailable");
    if (user != nullptr) *user = "";
    *pass = password;
  }
  bool showMsgBox(rfb::MsgBoxFlags, const char* title, const char* text) override {
    const std::string pin = pinFromPrompt(title, text);
    if (expectedPin.empty())
      throw std::runtime_error("server not trusted yet: " + pin);
    if (pin != expectedPin) throw std::runtime_error("fingerprint mismatch");
    return true;
  }
  void authSuccess() override { rfb::CConnection::authSuccess(); }
  // Like every regular viewer, the cursor shape and position are handled here
  // so moving the mouse never turns into a framebuffer update.
  void setCursor(int w, int h, const rfb::Point& hotspot, const uint8_t* data) override {
    std::lock_guard<std::mutex> lock(g_session.pix);
    // A 0x0 cursor is TigerVNC's emptyCursor: "this time I drew the cursor into
    // the framebuffer myself". Recording it (instead of ignoring it) is what lets
    // the UI hide its own cursor layer without ever losing the pointer.
    if (w <= 0 || h <= 0 || data == nullptr) {
      g_session.cursorBlank = true;
      g_session.cursorW = 0;
      g_session.cursorH = 0;
      g_session.cursorVersion++;
      return;
    }
    g_session.cursor.assign(data, data + static_cast<size_t>(w) * h * 4);
    g_session.cursorBlank = false;
    g_session.cursorW = w;
    g_session.cursorH = h;
    g_session.cursorHotX = hotspot.x;
    g_session.cursorHotY = hotspot.y;
    g_session.cursorVersion++;
  }
  void setCursorPos(const rfb::Point& pos) override {
    g_session.cursorX = pos.x;
    g_session.cursorY = pos.y;
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
    g_session.width = w;
    g_session.height = h;
  }
  bool dataRect(const rfb::Rect& rect, int encoding) override {
    const bool result = rfb::CConnection::dataRect(rect, encoding);
    if (result && rect.area() > 0) {
      std::lock_guard<std::mutex> lock(g_session.pix);
      if (!g_session.pendAny) {
        g_session.pendX0 = rect.tl.x;
        g_session.pendY0 = rect.tl.y;
        g_session.pendX1 = rect.br.x;
        g_session.pendY1 = rect.br.y;
        g_session.pendAny = true;
      } else {
        g_session.pendX0 = std::min(g_session.pendX0, rect.tl.x);
        g_session.pendY0 = std::min(g_session.pendY0, rect.tl.y);
        g_session.pendX1 = std::max(g_session.pendX1, rect.br.x);
        g_session.pendY1 = std::max(g_session.pendY1, rect.br.y);
      }
    }
    return result;
  }
  void framebufferUpdateEnd() override {
    CConnection::framebufferUpdateEnd();
    if (width <= 0 || height <= 0) return;
    rfb::ModifiablePixelBuffer* fb = getFramebuffer();
    if (fb == nullptr) return;
    // Encode the union of everything changed since the view last grabbed a
    // region, not just the last update, so no intermediate change is lost.
    bool full = g_session.wantFull.exchange(false);
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    {
      std::lock_guard<std::mutex> lock(g_session.pix);
      if (!full && !g_session.pendAny) return;
      if (!full) {
        x0 = std::max(0, g_session.pendX0);
        y0 = std::max(0, g_session.pendY0);
        x1 = std::min(width, g_session.pendX1);
        y1 = std::min(height, g_session.pendY1);
        if (x1 <= x0 || y1 <= y0) return;
      }
    }
    const rfb::Rect target = full ? rfb::Rect(0, 0, width, height)
                                  : rfb::Rect(x0, y0, x1, y1);
    int stride = 0;
    const uint8_t* base = fb->getBuffer(target, &stride);
    if (base == nullptr) return;
    const rfb::PixelFormat pf = fb->getPF();
    const size_t rowBytes = static_cast<size_t>(stride) * (pf.bpp / 8);
    const int rw = target.width();
    const int rh = target.height();
    std::vector<uint8_t> row(static_cast<size_t>(rw) * 3);
    {
      std::lock_guard<std::mutex> lock(g_session.pix);
      g_session.region.resize(static_cast<size_t>(rw) * rh * 4);
      for (int y = 0; y < rh; ++y) {
        pf.rgbFromBuffer(row.data(), base + static_cast<size_t>(y) * rowBytes, rw);
        uint8_t* dst = &g_session.region[static_cast<size_t>(y) * rw * 4];
        for (int x = 0; x < rw; ++x) {
          // HarmonyOS writePixels takes BGRA_8888.
          dst[x * 4 + 0] = row[static_cast<size_t>(x) * 3 + 2];
          dst[x * 4 + 1] = row[static_cast<size_t>(x) * 3 + 1];
          dst[x * 4 + 2] = row[static_cast<size_t>(x) * 3 + 0];
          dst[x * 4 + 3] = 255;
        }
      }
      g_session.rectX = target.tl.x;
      g_session.rectY = target.tl.y;
      g_session.rectW = rw;
      g_session.rectH = rh;
      g_session.version++;
    }
  }
private:
  std::string password;
  std::string expectedPin;
  int width = 0;
  int height = 0;
};

void sessionMain(std::string host, int port, std::string pin, std::string password) {
  while (!g_session.stop) {
    try {
      network::TcpSocket socket(host.c_str(), port);
      struct timeval timeout;
      timeout.tv_sec = 10;
      timeout.tv_usec = 0;
      setsockopt(socket.getFd(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
      setsockopt(socket.getFd(), SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
      LiveClient client(password, pin);
      client.setServerName(host.c_str());
      client.setStreams(&socket.inStream(), &socket.outStream());
      {
        std::lock_guard<std::mutex> lock(g_session.io);
        std::lock_guard<std::mutex> meta(g_session.meta);
        g_session.client = &client;
        g_session.running = true;
        g_session.error.clear();
      }
      {
        // Drop input queued while the previous connection was gone.
        std::lock_guard<std::mutex> queue(g_session.input);
        g_session.inputs.clear();
      }
      client.initialiseProtocol();
      while (!g_session.stop) {
        bool more = false;
        {
          std::lock_guard<std::mutex> lock(g_session.io);
          more = client.processMsg();
        }
        // Drain queued user input now that the socket is ours again.
        std::deque<QueuedInput> pending;
        {
          std::lock_guard<std::mutex> queue(g_session.input);
          pending.swap(g_session.inputs);
        }
        if (!pending.empty()) {
          std::lock_guard<std::mutex> lock(g_session.io);
          const int state = static_cast<int>(client.state());
          const bool writable = client.writer() != nullptr && client.getOutStream() != nullptr;
          if (state == rfb::CConnection::RFBSTATE_NORMAL && writable) {
            for (const QueuedInput& item : pending) {
              if (item.kind == 0) {
                client.writer()->writePointerEvent(rfb::Point(item.a, item.b),
                                                  static_cast<uint16_t>(item.c));
              } else if (item.b != 0) {
                client.sendKeyPress(item.a, 0, static_cast<uint32_t>(item.a));
              } else {
                client.sendKeyRelease(item.a);
              }
            }
            client.getOutStream()->flush();
          }
          // Leave evidence on device: queued input is useless if the session is
          // not writable, and that is invisible from the ArkTS side.
          std::ofstream note(g_session.dir + "/input-drain.log", std::ios::app);
          if (note) {
            note << "drained=" << pending.size() << " state=" << state
                 << " writable=" << (writable ? 1 : 0) << " events=";
            for (const QueuedInput& item : pending)
              note << (item.kind == 0 ? "P" : "K") << item.a << "," << item.b
                   << "," << item.c << ";";
            note << "\n";
          }
        }
        // A fresh view needs a non-incremental request; otherwise an idle
        // desktop never sends a full frame and the view stays mostly black.
        if (g_session.needFullRequest.exchange(false)) {
          std::lock_guard<std::mutex> lock(g_session.io);
          if (client.state() == rfb::CConnection::RFBSTATE_NORMAL &&
              client.writer() != nullptr && client.getOutStream() != nullptr) {
            client.writer()->writeFramebufferUpdateRequest(
                rfb::Rect(0, 0, g_session.width.load(), g_session.height.load()), false);
            client.getOutStream()->flush();
          }
        }
        if (!more) {
          pollfd fd{socket.getFd(), POLLIN, 0};
          // Short timeout: queued input must reach the server promptly even when
          // the desktop is idle and sends nothing.
          if (poll(&fd, 1, 5) < 0 && errno != EINTR) break;
        }
      }
      {
        std::lock_guard<std::mutex> meta(g_session.meta);
        g_session.client = nullptr;
      }
      client.close();
    } catch (const std::exception& error) {
      {
        std::lock_guard<std::mutex> meta(g_session.meta);
        g_session.error = error.what();
        g_session.running = false;
        g_session.client = nullptr;
      }
      for (int i = 0; i < 10 && !g_session.stop; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    } catch (...) {
      std::lock_guard<std::mutex> meta(g_session.meta);
      g_session.error = "unknown session error";
      g_session.running = false;
      g_session.client = nullptr;
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
  }
  std::lock_guard<std::mutex> meta(g_session.meta);
  g_session.running = false;
  g_session.client = nullptr;
}
}

size_t tianguan::copyRegion(void* dst, size_t capacity) {
  std::lock_guard<std::mutex> lock(g_session.pix);
  const size_t n = std::min(capacity, g_session.region.size());
  if (n > 0 && dst != nullptr) std::memcpy(dst, g_session.region.data(), n);
  return n;
}

// Copies the pending region and its rectangle atomically; returns the rectangle
// as "x,y,w,h" or "NONE" when the view is already up to date.
std::string tianguan::takeRegion(void* dst, size_t capacity) {
  std::lock_guard<std::mutex> lock(g_session.pix);
  const int current = g_session.version.load();
  if (current == g_session.takenVersion.load()) return "NONE";
  const size_t need = g_session.region.size();
  if (need == 0 || dst == nullptr || need > capacity) return "NONE";
  std::memcpy(dst, g_session.region.data(), need);
  g_session.takenVersion = current;
  // Everything up to this point is now in the view's mirror; start a fresh
  // accumulation window.
  g_session.pendAny = false;
  return std::to_string(g_session.rectX.load()) + "," +
         std::to_string(g_session.rectY.load()) + "," +
         std::to_string(g_session.rectW.load()) + "," +
         std::to_string(g_session.rectH.load());
}

size_t tianguan::copyCursor(void* dst, size_t capacity) {
  std::lock_guard<std::mutex> lock(g_session.pix);
  const size_t n = std::min(capacity, g_session.cursor.size());
  if (n > 0 && dst != nullptr) std::memcpy(dst, g_session.cursor.data(), n);
  return n;
}

std::string tianguan::requestFullFrame() {
  g_session.wantFull = true;
  g_session.needFullRequest = true;
  return "OK";
}

std::string tianguan::startSession(const std::string& dataDir) {
  if (g_session.thread.joinable()) return "ALREADY_RUNNING";
  std::string dir = dataDir;
  while (!dir.empty() && dir.back() == '/') dir.pop_back();
  std::string json;
  try {
    json = readSmallFile(dir + "/vnc-test.json", 4096, false);
  } catch (...) {
    return "FAIL session: no vnc-test.json";
  }
  std::string host;
  int port = 0;
  std::string pin;
  try {
    host = jsonValue(json, "host");
    port = std::stoi(jsonValue(json, "port"));
    if (json.find("\"expectedFingerprint\"") != std::string::npos)
      pin = jsonValue(json, "expectedFingerprint");
  } catch (const std::exception& error) {
    return std::string("FAIL session: ") + error.what();
  }
  std::string password;
  try {
    password = readPasswordFile(dir + "/vnc-test.pass");
  } catch (...) {
    return "FAIL session: no vnc-test.pass";
  }
  {
    std::lock_guard<std::mutex> meta(g_session.meta);
    g_session.dir = dir;
    g_session.stop = false;
  }
  g_session.thread = std::thread(sessionMain, host, port, pin, password);
  return "STARTED";
}

std::string tianguan::stopSession() {
  g_session.stop = true;
  if (g_session.thread.joinable()) g_session.thread.join();
  return "STOPPED";
}

std::string tianguan::sessionStatus() {
  std::lock_guard<std::mutex> meta(g_session.meta);
  std::string text = "running=" + std::string(g_session.running ? "1" : "0");
  text += " version=" + std::to_string(g_session.version.load());
  text += " size=" + std::to_string(g_session.width.load()) + "x" +
          std::to_string(g_session.height.load());
  text += " rect=" + std::to_string(g_session.rectX.load()) + "," +
          std::to_string(g_session.rectY.load()) + "," +
          std::to_string(g_session.rectW.load()) + "," +
          std::to_string(g_session.rectH.load());
  text += " cursor=" + std::to_string(g_session.cursorW.load()) + "x" +
          std::to_string(g_session.cursorH.load()) + "," +
          std::to_string(g_session.cursorHotX.load()) + "," +
          std::to_string(g_session.cursorHotY.load()) + "," +
          std::to_string(g_session.cursorVersion.load());
  text += std::string(" blank=") + (g_session.cursorBlank.load() ? "1" : "0");
  text += " cpos=" + std::to_string(g_session.cursorX.load()) + "," +
          std::to_string(g_session.cursorY.load());
  if (!g_session.error.empty()) text += " error=" + g_session.error;
  return text;
}

std::string tianguan::sendPointer(int x, int y, int mask, bool flush) {
  // Queue only: the session thread owns the socket and may be inside a blocking
  // read, so writing here would either block the UI or drop the event.
  {
    std::lock_guard<std::mutex> meta(g_session.meta);
    if (g_session.client == nullptr && !g_session.running) return "NOT_CONNECTED";
  }
  std::lock_guard<std::mutex> queue(g_session.input);
  // Coalesce motion like noVNC does: only the newest pointer position matters,
  // and queuing every move made the remote pointer lag behind the real one.
  if (mask == 0 && !g_session.inputs.empty() && g_session.inputs.back().kind == 0 &&
      g_session.inputs.back().c == 0) {
    g_session.inputs.back().a = x;
    g_session.inputs.back().b = y;
    return "COALESCED";
  }
  g_session.inputs.push_back(QueuedInput{0, x, y, mask});
  return "QUEUED";
}

std::string tianguan::sendKey(int keysym, int pressed, bool flush) {
  if (keysym <= 0 || keysym > 0x10ffff) return "FAIL key: invalid keysym";
  {
    std::lock_guard<std::mutex> meta(g_session.meta);
    if (g_session.client == nullptr && !g_session.running) return "NOT_CONNECTED";
  }
  std::lock_guard<std::mutex> queue(g_session.input);
  g_session.inputs.push_back(QueuedInput{1, keysym, pressed, 0});
  return "QUEUED";
}
