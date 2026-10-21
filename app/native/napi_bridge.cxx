// SPDX-License-Identifier: GPL-3.0-or-later
#include "diagnostics.h"
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
  napi_value value;
  napi_create_string_utf8(env, text.c_str(), text.size(), &value);
  return value;
}
napi_value StartSession(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok || argc < 1)
    return MakeString(env, "FAIL session: missing files directory");
  size_t length = 0;
  if (napi_get_value_string_utf8(env, argv[0], nullptr, 0, &length) != napi_ok || length > 4096)
    return MakeString(env, "FAIL session: invalid directory");
  std::string dir(length, '\0');
  size_t copied = 0;
  if (napi_get_value_string_utf8(env, argv[0], &dir[0], length + 1, &copied) != napi_ok)
    return MakeString(env, "FAIL session: invalid directory");
  dir.resize(copied);
  return MakeString(env, tianguan::startSession(dir));
}
napi_value StopSession(napi_env env, napi_callback_info) {
  return MakeString(env, tianguan::stopSession());
}
napi_value SessionStatus(napi_env env, napi_callback_info) {
  return MakeString(env, tianguan::sessionStatus());
}
napi_value SendPointer(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3];
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok || argc < 3)
    return MakeString(env, "FAIL pointer: need x, y, mask");
  int32_t x = 0, y = 0, mask = 0;
  if (napi_get_value_int32(env, argv[0], &x) != napi_ok ||
      napi_get_value_int32(env, argv[1], &y) != napi_ok ||
      napi_get_value_int32(env, argv[2], &mask) != napi_ok)
    return MakeString(env, "FAIL pointer: invalid arguments");
  return MakeString(env, tianguan::sendPointer(x, y, mask, true));
}
napi_value SendKey(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok || argc < 2)
    return MakeString(env, "FAIL key: need keysym, pressed");
  int32_t keysym = 0, pressed = 0;
  if (napi_get_value_int32(env, argv[0], &keysym) != napi_ok ||
      napi_get_value_int32(env, argv[1], &pressed) != napi_ok)
    return MakeString(env, "FAIL key: invalid arguments");
  return MakeString(env, tianguan::sendKey(keysym, pressed, true));
}
napi_value RequestFullFrame(napi_env env, napi_callback_info) {
  return MakeString(env, tianguan::requestFullFrame());
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
  size_t argc = 1;
  napi_value argv[1];
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok || argc < 1)
    return MakeString(env, "FAIL cursor: need buffer");
  void* data = nullptr;
  size_t length = 0;
  if (napi_get_arraybuffer_info(env, argv[0], &data, &length) != napi_ok)
    return MakeString(env, "FAIL cursor: not an ArrayBuffer");
  return MakeString(env, "OK " + std::to_string(tianguan::copyCursor(data, length)));
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
    {"takeRegion", nullptr, TakeRegion, nullptr, nullptr, nullptr, napi_default, nullptr}
  };
  if (napi_define_properties(env, exports, 11, properties) != napi_ok) return nullptr;
  return exports;
}
napi_module module = {1, 0, nullptr, Init, "tianguan_diag", nullptr, {0}};
}
extern "C" __attribute__((constructor, visibility("default"))) void RegisterTianguanDiag() {
  napi_module_register(&module);
}
