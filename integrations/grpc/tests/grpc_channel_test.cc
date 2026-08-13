#include <arpa/inet.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <grpc/event_engine/internal/memory_allocator_impl.h>
#include <grpc/event_engine/memory_request.h>
#include <grpcpp/create_channel_posix.h>
#include <grpcpp/generic/async_generic_service.h>
#include <grpcpp/generic/generic_stub.h>
#include <grpcpp/passive_listener.h>
#include <grpcpp/security/credentials.h>
#include <grpcpp/server_builder.h>
#include <grpcpp/support/byte_buffer.h>
#include <grpcpp/support/channel_arguments.h>
#include <grpcpp/support/slice.h>

#include "absl/functional/function_ref.h"
#include "absl/status/status.h"
#include "dmesh_endpoint.h"
#include "endpoint_transport.h"
#include "executor.h"

namespace dpumesh::grpc::testing {
namespace {

using EventEngine = grpc_event_engine::experimental::EventEngine;
using MemoryAllocator = grpc_event_engine::experimental::MemoryAllocator;
using MemoryAllocatorImpl =
    grpc_event_engine::experimental::internal::MemoryAllocatorImpl;
using MemoryRequest = grpc_event_engine::experimental::MemoryRequest;

class TestMemoryAllocator final : public MemoryAllocatorImpl {
 public:
  size_t Reserve(MemoryRequest request) override { return request.max(); }
  grpc_slice MakeSlice(MemoryRequest request) override {
    return grpc_slice_malloc(request.max());
  }
  void Release(size_t /*bytes*/) override {}
  void Shutdown() override {}
};

class ThreadExecutor final : public Executor {
 public:
  ThreadExecutor() : thread_([this] { RunLoop(); }) {}

  ~ThreadExecutor() override {
    {
      std::lock_guard<std::mutex> lock(mu_);
      stopping_ = true;
    }
    cv_.notify_one();
    thread_.join();
  }

  void Run(absl::AnyInvocable<void()> task) override {
    {
      std::lock_guard<std::mutex> lock(mu_);
      tasks_.push_back(std::move(task));
    }
    cv_.notify_one();
  }

 private:
  void RunLoop() {
    for (;;) {
      absl::AnyInvocable<void()> task;
      {
        std::unique_lock<std::mutex> lock(mu_);
        cv_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });
        if (tasks_.empty()) {
          if (stopping_) return;
          continue;
        }
        task = std::move(tasks_.front());
        tasks_.pop_front();
      }
      task();
    }
  }

  std::mutex mu_;
  std::condition_variable cv_;
  std::deque<absl::AnyInvocable<void()>> tasks_;
  bool stopping_ = false;
  std::thread thread_;
};

struct LinkState {
  std::mutex mu;
  std::weak_ptr<DmeshEndpointDriver> drivers[2];
  bool closed[2] = {false, false};
  size_t bytes[2] = {0, 0};
  size_t posts[2] = {0, 0};
};

class LinkedEndpointTransport final : public EndpointTransport {
 public:
  LinkedEndpointTransport(std::shared_ptr<LinkState> state, int side,
                          Executor* peer_inbound_executor)
      : state_(std::move(state)),
        side_(side),
        peer_inbound_executor_(peer_inbound_executor) {}

  void BindDriver(std::weak_ptr<DmeshEndpointDriver> driver) override {
    std::lock_guard<std::mutex> lock(state_->mu);
    state_->drivers[side_] = std::move(driver);
  }

  size_t MaxPostSize() const override { return 137; }

