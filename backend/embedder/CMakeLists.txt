# thelife-embedder (backend_lld.md §2.1) and its tokenizer.
find_path(SENTENCEPIECE_INCLUDE_DIR sentencepiece_processor.h REQUIRED)
find_library(SENTENCEPIECE_LIBRARY sentencepiece REQUIRED)
find_package(jsoncpp CONFIG REQUIRED)
# vcpkg's sentencepiece is a plain static library built against protobuf and Abseil.
find_package(protobuf CONFIG REQUIRED)
find_package(absl CONFIG REQUIRED)

add_library(thelife_tokenizer STATIC tokenizer.cc)
target_include_directories(thelife_tokenizer PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} PRIVATE ${SENTENCEPIECE_INCLUDE_DIR})
target_link_libraries(thelife_tokenizer PRIVATE ${SENTENCEPIECE_LIBRARY} protobuf::libprotobuf absl::flags
                      absl::flags_parse absl::log absl::check absl::strings thelife_warnings)

option(THELIFE_BUILD_EMBEDDER "Build thelife-embedder (fetches the ONNX Runtime GPU release, ~225 MB)" ON)
if(NOT THELIFE_BUILD_EMBEDDER)
  return()
endif()

# The official ONNX Runtime 1.26.0 linux-x64-gpu release: the last one built for CUDA 12 (spikes/embed/REPORT.md).
# THELIFE_ORT_ARCHIVE may point at an already downloaded copy; the hash is checked either way.
set(ORT_VERSION 1.26.0)
set(ORT_SHA256 cb7df7ee2ca0f962c7ce7c839aeae36223d146a91fb4646d62fb0046f297479f)
set(ORT_DIR "${CMAKE_BINARY_DIR}/_deps/onnxruntime-linux-x64-gpu-${ORT_VERSION}")
set(ORT_ARCHIVE "${CMAKE_BINARY_DIR}/_deps/onnxruntime-linux-x64-gpu-${ORT_VERSION}.tgz")
if(NOT EXISTS "${ORT_DIR}/lib/libonnxruntime.so.${ORT_VERSION}")
  if(DEFINED ENV{THELIFE_ORT_ARCHIVE} AND EXISTS "$ENV{THELIFE_ORT_ARCHIVE}")
    file(COPY_FILE "$ENV{THELIFE_ORT_ARCHIVE}" "${ORT_ARCHIVE}")
  elseif(NOT EXISTS "${ORT_ARCHIVE}")
    message(STATUS "Downloading ONNX Runtime ${ORT_VERSION} (GPU)")
    file(DOWNLOAD
      "https://github.com/microsoft/onnxruntime/releases/download/v${ORT_VERSION}/onnxruntime-linux-x64-gpu-${ORT_VERSION}.tgz"
      "${ORT_ARCHIVE}" SHOW_PROGRESS STATUS _st)
  endif()
  file(SHA256 "${ORT_ARCHIVE}" _sha)
  if(NOT _sha STREQUAL ORT_SHA256)
    file(REMOVE "${ORT_ARCHIVE}")
    message(FATAL_ERROR "ONNX Runtime archive hash mismatch: ${_sha}")
  endif()
  file(ARCHIVE_EXTRACT INPUT "${ORT_ARCHIVE}" DESTINATION "${CMAKE_BINARY_DIR}/_deps")
endif()

add_library(onnxruntime SHARED IMPORTED)
set_target_properties(onnxruntime PROPERTIES
  IMPORTED_LOCATION "${ORT_DIR}/lib/libonnxruntime.so.${ORT_VERSION}"
  IMPORTED_SONAME "libonnxruntime.so.1"
  INTERFACE_INCLUDE_DIRECTORIES "${ORT_DIR}/include")

add_library(thelife_embed_model STATIC model.cc)
target_include_directories(thelife_embed_model PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(thelife_embed_model PUBLIC onnxruntime PRIVATE thelife_tokenizer thelife_warnings)

add_executable(thelife-embedder main.cc)
target_link_libraries(thelife-embedder PRIVATE thelife_embed_model thelife_tokenizer JsonCpp::JsonCpp Threads::Threads
                      thelife_warnings)
# The CUDA provider library sits next to libonnxruntime and is dlopen'ed from there.
set_target_properties(thelife-embedder PROPERTIES BUILD_RPATH "${ORT_DIR}/lib" INSTALL_RPATH "${ORT_DIR}/lib")
