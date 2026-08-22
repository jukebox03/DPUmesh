#include "dmesh_grpc_runtime.h"

#include <arpa/inet.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

#include <grpc/event_engine/event_engine.h>
#include <grpc/event_engine/internal/memory_allocator_impl.h>
#include <grpc/event_engine/memory_request.h>
#include <grpc/impl/channel_arg_names.h>
#include <grpc/slice.h>
#include <grpcpp/create_channel.h>

#include "absl/status/status.h"
#include "dmesh_endpoint.h"

namespace dpumesh::grpc {
namespace {

using EventEngine = grpc_event_engine::experimental::EventEngine;
using MemoryAllocator = grpc_event_engine::experimental::MemoryAllocator;

// Unquota'd default for accepted connections: exact-size malloc slices.
class SliceMallocAllocator final
    : public grpc_event_engine::experimental::internal::MemoryAllocatorImpl {
 public:
  size_t Reserve(
      grpc_event_engine::experimental::MemoryRequest request) override {
    return request.max();
  }
  grpc_slice MakeSlice(
      grpc_event_engine::experimental::MemoryRequest request) override {
    return grpc_slice_malloc(request.max());
  }
  void Release(size_t /*bytes*/) override {}
  void Shutdown() override {}
};

MemoryAllocator MakeSliceMallocAllocator() {
  return MemoryAllocator(std::make_shared<SliceMallocAllocator>());
}

constexpr char kSyntheticTarget[] = "ipv4:127.0.0.1:1";

bool HasChannelArgument(const ::grpc::ChannelArguments& args,
                        const char* key) {
  const grpc_channel_args c_args = args.c_channel_args();
  for (size_t i = 0; i < c_args.num_args; ++i) {
    if (std::strcmp(c_args.args[i].key, key) == 0) return true;
  }
  return false;
}

EventEngine::ResolvedAddress NativeAddress(int pod, uint16_t port) {
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  const uint32_t host = (pod > 0 && pod <= 255) ? static_cast<uint32_t>(pod) : 0;
  address.sin_addr.s_addr = htonl(0x7f000000u | host);
  return EventEngine::ResolvedAddress(
      reinterpret_cast<const sockaddr*>(&address), sizeof(address));
}

// One connection can be owned by gRPC for substantially longer than the
// public Channel that created it. The lease gives the public channel owner a
// safe, idempotent way to retire that native connection without reaching into
// the Endpoint object retained by gRPC.
class ConnectionLease final {
 public:
  explicit ConnectionLease(std::function<void()> release)
      : release_(std::move(release)) {}

  bool ClaimClose() {
    return !closed_.exchange(true, std::memory_order_acq_rel);
  }

  void Release() {
    if (ClaimClose() && release_) release_();
  }

 private:
  const std::function<void()> release_;
  std::atomic<bool> closed_{false};
};

// Keeps the connection lease alive for exactly as long as gRPC keeps the
// Endpoint transport. A client Endpoint disappearing means gRPC has abandoned
// that HTTP/2 connection (including a failed handshaker or reconnect attempt),
// so it must use the prompt reset path instead of serially waiting for a FIN
// custody deadline. The one-shot lease makes Endpoint and public Channel
// teardown race safely.
class LeasedEndpointTransport final : public EndpointTransport {
 public:
  LeasedEndpointTransport(std::unique_ptr<EndpointTransport> delegate,
                          std::shared_ptr<ConnectionLease> lease,
                          std::function<void()> on_destroy)
      : delegate_(std::move(delegate)),
        lease_(std::move(lease)),
        on_destroy_(std::move(on_destroy)) {}

  ~LeasedEndpointTransport() override {
    Close();
    if (on_destroy_) on_destroy_();
  }

  void BindDriver(std::weak_ptr<DmeshEndpointDriver> driver) override {
    delegate_->BindDriver(std::move(driver));
  }
  size_t MaxPostSize() const override { return delegate_->MaxPostSize(); }
  PostResult Post(size_t length,
                  absl::FunctionRef<void(Reservation)> fill) override {
    return delegate_->Post(length, fill);
  }
  absl::Status Flush() override { return delegate_->Flush(); }
  void ResumeReceive() override { delegate_->ResumeReceive(); }
  void Close() override { lease_->Release(); }

