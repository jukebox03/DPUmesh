#include "dmesh_grpc_runtime.h"

#include <arpa/inet.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <utility>

#include <grpc/event_engine/event_engine.h>
#include <grpc/impl/channel_arg_names.h>
#include <grpcpp/create_channel.h>

#include "absl/status/status.h"
#include "dmesh_endpoint.h"
#include "dmesh_grpc_channel.h"

namespace dpumesh::grpc {
namespace {

using EventEngine = grpc_event_engine::experimental::EventEngine;
using MemoryAllocator = grpc_event_engine::experimental::MemoryAllocator;

constexpr char kSyntheticTarget[] = "ipv4:127.0.0.1:1";

bool HasChannelArgument(const ::grpc::ChannelArguments& args,
                        const char* key) {
  const grpc_channel_args c_args = args.c_channel_args();
  for (size_t i = 0; i < c_args.num_args; ++i) {
    if (std::strcmp(c_args.args[i].key, key) == 0) return true;
  }
  return false;
}

EventEngine::ResolvedAddress LogicalAddress(uint16_t port) {
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  return EventEngine::ResolvedAddress(
      reinterpret_cast<const sockaddr*>(&address), sizeof(address));
}

// Delegate every EventEngine operation except DPUmesh connection creation.
class DmeshClientEventEngine final : public EventEngine {
 public:
  DmeshClientEventEngine(DmeshRuntime* runtime, std::string target)
      : runtime_(runtime),
        target_(std::move(target)),
        delegate_(grpc_event_engine::experimental::GetDefaultEventEngine()) {}

  ~DmeshClientEventEngine() override {
    std::unordered_map<uint64_t, PendingConnect> pending;
    {
      std::lock_guard<std::mutex> lock(mu_);
      pending.swap(pending_);
    }
    for (auto& entry : pending) {
      if (entry.second.timer != TaskHandle::kInvalid) {
        (void)delegate_->Cancel(entry.second.timer);
      }
    }
  }

  bool IsWorkerThread() override { return delegate_->IsWorkerThread(); }

  absl::StatusOr<std::unique_ptr<DNSResolver>> GetDNSResolver(
      const DNSResolver::ResolverOptions& options) override {
    return delegate_->GetDNSResolver(options);
  }

  absl::StatusOr<std::unique_ptr<Listener>> CreateListener(
      Listener::AcceptCallback on_accept,
      absl::AnyInvocable<void(absl::Status)> on_shutdown,
      const grpc_event_engine::experimental::EndpointConfig& config,
      std::unique_ptr<
          grpc_event_engine::experimental::MemoryAllocatorFactory>
          memory_allocator_factory) override {
    return delegate_->CreateListener(
        std::move(on_accept), std::move(on_shutdown), config,
        std::move(memory_allocator_factory));
  }

  ConnectionHandle Connect(
      OnConnectCallback on_connect, const ResolvedAddress& /*addr*/,
      const grpc_event_engine::experimental::EndpointConfig& /*args*/,
      MemoryAllocator memory_allocator, Duration timeout) override {
    if (!on_connect || runtime_ == nullptr || target_.empty() ||
        !memory_allocator.IsValid()) {
      delegate_->Run(
          [on_connect = std::move(on_connect)]() mutable {
            if (on_connect) {
              on_connect(absl::InvalidArgumentError(
                  "DPUmesh EventEngine connect requires runtime, target and "
                  "allocator"));
            }
          });
      return ConnectionHandle::kInvalid;
    }

    const uint64_t id = next_connect_id_.fetch_add(1, std::memory_order_relaxed);
    ConnectionHandle handle{
        {reinterpret_cast<intptr_t>(this), static_cast<intptr_t>(id)}};
    {
      std::lock_guard<std::mutex> lock(mu_);
      pending_.emplace(id, PendingConnect{std::move(on_connect),
                                          TaskHandle::kInvalid});
    }

    std::weak_ptr<DmeshClientEventEngine> weak =
        std::static_pointer_cast<DmeshClientEventEngine>(shared_from_this());
    const TaskHandle timer = delegate_->RunAfter(
        timeout, [weak, id]() {
          if (auto self = weak.lock()) self->FinishTimeout(id);
        });
    {
      std::lock_guard<std::mutex> lock(mu_);
      auto found = pending_.find(id);
      if (found != pending_.end()) {
        found->second.timer = timer;
      } else if (timer != TaskHandle::kInvalid) {
        (void)delegate_->Cancel(timer);
      }
    }

    runtime_->Connect(
        target_,
        [weak, id, memory_allocator = std::move(memory_allocator)](
            absl::StatusOr<DmeshReactor::ConnectedTransport> connected) mutable {
          if (auto self = weak.lock()) {
            self->FinishConnect(id, std::move(memory_allocator),
                                std::move(connected));
          }
        });
    return handle;
  }

