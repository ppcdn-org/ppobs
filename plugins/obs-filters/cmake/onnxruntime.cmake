set(ONNXRUNTIME_VERSION "1.23.2" CACHE STRING "ONNX Runtime version")
set(ONNXRUNTIME_ROOT "" CACHE PATH "Root of the ONNX Runtime NuGet package")

# Fall back to the conventional location under .deps, which is where the
# unpacked NuGet package lands.  Without this the runtime has to be pointed at
# explicitly, and forgetting to do so silently compiles every inference path
# out - the plugin still builds and loads, but background removal
# becomes passthrough with only a log line to say why.
if(NOT ONNXRUNTIME_ROOT)
  set(_onnxruntime_default "${CMAKE_SOURCE_DIR}/.deps/onnxruntime-${ONNXRUNTIME_VERSION}")
  if(EXISTS "${_onnxruntime_default}/build/native/include/onnxruntime_c_api.h")
    set(ONNXRUNTIME_ROOT "${_onnxruntime_default}")
    message(STATUS "Found ONNX Runtime ${ONNXRUNTIME_VERSION} in .deps")
  endif()
  unset(_onnxruntime_default)
endif()

if(NOT ONNXRUNTIME_ROOT)
  message(
    STATUS
    "ONNX Runtime not configured; background removal will be unavailable. "
    "Unpack the Microsoft.ML.OnnxRuntime ${ONNXRUNTIME_VERSION} NuGet package to "
    ".deps/onnxruntime-${ONNXRUNTIME_VERSION}, or set ONNXRUNTIME_ROOT."
  )
  return()
endif()

set(ONNXRUNTIME_INCLUDE_DIR "${ONNXRUNTIME_ROOT}/build/native/include")
set(ONNXRUNTIME_DLL "${ONNXRUNTIME_ROOT}/runtimes/win-x64/native/onnxruntime.dll")

if(NOT EXISTS "${ONNXRUNTIME_INCLUDE_DIR}/onnxruntime_c_api.h" OR NOT EXISTS "${ONNXRUNTIME_DLL}")
  message(FATAL_ERROR "ONNXRUNTIME_ROOT does not contain the pinned Windows x64 ONNX Runtime ${ONNXRUNTIME_VERSION} package")
endif()

# A plain INTERFACE library rather than an IMPORTED one: OBS walks the link
# dependencies of every target while bundling the frontend, and an IMPORTED
# target is not visible outside the directory that declares it.
add_library(obs-onnxruntime INTERFACE)
add_library(OBS::onnxruntime ALIAS obs-onnxruntime)
target_include_directories(obs-onnxruntime INTERFACE "${ONNXRUNTIME_INCLUDE_DIR}")
