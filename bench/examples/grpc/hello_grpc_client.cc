/* Minimal gRPC client on DPUmesh. Stubs, RPC semantics, metadata and
 * deadlines are stock gRPC C++; only bootstrap differs — the channel target
 * is a Kubernetes Service name, not an address. */
#include <cstdio>
#include <memory>
#include <string>

#include <grpcpp/grpcpp.h>

#include "dmesh_api_ops.h"
#include "dmesh_grpc_runtime.h"
#include "dmesh_runtime.h"
#include "echo.grpc.pb.h"

int main(int argc, char** argv) {
  const std::string service = argc > 1 ? argv[1] : "echo-grpc-dpumesh";
  const std::string text = argc > 2 ? argv[2] : "hello";

  auto runtime = dpumesh::grpc::DmeshRuntime::Create(
      dpumesh::grpc::MakeNativeDmeshApiOps());
  if (!runtime.ok()) {
    std::fprintf(stderr, "runtime: %s\n", runtime.status().ToString().c_str());
    return 1;
  }
  auto channel = dpumesh::grpc::CreateDmeshChannel(
      *runtime, service, grpc::InsecureChannelCredentials());
  if (!channel.ok()) {
    std::fprintf(stderr, "channel: %s\n", channel.status().ToString().c_str());
    return 1;
  }
  auto stub = dpumesh::example::Echo::NewStub(*channel);

  dpumesh::example::EchoRequest request;
  request.set_text(text);
  dpumesh::example::EchoReply reply;
  grpc::ClientContext context;
  grpc::Status status = stub->Say(&context, request, &reply);
  if (!status.ok()) {
    std::fprintf(stderr, "Say: %s\n", status.error_message().c_str());
    return 1;
  }
  std::printf("%s\n", reply.text().c_str());
  return 0;
}