  PostResult Post(size_t length,
                  absl::FunctionRef<void(Reservation)> fill) override {
    std::shared_ptr<DmeshEndpointDriver> peer;
    std::vector<uint8_t> copied;
    {
      std::lock_guard<std::mutex> lock(state_->mu);
      if (state_->closed[side_] || state_->closed[1 - side_]) {
        return PostResult::Closed(absl::UnavailableError("link is closed"));
      }
      peer = state_->drivers[1 - side_].lock();
      if (peer == nullptr) {
        return PostResult::Closed(
            absl::UnavailableError("peer endpoint is not bound"));
      }
      reservation_.assign(length, 0);
      fill(Reservation{reservation_.data(), length});
      state_->bytes[side_] += length;
      ++state_->posts[side_];
      copied.assign(reservation_.begin(), reservation_.end());
    }
    peer_inbound_executor_->Run(
        [peer = std::move(peer), copied = std::move(copied)]() mutable {
          (void)peer->OnIncomingData(copied);
        });
    return PostResult::Accepted();
  }

  absl::Status Flush() override { return absl::OkStatus(); }

  void ResumeReceive() override {}

  void Close() override {
    std::shared_ptr<DmeshEndpointDriver> peer;
    {
      std::lock_guard<std::mutex> lock(state_->mu);
      if (state_->closed[side_]) return;
      state_->closed[side_] = true;
      peer = state_->drivers[1 - side_].lock();
    }
    if (peer != nullptr) peer->OnRemoteEof();
  }

 private:
  std::shared_ptr<LinkState> state_;
  int side_;
  Executor* const peer_inbound_executor_;
  std::vector<uint8_t> reservation_;
};

EventEngine::ResolvedAddress Address(uint16_t port) {
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  return EventEngine::ResolvedAddress(
      reinterpret_cast<const sockaddr*>(&address), sizeof(address));
}

bool Next(::grpc::CompletionQueue* cq, void* expected_tag) {
  void* tag = nullptr;
  bool ok = false;
  const auto result = cq->AsyncNext(
      &tag, &ok, std::chrono::system_clock::now() + std::chrono::seconds(10));
  return result == ::grpc::CompletionQueue::GOT_EVENT && ok &&
         tag == expected_tag;
}

::grpc::ByteBuffer MakeBuffer(const std::string& value) {
  ::grpc::Slice slice(value);
  return ::grpc::ByteBuffer(&slice, 1);
}

std::string Flatten(const ::grpc::ByteBuffer& buffer) {
  std::vector<::grpc::Slice> slices;
  if (!buffer.Dump(&slices).ok()) return {};
  std::string value;
  for (const auto& slice : slices) {
    value.append(reinterpret_cast<const char*>(slice.begin()), slice.size());
  }
  return value;
}

/// One echo server, reachable only through DmeshEndpoint pairs.
///
/// A channel is created from an endpoint whose peer is accepted by the server's
/// passive listener, which is the shape a DPUmesh session presents to gRPC: no
/// socket, no listener address, one connected endpoint per channel.
class EchoServer {
 public:
  /// `calls` is how many exchanges this server will serve. Each gets its own
  /// completion queue: a queue shared by two serving threads hands one thread
  /// the other's tag, and the call it belongs to is then abandoned mid-flight.
  explicit EchoServer(int calls = 1) {
    builder_.RegisterAsyncGenericService(&service_);
    for (int i = 0; i < calls; ++i) cqs_.push_back(builder_.AddCompletionQueue());
    builder_.experimental().AddPassiveListener(
        ::grpc::InsecureServerCredentials(), listener_);
    server_ = builder_.BuildAndStart();
  }

  ~EchoServer() {
    Shutdown();
    for (auto& cq : cqs_) cq->Shutdown();
    for (auto& thread : threads_) {
      if (thread.joinable()) thread.join();
    }
    for (auto& cq : cqs_) {
      void* tag = nullptr;
      bool ok = false;
      while (cq->Next(&tag, &ok)) {
      }
    }
  }

  bool ok() const { return server_ != nullptr && listener_ != nullptr; }

  void Shutdown() {
    if (server_ == nullptr || shutdown_) return;
    server_->Shutdown();
    shutdown_ = true;
  }

  bool Accept(std::unique_ptr<DmeshEndpoint> endpoint) {
    return listener_->AcceptConnectedEndpoint(std::move(endpoint)).ok();
  }