  bool CancelConnect(ConnectionHandle handle) override {
    if (handle.keys[0] != reinterpret_cast<intptr_t>(this) ||
        handle.keys[1] <= 0) {
      return false;
    }
    PendingConnect pending;
    {
      std::lock_guard<std::mutex> lock(mu_);
      auto found = pending_.find(static_cast<uint64_t>(handle.keys[1]));
      if (found == pending_.end()) return false;
      pending = std::move(found->second);
      pending_.erase(found);
    }
    if (pending.timer != TaskHandle::kInvalid) {
      (void)delegate_->Cancel(pending.timer);
    }
    // EventEngine cancellation destroys the callback before returning.
    pending.on_connect = {};
    return true;
  }

  void Run(Closure* closure) override { delegate_->Run(closure); }
  void Run(absl::AnyInvocable<void()> closure) override {
    delegate_->Run(std::move(closure));
  }
  TaskHandle RunAfter(Duration when, Closure* closure) override {
    return delegate_->RunAfter(when, closure);
  }
  TaskHandle RunAfter(Duration when,
                      absl::AnyInvocable<void()> closure) override {
    return delegate_->RunAfter(when, std::move(closure));
  }
  bool Cancel(TaskHandle handle) override { return delegate_->Cancel(handle); }

 private:
  struct PendingConnect {
    OnConnectCallback on_connect;
    TaskHandle timer = TaskHandle::kInvalid;
  };

  void FinishTimeout(uint64_t id) {
    PendingConnect pending;
    {
      std::lock_guard<std::mutex> lock(mu_);
      auto found = pending_.find(id);
      if (found == pending_.end()) return;
      pending = std::move(found->second);
      pending_.erase(found);
    }
    if (pending.on_connect) {
      pending.on_connect(absl::DeadlineExceededError(
          "DPUmesh connection attempt exceeded the gRPC deadline"));
    }
  }

  void FinishConnect(
      uint64_t id, MemoryAllocator memory_allocator,
      absl::StatusOr<DmeshReactor::ConnectedTransport> connected) {
    PendingConnect pending;
    {
      std::lock_guard<std::mutex> lock(mu_);
      auto found = pending_.find(id);
      if (found == pending_.end()) return;
      pending = std::move(found->second);
      pending_.erase(found);
    }
    if (pending.timer != TaskHandle::kInvalid) {
      (void)delegate_->Cancel(pending.timer);
    }
    if (!pending.on_connect) return;
    if (!connected.ok()) {
      pending.on_connect(connected.status());
      return;
    }
    pending.on_connect(std::make_unique<DmeshEndpoint>(
        std::move(connected->transport), connected->work_executor,
        runtime_->callback_executor(), std::move(memory_allocator),
        LogicalAddress(1), LogicalAddress(2)));
  }

  DmeshRuntime* const runtime_;
  const std::string target_;
  const std::shared_ptr<EventEngine> delegate_;
  std::mutex mu_;
  std::unordered_map<uint64_t, PendingConnect> pending_;
  std::atomic<uint64_t> next_connect_id_{1};
};

}  // namespace

namespace internal {

void SetDefaultAuthorityIfAbsent(
    const std::string& target, ::grpc::ChannelArguments* args) {
  if (args == nullptr || target.empty() ||
      HasChannelArgument(*args, GRPC_ARG_DEFAULT_AUTHORITY)) {
    return;
  }
  args->SetString(GRPC_ARG_DEFAULT_AUTHORITY, target);
}

}  // namespace internal

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
    DmeshRuntime* runtime, std::string target,
    std::shared_ptr<::grpc::ChannelCredentials> credentials,
    ::grpc::ChannelArguments args, GrpcChannelCallback callback) {
  if (runtime == nullptr || target.empty() || credentials == nullptr ||
      !callback) {
    if (callback) {
      callback(absl::InvalidArgumentError(
          "DPUmesh gRPC connect requires runtime, target, credentials and "
          "callback"));
    }
    return;
  }
  if (HasChannelArgument(args, GRPC_ARG_EVENT_ENGINE)) {
    callback(absl::InvalidArgumentError(
        "DPUmesh gRPC owns the channel EventEngine; a caller-supplied "
        "GRPC_ARG_EVENT_ENGINE is not supported"));
    return;
  }

  internal::SetDefaultAuthorityIfAbsent(target, &args);
  auto event_engine =
      std::make_shared<DmeshClientEventEngine>(runtime, std::move(target));
  args.SetPointerWithVtable(
      GRPC_ARG_EVENT_ENGINE, &event_engine,
      grpc_event_engine::experimental::grpc_event_engine_arg_vtable());
  auto channel = ::grpc::CreateCustomChannel(kSyntheticTarget, credentials, args);
  runtime->callback_executor()->Run(
      [callback = std::move(callback), channel = std::move(channel)]() mutable {
        if (channel == nullptr) {
          callback(absl::InternalError(
              "gRPC rejected the reconnectable DPUmesh channel"));
        } else {
          callback(std::move(channel));
        }
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
