// SPDX-License-Identifier: GPL-3.0-or-later
#include "diagnostics.h"
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <thread>
int main(int argc, char** argv) {
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
    std::cout << "status: " << tianguan::sessionStatus() << std::endl;
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
  const std::string result = tianguan::runDiagnostics();
  std::cout << result;
  return result.find("FAIL ") == std::string::npos ? 0 : 1;
}