  /// Serve one unary call. `done` reports whether the exchange completed.
  void ServeOne(std::shared_ptr<std::atomic<bool>> done) {
    ::grpc::ServerCompletionQueue* cq = cqs_[threads_.size()].get();
    threads_.emplace_back([this, cq, done] {
      constexpr uintptr_t kAccept = 1;
      constexpr uintptr_t kRead = 2;
      constexpr uintptr_t kWrite = 3;
      constexpr uintptr_t kFinish = 4;
      ::grpc::GenericServerContext context;
      ::grpc::GenericServerAsyncReaderWriter stream(&context);
      service_.RequestCall(&context, &stream, cq, cq,
                           reinterpret_cast<void*>(kAccept));
      if (!Next(cq, reinterpret_cast<void*>(kAccept))) return;
      if (context.method() != "/dpumesh.test.Echo/Unary") return;
      ::grpc::ByteBuffer request;
      stream.Read(&request, reinterpret_cast<void*>(kRead));
      if (!Next(cq, reinterpret_cast<void*>(kRead))) return;
      stream.Write(request, reinterpret_cast<void*>(kWrite));
      if (!Next(cq, reinterpret_cast<void*>(kWrite))) return;
      stream.Finish(::grpc::Status::OK, reinterpret_cast<void*>(kFinish));
      done->store(Next(cq, reinterpret_cast<void*>(kFinish)));
    });
  }

 private:
  ::grpc::AsyncGenericService service_;
  ::grpc::ServerBuilder builder_;
  std::unique_ptr<::grpc::experimental::PassiveListener> listener_;
  std::vector<std::unique_ptr<::grpc::ServerCompletionQueue>> cqs_;
  std::unique_ptr<::grpc::Server> server_;
  std::vector<std::thread> threads_;
  bool shutdown_ = false;
};

/// The executors and endpoint pair behind one channel.
struct Link {
  ThreadExecutor client_work;
  ThreadExecutor client_callbacks;
  ThreadExecutor server_work;
  ThreadExecutor server_callbacks;
  std::shared_ptr<LinkState> state = std::make_shared<LinkState>();

  std::unique_ptr<DmeshEndpoint> ClientEndpoint(uint16_t port) {
    return std::make_unique<DmeshEndpoint>(
        std::make_unique<LinkedEndpointTransport>(state, 0, &server_work),
        UnownedExecutor(&client_callbacks),
        MemoryAllocator(std::make_shared<TestMemoryAllocator>()),
        Address(port + 1), Address(port));
  }

  std::unique_ptr<DmeshEndpoint> ServerEndpoint(uint16_t port) {
    return std::make_unique<DmeshEndpoint>(
        std::make_unique<LinkedEndpointTransport>(state, 1, &client_work),
        UnownedExecutor(&server_callbacks),
        MemoryAllocator(std::make_shared<TestMemoryAllocator>()),
        Address(port), Address(port + 1));
  }

  /// Half-close one side, as a peer FIN does.
  void Close(int side) {
    std::shared_ptr<DmeshEndpointDriver> peer;
    {
      std::lock_guard<std::mutex> lock(state->mu);
      if (state->closed[side]) return;
      state->closed[side] = true;
      peer = state->drivers[1 - side].lock();
    }
    if (peer != nullptr) peer->OnRemoteEof();
  }
};

struct CallResult {
  bool client_ok = false;
  bool server_ok = false;
  ::grpc::Status status;
  std::string response;
};

