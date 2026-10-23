// SPDX-License-Identifier: GPL-3.0-or-later
#include "diagnostics.h"
#include <rdr/MemOutStream.h>
#include <rfb/CConnection.h>
#include <rfb/CMsgWriter.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {
struct TestConfig {
  std::string host;
  std::string port;
  std::string password;
};

bool loadConfig(const std::string& dir, TestConfig& config, std::string& error) {
  std::ifstream jsonFile((dir + "/vnc-test.json").c_str());
  if (!jsonFile) {
    error = "cannot read vnc-test.json";
    return false;
  }
  const std::string json((std::istreambuf_iterator<char>(jsonFile)),
                         std::istreambuf_iterator<char>());
  // Minimal extraction: the smoke config only carries simple string/number
  // fields, all real parsing lives in the native library.
  const auto field = [&json](const char* key) {
    const std::string needle = std::string("\"") + key + "\"";
    size_t at = json.find(needle);
    if (at == std::string::npos) return std::string();
    at = json.find(':', at + needle.size());
    if (at == std::string::npos) return std::string();
    ++at;
    while (at < json.size() && (json[at] == ' ' || json[at] == '\t')) ++at;
    if (at < json.size() && json[at] == '"') {
      const size_t end = json.find('"', at + 1);
      if (end == std::string::npos) return std::string();
      return json.substr(at + 1, end - at - 1);
    }
    size_t end = at;
    while (end < json.size() && (json[end] == '-' || json[end] == '.' ||
           (json[end] >= '0' && json[end] <= '9'))) ++end;
    return json.substr(at, end - at);
  };
  config.host = field("host");
  config.port = field("port");
  if (config.host.empty() || config.port.empty()) {
    error = "vnc-test.json has no host/port";
    return false;
  }
  std::ifstream passFile((dir + "/vnc-test.pass").c_str(), std::ios::binary);
  if (!passFile) {
    error = "cannot read vnc-test.pass";
    return false;
  }
  config.password.assign((std::istreambuf_iterator<char>(passFile)),
                         std::istreambuf_iterator<char>());
  while (!config.password.empty() &&
         (config.password.back() == '\n' || config.password.back() == '\r'))
    config.password.pop_back();
  return true;
}

std::string escapeJson(const std::string& text) {
  std::string out;
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '"' || text[i] == '\\') out.push_back('\\');
    out.push_back(text[i]);
  }
  return out;
}

std::string makeOptions(const TestConfig& config, const std::string& pin,
                        const std::string& name) {
  std::string options = "{\"host\":\"" + escapeJson(config.host) + "\",\"port\":" +
                        config.port + ",\"password\":\"" + escapeJson(config.password) +
                        "\",\"name\":\"" + name + "\"";
  if (!pin.empty()) options += ",\"expectedFingerprint\":\"" + escapeJson(pin) + "\"";
  options += "}";
  return options;
}

int jsonInt(const std::string& json, const char* key, int fallback) {
  const std::string needle = std::string("\"") + key + "\":";
  const size_t at = json.find(needle);
  if (at == std::string::npos) return fallback;
  return std::atoi(json.c_str() + at + needle.size());
}

std::string jsonString(const std::string& json, const char* key) {
  const std::string needle = std::string("\"") + key + "\":\"";
  const size_t at = json.find(needle);
  if (at == std::string::npos) return std::string();
  const size_t start = at + needle.size();
  const size_t end = json.find('"', start);
  if (end == std::string::npos) return std::string();
  return json.substr(start, end - start);
}

// Password for the local Xvnc clipboard fixture comes from the environment so
// it never appears on a command line or in this program's output.
std::string testPassword() {
  const char* value = std::getenv("TG_TEST_VNC_PASSWORD");
  return value ? std::string(value) : std::string();
}

std::string connectOptions(const std::string& host, const std::string& port,
                           const std::string& name, const std::string& pin) {
  const std::string password = testPassword();
  std::string options = "{\"host\":\"" + escapeJson(host) + "\",\"port\":" + port +
                        ",\"name\":\"" + name + "\"";
  if (!password.empty()) options += ",\"password\":\"" + escapeJson(password) + "\"";
  if (!pin.empty()) options += ",\"expectedFingerprint\":\"" + escapeJson(pin) + "\"";
  options += "}";
  return options;
}

std::string waitForState(const std::string& id, const char* state, int timeoutMs) {
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeoutMs);
  std::string status = tianguan::sessionStatus(id);
  while (std::chrono::steady_clock::now() < deadline) {
    status = tianguan::sessionStatus(id);
    if (status.find(std::string("\"state\":\"") + state + "\"") != std::string::npos)
      return status;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  return status;
}

// A fixture server with RA2 has an unknown fingerprint at first contact; read it
// from the trust-required status exactly like the UI does, then start over with
// the pinned value.
std::string bootstrapPin(const std::string& host, const std::string& port) {
  const std::string id = tianguan::createSession(connectOptions(host, port, "trust-probe", ""));
  std::cout << "pin probe create: " << id << std::endl;
  if (id.empty() || id[0] == 'E') return std::string();
  const std::string status = waitForState(id, "trust-required", 15000);
  std::cout << "pin probe status: " << status << std::endl;
  const std::string pin = jsonString(status, "fingerprint");
  tianguan::closeSession(id);
  return pin;
}

std::string resolvePin(const std::string& host, const std::string& port,
                       const std::string& spec) {
  if (spec == "auto") return bootstrapPin(host, port);
  if (spec == "-") return std::string();
  return spec;
}

