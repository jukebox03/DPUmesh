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

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "dmesh_runtime.h"

namespace dpumesh::grpc {

namespace internal {

// Set target as authority only when the application omitted it.
void SetDefaultAuthorityIfAbsent(
    const std::string& target, ::grpc::ChannelArguments* args);

}  // namespace internal

// Lazy gRPC channel for a configured DPUmesh Service name. Each gRPC
// connection opens one targeted QP. The channel shares ownership of the
// runtime, and DPUmesh owns GRPC_ARG_EVENT_ENGINE.
absl::StatusOr<std::shared_ptr<::grpc::Channel>> CreateDmeshChannel(
    std::shared_ptr<DmeshRuntime> runtime, const std::string& target,
    const std::shared_ptr<::grpc::ChannelCredentials>& creds,
    const ::grpc::ChannelArguments& args = {});

using MemoryAllocatorFactory = std::function<
    grpc_event_engine::experimental::MemoryAllocator()>;
using GrpcServerAcceptErrorCallback =
    std::function<void(const absl::Status&)>;

// Owns a native-accept attachment to a started gRPC PassiveListener. Detach()
// disables new injection and waits for in-flight callbacks before returning.
// The attachment shares ownership of the runtime; the listener must outlive
// this object.
class DmeshGrpcServerAttachment final {
 public:
  ~DmeshGrpcServerAttachment();

  DmeshGrpcServerAttachment(const DmeshGrpcServerAttachment&) = delete;
  DmeshGrpcServerAttachment& operator=(const DmeshGrpcServerAttachment&) =
      delete;

  void Detach();

 private:
  struct State;

  friend absl::StatusOr<std::unique_ptr<DmeshGrpcServerAttachment>>
  AttachDmeshGrpcServer(
      std::shared_ptr<DmeshRuntime>, ::grpc::experimental::PassiveListener*,
      MemoryAllocatorFactory, GrpcServerAcceptErrorCallback);

  DmeshGrpcServerAttachment(std::shared_ptr<DmeshRuntime> runtime,
                            std::shared_ptr<State> state);

  std::shared_ptr<DmeshRuntime> runtime_;
  std::shared_ptr<State> state_;
};

// Routes subsequent native inbound QPs to a started gRPC PassiveListener.
// An omitted allocator factory uses an unquota'd malloc-backed allocator.
absl::StatusOr<std::unique_ptr<DmeshGrpcServerAttachment>>
AttachDmeshGrpcServer(
    std::shared_ptr<DmeshRuntime> runtime,
    ::grpc::experimental::PassiveListener* passive_listener,
    MemoryAllocatorFactory allocator_factory = {},
    GrpcServerAcceptErrorCallback on_error = {});

}  // namespace dpumesh::grpc

#endif