/// One unary echo over `channel`, waited to completion.
CallResult UnaryEcho(const std::shared_ptr<::grpc::Channel>& channel,
                     const std::string& payload) {
  CallResult result;
  ::grpc::GenericStub stub(channel);
  ::grpc::CompletionQueue cq;
  ::grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() +
                       std::chrono::seconds(10));
  ::grpc::ByteBuffer request = MakeBuffer(payload);
  ::grpc::ByteBuffer response;
  constexpr uintptr_t kClientFinish = 5;
  auto call = stub.PrepareUnaryCall(&context, "/dpumesh.test.Echo/Unary",
                                    request, &cq);
  call->StartCall();
  call->Finish(&response, &result.status, reinterpret_cast<void*>(kClientFinish));
  result.client_ok = Next(&cq, reinterpret_cast<void*>(kClientFinish));
  result.response = Flatten(response);
  cq.Shutdown();
  void* tag = nullptr;
  bool ok = false;
  while (cq.Next(&tag, &ok)) {
  }
  return result;
}

std::shared_ptr<::grpc::Channel> Connect(EchoServer* server, Link* link,
                                         uint16_t port) {
  if (!server->Accept(link->ServerEndpoint(port))) return nullptr;
  return ::grpc::experimental::CreateChannelFromEndpoint(
      link->ClientEndpoint(port), ::grpc::InsecureChannelCredentials(),
      ::grpc::ChannelArguments());
}

bool RunUnaryEcho() {
  // The server holds the accepted endpoint until it shuts down, and that
  // endpoint schedules onto the link's executors: the link must outlive it.
  Link link;
  EchoServer server;
  if (!server.ok()) return false;
  auto served = std::make_shared<std::atomic<bool>>(false);
  server.ServeOne(served);
  auto channel = Connect(&server, &link, 50051);
  if (channel == nullptr) return false;

  const std::string payload(4096, 'd');
  const CallResult call = UnaryEcho(channel, payload);

  size_t client_bytes = 0;
  size_t server_bytes = 0;
  size_t client_posts = 0;
  size_t server_posts = 0;
  {
    std::lock_guard<std::mutex> lock(link.state->mu);
    client_bytes = link.state->bytes[0];
    server_bytes = link.state->bytes[1];
    client_posts = link.state->posts[0];
    server_posts = link.state->posts[1];
  }
  channel.reset();

  std::cout << "client_bytes=" << client_bytes
            << " server_bytes=" << server_bytes
            << " client_posts=" << client_posts
            << " server_posts=" << server_posts << '\n';
  return call.client_ok && served->load() && call.status.ok() &&
         call.response == payload && client_bytes > payload.size() &&
         server_bytes > payload.size() && client_posts > 1 && server_posts > 1;
}

/// Channels created and destroyed against one service, in one process.
///
/// This is the shape of a DPUmesh L7 session opening and closing repeatedly:
/// every cycle builds a new endpoint pair and drops it, and no cycle may
/// inherit anything from the one before it.
bool RunChannelChurn() {
  constexpr int kCycles = 12;
  std::vector<std::unique_ptr<Link>> links;   // outlive the server
  EchoServer server(kCycles);
  if (!server.ok()) return false;
  const std::string payload(1024, 'c');
  for (int i = 0; i < kCycles; ++i) {
    links.push_back(std::make_unique<Link>());
    auto served = std::make_shared<std::atomic<bool>>(false);
    server.ServeOne(served);
    auto channel =
        Connect(&server, links.back().get(), static_cast<uint16_t>(50100 + 2 * i));
    if (channel == nullptr) {
      std::cerr << "churn cycle " << i << ": no channel\n";
      return false;
    }
    const CallResult call = UnaryEcho(channel, payload);
    if (!call.client_ok || !call.status.ok() || call.response != payload ||
        !served->load()) {
      std::cerr << "churn cycle " << i << " failed: " << call.status.error_message()
                << '\n';
      return false;
    }
    channel.reset();
  }
  std::cout << "channel churn cycles=" << kCycles << '\n';
  return true;
}