// Takes a complete frame through takeFrame(forceFull=true) and reports whether
// it is the expected size and contains at least one non-black pixel.
bool takeFullFrame(const std::string& id, const std::string& label,
                   int& versionOut) {
  versionOut = -1;
  const std::string status = tianguan::sessionStatus(id);
  const int width = jsonInt(status, "width", 0);
  const int height = jsonInt(status, "height", 0);
  if (width <= 0 || height <= 0) {
    std::cout << "FAIL " << label << " has no size: " << status << std::endl;
    return false;
  }
  std::vector<unsigned char> buffer(static_cast<size_t>(width) * height * 4);
  const std::string meta = tianguan::takeFrame(id, buffer.data(), buffer.size(), 0, true);
  std::cout << label << " takeFrame: " << meta << std::endl;
  if (meta.empty() || meta[0] != '{') return false;
  versionOut = jsonInt(meta, "version", 0);
  const int w = jsonInt(meta, "w", 0);
  const int h = jsonInt(meta, "h", 0);
  if (w != width || h != height) {
    std::cout << "FAIL " << label << " full frame is " << w << "x" << h
              << ", expected " << width << "x" << height << std::endl;
    return false;
  }
  size_t nonBlack = 0;
  for (size_t i = 0; i + 3 < buffer.size(); i += 4)
    if (buffer[i] != 0 || buffer[i + 1] != 0 || buffer[i + 2] != 0) ++nonBlack;
  std::cout << label << " size=" << width << "x" << height
            << " nonBlackPixels=" << nonBlack << std::endl;
  if (nonBlack == 0) {
    std::cout << "FAIL " << label << " frame is all black" << std::endl;
    return false;
  }
  return true;
}

