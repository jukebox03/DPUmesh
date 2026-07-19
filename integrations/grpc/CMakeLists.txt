cmake_minimum_required(VERSION 3.20)

project(grpc_dpumesh VERSION 0.2.0 LANGUAGES C CXX)

include(CTest)

set(DPUMESH_GRPC_REQUIRED_VERSION "1.80.0")
set(DPUMESH_GRPC_SOURCE_DIR "" CACHE PATH
    "Path to an exact gRPC v1.80.0 source checkout")
set(DPUMESH_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." CACHE PATH
    "Path to the DPUmesh source tree")
option(DPUMESH_GRPC_ENABLE_SANITIZERS
       "Enable AddressSanitizer and UndefinedBehaviorSanitizer for adapter tests"
       OFF)
option(DPUMESH_GRPC_ENABLE_TSAN
       "Enable ThreadSanitizer for adapter tests"
       OFF)
option(DPUMESH_GRPC_BUILD_QPS_BENCHMARK
       "Build the grpc.testing.BenchmarkService unary QPS harness"
       ON)

if(DPUMESH_GRPC_ENABLE_SANITIZERS AND DPUMESH_GRPC_ENABLE_TSAN)
  message(FATAL_ERROR
    "ASan/UBSan and ThreadSanitizer must use separate build configurations")
endif()

