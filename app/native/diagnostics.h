// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <string>
namespace tianguan {
std::string runDiagnostics();
std::string runNetworkTest(const std::string& dataDir);
// Live session: keeps one VNC connection open, rewrites snapshot.jpg as frames
// arrive, and forwards pointer events from the UI.
std::string startSession(const std::string& dataDir);
std::string stopSession();
std::string sessionStatus();
std::string sendPointer(int x, int y, int mask, bool flush = false);
std::string sendKey(int keysym, int pressed, bool flush = false);
// Ask the session to encode a complete frame next time so a fresh view is
// fully painted instead of only the recently changed tiles.
std::string requestFullFrame();
// Copy the last changed rectangle (RGBA8888) or the cursor image (RGBA8888)
// into the caller's buffer; returns the number of bytes written.
size_t copyRegion(void* dst, size_t capacity);
size_t copyCursor(void* dst, size_t capacity);
// Copy the pending changed region together with its rectangle in one step.
// Returns "x,y,w,h", or "NONE" when there is nothing new to draw.
std::string takeRegion(void* dst, size_t capacity);
}