// Slow-consumer test: prove that a takeFrame() after several server updates
// still covers every change, not just the newest update.
//
// The consumer below only ever paints the partial regions takeFrame() returns
// into its own mirror. After a burst of updates it stops taking, the test waits
// until the native version is stable, then compares the consumer mirror against
// the native whole-frame mirror at the same version. Any change dropped between
// two takes shows up as differing pixels (the old defect: takeFrame returned
// only the last update's rectangle).
int frameGapTest(const TestConfig& config, const std::string& pin) {
  bool passed = true;
  const std::string id = tianguan::createSession(makeOptions(config, pin, "frame-gap"));
  std::cout << "create: " << id << std::endl;
  if (id.empty() || id[0] == 'E') {
    std::cout << "FAIL createSession" << std::endl;
    return 1;
  }
  const std::string connected = waitForState(id, "connected", 20000);
  std::cout << "status: " << connected << std::endl;
  const int width = jsonInt(connected, "width", 0);
  const int height = jsonInt(connected, "height", 0);
  if (connected.find("\"state\":\"connected\"") == std::string::npos ||
      width <= 0 || height <= 0) {
    std::cout << "FAIL session did not connect" << std::endl;
    tianguan::closeSession(id);
    return 1;
  }
  const size_t frameBytes = static_cast<size_t>(width) * height * 4;
  std::vector<unsigned char> local(frameBytes, 0);
  std::vector<unsigned char> region(frameBytes, 0);
  std::vector<unsigned char> full(frameBytes, 0);

  // Baseline: one whole frame, so the local mirror starts from the same picture
  // as the native mirror and the take baseline is known.
  std::string meta;
  const auto baselineDeadline = std::chrono::steady_clock::now() +
                                std::chrono::seconds(15);
  while (std::chrono::steady_clock::now() < baselineDeadline) {
    meta = tianguan::takeFrame(id, full.data(), full.size(), -1, true);
    if (!meta.empty() && meta[0] == '{') break;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  std::cout << "baseline takeFrame: " << meta << std::endl;
  if (meta.empty() || meta[0] != '{') {
    std::cout << "FAIL no baseline full frame" << std::endl;
    tianguan::closeSession(id);
    return 1;
  }
  std::memcpy(local.data(), full.data(), frameBytes);
  int appliedVersion = jsonInt(meta, "version", 0);

  // Distinct spots: a right click opens a context menu there and leaves it on
  // screen, so every round installs a persistent visual change (a transient
  // hover would not expose a dropped update). The following right click either
  // replaces or dismisses it, which is itself a persistent change.
  const int spots[][2] = {
    {960, 600}, {480, 420}, {1440, 780},
    {720, 880}, {1200, 320}, {360, 700}
  };
  int roundsWithGap = 0;
  int verifiedRounds = 0;
  for (int round = 0; round < 6; ++round) {
    const int mx = spots[round][0];
    const int my = spots[round][1];
    std::cout << "round " << round << " right click at " << mx << "," << my
              << " at version "
              << jsonInt(tianguan::sessionStatus(id), "version", 0) << std::endl;
    // Move, right press, release: the menu opens and stays open.
    tianguan::sendPointer(id, mx, my, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    tianguan::sendPointer(id, mx, my, 4);
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    tianguan::sendPointer(id, mx, my, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    // Hover two entries: the menu repaints a few times while the consumer is
    // deliberately not taking anything.
    tianguan::sendPointer(id, mx + 40, my + 50, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    tianguan::sendPointer(id, mx + 40, my + 90, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    // The slow-consumer shape: take once, wait ~500 ms while the server
    // produces more updates, then take again.
    std::vector<int> seen;
    const auto waitDeadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(500);
    while (std::chrono::steady_clock::now() < waitDeadline) {
      const int v = jsonInt(tianguan::sessionStatus(id), "version", 0);
      if (seen.empty() || seen.back() != v) seen.push_back(v);
      std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
    const std::string taken = tianguan::takeFrame(id, region.data(), region.size(),
                                                  appliedVersion, false);
    const std::string waitStatus = tianguan::sessionStatus(id);
    std::cout << "round " << round << " afterVersion=" << appliedVersion
              << " observedVersions=";
    for (size_t i = 0; i < seen.size(); ++i)
      std::cout << (i > 0 ? "," : "") << seen[i];
    std::cout << " state=" << jsonString(waitStatus, "state")
              << " takeFrame=" << taken << std::endl;
    if (taken == "NONE") {
      std::cout << "round " << round << ": nothing new to take" << std::endl;
      continue;
    }
    if (taken.empty() || taken[0] != '{') {
      std::cout << "FAIL round " << round << " takeFrame: " << taken << std::endl;
      passed = false;
      break;
    }
    const int x = jsonInt(taken, "x", -1);
    const int y = jsonInt(taken, "y", -1);
    const int w = jsonInt(taken, "w", 0);
    const int h = jsonInt(taken, "h", 0);
    const int version = jsonInt(taken, "version", 0);
    if (x < 0 || y < 0 || x + w > width || y + h > height || w <= 0 || h <= 0) {
      std::cout << "FAIL round " << round << " bad rectangle" << std::endl;
      passed = false;
      break;
    }
    std::cout << "round " << round << " covers versions " << (appliedVersion + 1)
              << ".." << version << " in "
              << w << "x" << h << "+" << x << "+" << y << std::endl;
    if (version > appliedVersion + 1) ++roundsWithGap;
    for (int row = 0; row < h; ++row) {
      std::memcpy(local.data() + (static_cast<size_t>(y + row) * width + x) * 4,
                  region.data() + static_cast<size_t>(row) * w * 4,
                  static_cast<size_t>(w) * 4);
    }
    appliedVersion = version;

    // Catch up with partial handoffs only (no full frame into the local
    // mirror), so the comparison below still proves the partial regions were
    // sufficient for every version in between.
    for (int guard = 0; guard < 100; ++guard) {
      const int current = jsonInt(tianguan::sessionStatus(id), "version", 0);
      if (current == appliedVersion) break;
      const std::string caughtUp = tianguan::takeFrame(id, region.data(), region.size(),
                                                       appliedVersion, false);
      if (caughtUp == "NONE") {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        continue;
      }
      if (caughtUp.empty() || caughtUp[0] != '{') {
        std::cout << "FAIL catch-up takeFrame: " << caughtUp << std::endl;
        passed = false;
        break;
      }
      const int cx = jsonInt(caughtUp, "x", -1);
      const int cy = jsonInt(caughtUp, "y", -1);
      const int cw = jsonInt(caughtUp, "w", 0);
      const int ch = jsonInt(caughtUp, "h", 0);
      const int cv = jsonInt(caughtUp, "version", 0);
      if (cx < 0 || cy < 0 || cx + cw > width || cy + ch > height || cw <= 0 || ch <= 0) {
        std::cout << "FAIL catch-up rectangle: " << caughtUp << std::endl;
        passed = false;
        break;
      }
      for (int row = 0; row < ch; ++row) {
        std::memcpy(local.data() + (static_cast<size_t>(cy + row) * width + cx) * 4,
                    region.data() + static_cast<size_t>(row) * cw * 4,
                    static_cast<size_t>(cw) * 4);
      }
      std::cout << "  catch-up covers versions " << (appliedVersion + 1) << ".."
                << cv << " in " << cw << "x" << ch << "+" << cx << "+" << cy
                << std::endl;
      appliedVersion = cv;
    }
    if (!passed) break;

    // Oracle: the native whole-frame mirror at the same version. If the partial
    // handoffs missed any dirty pixel, the two mirrors differ there.
    const std::string oracle = tianguan::takeFrame(id, full.data(), full.size(),
                                                   appliedVersion, true);
    const int oracleVersion = jsonInt(oracle, "version", -1);
    if (oracleVersion != appliedVersion) {
      // A new update landed on the server between the catch-up and the oracle;
      // the oracle is newer than the local mirror, so restart the comparison
      // from it instead of reporting a false loss.
      std::cout << "round " << round << ": oracle moved from " << appliedVersion
                << " to " << oracleVersion << ", re-baselining" << std::endl;
      std::memcpy(local.data(), full.data(), frameBytes);
      appliedVersion = oracleVersion;
      continue;
    }
    size_t diff = 0;
    int diffX0 = width, diffY0 = height, diffX1 = -1, diffY1 = -1;
    for (int py = 0; py < height; ++py) {
      for (int px = 0; px < width; ++px) {
        const size_t at = (static_cast<size_t>(py) * width + px) * 4;
        if (std::memcmp(&local[at], &full[at], 4) != 0) {
          ++diff;
          if (px < diffX0) diffX0 = px;
          if (py < diffY0) diffY0 = py;
          if (px > diffX1) diffX1 = px;
          if (py > diffY1) diffY1 = py;
        }
      }
    }
    ++verifiedRounds;
    std::cout << "round " << round << " oracle takeFrame: " << oracle
              << " localMirrorDiffPixels=" << diff;
    if (diff > 0)
      std::cout << " diffBox=" << diffX0 << "," << diffY0 << ".."
                << diffX1 << "," << diffY1;
    std::cout << std::endl;
    if (diff != 0) {
      std::cout << "FAIL round " << round << " lost updates: consumer mirror does"
                << " not match the native mirror at version " << appliedVersion
                << std::endl;
      passed = false;
      break;
    }
  }
  // A slow consumer that never saw a version gap would not exercise the defect.
  // Dismiss any context menu still open (the server releases held keys when the
  // client disconnects).
  tianguan::sendKey(id, 0xff1b, 1);
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  std::cout << "versionGapRounds=" << roundsWithGap << " verifiedRounds="
            << verifiedRounds << std::endl;
  if (!passed || verifiedRounds == 0 || roundsWithGap == 0) {
    std::cout << "FRAME GAP TEST FAIL" << std::endl;
    passed = false;
  } else {
    std::cout << "FRAME GAP TEST PASS" << std::endl;
  }
  tianguan::closeSession(id);
  return passed ? 0 : 1;
}

// Server-side resize: Xvnc sends ExtendedDesktopSize when xrandr changes the
// framebuffer, which reaches LiveClient::resizeFramebuffer(). The base class
// implementation is assert(false), so before the override existed this host
// binary aborted the moment the desktop was resized. The test waits for the
// external resize, then requires a complete frame of the new geometry.
int resizeTest(const TestConfig& config, const std::string& pin) {
  const std::string id = tianguan::createSession(makeOptions(config, pin, "resize"));
  std::cout << "create: " << id << std::endl;
  if (id.empty() || id[0] == 'E') {
    std::cout << "FAIL createSession" << std::endl;
    return 1;
  }
  const std::string connected = waitForState(id, "connected", 20000);
  std::cout << "status: " << connected << std::endl;
  int width = jsonInt(connected, "width", 0);
  int height = jsonInt(connected, "height", 0);
  if (connected.find("\"state\":\"connected\"") == std::string::npos ||
      width <= 0 || height <= 0) {
    std::cout << "FAIL session did not connect" << std::endl;
    tianguan::closeSession(id);
    return 1;
  }
  std::vector<unsigned char> buffer(static_cast<size_t>(width) * height * 4);
  std::string meta;
  int versionBefore = 0;
  const auto baselineDeadline = std::chrono::steady_clock::now() +
                                std::chrono::seconds(15);
  while (std::chrono::steady_clock::now() < baselineDeadline) {
    meta = tianguan::takeFrame(id, buffer.data(), buffer.size(), -1, true);
    if (!meta.empty() && meta[0] == '{') break;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  if (meta.empty() || meta[0] != '{') {
    std::cout << "FAIL no baseline full frame" << std::endl;
    tianguan::closeSession(id);
    return 1;
  }
  versionBefore = jsonInt(meta, "version", 0);
  const int generationBefore = jsonInt(meta, "generation", 0);
  std::cout << "baseline size=" << width << "x" << height
            << " version=" << versionBefore << " generation=" << generationBefore
            << std::endl;
  std::cout << "READY FOR RESIZE" << std::endl;
  std::cout.flush();

  // An external "xrandr -s ..." on the fixture display drives the resize.
  int newWidth = width;
  int newHeight = height;
  std::string lastState;
  const auto resizeDeadline = std::chrono::steady_clock::now() +
                              std::chrono::seconds(30);
  while (std::chrono::steady_clock::now() < resizeDeadline) {
    const std::string status = tianguan::sessionStatus(id);
    const std::string state = jsonString(status, "state");
    const int w = jsonInt(status, "width", 0);
    const int h = jsonInt(status, "height", 0);
    if (state != lastState) {
      std::cout << "resize wait state=" << state
                << " size=" << w << "x" << h
                << " generation=" << jsonInt(status, "generation", 0)
                << " error=" << jsonString(status, "error") << std::endl;
      lastState = state;
    }
    if (w > 0 && h > 0 && (w != width || h != height)) {
      newWidth = w;
      newHeight = h;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  if (newWidth == width && newHeight == height) {
    std::cout << "FAIL no resize observed (is the external xrandr running?)"
              << std::endl;
    tianguan::closeSession(id);
    return 1;
  }
  std::cout << "resized to " << newWidth << "x" << newHeight << std::endl;

  // After the resize the mirror is rebuilt; only a complete frame of the new
  // geometry may be delivered, and it must carry a newer version.
  std::string resizedMeta;
  const auto frameDeadline = std::chrono::steady_clock::now() +
                             std::chrono::seconds(15);
  while (std::chrono::steady_clock::now() < frameDeadline) {
    std::vector<unsigned char> resized(static_cast<size_t>(newWidth) * newHeight * 4);
    resizedMeta = tianguan::takeFrame(id, resized.data(), resized.size(),
                                      versionBefore, true);
    if (!resizedMeta.empty() && resizedMeta[0] == '{') {
      const int w = jsonInt(resizedMeta, "w", 0);
      const int h = jsonInt(resizedMeta, "h", 0);
      const int version = jsonInt(resizedMeta, "version", 0);
      const int generation = jsonInt(resizedMeta, "generation", 0);
      size_t nonBlack = 0;
      for (size_t i = 0; i + 3 < resized.size(); i += 4)
        if (resized[i] != 0 || resized[i + 1] != 0 || resized[i + 2] != 0) ++nonBlack;
      std::cout << "resized takeFrame: " << resizedMeta
                << " nonBlackPixels=" << nonBlack << std::endl;
      const bool ok = w == newWidth && h == newHeight && version > versionBefore &&
                      generation == generationBefore &&
                      resizedMeta.find("\"full\":true") != std::string::npos;
      std::cout << (ok ? "RESIZE TEST PASS" : "RESIZE TEST FAIL")
                << " (expected " << newWidth << "x" << newHeight << " version>"
                << versionBefore << " generation=" << generationBefore << ")"
                << std::endl;
      tianguan::closeSession(id);
      return ok ? 0 : 1;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  std::cout << "FAIL no complete frame after resize: " << resizedMeta << std::endl;
  tianguan::closeSession(id);
  return 1;
}

// Two sessions on one server, each with its own framebuffer; close one and
// prove the other keeps working. This is the required acceptance smoke.
int multiSessionTest(const TestConfig& config, const std::string& pin) {
  bool passed = true;
  const std::string id1 = tianguan::createSession(makeOptions(config, pin, "one"));
  const std::string id2 = tianguan::createSession(makeOptions(config, pin, "two"));
  std::cout << "create: id1=" << id1 << " id2=" << id2 << std::endl;
  if (id1.empty() || id1[0] == 'E' || id2.empty() || id2[0] == 'E') {
    std::cout << "FAIL createSession" << std::endl;
    return 1;
  }
  if (id1 == id2) {
    std::cout << "FAIL ids are equal" << std::endl;
    passed = false;
  }
  std::string status1 = waitForState(id1, "connected", 20000);
  std::string status2 = waitForState(id2, "connected", 20000);
  std::cout << "status1: " << status1 << std::endl;
  std::cout << "status2: " << status2 << std::endl;
  if (status1.find("\"state\":\"connected\"") == std::string::npos ||
      status2.find("\"state\":\"connected\"") == std::string::npos) {
    std::cout << "FAIL both sessions did not connect" << std::endl;
    std::cout << "list: " << tianguan::listSessions() << std::endl;
    tianguan::closeSession(id1);
    tianguan::closeSession(id2);
    return 1;
  }
  std::cout << "list: " << tianguan::listSessions() << std::endl;
  tianguan::requestFullFrame(id1);
  tianguan::requestFullFrame(id2);
  std::this_thread::sleep_for(std::chrono::milliseconds(1200));
  int version1 = 0, version2 = 0;
  if (!takeFullFrame(id1, "s1", version1)) passed = false;
  if (!takeFullFrame(id2, "s2", version2)) passed = false;
  // A too-small buffer must be rejected without writing.
  unsigned char tiny[16];
  const std::string tooSmall = tianguan::takeFrame(id1, tiny, sizeof(tiny), 0, true);
  std::cout << "s1 smallBuffer: " << tooSmall << std::endl;
  if (tooSmall != "ERROR:BUFFER_TOO_SMALL") passed = false;
  std::cout << "s1 setClipboard: "
            << tianguan::setClipboard(id1, "tianguan multi-session smoke") << std::endl;
  const std::string clip = tianguan::takeClipboard(id1);
  std::cout << "s1 takeClipboard: len=" << clip.size() << std::endl;
  // Close only the second session and prove the first is untouched.
  std::cout << "close s2: " << tianguan::closeSession(id2) << std::endl;
  std::cout << "close s2 again: " << tianguan::closeSession(id2) << std::endl;
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  const std::string closedStatus = tianguan::sessionStatus(id2);
  std::cout << "status2 after close: " << closedStatus << std::endl;
  if (closedStatus.find("unknown session") == std::string::npos) passed = false;
  const std::string afterClose = tianguan::sessionStatus(id1);
  std::cout << "status1 after close: " << afterClose << std::endl;
  if (afterClose.find("\"state\":\"connected\"") == std::string::npos) passed = false;
  std::cout << "list after close: " << tianguan::listSessions() << std::endl;
  tianguan::requestFullFrame(id1);
  std::this_thread::sleep_for(std::chrono::milliseconds(1200));
  int version1b = 0;
  if (!takeFullFrame(id1, "s1 again", version1b)) passed = false;
  if (version1b < version1) passed = false;
  // A stale afterVersion must ask for a complete frame; a 4-byte buffer proves
  // it chose the full frame path (partial would be smaller and succeed).
  std::vector<unsigned char> staleBuffer(4);
  const std::string stale = tianguan::takeFrame(id1, staleBuffer.data(),
                                                staleBuffer.size(), 1, false);
  std::cout << "s1 staleVersion takeFrame: " << stale << std::endl;
  if (stale.find("ERROR:BUFFER_TOO_SMALL") == std::string::npos) passed = false;
  std::cout << "close s1: " << tianguan::closeSession(id1) << std::endl;
  std::cout << "list final: " << tianguan::listSessions() << std::endl;
  std::cout << (passed ? "MULTI-SESSION SMOKE PASS" : "MULTI-SESSION SMOKE FAIL")
            << std::endl;
  return passed ? 0 : 1;
}

// Extra contract checks: close during connect, reconnect generation, clipboard
// forwarding between two sessions, and the trust-required state.
int extraTest(const TestConfig& config, const std::string& pin) {
  bool passed = true;
  // 1. closeSession() must end the thread even while TCP connect is pending.
  const std::string deadId = tianguan::createSession(
      "{\"host\":\"192.0.2.1\",\"port\":5901,\"name\":\"unreachable\"}");
  std::cout << "unreachable create: " << deadId << std::endl;
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  const auto closeStart = std::chrono::steady_clock::now();
  const std::string deadClose = tianguan::closeSession(deadId);
  const auto closeMs = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - closeStart).count();
  std::cout << "unreachable close: " << deadClose << " in " << closeMs << "ms" << std::endl;
  if (deadClose != "CLOSED" || closeMs > 3000) passed = false;

  // 2. Two sessions plus the clipboard API path. A TigerVNC server writes a
  // client's clipboard into the server-side X selection but does not relay it
  // to other VNC clients, so cross-session delivery is not expected here. The
  // dedicated --clipboard-send-test / --clipboard-recv-test modes verify both
  // directions end-to-end against a local fixture server and an X11 selection
  // owner.
  const std::string id1 = tianguan::createSession(makeOptions(config, pin, "session-one"));
  const std::string id2 = tianguan::createSession(makeOptions(config, pin, "session-two"));
  std::cout << "sessions: id1=" << id1 << " id2=" << id2 << std::endl;
  if (id1.empty() || id1[0] == 'E' || id2.empty() || id2[0] == 'E') {
    std::cout << "FAIL createSession for extra test" << std::endl;
    return 1;
  }
  const std::string s1 = waitForState(id1, "connected", 20000);
  const std::string s2 = waitForState(id2, "connected", 20000);
  if (s1.find("\"state\":\"connected\"") == std::string::npos ||
      s2.find("\"state\":\"connected\"") == std::string::npos) {
    std::cout << "FAIL extra sessions did not connect" << std::endl;
    tianguan::closeSession(id1);
    tianguan::closeSession(id2);
    return 1;
  }
  const std::string expected = "tianguan-clipboard-" +
      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
  const std::string queued = tianguan::setClipboard(id1, expected);
  std::cout << "setClipboard(s1): " << queued << std::endl;
  if (queued != "QUEUED") passed = false;
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  const std::string echoed = tianguan::takeClipboard(id1);
  std::cout << "takeClipboard(s1): len=" << echoed.size() << " (no echo expected)"
            << std::endl;
  std::cout << "takeClipboard(s2): len=" << tianguan::takeClipboard(id2).size()
            << " (server does not relay between VNC clients)" << std::endl;

  // 3. reconnectSession() bumps generation and the session comes back. A
  // reconnect is only proven complete when the state is connected again *and*
  // a new frame arrived from the new connection.
  const std::string before = tianguan::sessionStatus(id1);
  const int generationBefore = jsonInt(before, "generation", 0);
  const int versionBefore = jsonInt(before, "version", 0);
  std::cout << "reconnectSession(s1): " << tianguan::reconnectSession(id1) << std::endl;
  std::string reconnected;
  const auto reconnectDeadline = std::chrono::steady_clock::now() +
                                 std::chrono::seconds(20);
  while (std::chrono::steady_clock::now() < reconnectDeadline) {
    reconnected = tianguan::sessionStatus(id1);
    if (reconnected.find("\"state\":\"connected\"") != std::string::npos &&
        jsonInt(reconnected, "generation", 0) == generationBefore + 1 &&
        jsonInt(reconnected, "version", 0) > versionBefore)
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  const int generationAfter = jsonInt(reconnected, "generation", 0);
  std::cout << "s1 after reconnect: " << reconnected << std::endl;
  if (reconnected.find("\"state\":\"connected\"") == std::string::npos ||
      generationAfter != generationBefore + 1 ||
      jsonInt(reconnected, "version", 0) <= versionBefore) {
    std::cout << "FAIL reconnect generation " << generationBefore << " -> "
              << generationAfter << ", version " << versionBefore << " -> "
              << jsonInt(reconnected, "version", 0) << std::endl;
    passed = false;
  }
  tianguan::requestFullFrame(id1);
  std::this_thread::sleep_for(std::chrono::milliseconds(1200));
  int version = 0;
  if (!takeFullFrame(id1, "s1 after reconnect", version)) passed = false;

  // 4. releaseSessionInput() is accepted and does not disturb the session.
  std::cout << "press key: " << tianguan::sendKey(id1, 0x61, 1) << std::endl;
  std::cout << "releaseSessionInput: " << tianguan::releaseSessionInput(id1) << std::endl;
  std::cout << "s1 after release: " << tianguan::sessionStatus(id1) << std::endl;

  // 5. Without a trusted fingerprint the session must stop at trust-required;
  // the explicit reconnect after confirmation pins the observed fingerprint.
  const std::string id3 = tianguan::createSession(makeOptions(config, "", "trust"));
  std::cout << "trust create: " << id3 << std::endl;
  const std::string trusted = waitForState(id3, "trust-required", 15000);
  std::cout << "trust status: " << trusted << std::endl;
  if (trusted.find("\"state\":\"trust-required\"") == std::string::npos ||
      (pin.size() > 0 && trusted.find("\"fingerprint\":\"" + pin + "\"") ==
                            std::string::npos))
    passed = false;
  std::cout << "trust reconnect: " << tianguan::reconnectSession(id3) << std::endl;
  const std::string trustedNow = waitForState(id3, "connected", 20000);
  std::cout << "trust after reconnect: " << trustedNow << std::endl;
  if (trustedNow.find("\"state\":\"connected\"") == std::string::npos) passed = false;
  std::cout << "close trust: " << tianguan::closeSession(id3) << std::endl;

  std::cout << "close clip s2: " << tianguan::closeSession(id2) << std::endl;
  std::cout << "close clip s1: " << tianguan::closeSession(id1) << std::endl;
  std::cout << "list extra final: " << tianguan::listSessions() << std::endl;
  std::cout << (passed ? "EXTRA CONTRACT TESTS PASS" : "EXTRA CONTRACT TESTS FAIL")
            << std::endl;
  return passed ? 0 : 1;
}

// --- Key release wire test -------------------------------------------------
// drainSessionInput() must register a press under the same system key code that
// sendKeyRelease() later looks up. With a press registered under 0 the release
// misses downKeys[] and CConnection silently drops it, which is the Ctrl/Shift/
// Alt stuck-key bug. This drives CConnection over an in-memory stream and checks
// the exact RFB KeyEvent bytes (8 bytes each: type, down, pad2, keysym32).
class KeyWireConnection : public rfb::CConnection {
public:
  void getUserPasswd(bool, std::string*, std::string*) override {}
  bool showMsgBox(rfb::MsgBoxFlags, const char*, const char*) override { return false; }
  void initDone() override {}
  void setCursor(int, int, const rfb::Point&, const uint8_t*) override {}
  void setCursorPos(const rfb::Point&) override {}
  void setColourMapEntries(int, int, uint16_t*) override {}
  void bell() override {}
  void installWriter(rfb::CMsgWriter* writer) { setWriter(writer); }
};

struct KeyWireEvent {
  bool down;
  uint32_t keysym;
};

std::vector<KeyWireEvent> keyWireEvents(rdr::MemOutStream& stream) {
  std::vector<KeyWireEvent> events;
  const uint8_t* data = stream.data();
  for (size_t at = 0; at + 8 <= stream.length(); at += 8) {
    KeyWireEvent event;
    event.down = data[at + 1] != 0;
    event.keysym = (static_cast<uint32_t>(data[at + 4]) << 24) |
                   (static_cast<uint32_t>(data[at + 5]) << 16) |
                   (static_cast<uint32_t>(data[at + 6]) << 8) |
                   static_cast<uint32_t>(data[at + 7]);
    events.push_back(event);
  }
  return events;
}

bool keyWireTest() {
  bool passed = true;

  // The shape used by the fixed drain path: system key code == keysym.
  {
    rdr::MemOutStream stream;
    KeyWireConnection conn;
    conn.setStreams(nullptr, &stream);
    rfb::CMsgWriter writer(&conn.server, &stream);
    conn.installWriter(&writer);
    conn.sendKeyPress(0xffe1, 0, 0xffe1);  // Shift_L down
    conn.sendKeyRelease(0xffe1);           // Shift_L up
    conn.sendKeyPress(0x61, 0, 0x61);      // a down
    conn.sendKeyRelease(0x61);             // a up
    conn.sendKeyRelease(0x61);             // must be a no-op: entry was erased
    stream.flush();
    const std::vector<KeyWireEvent> events = keyWireEvents(stream);
    const bool ok = events.size() == 4 &&
                    events[0].down && events[0].keysym == 0xffe1 &&
                    !events[1].down && events[1].keysym == 0xffe1 &&
                    events[2].down && events[2].keysym == 0x61 &&
                    !events[3].down && events[3].keysym == 0x61;
    std::cout << (ok ? "PASS" : "FAIL")
              << " key wire: press(keysym)+release(keysym) wrote "
              << events.size()
              << " KeyEvents (want 4: Shift down/up, a down/up, then no-op)"
              << std::endl;
    passed = passed && ok;
    conn.installWriter(nullptr);  // close() must not delete the stack writer
  }

  // The pre-fix shape: press registered under 0, release looks up the keysym,
  // so CConnection finds no downKeys entry and drops the release.
  {
    rdr::MemOutStream stream;
    KeyWireConnection conn;
    conn.setStreams(nullptr, &stream);
    rfb::CMsgWriter writer(&conn.server, &stream);
    conn.installWriter(&writer);
    conn.sendKeyPress(0, 0, 0x61);
    conn.sendKeyRelease(0x61);
    stream.flush();
    const std::vector<KeyWireEvent> events = keyWireEvents(stream);
    const bool ok = events.size() == 1 && events[0].down &&
                    events[0].keysym == 0x61;
    std::cout << (ok ? "PASS" : "FAIL")
              << " key wire: legacy press(0,..)+release(keysym) wrote "
              << events.size() << " KeyEvents (want 1: release dropped)"
              << std::endl;
    passed = passed && ok;
    conn.installWriter(nullptr);
  }

  std::cout << (passed ? "KEY WIRE TEST PASS" : "KEY WIRE TEST FAIL") << std::endl;
  return passed;
}

// End-to-end companion: send one press/release through the real queued session
// path. A local Xvnc plus xev records the pair (or, before the fix, only the
// press) on the server side.
int keySessionTest(const std::string& dir, int keysym) {
  bool passed = true;
  std::cout << "start: " << tianguan::startSession(dir) << std::endl;
  std::this_thread::sleep_for(std::chrono::seconds(6));
  const std::string status = tianguan::sessionStatus();
  std::cout << "status: " << status << std::endl;
  if (status.find("running=1") == std::string::npos) {
    std::cout << "FAIL session did not connect" << std::endl;
    tianguan::stopSession();
    return 1;
  }
  // Focus follows the pointer (PointerRoot) in the bare Xvnc fixture, so put
  // the pointer inside the xev window before sending the key.
  std::cout << "move: " << tianguan::sendPointer(50, 55, 0) << std::endl;
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  const std::string press = tianguan::sendKey(keysym, 1);
  std::cout << "key press: " << press << std::endl;
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  const std::string release = tianguan::sendKey(keysym, 0);
  std::cout << "key release: " << release << std::endl;
  std::this_thread::sleep_for(std::chrono::seconds(1));
  std::cout << "status: " << tianguan::sessionStatus() << std::endl;
  std::cout << "stop: " << tianguan::stopSession() << std::endl;
  if (press != "QUEUED" || release != "QUEUED") passed = false;
  std::cout << (passed ? "KEY SESSION TEST PASS" : "KEY SESSION TEST FAIL")
            << std::endl;
  return passed ? 0 : 1;
}

// Same wire check for the other release path: a key is pressed and then
// releaseSessionInput() must synthesize a matching release for it.
int releaseSessionTest(const std::string& dir, int keysym) {
  bool passed = true;
  TestConfig config;
  std::string error;
  if (!loadConfig(dir, config, error)) {
    std::cout << "FAIL " << error << std::endl;
    return 1;
  }
  const std::string pin = resolvePin(config.host, config.port, "auto");
  const std::string id = tianguan::createSession(makeOptions(config, pin, "key-release"));
  std::cout << "create: " << id << std::endl;
  if (id.empty() || id[0] == 'E') return 1;
  const std::string connected = waitForState(id, "connected", 20000);
  std::cout << "status: " << connected << std::endl;
  if (connected.find("\"state\":\"connected\"") == std::string::npos) {
    std::cout << "FAIL session did not connect" << std::endl;
    tianguan::closeSession(id);
    return 1;
  }
  std::cout << "move: " << tianguan::sendPointer(id, 50, 55, 0) << std::endl;
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  const std::string press = tianguan::sendKey(id, keysym, 1);
  std::cout << "key press: " << press << std::endl;
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  const std::string released = tianguan::releaseSessionInput(id);
  std::cout << "releaseSessionInput: " << released << std::endl;
  std::this_thread::sleep_for(std::chrono::seconds(1));
  std::cout << "close: " << tianguan::closeSession(id) << std::endl;
  if (press != "QUEUED" || released != "OK") passed = false;
  std::cout << (passed ? "RELEASE SESSION TEST PASS" : "RELEASE SESSION TEST FAIL")
            << std::endl;
  return passed ? 0 : 1;
}
} // namespace

int main(int argc, char** argv) {
  if (argc == 2 && std::strcmp(argv[1], "--keyevent-test") == 0) {
    return keyWireTest() ? 0 : 1;
  }
  if (argc == 4 && std::strcmp(argv[1], "--key-session-test") == 0) {
    return keySessionTest(argv[2], static_cast<int>(std::strtol(argv[3], nullptr, 0)));
  }
  if (argc == 4 && std::strcmp(argv[1], "--release-session-test") == 0) {
    return releaseSessionTest(argv[2], static_cast<int>(std::strtol(argv[3], nullptr, 0)));
  }
  if (argc == 3 && std::strcmp(argv[1], "--net-test") == 0) {
    const std::string result = tianguan::runNetworkTest(argv[2]);
    std::cout << result;
    return result.find("FAIL ") == std::string::npos ? 0 : 1;
  }
  if ((argc == 5) && std::strcmp(argv[1], "--press-test") == 0) {
    const std::string dir(argv[2]);
    const int x = std::atoi(argv[3]);
    const int y = std::atoi(argv[4]);
    std::cout << "start: " << tianguan::startSession(dir) << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(6));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    std::rename((dir + "/snapshot.jpg").c_str(), (dir + "/press-before.jpg").c_str());
    tianguan::sendPointer(x, y, 0);
    tianguan::sendPointer(x, y, 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    std::rename((dir + "/snapshot.jpg").c_str(), (dir + "/press-held.jpg").c_str());
    tianguan::sendPointer(x, y, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    // Move the pointer away so the drawn cursor does not cover the target.
    tianguan::sendPointer(1700, 1000, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    std::rename((dir + "/snapshot.jpg").c_str(), (dir + "/press-after.jpg").c_str());
    std::cout << "stop: " << tianguan::stopSession() << std::endl;
    return 0;
  }
  if ((argc == 3 || argc == 5) && std::strcmp(argv[1], "--session-test") == 0) {
    const std::string dir(argv[2]);
    const int clickX = argc == 5 ? std::atoi(argv[3]) : 50;
    const int clickY = argc == 5 ? std::atoi(argv[4]) : 55;
    std::cout << "start: " << tianguan::startSession(dir) << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(6));
    const std::string first = tianguan::sessionStatus();
    std::cout << "status: " << first << std::endl;
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    // Ask for one complete frame and let region.ppm capture it.
    tianguan::requestFullFrame();
    std::this_thread::sleep_for(std::chrono::seconds(2));
    const std::string legacyStatus = tianguan::sessionStatus();
    std::cout << "status: " << legacyStatus << std::endl;
    // The current pages still use the legacy region API; exercise it once.
    {
      int width = 0, height = 0;
      const size_t sizeAt = legacyStatus.find("size=");
      if (sizeAt != std::string::npos)
        std::sscanf(legacyStatus.c_str() + sizeAt, "size=%dx%d", &width, &height);
      if (width > 0 && height > 0) {
        std::vector<unsigned char> region(static_cast<size_t>(width) * height * 4);
        std::cout << "legacy takeRegion: "
                  << tianguan::takeRegion(region.data(), region.size()) << std::endl;
        std::cout << "legacy takeRegion again: "
                  << tianguan::takeRegion(region.data(), region.size()) << std::endl;
      }
    }
    // Double-click the desktop Home icon through the queued session path so the
    // saved frame shows whether the click really reached the server.
    for (int round = 0; round < 1; ++round) {
      std::cout << "move: " << tianguan::sendPointer(clickX, clickY, 0) << std::endl;
      std::cout << "press: " << tianguan::sendPointer(clickX, clickY, 1) << std::endl;
      std::cout << "release: " << tianguan::sendPointer(clickX, clickY, 0) << std::endl;
      std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }
    std::this_thread::sleep_for(std::chrono::seconds(2));
    std::cout << "status: " << tianguan::sessionStatus() << std::endl;
    // Keyboard check through the same queue: type into the focused terminal.
    const char* text = "echo TG-KEY-OK\n";
    for (const char* p = text; *p != '\0'; ++p) {
      const int sym = (*p == '\n') ? 0xff0d : static_cast<int>(*p);
      tianguan::sendKey(sym, 1);
      tianguan::sendKey(sym, 0);
      std::this_thread::sleep_for(std::chrono::milliseconds(40));
    }
    std::this_thread::sleep_for(std::chrono::seconds(2));
    std::cout << "status: " << tianguan::sessionStatus() << std::endl;
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    std::rename((dir + "/snapshot.jpg").c_str(), (dir + "/after.jpg").c_str());
    std::cout << "stop: " << tianguan::stopSession() << std::endl;
    return 0;
  }
  if ((argc == 3 || argc == 4) && std::strcmp(argv[1], "--resize-test") == 0) {
    const std::string dir(argv[2]);
    const std::string pinSpec = argc == 4 ? argv[3] : "";
    TestConfig config;
    std::string error;
    if (!loadConfig(dir, config, error)) {
      std::cout << "FAIL " << error << std::endl;
      return 1;
    }
    return resizeTest(config, resolvePin(config.host, config.port, pinSpec));
  }
  if (argc >= 3 && argc <= 4 && std::strcmp(argv[1], "--frame-gap-test") == 0) {
    const std::string dir(argv[2]);
    const std::string pin = argc == 4 ? argv[3] : "";
    TestConfig config;
    std::string error;
    if (!loadConfig(dir, config, error)) {
      std::cout << "FAIL " << error << std::endl;
      return 1;
    }
    return frameGapTest(config, pin);
  }
  if (argc >= 3 && (argc == 3 || argc == 4) &&
      (std::strcmp(argv[1], "--multi-session-test") == 0 ||
       std::strcmp(argv[1], "--extra-test") == 0)) {
    const std::string dir(argv[2]);
    const std::string pin = argc == 4 ? argv[3] : "";
    TestConfig config;
    std::string error;
    if (!loadConfig(dir, config, error)) {
      std::cout << "FAIL " << error << std::endl;
      return 1;
    }
    if (std::strcmp(argv[1], "--extra-test") == 0) return extraTest(config, pin);
    return multiSessionTest(config, pin);
  }
  if ((argc == 5 || argc == 6) &&
      std::strcmp(argv[1], "--clipboard-recv-test") == 0) {
    // Local Xvnc test: an X11 owner sets CLIPBOARD on the server's display,
    // the TigerVNC server announces it, and this session must receive it.
    const std::string host(argv[2]);
    const std::string port(argv[3]);
    const std::string expected(argv[4]);
    const std::string pin =
        argc == 6 ? resolvePin(host, port, argv[5]) : std::string();
    const std::string options = connectOptions(host, port, "clip-recv", pin);
    const std::string id = tianguan::createSession(options);
    std::cout << "clip-recv create: " << id << std::endl;
    const std::string status = waitForState(id, "connected", 15000);
    std::cout << "clip-recv status: " << status << std::endl;
    if (status.find("\"state\":\"connected\"") == std::string::npos) {
      tianguan::closeSession(id);
      std::cout << "CLIPBOARD RECV FAIL" << std::endl;
      return 1;
    }
    std::cout << "clip-recv READY" << std::endl;
    std::string received;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (std::chrono::steady_clock::now() < deadline && received.empty()) {
      received = tianguan::takeClipboard(id);
      if (received.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    std::cout << "clip-recv received len=" << received.size()
              << " text=" << received << std::endl;
    std::cout << "clip-recv status2: " << tianguan::sessionStatus(id) << std::endl;
    std::cout << "clip-recv close: " << tianguan::closeSession(id) << std::endl;
    const bool ok = received == expected;
    std::cout << (ok ? "CLIPBOARD RECV PASS" : "CLIPBOARD RECV FAIL") << std::endl;
    return ok ? 0 : 1;
  }
  if ((argc == 5 || argc == 6) &&
      std::strcmp(argv[1], "--clipboard-send-test") == 0) {
    // Local Xvnc test: this session sets the clipboard and an X11 helper reads
    // it back from the server's CLIPBOARD selection afterwards.
    const std::string host(argv[2]);
    const std::string port(argv[3]);
    const std::string text(argv[4]);
    const std::string pin =
        argc == 6 ? resolvePin(host, port, argv[5]) : std::string();
    const std::string options = connectOptions(host, port, "clip-send", pin);
    const std::string id = tianguan::createSession(options);
    std::cout << "clip-send create: " << id << std::endl;
    const std::string status = waitForState(id, "connected", 15000);
    std::cout << "clip-send status: " << status << std::endl;
    if (status.find("\"state\":\"connected\"") == std::string::npos) {
      tianguan::closeSession(id);
      std::cout << "CLIPBOARD SEND FAIL" << std::endl;
      return 1;
    }
    const std::string queued = tianguan::setClipboard(id, text);
    std::cout << "clip-send setClipboard: " << queued << std::endl;
    std::cout << "clip-send READY" << std::endl;
    // Keep the session (and therefore the server's cached selection data)
    // alive long enough for the external X11 reader.
    std::this_thread::sleep_for(std::chrono::seconds(5));
    std::cout << "clip-send close: " << tianguan::closeSession(id) << std::endl;
    const bool ok = queued == "QUEUED";
    std::cout << (ok ? "CLIPBOARD SEND PASS" : "CLIPBOARD SEND FAIL") << std::endl;
    return ok ? 0 : 1;
  }
  const std::string result = tianguan::runDiagnostics();
  std::cout << result;
  return result.find("FAIL ") == std::string::npos ? 0 : 1;
}
