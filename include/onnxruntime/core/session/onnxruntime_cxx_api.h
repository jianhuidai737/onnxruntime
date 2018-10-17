// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#pragma once
#include "onnxruntime_c_api.h"
#include <vector>
#include <string>
#include <stdexcept>
#include <memory>

//TODO: encode error code in the message?
#define ONNXRUNTIME_TRHOW_ON_ERROR(expr)                                                \
  do {                                                                                  \
    auto onnx_status = (expr);                                                          \
    if (onnx_status != nullptr) {                                                       \
      std::string onnx_runtime_error_message = ONNXRuntimeGetErrorMessage(onnx_status); \
      ReleaseONNXStatus(onnx_status);                                                   \
      throw std::runtime_error(onnx_runtime_error_message);                             \
    }                                                                                   \
  } while (0);

#define ONNXRUNTIME_REDIRECT_SIMPLE_FUNCTION_CALL(NAME)    \
  decltype(ONNXRuntime##NAME(value.get())) NAME() { \
    return ONNXRuntime##NAME(value.get());          \
  }

namespace onnxruntime {
class SessionOptionsWrapper {
 private:
  std::unique_ptr<ONNXRuntimeSessionOptions, decltype(&ONNXRuntimeReleaseObject)> value;
  ONNXEnvPtr env_;
  SessionOptionsWrapper(_In_ ONNXEnvPtr env, ONNXRuntimeSessionOptions* p) : value(p, ONNXRuntimeReleaseObject), env_(env){};

 public:
  SessionOptionsWrapper(_In_ ONNXEnvPtr env) : value(ONNXRuntimeCreateSessionOptions(), ONNXRuntimeReleaseObject), env_(env){};
  ONNXRUNTIME_REDIRECT_SIMPLE_FUNCTION_CALL(EnableSequentialExecution)
  ONNXRUNTIME_REDIRECT_SIMPLE_FUNCTION_CALL(DisableSequentialExecution)
  ONNXRUNTIME_REDIRECT_SIMPLE_FUNCTION_CALL(DisableProfiling)
  ONNXRUNTIME_REDIRECT_SIMPLE_FUNCTION_CALL(EnableMemPattern)
  ONNXRUNTIME_REDIRECT_SIMPLE_FUNCTION_CALL(DisableMemPattern)
  ONNXRUNTIME_REDIRECT_SIMPLE_FUNCTION_CALL(EnableCpuMemArena)
  ONNXRUNTIME_REDIRECT_SIMPLE_FUNCTION_CALL(DisableCpuMemArena)
  void EnableProfiling(_In_ const char* profile_file_prefix) {
    ONNXRuntimeEnableProfiling(value.get(), profile_file_prefix);
  }

  void SetSessionLogId(const char* logid) {
    ONNXRuntimeSetSessionLogId(value.get(), logid);
  }
  void SetSessionLogVerbosityLevel(uint32_t session_log_verbosity_level) {
    ONNXRuntimeSetSessionLogVerbosityLevel(value.get(), session_log_verbosity_level);
  }
  void SetSessionThreadPoolSize(int session_thread_pool_size) {
    ONNXRuntimeSetSessionThreadPoolSize(value.get(), session_thread_pool_size);
  }

  /**
  * The order of invocation indicates the preference order as well. In other words call this method
  * on your most preferred execution provider first followed by the less preferred ones.
  * Calling this API is optional in which case onnxruntime will use its internal CPU execution provider.
  */
  void AppendExecutionProvider(_In_ ONNXRuntimeProviderFactoryPtr* f) {
    ONNXRuntimeSessionOptionsAppendExecutionProvider(value.get(), f);
  }

  SessionOptionsWrapper clone() const {
    ONNXRuntimeSessionOptions* p = ONNXRuntimeCloneSessionOptions(value.get());
    return SessionOptionsWrapper(env_, p);
  }
#ifdef _WIN32
  ONNXSessionPtr ONNXRuntimeCreateInferenceSession(_In_ const wchar_t* model_path) {
    ONNXSessionPtr ret;
    ONNXRUNTIME_TRHOW_ON_ERROR(::ONNXRuntimeCreateInferenceSession(env_, model_path, value.get(), &ret));
    return ret;
  }
#else
  ONNXSessionPtr ONNXRuntimeCreateInferenceSession(_In_ const char* model_path) {
    ONNXSessionPtr ret;
    ONNXRUNTIME_TRHOW_ON_ERROR(::ONNXRuntimeCreateInferenceSession(env_, model_path, value.get(), &ret));
    return ret;
  }
#endif
  void AddCustomOp(_In_ const char* custom_op_path) {
    ONNXRuntimeAddCustomOp(value.get(), custom_op_path);
  }
};
inline ONNXValuePtr ONNXRuntimeCreateTensorAsONNXValue(_Inout_ ONNXRuntimeAllocator* env, const std::vector<size_t>& shape, OnnxRuntimeTensorElementDataType type) {
  ONNXValuePtr ret;
  ONNXRUNTIME_TRHOW_ON_ERROR(::ONNXRuntimeCreateTensorAsONNXValue(env, shape.data(), shape.size(), type, &ret));
  return ret;
}

inline ONNXValuePtr ONNXRuntimeCreateTensorWithDataAsONNXValue(_In_ const ONNXRuntimeAllocatorInfo* info, _In_ void* p_data, size_t p_data_len, const std::vector<size_t>& shape, OnnxRuntimeTensorElementDataType type) {
  ONNXValuePtr ret;
  ONNXRUNTIME_TRHOW_ON_ERROR(::ONNXRuntimeCreateTensorWithDataAsONNXValue(info, p_data, p_data_len, shape.data(), shape.size(), type, &ret));
  return ret;
}

}  // namespace onnxruntime

#undef ONNXRUNTIME_REDIRECT_SIMPLE_FUNCTION_CALL