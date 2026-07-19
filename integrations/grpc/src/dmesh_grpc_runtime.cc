#include "dmesh_grpc_runtime.h"

#include <arpa/inet.h>

#include <condition_variable>
#include <mutex>
#include <utility>

#include <grpc/impl/channel_arg_names.h>

#include "absl/status/status.h"
#include "dmesh_endpoint.h"
#include "dmesh_grpc_channel.h"

namespace dpumesh::grpc {
namespace {

using EventEngine = grpc_event_engine::experimental::EventEngine;

EventEngine::ResolvedAddress LogicalAddress(uint16_t port) {
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  return EventEngine::ResolvedAddress(
      reinterpret_cast<const sockaddr*>(&address), sizeof(address));
}

}  // namespace

struct DmeshGrpcServerAttachment::State {
  State(::grpc::experimental::PassiveListener* listener,
        MemoryAllocatorFactory allocator_factory,
        GrpcServerAcceptErrorCallback on_error, Executor* callback_executor)
      : listener(listener),
        allocator_factory(std::move(allocator_factory)),
        on_error(std::move(on_error)),
        callback_executor(callback_executor) {}

  void Accept(DmeshReactor::ConnectedTransport connected) {
    {
      std::lock_guard<std::mutex> lock(mu);
      if (!active) return;
      ++in_flight;
    }

    absl::Status status = absl::OkStatus();
    auto allocator = allocator_factory();
    if (!allocator.IsValid()) {
      status = absl::ResourceExhaustedError(
          "DPUmesh gRPC allocator factory returned an invalid allocator");
    } else {
      auto endpoint = std::make_unique<DmeshEndpoint>(
          std::move(connected.transport), connected.work_executor,
          callback_executor, std::move(allocator), LogicalAddress(2),
          LogicalAddress(1));
      status = listener->AcceptConnectedEndpoint(std::move(endpoint));
    }
    if (!status.ok() && on_error) on_error(status);

    {
      std::lock_guard<std::mutex> lock(mu);
      --in_flight;
      if (in_flight == 0) idle.notify_all();
    }
  }

  void Deactivate() {
    std::unique_lock<std::mutex> lock(mu);
    active = false;
    idle.wait(lock, [this] { return in_flight == 0; });
    listener = nullptr;
    allocator_factory = {};
    on_error = {};
  }

  std::mutex mu;
  std::condition_variable idle;
  bool active = true;
  size_t in_flight = 0;
  ::grpc::experimental::PassiveListener* listener;
  MemoryAllocatorFactory allocator_factory;
  GrpcServerAcceptErrorCallback on_error;
  Executor* callback_executor;
};

DmeshGrpcServerAttachment::DmeshGrpcServerAttachment(
    DmeshRuntime* runtime, std::shared_ptr<State> state)
    : runtime_(runtime), state_(std::move(state)) {}

DmeshGrpcServerAttachment::~DmeshGrpcServerAttachment() { Detach(); }

void DmeshGrpcServerAttachment::Detach() {
  if (state_ == nullptr) return;
  state_->Deactivate();
  if (runtime_ != nullptr) (void)runtime_->SetAcceptCallback({});
  state_.reset();
  runtime_ = nullptr;
}

void ConnectDmeshGrpcChannel(
    DmeshRuntime* runtime, std::string service,
    grpc_event_engine::experimental::MemoryAllocator allocator,
    std::shared_ptr<::grpc::ChannelCredentials> credentials,
    ::grpc::ChannelArguments args, GrpcChannelCallback callback) {
  if (runtime == nullptr || service.empty() || !allocator.IsValid() ||
      credentials == nullptr || !callback) {
    if (callback) {
      callback(absl::InvalidArgumentError(
          "DPUmesh gRPC connect requires runtime, service, allocator, "
          "credentials and callback"));
    }
    return;
  }

  args.SetString(GRPC_ARG_DEFAULT_AUTHORITY, service);
  Executor* callback_executor = runtime->callback_executor();
  runtime->Connect(
      std::move(service),
      [allocator = std::move(allocator), credentials = std::move(credentials),
       args = std::move(args), callback = std::move(callback),
       callback_executor](
          absl::StatusOr<DmeshReactor::ConnectedTransport> connected) mutable {
        if (!connected.ok()) {
          callback(connected.status());
          return;
        }
        auto endpoint = std::make_unique<DmeshEndpoint>(
            std::move(connected->transport), connected->work_executor,
            callback_executor, std::move(allocator), LogicalAddress(1),
            LogicalAddress(2));
        auto channel = CreateDmeshGrpcChannel(
            std::move(endpoint), std::move(credentials), args);
        if (channel == nullptr) {
          callback(absl::InternalError(
              "gRPC rejected the connected DPUmesh endpoint"));
          return;
        }
        callback(std::move(channel));
      });
}

absl::StatusOr<std::unique_ptr<DmeshGrpcServerAttachment>>
AttachDmeshGrpcServer(
    DmeshRuntime* runtime,
    ::grpc::experimental::PassiveListener* passive_listener,
    MemoryAllocatorFactory allocator_factory,
    GrpcServerAcceptErrorCallback on_error) {
  if (runtime == nullptr || passive_listener == nullptr || !allocator_factory) {
    return absl::InvalidArgumentError(
        "DPUmesh gRPC server attachment requires runtime, listener and "
        "allocator factory");
  }

  auto state = std::make_shared<DmeshGrpcServerAttachment::State>(
      passive_listener, std::move(allocator_factory), std::move(on_error),
      runtime->callback_executor());
  const absl::Status install_status = runtime->SetAcceptCallback(
      [state](DmeshReactor::ConnectedTransport connected) {
        state->Accept(std::move(connected));
      });
  if (!install_status.ok()) {
    state->Deactivate();
    return install_status;
  }
  return std::unique_ptr<DmeshGrpcServerAttachment>(
      new DmeshGrpcServerAttachment(runtime, std::move(state)));
}

}  // namespace dpumesh::grpc
