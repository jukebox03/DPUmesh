/* Minimal gRPC server on DPUmesh. The service and its handler are stock gRPC
 * C++; only bootstrap differs — connections arrive from the transport through
 * a PassiveListener instead of a bound port. The process registers as
 * $DPUMESH_SERVICE. */
#include <cstdio>
#include <memory>

#include <grpcpp/grpcpp.h>
#include <grpcpp/passive_listener.h>
#include <grpcpp/security/server_credentials.h>

#include "dmesh_api_ops.h"
#include "dmesh_grpc_runtime.h"
#include "dmesh_runtime.h"
#include "echo.grpc.pb.h"

namespace {

class EchoService final : public dpumesh::example::Echo::CallbackService {
  grpc::ServerUnaryReactor* Say(grpc::CallbackServerContext* context,
                                const dpumesh::example::EchoRequest* request,
                                dpumesh::example::EchoReply* reply) override {
    reply->set_text(request->text());
    grpc::ServerUnaryReactor* reactor = context->DefaultReactor();
    reactor->Finish(grpc::Status::OK);
    return reactor;
  }
};

}  // namespace

int main() {
  auto runtime = dpumesh::grpc::DmeshRuntime::Create(
      dpumesh::grpc::MakeNativeDmeshApiOps());
  if (!runtime.ok()) {
    std::fprintf(stderr, "runtime: %s\n", runtime.status().ToString().c_str());
    return 1;
  }

  EchoService service;
  grpc::ServerBuilder builder;
  builder.RegisterService(&service);
  std::unique_ptr<grpc::experimental::PassiveListener> listener;
  builder.experimental().AddPassiveListener(grpc::InsecureServerCredentials(),
                                            listener);
  std::unique_ptr<grpc::Server> server = builder.BuildAndStart();
  if (server == nullptr) {
    std::fprintf(stderr, "server did not start\n");
    return 1;
  }
  auto attachment =
      dpumesh::grpc::AttachDmeshGrpcServer(*runtime, listener.get());
  if (!attachment.ok()) {
    std::fprintf(stderr, "attach: %s\n",
                 attachment.status().ToString().c_str());
    return 1;
  }
  std::fprintf(stderr, "hello_grpc_server: READY\n");
  server->Wait();
  return 0;
}