if(DPUMESH_GRPC_SOURCE_DIR)
  if(NOT EXISTS "${DPUMESH_GRPC_SOURCE_DIR}/include/grpc/event_engine/event_engine.h")
    message(FATAL_ERROR
      "DPUMESH_GRPC_SOURCE_DIR does not contain the EventEngine headers")
  endif()

  file(STRINGS "${DPUMESH_GRPC_SOURCE_DIR}/CMakeLists.txt"
       _grpc_version_line REGEX "^set\\(PACKAGE_VERSION[ \\t]+\"[^\"]+\"\\)")
  string(REGEX REPLACE ".*\"([^\"]+)\".*" "\\1"
         _grpc_source_version "${_grpc_version_line}")
  if(NOT _grpc_source_version VERSION_EQUAL DPUMESH_GRPC_REQUIRED_VERSION)
    message(FATAL_ERROR
      "gRPC ${DPUMESH_GRPC_REQUIRED_VERSION} is required; source is ${_grpc_source_version}")
  endif()

  set(gRPC_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  if(DPUMESH_GRPC_BUILD_QPS_BENCHMARK)
    set(gRPC_BUILD_CODEGEN ON CACHE BOOL "" FORCE)
    set(gRPC_BUILD_GRPC_CPP_PLUGIN ON CACHE BOOL "" FORCE)
    set(gRPC_BUILD_GRPC_CSHARP_PLUGIN OFF CACHE BOOL "" FORCE)
    set(gRPC_BUILD_GRPC_NODE_PLUGIN OFF CACHE BOOL "" FORCE)
    set(gRPC_BUILD_GRPC_OBJECTIVE_C_PLUGIN OFF CACHE BOOL "" FORCE)
    set(gRPC_BUILD_GRPC_PHP_PLUGIN OFF CACHE BOOL "" FORCE)
    set(gRPC_BUILD_GRPC_PYTHON_PLUGIN OFF CACHE BOOL "" FORCE)
    set(gRPC_BUILD_GRPC_RUBY_PLUGIN OFF CACHE BOOL "" FORCE)
  else()
    set(gRPC_BUILD_CODEGEN OFF CACHE BOOL "" FORCE)
  endif()
  set(gRPC_INSTALL OFF CACHE BOOL "" FORCE)
  set(gRPC_SSL_PROVIDER package CACHE STRING "" FORCE)
  set(gRPC_ZLIB_PROVIDER package CACHE STRING "" FORCE)
  set(gRPC_ABSL_PROVIDER module CACHE STRING "" FORCE)
  set(gRPC_CARES_PROVIDER module CACHE STRING "" FORCE)
  set(gRPC_PROTOBUF_PROVIDER module CACHE STRING "" FORCE)
  set(gRPC_RE2_PROVIDER module CACHE STRING "" FORCE)
  set(protobuf_INSTALL OFF CACHE BOOL "" FORCE)
  set(protobuf_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(utf8_range_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
  set(utf8_range_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
  set(RE2_BUILD_TESTING OFF CACHE BOOL "" FORCE)
  add_subdirectory("${DPUMESH_GRPC_SOURCE_DIR}"
                   "${CMAKE_CURRENT_BINARY_DIR}/third_party/grpc"
                   EXCLUDE_FROM_ALL)
  if(TARGET grpc++ AND NOT TARGET gRPC::grpc++)
    add_library(gRPC::grpc++ ALIAS grpc++)
  endif()
  if(TARGET grpc AND NOT TARGET gRPC::grpc)
    add_library(gRPC::grpc ALIAS grpc)
  endif()
else()
  find_package(gRPC ${DPUMESH_GRPC_REQUIRED_VERSION} EXACT CONFIG REQUIRED)
endif()

find_package(Threads REQUIRED)

add_library(grpc_dpumesh_endpoint STATIC
  src/dmesh_endpoint.cc
  src/dmesh_grpc_channel.cc)
target_compile_features(grpc_dpumesh_endpoint PUBLIC cxx_std_17)
target_compile_options(grpc_dpumesh_endpoint PRIVATE -Wall -Wextra)
target_include_directories(grpc_dpumesh_endpoint
  PUBLIC "${CMAKE_CURRENT_LIST_DIR}/include"
  PRIVATE "${CMAKE_CURRENT_LIST_DIR}/src")
target_link_libraries(grpc_dpumesh_endpoint
  PUBLIC gRPC::grpc++
  PRIVATE Threads::Threads)

if(DPUMESH_GRPC_ENABLE_SANITIZERS)
  target_compile_options(grpc_dpumesh_endpoint PRIVATE
    -fsanitize=address,undefined -fno-omit-frame-pointer)
endif()
if(DPUMESH_GRPC_ENABLE_TSAN)
  target_compile_options(grpc_dpumesh_endpoint PRIVATE
    -fsanitize=thread -fno-omit-frame-pointer)
endif()

find_path(DPUMESH_INCLUDE_DIR dpumesh/dmesh.h
  HINTS "${DPUMESH_ROOT}/include")
find_library(DPUMESH_LIBRARY NAMES dpumesh
  HINTS "${DPUMESH_ROOT}/build/lib")

if(DPUMESH_INCLUDE_DIR)
  add_library(grpc_dpumesh_reactor STATIC
    src/dmesh_reactor.cc
    src/dmesh_runtime.cc
    src/dmesh_grpc_runtime.cc)
  target_compile_features(grpc_dpumesh_reactor PUBLIC cxx_std_17)
  target_compile_options(grpc_dpumesh_reactor PRIVATE -Wall -Wextra)
  target_include_directories(grpc_dpumesh_reactor
    PUBLIC "${CMAKE_CURRENT_LIST_DIR}/include"
    PRIVATE
      "${CMAKE_CURRENT_LIST_DIR}/src"
      "${DPUMESH_INCLUDE_DIR}")
  target_link_libraries(grpc_dpumesh_reactor
    PUBLIC grpc_dpumesh_endpoint
    PRIVATE Threads::Threads)
  if(DPUMESH_GRPC_ENABLE_SANITIZERS)
    target_compile_options(grpc_dpumesh_reactor PRIVATE
      -fsanitize=address,undefined -fno-omit-frame-pointer)
  endif()
  if(DPUMESH_GRPC_ENABLE_TSAN)
    target_compile_options(grpc_dpumesh_reactor PRIVATE
      -fsanitize=thread -fno-omit-frame-pointer)
  endif()
else()
  message(STATUS
    "DPUmesh public headers were not found; the reactor target will not be built")
endif()

if(DPUMESH_INCLUDE_DIR AND DPUMESH_LIBRARY)
  add_library(dpumesh_native UNKNOWN IMPORTED)
  set_target_properties(dpumesh_native PROPERTIES
    IMPORTED_LOCATION "${DPUMESH_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${DPUMESH_INCLUDE_DIR}")

  add_library(grpc_dpumesh_native STATIC src/dmesh_api_ops.cc)
  target_compile_features(grpc_dpumesh_native PUBLIC cxx_std_17)
  target_compile_options(grpc_dpumesh_native PRIVATE -Wall -Wextra)
  target_include_directories(grpc_dpumesh_native
    PUBLIC "${CMAKE_CURRENT_LIST_DIR}/include"
    PRIVATE "${CMAKE_CURRENT_LIST_DIR}/src")
  target_link_libraries(grpc_dpumesh_native PUBLIC dpumesh_native)

  add_library(grpc_dpumesh INTERFACE)
  target_link_libraries(grpc_dpumesh INTERFACE
    grpc_dpumesh_endpoint grpc_dpumesh_reactor grpc_dpumesh_native)

  if(DPUMESH_GRPC_BUILD_QPS_BENCHMARK AND DPUMESH_GRPC_SOURCE_DIR)
    set(DPUMESH_QPS_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/qps_gen")
    set(DPUMESH_QPS_MESSAGES_PROTO
        "${DPUMESH_GRPC_SOURCE_DIR}/src/proto/grpc/testing/messages.proto")
    set(DPUMESH_QPS_SERVICE_PROTO
        "${DPUMESH_GRPC_SOURCE_DIR}/src/proto/grpc/testing/benchmark_service.proto")
    set(DPUMESH_QPS_GENERATED
      "${DPUMESH_QPS_GEN_DIR}/src/proto/grpc/testing/messages.pb.cc"
      "${DPUMESH_QPS_GEN_DIR}/src/proto/grpc/testing/messages.pb.h"
      "${DPUMESH_QPS_GEN_DIR}/src/proto/grpc/testing/benchmark_service.pb.cc"
      "${DPUMESH_QPS_GEN_DIR}/src/proto/grpc/testing/benchmark_service.pb.h"
      "${DPUMESH_QPS_GEN_DIR}/src/proto/grpc/testing/benchmark_service.grpc.pb.cc"
      "${DPUMESH_QPS_GEN_DIR}/src/proto/grpc/testing/benchmark_service.grpc.pb.h")

    add_custom_command(
      OUTPUT ${DPUMESH_QPS_GENERATED}
      COMMAND "${CMAKE_COMMAND}" -E make_directory
              "${DPUMESH_QPS_GEN_DIR}/src/proto/grpc/testing"
      COMMAND $<TARGET_FILE:protoc>
      ARGS
        "--proto_path=${DPUMESH_GRPC_SOURCE_DIR}"
        "--cpp_out=${DPUMESH_QPS_GEN_DIR}"
        "--grpc_out=${DPUMESH_QPS_GEN_DIR}"
        "--plugin=protoc-gen-grpc=$<TARGET_FILE:grpc_cpp_plugin>"
        "src/proto/grpc/testing/messages.proto"
        "src/proto/grpc/testing/benchmark_service.proto"
      DEPENDS
        protoc grpc_cpp_plugin
        "${DPUMESH_QPS_MESSAGES_PROTO}"
        "${DPUMESH_QPS_SERVICE_PROTO}"
      WORKING_DIRECTORY "${DPUMESH_GRPC_SOURCE_DIR}"
      COMMENT "Generating the official gRPC QPS BenchmarkService protocol"
      VERBATIM)

    add_library(grpc_dpumesh_qps_proto STATIC
      "${DPUMESH_QPS_GEN_DIR}/src/proto/grpc/testing/messages.pb.cc"
      "${DPUMESH_QPS_GEN_DIR}/src/proto/grpc/testing/benchmark_service.pb.cc"
      "${DPUMESH_QPS_GEN_DIR}/src/proto/grpc/testing/benchmark_service.grpc.pb.cc")
    target_compile_features(grpc_dpumesh_qps_proto PUBLIC cxx_std_17)
    target_include_directories(grpc_dpumesh_qps_proto
      PUBLIC "${DPUMESH_QPS_GEN_DIR}")
    target_link_libraries(grpc_dpumesh_qps_proto PUBLIC gRPC::grpc++)

    add_executable(grpc_dpumesh_qps_benchmark
      bench/qps_benchmark.cc)
    target_compile_features(grpc_dpumesh_qps_benchmark PRIVATE cxx_std_17)
    target_compile_options(grpc_dpumesh_qps_benchmark PRIVATE -Wall -Wextra)
    target_include_directories(grpc_dpumesh_qps_benchmark PRIVATE
      "${CMAKE_CURRENT_LIST_DIR}/src")
    target_link_libraries(grpc_dpumesh_qps_benchmark PRIVATE
      grpc_dpumesh grpc_dpumesh_qps_proto Threads::Threads)
  endif()
else()
  message(STATUS
    "libdpumesh was not found; the native wrapper target will not be built")
endif()

if(BUILD_TESTING)
  add_executable(grpc_dpumesh_endpoint_test
    tests/endpoint_test.cc
    tests/fake_endpoint_transport.cc)
  target_compile_features(grpc_dpumesh_endpoint_test PRIVATE cxx_std_17)
  target_compile_options(grpc_dpumesh_endpoint_test
    PRIVATE -Wall -Wextra)
  target_include_directories(grpc_dpumesh_endpoint_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/src"
    "${CMAKE_CURRENT_LIST_DIR}/tests")
  target_link_libraries(grpc_dpumesh_endpoint_test
    PRIVATE grpc_dpumesh_endpoint Threads::Threads)
  if(DPUMESH_GRPC_ENABLE_SANITIZERS)
    target_compile_options(grpc_dpumesh_endpoint_test PRIVATE
      -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(grpc_dpumesh_endpoint_test PRIVATE
      -fsanitize=address,undefined)
  endif()
  if(DPUMESH_GRPC_ENABLE_TSAN)
    target_compile_options(grpc_dpumesh_endpoint_test PRIVATE
      -fsanitize=thread -fno-omit-frame-pointer)
    target_link_options(grpc_dpumesh_endpoint_test PRIVATE
      -fsanitize=thread)
  endif()
  add_test(NAME grpc_dpumesh_endpoint_test
           COMMAND grpc_dpumesh_endpoint_test)

  add_executable(grpc_dpumesh_channel_test
    tests/grpc_channel_test.cc)
  target_compile_features(grpc_dpumesh_channel_test PRIVATE cxx_std_17)
  target_compile_options(grpc_dpumesh_channel_test PRIVATE -Wall -Wextra)
  target_include_directories(grpc_dpumesh_channel_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/src")
  target_link_libraries(grpc_dpumesh_channel_test PRIVATE
    grpc_dpumesh_endpoint Threads::Threads)
  if(DPUMESH_GRPC_ENABLE_SANITIZERS)
    target_compile_options(grpc_dpumesh_channel_test PRIVATE
      -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(grpc_dpumesh_channel_test PRIVATE
      -fsanitize=address,undefined)
  endif()
  if(DPUMESH_GRPC_ENABLE_TSAN)
    target_compile_options(grpc_dpumesh_channel_test PRIVATE
      -fsanitize=thread -fno-omit-frame-pointer)
    target_link_options(grpc_dpumesh_channel_test PRIVATE
      -fsanitize=thread)
  endif()
  add_test(NAME grpc_dpumesh_channel_test
           COMMAND grpc_dpumesh_channel_test)

  if(DPUMESH_INCLUDE_DIR)
    add_executable(grpc_dpumesh_reactor_test
      tests/reactor_test.cc
      tests/fake_dmesh_ops.cc
      tests/fake_endpoint_transport.cc)
    target_compile_features(grpc_dpumesh_reactor_test PRIVATE cxx_std_17)
    target_compile_options(grpc_dpumesh_reactor_test
      PRIVATE -Wall -Wextra)
    target_include_directories(grpc_dpumesh_reactor_test PRIVATE
      "${CMAKE_CURRENT_LIST_DIR}/src"
      "${CMAKE_CURRENT_LIST_DIR}/tests"
      "${DPUMESH_INCLUDE_DIR}")
    target_link_libraries(grpc_dpumesh_reactor_test PRIVATE
      grpc_dpumesh_reactor Threads::Threads)
    if(DPUMESH_GRPC_ENABLE_SANITIZERS)
      target_compile_options(grpc_dpumesh_reactor_test PRIVATE
        -fsanitize=address,undefined -fno-omit-frame-pointer)
      target_link_options(grpc_dpumesh_reactor_test PRIVATE
        -fsanitize=address,undefined)
    endif()
    if(DPUMESH_GRPC_ENABLE_TSAN)
      target_compile_options(grpc_dpumesh_reactor_test PRIVATE
        -fsanitize=thread -fno-omit-frame-pointer)
      target_link_options(grpc_dpumesh_reactor_test PRIVATE
        -fsanitize=thread)
    endif()
    add_test(NAME grpc_dpumesh_reactor_test
             COMMAND grpc_dpumesh_reactor_test)
  endif()

  if(DPUMESH_INCLUDE_DIR AND DPUMESH_LIBRARY)
    add_executable(grpc_dpumesh_native_link_test
      tests/native_api_link_test.cc)
    target_compile_features(grpc_dpumesh_native_link_test PRIVATE cxx_std_17)
    target_include_directories(grpc_dpumesh_native_link_test PRIVATE
      "${CMAKE_CURRENT_LIST_DIR}/src")
    target_link_libraries(grpc_dpumesh_native_link_test
      PRIVATE grpc_dpumesh)
    if(DPUMESH_GRPC_ENABLE_SANITIZERS)
      target_compile_options(grpc_dpumesh_native_link_test PRIVATE
        -fsanitize=address,undefined -fno-omit-frame-pointer)
      target_link_options(grpc_dpumesh_native_link_test PRIVATE
        -fsanitize=address,undefined)
    endif()
    if(DPUMESH_GRPC_ENABLE_TSAN)
      target_compile_options(grpc_dpumesh_native_link_test PRIVATE
        -fsanitize=thread -fno-omit-frame-pointer)
      target_link_options(grpc_dpumesh_native_link_test PRIVATE
        -fsanitize=thread)
    endif()
    add_test(NAME grpc_dpumesh_native_link_test
             COMMAND grpc_dpumesh_native_link_test)

    # This executable deliberately is not registered with CTest. It opens a real
    # DPUmesh channel and therefore must only be run in a BlueField-enabled pod.
    add_executable(grpc_dpumesh_native_runtime_smoke
      tests/native_runtime_smoke.cc)
    target_compile_features(grpc_dpumesh_native_runtime_smoke PRIVATE cxx_std_17)
    target_compile_options(grpc_dpumesh_native_runtime_smoke
      PRIVATE -Wall -Wextra)
    target_include_directories(grpc_dpumesh_native_runtime_smoke PRIVATE
      "${CMAKE_CURRENT_LIST_DIR}/src")
    target_link_libraries(grpc_dpumesh_native_runtime_smoke
      PRIVATE grpc_dpumesh Threads::Threads)
    if(DPUMESH_GRPC_ENABLE_SANITIZERS)
      target_compile_options(grpc_dpumesh_native_runtime_smoke PRIVATE
        -fsanitize=address,undefined -fno-omit-frame-pointer)
      target_link_options(grpc_dpumesh_native_runtime_smoke PRIVATE
        -fsanitize=address,undefined)
    endif()
    if(DPUMESH_GRPC_ENABLE_TSAN)
      target_compile_options(grpc_dpumesh_native_runtime_smoke PRIVATE
        -fsanitize=thread -fno-omit-frame-pointer)
      target_link_options(grpc_dpumesh_native_runtime_smoke PRIVATE
        -fsanitize=thread)
    endif()

    # Manual two-pod hardware test. Run one copy in server mode and another in
    # client mode; unlike native_runtime_smoke this executes a real gRPC RPC.
    add_executable(grpc_dpumesh_native_grpc_smoke
      tests/native_grpc_smoke.cc)
    target_compile_features(grpc_dpumesh_native_grpc_smoke PRIVATE cxx_std_17)
    target_compile_options(grpc_dpumesh_native_grpc_smoke
      PRIVATE -Wall -Wextra)
    target_include_directories(grpc_dpumesh_native_grpc_smoke PRIVATE
      "${CMAKE_CURRENT_LIST_DIR}/src")
    target_link_libraries(grpc_dpumesh_native_grpc_smoke
      PRIVATE grpc_dpumesh Threads::Threads)
    if(DPUMESH_GRPC_ENABLE_SANITIZERS)
      target_compile_options(grpc_dpumesh_native_grpc_smoke PRIVATE
        -fsanitize=address,undefined -fno-omit-frame-pointer)
      target_link_options(grpc_dpumesh_native_grpc_smoke PRIVATE
        -fsanitize=address,undefined)
    endif()
    if(DPUMESH_GRPC_ENABLE_TSAN)
      target_compile_options(grpc_dpumesh_native_grpc_smoke PRIVATE
        -fsanitize=thread -fno-omit-frame-pointer)
      target_link_options(grpc_dpumesh_native_grpc_smoke PRIVATE
        -fsanitize=thread)
    endif()
  endif()
endif()