 private:
  const std::unique_ptr<EndpointTransport> delegate_;
  const std::shared_ptr<ConnectionLease> lease_;
  const std::function<void()> on_destroy_;
};

// Delegate every EventEngine operation except DPUmesh connection creation.
class DmeshClientEventEngine final : public EventEngine {
 public:
  DmeshClientEventEngine(std::shared_ptr<DmeshRuntime> runtime,
                         std::string target)
      : runtime_(std::move(runtime)),
        target_(std::move(target)),
        delegate_(grpc_event_engine::experimental::GetDefaultEventEngine()) {}

  ~DmeshClientEventEngine() override {
    ReleaseConnections();
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
    bool closing = false;
    {
      std::lock_guard<std::mutex> lock(mu_);
      closing = closing_;
      if (!closing) {
        pending_.emplace(id, PendingConnect{std::move(on_connect),
                                            TaskHandle::kInvalid});
      }
    }
    if (closing) {
      delegate_->Run(
          [on_connect = std::move(on_connect)]() mutable {
            on_connect(absl::CancelledError(
                "DPUmesh channel is being released"));
          });
      return ConnectionHandle::kInvalid;
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

  // Called by the aliasing shared_ptr that represents the public channel as
  // the underlying grpc::Channel begins its asynchronous teardown.
  void ReleaseConnections() {
    std::vector<std::shared_ptr<ConnectionLease>> leases;
    {
      std::lock_guard<std::mutex> lock(mu_);
      closing_ = true;
      leases.reserve(active_.size());
      for (auto& entry : active_) {
        if (auto lease = entry.second.lock()) {
          leases.push_back(std::move(lease));
        }
      }
      active_.clear();
    }
    for (const auto& lease : leases) lease->Release();
  }

  void ForgetConnection(uint64_t id) {
    std::lock_guard<std::mutex> lock(mu_);
    active_.erase(id);
  }

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
    std::shared_ptr<ConnectionLease> lease;
    bool abandoned = false;
    {
      std::lock_guard<std::mutex> lock(mu_);
      auto found = pending_.find(id);
      if (found == pending_.end()) {
        abandoned = true;
      } else {
        pending = std::move(found->second);
        pending_.erase(found);
        abandoned = closing_;
        if (!abandoned && connected.ok()) {
          lease = std::make_shared<ConnectionLease>(
              std::move(connected->release));
          active_.emplace(id, lease);
        }
      }
    }
    if (pending.timer != TaskHandle::kInvalid) {
      (void)delegate_->Cancel(pending.timer);
    }
    // CancelConnect, timeout, or channel release can win after the reactor has
    // created the QP but before gRPC accepts its Endpoint. No Endpoint exists
    // to own that QP, so explicitly retire the orphaned native connection.
    if (abandoned) {
      if (connected.ok() && connected->release) connected->release();
      return;
    }
    if (!pending.on_connect) return;
    if (!connected.ok()) {
      pending.on_connect(connected.status());
      return;
    }
    std::shared_ptr<Executor> callback_executor =
        std::move(connected->callback_executor);
    if (callback_executor == nullptr) {
      callback_executor = runtime_->callback_executor();
    }
    std::weak_ptr<DmeshClientEventEngine> weak =
        std::static_pointer_cast<DmeshClientEventEngine>(shared_from_this());
    auto transport = std::make_unique<LeasedEndpointTransport>(
        std::move(connected->transport), std::move(lease),
        [weak, id] {
          if (auto self = weak.lock()) self->ForgetConnection(id);
        });
    pending.on_connect(std::make_unique<DmeshEndpoint>(
        std::move(transport), std::move(callback_executor),
        std::move(memory_allocator),
        NativeAddress(connected->peer_pod, connected->peer_port),
        NativeAddress(connected->local_pod, connected->local_port)));
  }

  const std::shared_ptr<DmeshRuntime> runtime_;
  const std::string target_;
  const std::shared_ptr<EventEngine> delegate_;
  std::mutex mu_;
  std::unordered_map<uint64_t, PendingConnect> pending_;
  std::unordered_map<uint64_t, std::weak_ptr<ConnectionLease>> active_;
  bool closing_ = false;
  std::atomic<uint64_t> next_connect_id_{1};
};

// grpc::Channel has no public shutdown operation. An aliasing shared_ptr lets
// us attach a deterministic native-connection lifetime without changing the
// type applications and generated stubs consume.
class DmeshChannelOwner final {
 public:
  DmeshChannelOwner(std::shared_ptr<::grpc::Channel> channel,
                    std::shared_ptr<DmeshClientEventEngine> event_engine)
      : channel_(std::move(channel)), event_engine_(std::move(event_engine)) {}

  ~DmeshChannelOwner() {
    // Stop the client channel from launching another reconnect before closing
    // the native connections its orphaned Endpoints may still retain.
    channel_.reset();
    event_engine_->ReleaseConnections();
    event_engine_.reset();
  }

