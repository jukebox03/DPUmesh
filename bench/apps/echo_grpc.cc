/* gRPC echo server for the L4 evaluation harness.
 *
 * Serves BenchmarkService::UnaryCall, returning `response_size` bytes, and is
 * the server-side peer of bench_grpc. BENCH_TRANSPORT selects the listener:
 * `dmesh` accepts native connections through the DPUmesh passive listener,
 * `tcp` binds a socket. The process runs until killed.
 *
 * BENCH_FAIL_EVERY makes the server fail on purpose: every Nth call answers
 * INTERNAL instead of a payload. A retry policy and a failure-accrual policy
 * are both statements about what happens when a backend fails, so neither can
 * be tested against a server that always succeeds.
 */

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

#include <grpcpp/grpcpp.h>
#include <grpcpp/passive_listener.h>
#include <grpcpp/security/server_credentials.h>
#include <grpcpp/server_builder.h>

#include "dmesh_api_ops.h"
#include "dmesh_grpc_runtime.h"
#include "dmesh_runtime.h"
#include "src/proto/grpc/testing/benchmark_service.grpc.pb.h"

namespace {

using dpumesh::grpc::DmeshRuntime;
using ::grpc::testing::BenchmarkService;
using ::grpc::testing::SimpleRequest;
using ::grpc::testing::SimpleResponse;

class EchoService final : public BenchmarkService::CallbackService {
 public:
  explicit EchoService(long fail_every) : fail_every_(fail_every) {}

  ::grpc::ServerUnaryReactor* UnaryCall(
      ::grpc::CallbackServerContext* context, const SimpleRequest* request,
      SimpleResponse* response) override {
    ::grpc::ServerUnaryReactor* reactor = context->DefaultReactor();
    if (fail_every_ > 0 &&
        (calls_.fetch_add(1, std::memory_order_relaxed) + 1) % fail_every_ == 0) {
      reactor->Finish(
          ::grpc::Status(::grpc::StatusCode::INTERNAL, "induced failure"));
      return reactor;
    }
    if (request->response_size() < 0) {
      reactor->Finish(::grpc::Status(::grpc::StatusCode::INVALID_ARGUMENT,
                                     "negative response size"));
      return reactor;
    }
    if (request->response_size() > 0) {
      response->mutable_payload()->set_type(request->response_type());
      response->mutable_payload()->set_body(
          std::string(static_cast<size_t>(request->response_size()), '\0'));
    }
    reactor->Finish(::grpc::Status::OK);
    return reactor;
  }

 private:
  const long fail_every_;
  std::atomic<long> calls_{0};
};

}  // namespace

int main() {
  std::string transport = "dmesh";
  int reactors = 8;
  int port = 9091;
  if (const char* t = std::getenv("BENCH_TRANSPORT")) transport = t;
  if (const char* r = std::getenv("BENCH_REACTORS")) reactors = std::atoi(r);
  if (const char* p = std::getenv("ECHO_PORT")) port = std::atoi(p);
  long fail_every = 0;
  if (const char* f = std::getenv("BENCH_FAIL_EVERY")) fail_every = std::atol(f);
  if (fail_every < 0) fail_every = 0;
  if (reactors < 1) reactors = 1;

  std::shared_ptr<DmeshRuntime> runtime;
  if (transport == "dmesh") {
    DmeshRuntime::Options options;
    options.reactor_count = static_cast<size_t>(reactors);
    auto created = DmeshRuntime::Create(dpumesh::grpc::MakeNativeDmeshApiOps(),
                                        options);
    if (!created.ok()) {
      std::fprintf(stderr, "[echo_grpc] runtime init failed: %s\n",
                   created.status().ToString().c_str());
      return 1;
    }
    runtime = std::move(*created);
  } else if (transport != "tcp") {
    std::fprintf(stderr, "[echo_grpc] BENCH_TRANSPORT must be dmesh or tcp\n");
    return 1;
  }

  EchoService service(fail_every);
  ::grpc::ServerBuilder builder;
  // CallbackService keeps this immediate handler on gRPC's bounded callback
  // engine; a synchronous Service would spawn one OS worker per queued call
  // under overload.
  builder.RegisterService(&service);
  int selected_port = 0;
  std::unique_ptr<::grpc::experimental::PassiveListener> listener;
  if (transport == "tcp") {
    char endpoint[64];
    std::snprintf(endpoint, sizeof endpoint, "0.0.0.0:%d", port);
    builder.AddListeningPort(endpoint, ::grpc::InsecureServerCredentials(),
                             &selected_port);
  } else {
    builder.experimental().AddPassiveListener(
        ::grpc::InsecureServerCredentials(), listener);
  }

  std::unique_ptr<::grpc::Server> server = builder.BuildAndStart();
  if (server == nullptr) {
    std::fprintf(stderr, "[echo_grpc] failed to start server\n");
    return 1;
  }
  if (transport == "tcp" && selected_port == 0) {
    std::fprintf(stderr, "[echo_grpc] failed to bind :%d\n", port);
    return 1;
  }

  std::unique_ptr<dpumesh::grpc::DmeshGrpcServerAttachment> attachment;
  if (transport == "dmesh") {
    auto attached = dpumesh::grpc::AttachDmeshGrpcServer(
        runtime, listener.get(), {},
        [](absl::Status status) {
          std::fprintf(stderr, "[echo_grpc] accept error: %s\n",
                       status.ToString().c_str());
        });
    if (!attached.ok()) {
      std::fprintf(stderr, "[echo_grpc] attach failed: %s\n",
                   attached.status().ToString().c_str());
      return 1;
    }
    attachment = std::move(*attached);
  }

  std::fprintf(stderr, "[echo_grpc] READY transport=%s reactors=%d port=%d\n",
               transport.c_str(), reactors,
               transport == "tcp" ? selected_port : 0);
  server->Wait();
  return 0;
}
