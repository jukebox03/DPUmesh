#ifndef DPUMESH_GRPC_DMESH_GRPC_RUNTIME_H
#define DPUMESH_GRPC_DMESH_GRPC_RUNTIME_H

#include <functional>
#include <memory>
#include <string>

#include <grpc/event_engine/memory_allocator.h>
#include <grpcpp/channel.h>
#include <grpcpp/passive_listener.h>
#include <grpcpp/security/credentials.h>
#include <grpcpp/support/channel_arguments.h>

#include "absl/functional/any_invocable.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "dmesh_runtime.h"

namespace dpumesh::grpc {

using GrpcChannelCallback = absl::AnyInvocable<void(
    absl::StatusOr<std::shared_ptr<::grpc::Channel>>)>;

// Asynchronously opens a native DPUmesh QP, wraps it in DmeshEndpoint, and
// transfers it to a gRPC chttp2 client channel. The callback runs on the
// DmeshRuntime callback executor.
void ConnectDmeshGrpcChannel(
    DmeshRuntime* runtime, std::string service,
    grpc_event_engine::experimental::MemoryAllocator allocator,
    std::shared_ptr<::grpc::ChannelCredentials> credentials,
    ::grpc::ChannelArguments args, GrpcChannelCallback callback);

using MemoryAllocatorFactory = std::function<
    grpc_event_engine::experimental::MemoryAllocator()>;
using GrpcServerAcceptErrorCallback =
    std::function<void(const absl::Status&)>;

// Owns a native-accept attachment to a started gRPC PassiveListener. Detach()
// disables new injection and waits for in-flight callbacks before returning.
// The runtime and listener must outlive this object.
class DmeshGrpcServerAttachment final {
 public:
  ~DmeshGrpcServerAttachment();

  DmeshGrpcServerAttachment(const DmeshGrpcServerAttachment&) = delete;
  DmeshGrpcServerAttachment& operator=(const DmeshGrpcServerAttachment&) =
      delete;

  void Detach();

  struct State;

 private:
  friend absl::StatusOr<std::unique_ptr<DmeshGrpcServerAttachment>>
  AttachDmeshGrpcServer(
      DmeshRuntime*, ::grpc::experimental::PassiveListener*,
      MemoryAllocatorFactory, GrpcServerAcceptErrorCallback);

  DmeshGrpcServerAttachment(DmeshRuntime* runtime,
                            std::shared_ptr<State> state);

  DmeshRuntime* runtime_;
  std::shared_ptr<State> state_;
};

// Routes subsequent native inbound QPs to a started gRPC PassiveListener.
absl::StatusOr<std::unique_ptr<DmeshGrpcServerAttachment>>
AttachDmeshGrpcServer(
    DmeshRuntime* runtime,
    ::grpc::experimental::PassiveListener* passive_listener,
    MemoryAllocatorFactory allocator_factory,
    GrpcServerAcceptErrorCallback on_error = {});

}  // namespace dpumesh::grpc

#endif