/// Several channels to one service, with their calls in flight together.
bool RunConcurrentChannels() {
  constexpr int kChannels = 4;
  std::vector<std::unique_ptr<Link>> links;   // outlive the server
  EchoServer server(kChannels);
  if (!server.ok()) return false;
  std::vector<std::shared_ptr<::grpc::Channel>> channels;
  std::vector<std::shared_ptr<std::atomic<bool>>> served;
  for (int i = 0; i < kChannels; ++i) {
    links.push_back(std::make_unique<Link>());
    served.push_back(std::make_shared<std::atomic<bool>>(false));
    server.ServeOne(served.back());
    auto channel =
        Connect(&server, links.back().get(), static_cast<uint16_t>(50200 + 2 * i));
    if (channel == nullptr) return false;
    channels.push_back(std::move(channel));
  }

  std::vector<CallResult> results(kChannels);
  std::vector<std::thread> callers;
  for (int i = 0; i < kChannels; ++i) {
    callers.emplace_back([&, i] {
      results[i] = UnaryEcho(channels[i], std::string(2048, 'a' + i));
    });
  }
  for (auto& caller : callers) caller.join();

  for (int i = 0; i < kChannels; ++i) {
    if (!results[i].client_ok || !results[i].status.ok() ||
        results[i].response != std::string(2048, 'a' + i) ||
        !served[i]->load()) {
      std::cerr << "channel " << i << " failed: " << results[i].status.error_message()
                << '\n';
      return false;
    }
  }
  channels.clear();
  std::cout << "concurrent channels=" << kChannels << '\n';
  return true;
}

/// A closed peer ends the channel rather than hanging a call on it.
bool RunPeerClose(int side, const char* name) {
  Link link;
  EchoServer server(1);
  if (!server.ok()) return false;
  auto served = std::make_shared<std::atomic<bool>>(false);
  server.ServeOne(served);
  auto channel = Connect(&server, &link, 50300);
  if (channel == nullptr) return false;

  const std::string payload(512, 'p');
  if (!UnaryEcho(channel, payload).status.ok()) {
    std::cerr << name << ": the first call did not complete\n";
    return false;
  }

  link.Close(side);
  const CallResult after = UnaryEcho(channel, payload);
  channel.reset();
  if (after.status.ok()) {
    std::cerr << name << ": a call succeeded after the peer closed\n";
    return false;
  }
  std::cout << name << " -> " << after.status.error_code() << '\n';
  return true;
}

/// A graceful server shutdown sends GOAWAY and retires the existing channel.
bool RunServerGoAway() {
  Link link;
  EchoServer server(1);
  if (!server.ok()) return false;
  auto served = std::make_shared<std::atomic<bool>>(false);
  server.ServeOne(served);
  auto channel = Connect(&server, &link, 50400);
  if (channel == nullptr) return false;

  const std::string payload(512, 'g');
  const CallResult before = UnaryEcho(channel, payload);
  if (!before.client_ok || !before.status.ok() || before.response != payload ||
      !served->load()) {
    std::cerr << "GOAWAY: the first call did not complete\n";
    return false;
  }

  server.Shutdown();
  const CallResult after = UnaryEcho(channel, payload);
  channel.reset();
  if (after.status.ok()) {
    std::cerr << "GOAWAY: a call succeeded after server shutdown\n";
    return false;
  }
  std::cout << "server GOAWAY -> " << after.status.error_code() << '\n';
  return true;
}

}  // namespace
}  // namespace dpumesh::grpc::testing

int main() {
  using namespace dpumesh::grpc::testing;  // NOLINT(build/namespaces)
  const struct {
    const char* name;
    bool (*run)();
  } cases[] = {
      {"unary echo", RunUnaryEcho},
      {"channel churn", RunChannelChurn},
      {"concurrent channels", RunConcurrentChannels},
      {"server GOAWAY", RunServerGoAway},
  };
  for (const auto& c : cases) {
    if (!c.run()) {
      std::cerr << "gRPC over DmeshEndpoint failed: " << c.name << '\n';
      return 1;
    }
  }
  if (!RunPeerClose(1, "server FIN") || !RunPeerClose(0, "client FIN")) {
    return 1;
  }
  std::cout << "gRPC over DmeshEndpoint passed\n";
  return 0;
}
