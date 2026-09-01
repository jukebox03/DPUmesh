# The README's gRPC bootstrap as a complete build: generate the proto,
# compile the two programs, link grpc_dpumesh. A standalone application's
# CMakeLists has exactly these three steps.
set(HELLO_GRPC_GEN "${CMAKE_CURRENT_BINARY_DIR}/gen")

add_custom_command(
  OUTPUT "${HELLO_GRPC_GEN}/echo.pb.cc" "${HELLO_GRPC_GEN}/echo.pb.h"
         "${HELLO_GRPC_GEN}/echo.grpc.pb.cc" "${HELLO_GRPC_GEN}/echo.grpc.pb.h"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${HELLO_GRPC_GEN}"
  COMMAND $<TARGET_FILE:protoc>
  ARGS
    "--proto_path=${CMAKE_CURRENT_SOURCE_DIR}"
    "--cpp_out=${HELLO_GRPC_GEN}"
    "--grpc_out=${HELLO_GRPC_GEN}"
    "--plugin=protoc-gen-grpc=$<TARGET_FILE:grpc_cpp_plugin>"
    echo.proto
  DEPENDS protoc grpc_cpp_plugin "${CMAKE_CURRENT_SOURCE_DIR}/echo.proto"
  VERBATIM)

add_library(hello_grpc_proto STATIC
  "${HELLO_GRPC_GEN}/echo.pb.cc"
  "${HELLO_GRPC_GEN}/echo.grpc.pb.cc")
target_compile_features(hello_grpc_proto PUBLIC cxx_std_17)
target_include_directories(hello_grpc_proto PUBLIC "${HELLO_GRPC_GEN}")
target_link_libraries(hello_grpc_proto PUBLIC gRPC::grpc++)

foreach(app hello_grpc_client hello_grpc_server)
  add_executable(${app} "${app}.cc")
  target_compile_features(${app} PRIVATE cxx_std_17)
  target_compile_options(${app} PRIVATE -Wall -Wextra)
  target_link_libraries(${app} PRIVATE
    grpc_dpumesh hello_grpc_proto Threads::Threads)
endforeach()
