// SPDX-License-Identifier: GPL-3.0-or-later
#include <rfb/CConnection.h>
#include <rfb/CMsgWriter.h>
#include <rfb/PixelBuffer.h>
#include <rfb/CSecurityRSAAES.h>
#include <network/TcpSocket.h>
#include <rdr/FdInStream.h>
#include <rdr/FdOutStream.h>
#include <rdr/Exception.h>
#include <chrono>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <poll.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

class Client : public rfb::CConnection {
  bool ra2Test;
  bool identityConfirmed = false;
  bool authenticated = false;
  bool centreDecoded = false;
  std::string credentialFile;
  std::string fingerprint;
public:
  bool done = false;
  explicit Client(bool fixture, bool real = false,
                  const std::string& path = "", const std::string& pin = "")
    : ra2Test(real), credentialFile(path), fingerprint(pin) {
    auto types = rfb::parseSecTypes(fixture ? "None" : (real ? "RA2_256" : "RA2_256,RA2"));
    security.SetSecTypes(types);
    setShared(true);
    setPreferredEncoding(0);
  }
  void getUserPasswd(bool secure, std::string* user, std::string* password) override {
    if (ra2Test)
      std::cerr << "RA2_256 credential callback\n";
    if (!ra2Test || !secure || user != nullptr || password == nullptr || !identityConfirmed)
      throw std::runtime_error("Credential input disabled in this experiment");
    // Open only after the independently supplied truncated fingerprint matched.
    int fd = open(credentialFile.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) throw std::runtime_error("Credential file rejected");
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || (st.st_mode & 07777) != 0600 ||
        st.st_size <= 0 || st.st_size > 4096) {
      ::close(fd);
      throw std::runtime_error("Credential file rejected");
    }
    std::string value(static_cast<size_t>(st.st_size), '\0');
    size_t offset = 0;
    while (offset < value.size()) {
      ssize_t count = read(fd, &value[offset], value.size() - offset);
      if (count <= 0) {
        ::close(fd);
        throw std::runtime_error("Credential file rejected");
      }
      offset += static_cast<size_t>(count);
    }
    ::close(fd);
    if (!value.empty() && value.back() == '\n') value.pop_back();
    if (value.empty() || value.find_first_of("\r\n") != std::string::npos ||
        value.find('\0') != std::string::npos)
      throw std::runtime_error("Credential file rejected");
    *password = std::move(value);
  }
  bool showMsgBox(rfb::MsgBoxFlags flags, const char* title, const char* text) override {
    identityConfirmed = false;
    if (!ra2Test)
      throw std::runtime_error("Identity confirmation requires a future UI");
    const std::string expected =
      "The server has provided the following identifying information:\n"
      "Fingerprint: " + fingerprint + "\n"
      "Please verify that the information is correct and press \"Yes\". "
      "Otherwise press \"No\"";
    if (flags != rfb::MsgBoxFlags::M_YESNO || !title || !text ||
        std::strcmp(title, "Server key fingerprint") || expected != text) {
      std::cerr << "identity rejected\n";
      return false;
    }
    identityConfirmed = true;
    return true;
  }
  void authSuccess() override {
    rfb::CConnection::authSuccess();
    if (ra2Test) {
      if (!identityConfirmed) throw std::runtime_error("Identity not confirmed");
      authenticated = true;
      std::cout << "RA2_256 authentication complete\n" << std::flush;
    }
  }
  void setCursor(int, int, const rfb::Point&, const uint8_t*) override {}
  void setCursorPos(const rfb::Point&) override {}
  void setColourMapEntries(int, int, uint16_t*) override {
    throw std::runtime_error("Indexed colour is outside this fixture");
  }
  void bell() override {}
  void initDone() override {
    if (ra2Test) {
      if (!authenticated || server.width() != 64 || server.height() != 48 || !server.pf().trueColour)
        throw std::runtime_error("Unexpected RA2 test framebuffer");
      rfb::PixelFormat pf(32, 24, false, true, 255, 255, 255, 16, 8, 0);
      setPF(pf);
      setFramebuffer(new rfb::ManagedPixelBuffer(pf, 64, 48));
      return;
    }
    if (server.width() != 2 || server.height() != 1)
      throw std::runtime_error("Unexpected fixture dimensions");
    setFramebuffer(new rfb::ManagedPixelBuffer(server.pf(), server.width(), server.height()));
  }
  bool dataRect(const rfb::Rect& rect, int encoding) override {
    bool decoded = rfb::CConnection::dataRect(rect, encoding);
    if (ra2Test && decoded && rect.tl.x <= 32 && rect.tl.y <= 24 &&
        rect.br.x > 32 && rect.br.y > 24)
      centreDecoded = true;
    return decoded;
  }
  void framebufferUpdateEnd() override {
    rfb::CConnection::framebufferUpdateEnd();
    if (ra2Test) {
      if (!authenticated || !centreDecoded) return;
      int stride;
      const auto* p = getFramebuffer()->getBuffer(rfb::Rect(32, 24, 33, 25), &stride);
      uint8_t rgb[3];
      getFramebuffer()->getPF().rgbFromBuffer(rgb, p, 1);
      if (rgb[0] != 0x11 || rgb[1] != 0x22 || rgb[2] != 0x33)
        throw std::runtime_error("RA2 test framebuffer mismatch");
      sendKeyPress(1, 0, 0x61);
      sendKeyRelease(1);
      writer()->writePointerEvent(rfb::Point(16, 12), 1);
      writer()->writePointerEvent(rfb::Point(16, 12), 0);
      getOutStream()->flush();
      done = true;
      std::cout << "RA2_256 first frame RGB=112233; key+pointer sent\n";
      return;
    }
    int stride;
    const auto* p = getFramebuffer()->getBuffer(rfb::Rect(0, 0, 2, 1), &stride);
    const unsigned char expected[] = {0x33, 0x22, 0x11, 0, 0x66, 0x55, 0x44, 0};
    if (std::memcmp(p, expected, sizeof(expected)))
      throw std::runtime_error("Decoded TigerVNC pixel buffer mismatch");
    sendKeyPress(1, 0, 0x61);
    sendKeyRelease(1);
    writer()->writePointerEvent(rfb::Point(1, 0), 1);
    getOutStream()->flush();
    done = true;
    std::cout << "RFB 3.8 ServerInit; Raw pixels=112233,445566; key+pointer sent\n";
  }
};
int main(int argc, char** argv) {
  signal(SIGPIPE, SIG_IGN);
  const bool ra2Test = argc > 1 && std::string(argv[1]) == "--ra2-test";
  if (ra2Test) signal(SIGALRM, [](int) { _exit(1); });
  alarm(ra2Test ? 20 : 4); // Includes socket construction and every upstream blocking operation.
  try {
    if (argc == 1) {
      std::cout << "Use ctest for isolated loopback tests; --capabilities for RA2_256 factory check\n";
      return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--capabilities") {
      Client c(false);
      std::unique_ptr<rfb::CSecurity> s(c.security.GetCSecurity(&c, rfb::secTypeRA256));
      if (!dynamic_cast<rfb::CSecurityRSAAES*>(s.get()) || s->getType() != rfb::secTypeRA256)
        throw std::runtime_error("RA2_256 factory failed");
      std::cout << "RA2_256: real CSecurityRSAAES created; authentication NOT tested; X509 disabled\n";
      return 0;
    }
    bool fixture = argc == 3 && std::string(argv[1]) == "--fixture-loopback-none";
    bool strict = argc == 3 && std::string(argv[1]) == "--fixture-loopback-strict";
    if (!fixture && !strict && !ra2Test) throw std::runtime_error("Only explicit loopback fixture modes are supported");
    std::string path, pin;
    if (ra2Test) {
      if (argc != 7 || std::string(argv[3]) != "--credential-file" ||
          std::string(argv[5]) != "--server-fingerprint")
        throw std::runtime_error("Invalid RA2 test arguments");
      path = argv[4];
      pin = argv[6];
      // Upstream displays only the first eight SHA-1 bytes, not a full public-key pin.
      if (path.empty() || pin.size() != 23) throw std::runtime_error("Invalid RA2 test arguments");
      for (size_t i = 0; i < pin.size(); ++i) {
        if (i % 3 == 2 ? pin[i] != '-' :
            !((pin[i] >= '0' && pin[i] <= '9') || (pin[i] >= 'a' && pin[i] <= 'f')))
          throw std::runtime_error("Invalid RA2 test arguments");
      }
    }
    size_t used;
    int port = std::stoi(argv[2], &used);
    if (used != std::strlen(argv[2]) || port < 1024 || port > 65535 || (ra2Test && port == 5901))
      throw std::runtime_error("Invalid fixture port");
    network::TcpSocket socket("127.0.0.1", port);
    Client c(fixture, ra2Test, path, pin);
    c.setServerName(ra2Test ? "isolated-ra2-test" : "isolated-loopback-fixture");
    c.setStreams(&socket.inStream(), &socket.outStream());
    c.initialiseProtocol();
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(ra2Test ? 15 : 2);
    while (!c.done) {
      if (std::chrono::steady_clock::now() > deadline) throw std::runtime_error("Protocol deadline exceeded");
      if (!c.processMsg()) {
        pollfd fd{socket.getFd(), POLLIN, 0};
        if (poll(&fd, 1, 20) < 0) throw std::runtime_error("poll failed");
      }
    }
    c.close();
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "error: " << (ra2Test ? "RA2 test failed" : e.what()) << '\n';

  }
  return 1;
}
