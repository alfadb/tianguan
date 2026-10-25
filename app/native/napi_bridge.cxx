// SPDX-License-Identifier: GPL-3.0-or-later
#include "diagnostics.h"
#include "render.h"
#include <napi/native_api.h>
#include <exception>
#include <memory>
#include <string>
#include <utility>

namespace {
struct Work {
  napi_async_work work = nullptr;
  napi_deferred deferred = nullptr;
  std::string result;
  std::string directory;
  bool network = false;
  bool failed = false;
};
void Execute(napi_env, void* data) {
  Work* work = static_cast<Work*>(data);
  try {
    work->result = work->network ? tianguan::runNetworkTest(work->directory)
                                 : tianguan::runDiagnostics();
  } catch (const std::exception& error) {
    work->failed = true;
    work->result = std::string("Native diagnostics could not complete: ") + error.what();
  } catch (...) {
    work->failed = true;
    work->result = "Native diagnostics could not complete: unknown exception";
  }
}
void Complete(napi_env env, napi_status status, void* data) {
  std::unique_ptr<Work> work(static_cast<Work*>(data));
  napi_value value;
  if (status != napi_ok || work->failed) {
    napi_value message;
    const char* reason = work->failed ? work->result.c_str() : "Native diagnostics could not complete";
    napi_create_string_utf8(env, reason, NAPI_AUTO_LENGTH, &message);
    napi_create_error(env, nullptr, message, &value);
    napi_reject_deferred(env, work->deferred, value);
  } else {
    napi_create_string_utf8(env, work->result.c_str(), work->result.size(), &value);
    napi_resolve_deferred(env, work->deferred, value);
  }
  napi_delete_async_work(env, work->work);
}
napi_value Run(napi_env env, napi_callback_info) {
  std::unique_ptr<Work> work(new Work);
  napi_value promise, name;
  if (napi_create_promise(env, &work->deferred, &promise) != napi_ok) {
    napi_throw_error(env, nullptr, "Could not create diagnostics promise");
    return nullptr;
  }
  napi_create_string_utf8(env, "tianguan diagnostics", NAPI_AUTO_LENGTH, &name);
  napi_status status = napi_create_async_work(env, nullptr, name, Execute, Complete, work.get(), &work->work);
  if (status == napi_ok) status = napi_queue_async_work(env, work->work);
  if (status != napi_ok) {
    if (work->work) napi_delete_async_work(env, work->work);
    napi_value message, error;
    napi_create_string_utf8(env, "Could not queue native diagnostics", NAPI_AUTO_LENGTH, &message);
    napi_create_error(env, nullptr, message, &error);
    napi_reject_deferred(env, work->deferred, error);
  } else {
    work.release();
  }
  return promise;
}
napi_value RunNetwork(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_status args = napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  if (args != napi_ok || argc < 1) {
    napi_throw_error(env, nullptr, "runNetworkTest requires the application files directory");
    return nullptr;
  }
  size_t length = 0;
  if (napi_get_value_string_utf8(env, argv[0], nullptr, 0, &length) != napi_ok || length > 4096) {
    napi_throw_error(env, nullptr, "invalid directory argument");
    return nullptr;
  }
  std::unique_ptr<Work> work(new Work);
  std::string directory(length + 1, '\0');
  size_t copied = 0;
  if (napi_get_value_string_utf8(env, argv[0], &directory[0], directory.size(), &copied) != napi_ok) {
    napi_throw_error(env, nullptr, "invalid directory argument");
    return nullptr;
  }
  directory.resize(copied);
  work->directory = std::move(directory);
  work->network = true;
  napi_value promise, name;
  if (napi_create_promise(env, &work->deferred, &promise) != napi_ok) {
    napi_throw_error(env, nullptr, "Could not create network test promise");
    return nullptr;
  }
  napi_create_string_utf8(env, "tianguan network test", NAPI_AUTO_LENGTH, &name);
  napi_status status = napi_create_async_work(env, nullptr, name, Execute, Complete, work.get(), &work->work);
  if (status == napi_ok) status = napi_queue_async_work(env, work->work);
  if (status != napi_ok) {
    if (work->work) napi_delete_async_work(env, work->work);
    napi_value message, error;
    napi_create_string_utf8(env, "Could not queue network test", NAPI_AUTO_LENGTH, &message);
    napi_create_error(env, nullptr, message, &error);
    napi_reject_deferred(env, work->deferred, error);
  } else {
    work.release();
  }
  return promise;
}
napi_value MakeString(napi_env env, const std::string& text) {
  napi_value value = nullptr;
  // A clipboard string can be up to 1 MiB; never return an uninitialized value
  // if the engine refuses to allocate it.
  if (napi_create_string_utf8(env, text.c_str(), text.size(), &value) != napi_ok)
    napi_create_string_utf8(env, "", 0, &value);
  return value;
}
// Reads a UTF-8 string argument, rejecting lengths above the caller's limit so
// a huge ArkTS string can never force an unbounded native allocation.
bool GetStringArg(napi_env env, napi_value value, size_t limit, std::string& out) {
  size_t length = 0;
  if (napi_get_value_string_utf8(env, value, nullptr, 0, &length) != napi_ok) return false;
  if (length > limit) return false;
  out.assign(length + 1, '\0');
  size_t copied = 0;
  if (napi_get_value_string_utf8(env, value, &out[0], out.size(), &copied) != napi_ok) return false;
  out.resize(copied);
  return true;
}
bool GetIntArg(napi_env env, napi_value value, int& out) {
  int32_t number = 0;
  if (napi_get_value_int32(env, value, &number) != napi_ok) return false;
  out = number;
  return true;
}
bool GetDoubleArg(napi_env env, napi_value value, double& out) {
  if (napi_get_value_double(env, value, &out) != napi_ok) return false;
  return true;
}
napi_value StartSession(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok || argc < 1)
    return MakeString(env, "FAIL session: missing files directory");
  std::string dir;
  if (!GetStringArg(env, argv[0], 4096, dir))
    return MakeString(env, "FAIL session: invalid directory");
  return MakeString(env, tianguan::startSession(dir));
}
napi_value StopSession(napi_env env, napi_callback_info) {
  return MakeString(env, tianguan::stopSession());
}
napi_value SessionStatus(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok) argc = 0;
  if (argc >= 1) {
    std::string id;
    if (!GetStringArg(env, argv[0], 256, id))
      return MakeString(env, "{\"state\":\"failed\",\"error\":\"invalid session id\",\"errorCode\":\"none\"}");
    return MakeString(env, tianguan::sessionStatus(id));
  }
  // Zero arguments keeps the legacy text status the existing pages parse.
  return MakeString(env, tianguan::sessionStatus());
}
napi_value SendPointer(napi_env env, napi_callback_info info) {
  size_t argc = 4;
  napi_value argv[4];
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok)
    return MakeString(env, "FAIL pointer: invalid arguments");
  if (argc >= 4) {
    std::string id;
    int x = 0, y = 0, mask = 0;
    if (!GetStringArg(env, argv[0], 256, id) || !GetIntArg(env, argv[1], x) ||
        !GetIntArg(env, argv[2], y) || !GetIntArg(env, argv[3], mask))
      return MakeString(env, "INVALID_SESSION");
    return MakeString(env, tianguan::sendPointer(id, x, y, mask));
  }
  if (argc >= 3) {
    int x = 0, y = 0, mask = 0;
    if (!GetIntArg(env, argv[0], x) || !GetIntArg(env, argv[1], y) ||
        !GetIntArg(env, argv[2], mask))
      return MakeString(env, "FAIL pointer: invalid arguments");
    return MakeString(env, tianguan::sendPointer(x, y, mask, true));
  }
  return MakeString(env, "FAIL pointer: need x, y, mask");
}
napi_value SendKey(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3];
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok)
    return MakeString(env, "FAIL key: invalid arguments");
  if (argc >= 3) {
    std::string id;
    int keysym = 0, pressed = 0;
    if (!GetStringArg(env, argv[0], 256, id) || !GetIntArg(env, argv[1], keysym) ||
        !GetIntArg(env, argv[2], pressed))
      return MakeString(env, "INVALID_SESSION");
    return MakeString(env, tianguan::sendKey(id, keysym, pressed));
  }
  if (argc >= 2) {
    int keysym = 0, pressed = 0;
    if (!GetIntArg(env, argv[0], keysym) || !GetIntArg(env, argv[1], pressed))
      return MakeString(env, "FAIL key: invalid arguments");
    return MakeString(env, tianguan::sendKey(keysym, pressed, true));
  }
  return MakeString(env, "FAIL key: need keysym, pressed");
}
napi_value RequestFullFrame(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok) argc = 0;
  if (argc >= 1) {
    std::string id;
    if (!GetStringArg(env, argv[0], 256, id)) return MakeString(env, "INVALID_SESSION");
    return MakeString(env, tianguan::requestFullFrame(id));
  }
  return MakeString(env, tianguan::requestFullFrame());
}
napi_value CreateSession(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  std::string options;
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok || argc < 1 ||
      !GetStringArg(env, argv[0], 64 * 1024, options))
    return MakeString(env, "ERROR:invalid options");
  return MakeString(env, tianguan::createSession(options));
}
napi_value CloseSession(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  std::string id;
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok || argc < 1 ||
      !GetStringArg(env, argv[0], 256, id))
    return MakeString(env, "INVALID_SESSION");
  return MakeString(env, tianguan::closeSession(id));
}
// Non-blocking counterpart of closeSession(): never joins a session thread, so
// a quit flow can poll it from the UI thread without stalling.
napi_value PendingCloses(napi_env env, napi_callback_info) {
  napi_value value = nullptr;
  if (napi_create_uint32(env, static_cast<uint32_t>(tianguan::pendingCloses()), &value) != napi_ok)
    return nullptr;
  return value;
}
napi_value ListSessions(napi_env env, napi_callback_info) {
  return MakeString(env, tianguan::listSessions());
}
napi_value ReconnectSession(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  std::string id;
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok || argc < 1 ||
      !GetStringArg(env, argv[0], 256, id))
    return MakeString(env, "INVALID_SESSION");
  return MakeString(env, tianguan::reconnectSession(id));
}
napi_value TakeFrame(napi_env env, napi_callback_info info) {
  size_t argc = 4;
  napi_value argv[4];
  std::string id;
  void* data = nullptr;
  size_t length = 0;
  int afterVersion = 0;
  bool forceFull = false;
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok || argc < 4 ||
      !GetStringArg(env, argv[0], 256, id) ||
      napi_get_arraybuffer_info(env, argv[1], &data, &length) != napi_ok ||
      !GetIntArg(env, argv[2], afterVersion) ||
      napi_get_value_bool(env, argv[3], &forceFull) != napi_ok)
    return MakeString(env, "ERROR:INVALID_ARGUMENTS");
  return MakeString(env, tianguan::takeFrame(id, data, length, afterVersion, forceFull));
}
napi_value ReleaseSessionInput(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  std::string id;
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok || argc < 1 ||
      !GetStringArg(env, argv[0], 256, id))
    return MakeString(env, "INVALID_SESSION");
  return MakeString(env, tianguan::releaseSessionInput(id));
}
napi_value SetClipboard(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  std::string id;
  std::string text;
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok || argc < 2 ||
      !GetStringArg(env, argv[0], 256, id))
    return MakeString(env, "INVALID_SESSION");
  // 1 MiB is the contract limit; reject instead of silently truncating.
  if (!GetStringArg(env, argv[1], 1024 * 1024, text))
    return MakeString(env, "ERROR:TOO_LARGE");
  return MakeString(env, tianguan::setClipboard(id, text));
}
napi_value TakeClipboard(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  std::string id;
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok || argc < 1 ||
      !GetStringArg(env, argv[0], 256, id))
    return MakeString(env, "");
  return MakeString(env, tianguan::takeClipboard(id));
}
napi_value CopyRegion(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok || argc < 1)
    return MakeString(env, "FAIL region: need buffer");
  void* data = nullptr;
  size_t length = 0;
  if (napi_get_arraybuffer_info(env, argv[0], &data, &length) != napi_ok)
    return MakeString(env, "FAIL region: not an ArrayBuffer");
  return MakeString(env, "OK " + std::to_string(tianguan::copyRegion(data, length)));
}
napi_value TakeRegion(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok || argc < 1)
    return MakeString(env, "NONE");
  void* data = nullptr;
  size_t length = 0;
  if (napi_get_arraybuffer_info(env, argv[0], &data, &length) != napi_ok)
    return MakeString(env, "NONE");
  return MakeString(env, tianguan::takeRegion(data, length));
}
napi_value CopyCursor(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok)
    return MakeString(env, "FAIL cursor: invalid arguments");
  if (argc >= 2) {
    std::string id;
    void* data = nullptr;
    size_t length = 0;
    if (!GetStringArg(env, argv[0], 256, id))
      return MakeString(env, "INVALID_SESSION");
    if (napi_get_arraybuffer_info(env, argv[1], &data, &length) != napi_ok)
      return MakeString(env, "FAIL cursor: not an ArrayBuffer");
    return MakeString(env, "OK " + std::to_string(tianguan::copyCursorFor(id, data, length)));
  }
  if (argc >= 1) {
    void* data = nullptr;
    size_t length = 0;
    if (napi_get_arraybuffer_info(env, argv[0], &data, &length) != napi_ok)
      return MakeString(env, "FAIL cursor: not an ArrayBuffer");
    return MakeString(env, "OK " + std::to_string(tianguan::copyCursor(data, length)));
  }
  return MakeString(env, "FAIL cursor: need buffer");
}
// Remote-page native renderer (render.h): one EGL thread per session bound to
// that session's XComponent surface. The optional third argument starts the
// renderer parked (a surface created while the window is hidden).
napi_value AttachRenderSurface(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3];
  std::string sessionId;
  std::string surfaceId;
  bool startSuspended = false;
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok || argc < 2 ||
      !GetStringArg(env, argv[0], 256, sessionId) ||
      !GetStringArg(env, argv[1], 32, surfaceId))
    return MakeString(env, "ERROR:INVALID_ARGUMENTS");
  if (argc >= 3 && napi_get_value_bool(env, argv[2], &startSuspended) != napi_ok)
    startSuspended = false;
  return MakeString(env, tianguan::attachRenderSurface(sessionId, surfaceId, startSuspended));
}
napi_value DetachRenderSurface(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  std::string sessionId;
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok || argc < 1 ||
      !GetStringArg(env, argv[0], 256, sessionId))
    return MakeString(env, "INVALID_SESSION");
  return MakeString(env, tianguan::detachRenderSurface(sessionId));
}
napi_value RenderStats(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  std::string sessionId;
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok || argc < 1 ||
      !GetStringArg(env, argv[0], 256, sessionId))
    return MakeString(env, "ERROR:NO_RENDERER");
  return MakeString(env, tianguan::renderStats(sessionId));
}
napi_value SuspendRenderSurface(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  std::string sessionId;
  bool suspended = false;
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok || argc < 2 ||
      !GetStringArg(env, argv[0], 256, sessionId) ||
      napi_get_value_bool(env, argv[1], &suspended) != napi_ok)
    return MakeString(env, "INVALID_SESSION");
  return MakeString(env, tianguan::setRenderSurfaceSuspended(sessionId, suspended));
}
// View transform for the native renderer: zoom multiple of the contain fit
// (clamped natively to [1, maxZoom]) plus a pan offset in surface pixels,
// clamped by the renderer against the live surface size. Returns "OK" or an
// error string.
napi_value SetViewTransform(napi_env env, napi_callback_info info) {
  size_t argc = 4;
  napi_value argv[4];
  std::string sessionId;
  double zoom = 1.0;
  double panX = 0.0;
  double panY = 0.0;
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok || argc < 4 ||
      !GetStringArg(env, argv[0], 256, sessionId) || !GetDoubleArg(env, argv[1], zoom) ||
      !GetDoubleArg(env, argv[2], panX) || !GetDoubleArg(env, argv[3], panY))
    return MakeString(env, "ERROR:INVALID_ARGUMENTS");
  return MakeString(env, tianguan::setViewTransform(sessionId, zoom, panX, panY));
}
napi_value Init(napi_env env, napi_value exports) {
  napi_property_descriptor properties[] = {
    {"runDiagnostics", nullptr, Run, nullptr, nullptr, nullptr, napi_default, nullptr},
    {"runNetworkTest", nullptr, RunNetwork, nullptr, nullptr, nullptr, napi_default, nullptr},
    {"startSession", nullptr, StartSession, nullptr, nullptr, nullptr, napi_default, nullptr},
    {"stopSession", nullptr, StopSession, nullptr, nullptr, nullptr, napi_default, nullptr},
    {"sessionStatus", nullptr, SessionStatus, nullptr, nullptr, nullptr, napi_default, nullptr},
    {"sendPointer", nullptr, SendPointer, nullptr, nullptr, nullptr, napi_default, nullptr},
    {"sendKey", nullptr, SendKey, nullptr, nullptr, nullptr, napi_default, nullptr},
    {"requestFullFrame", nullptr, RequestFullFrame, nullptr, nullptr, nullptr, napi_default, nullptr},
    {"copyRegion", nullptr, CopyRegion, nullptr, nullptr, nullptr, napi_default, nullptr},
    {"copyCursor", nullptr, CopyCursor, nullptr, nullptr, nullptr, napi_default, nullptr},
    {"takeRegion", nullptr, TakeRegion, nullptr, nullptr, nullptr, napi_default, nullptr},
    {"createSession", nullptr, CreateSession, nullptr, nullptr, nullptr, napi_default, nullptr},
    {"closeSession", nullptr, CloseSession, nullptr, nullptr, nullptr, napi_default, nullptr},
    {"pendingCloses", nullptr, PendingCloses, nullptr, nullptr, nullptr, napi_default, nullptr},
    {"listSessions", nullptr, ListSessions, nullptr, nullptr, nullptr, napi_default, nullptr},
    {"reconnectSession", nullptr, ReconnectSession, nullptr, nullptr, nullptr, napi_default, nullptr},
    {"takeFrame", nullptr, TakeFrame, nullptr, nullptr, nullptr, napi_default, nullptr},
    {"releaseSessionInput", nullptr, ReleaseSessionInput, nullptr, nullptr, nullptr, napi_default, nullptr},
    {"setClipboard", nullptr, SetClipboard, nullptr, nullptr, nullptr, napi_default, nullptr},
    {"takeClipboard", nullptr, TakeClipboard, nullptr, nullptr, nullptr, napi_default, nullptr},
    {"attachRenderSurface", nullptr, AttachRenderSurface, nullptr, nullptr, nullptr, napi_default, nullptr},
    {"detachRenderSurface", nullptr, DetachRenderSurface, nullptr, nullptr, nullptr, napi_default, nullptr},
    {"renderStats", nullptr, RenderStats, nullptr, nullptr, nullptr, napi_default, nullptr},
    {"setViewTransform", nullptr, SetViewTransform, nullptr, nullptr, nullptr, napi_default, nullptr},
    {"suspendRenderSurface", nullptr, SuspendRenderSurface, nullptr, nullptr, nullptr, napi_default, nullptr}
  };
  if (napi_define_properties(env, exports, 25, properties) != napi_ok) return nullptr;
  return exports;
}
napi_module module = {1, 0, nullptr, Init, "tianguan_diag", nullptr, {0}};
}
extern "C" __attribute__((constructor, visibility("default"))) void RegisterTianguanDiag() {
  napi_module_register(&module);
}