  ::grpc::Channel* channel() const { return channel_.get(); }

 private:
  std::shared_ptr<::grpc::Channel> channel_;
  std::shared_ptr<DmeshClientEventEngine> event_engine_;
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
        GrpcServerAcceptErrorCallback on_error,
        std::shared_ptr<Executor> callback_executor)
      : listener(listener),
        allocator_factory(std::move(allocator_factory)),
        on_error(std::move(on_error)),
        callback_executor(std::move(callback_executor)) {}

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
      std::shared_ptr<Executor> endpoint_callbacks =
          std::move(connected.callback_executor);
      if (endpoint_callbacks == nullptr) endpoint_callbacks = callback_executor;
      auto endpoint = std::make_unique<DmeshEndpoint>(
          std::move(connected.transport), std::move(endpoint_callbacks),
          std::move(allocator),
          NativeAddress(connected.peer_pod, connected.peer_port),
          NativeAddress(connected.local_pod, connected.local_port));
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
    callback_executor.reset();
  }

  std::mutex mu;
  std::condition_variable idle;
  bool active = true;
  size_t in_flight = 0;
  ::grpc::experimental::PassiveListener* listener;
  MemoryAllocatorFactory allocator_factory;
  GrpcServerAcceptErrorCallback on_error;
  std::shared_ptr<Executor> callback_executor;
};

DmeshGrpcServerAttachment::DmeshGrpcServerAttachment(
    std::shared_ptr<DmeshRuntime> runtime, std::shared_ptr<State> state)
    : runtime_(std::move(runtime)), state_(std::move(state)) {}

DmeshGrpcServerAttachment::~DmeshGrpcServerAttachment() { Detach(); }

void DmeshGrpcServerAttachment::Detach() {
  if (state_ == nullptr) return;
  state_->Deactivate();
  if (runtime_ != nullptr) (void)runtime_->SetAcceptCallback({});
  state_.reset();
  runtime_.reset();
}

absl::StatusOr<std::shared_ptr<::grpc::Channel>> CreateDmeshChannel(
    std::shared_ptr<DmeshRuntime> runtime, const std::string& target,
    const std::shared_ptr<::grpc::ChannelCredentials>& creds,
    const ::grpc::ChannelArguments& args) {
  if (runtime == nullptr || target.empty() || creds == nullptr) {
    return absl::InvalidArgumentError(
        "DPUmesh gRPC channel requires runtime, target and credentials");
  }
  if (HasChannelArgument(args, GRPC_ARG_EVENT_ENGINE)) {
    return absl::InvalidArgumentError(
        "DPUmesh gRPC owns the channel EventEngine; a caller-supplied "
        "GRPC_ARG_EVENT_ENGINE is not supported");
  }

  ::grpc::ChannelArguments channel_args = args;
  internal::SetDefaultAuthorityIfAbsent(target, &channel_args);
  auto event_engine =
      std::make_shared<DmeshClientEventEngine>(std::move(runtime), target);
  channel_args.SetPointerWithVtable(
      GRPC_ARG_EVENT_ENGINE, &event_engine,
      grpc_event_engine::experimental::grpc_event_engine_arg_vtable());
  auto channel =
      ::grpc::CreateCustomChannel(kSyntheticTarget, creds, channel_args);
  if (channel == nullptr) {
    return absl::InternalError(
        "gRPC rejected the reconnectable DPUmesh channel");
  }
  auto owner =
      std::make_shared<DmeshChannelOwner>(std::move(channel), event_engine);
  ::grpc::Channel* const channel_ptr = owner->channel();
  return std::shared_ptr<::grpc::Channel>(std::move(owner), channel_ptr);
}

absl::StatusOr<std::unique_ptr<DmeshGrpcServerAttachment>>
AttachDmeshGrpcServer(
    std::shared_ptr<DmeshRuntime> runtime,
    ::grpc::experimental::PassiveListener* passive_listener,
    MemoryAllocatorFactory allocator_factory,
    GrpcServerAcceptErrorCallback on_error) {
  if (runtime == nullptr || passive_listener == nullptr) {
    return absl::InvalidArgumentError(
        "DPUmesh gRPC server attachment requires runtime and listener");
  }
  if (!allocator_factory) {
    allocator_factory = [] { return MakeSliceMallocAllocator(); };
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
      new DmeshGrpcServerAttachment(std::move(runtime), std::move(state)));
}

}  // namespace dpumesh::grpc
