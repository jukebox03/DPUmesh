#include <arpa/inet.h>

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
#include <grpcpp/generic/async_generic_service.h>
#include <grpcpp/generic/generic_stub.h>
#include <grpcpp/passive_listener.h>
#include <grpcpp/security/credentials.h>
#include <grpcpp/server_builder.h>
#include <grpcpp/support/byte_buffer.h>
#include <grpcpp/support/slice.h>

#include "absl/status/status.h"
#include "dmesh_endpoint.h"
#include "dmesh_grpc_channel.h"
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

  PostResult Post(absl::Span<const uint8_t> bytes) override {
    std::shared_ptr<DmeshEndpointDriver> peer;
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
      state_->bytes[side_] += bytes.size();
      ++state_->posts[side_];
    }
    std::vector<uint8_t> copied(bytes.begin(), bytes.end());
    peer_inbound_executor_->Run(
        [peer = std::move(peer), copied = std::move(copied)]() mutable {
          (void)peer->OnIncomingData(copied);
        });
    return PostResult::Accepted();
  }

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

bool RunUnaryEcho() {
  ThreadExecutor client_work;
  ThreadExecutor client_callbacks;
  ThreadExecutor server_work;
  ThreadExecutor server_callbacks;
  auto link = std::make_shared<LinkState>();

  auto client_endpoint = std::make_unique<DmeshEndpoint>(
      std::make_unique<LinkedEndpointTransport>(link, 0, &server_work),
      &client_work, &client_callbacks,
      MemoryAllocator(std::make_shared<TestMemoryAllocator>()), Address(50052),
      Address(50051));
  auto server_endpoint = std::make_unique<DmeshEndpoint>(
      std::make_unique<LinkedEndpointTransport>(link, 1, &client_work),
      &server_work, &server_callbacks,
      MemoryAllocator(std::make_shared<TestMemoryAllocator>()), Address(50051),
      Address(50052));

  ::grpc::AsyncGenericService service;
  ::grpc::ServerBuilder builder;
  std::unique_ptr<::grpc::experimental::PassiveListener> passive_listener;
  builder.RegisterAsyncGenericService(&service);
  auto server_cq = builder.AddCompletionQueue();
  builder.experimental().AddPassiveListener(
      ::grpc::InsecureServerCredentials(), passive_listener);
  std::unique_ptr<::grpc::Server> server = builder.BuildAndStart();
  if (server == nullptr || passive_listener == nullptr) return false;
  if (!passive_listener->AcceptConnectedEndpoint(std::move(server_endpoint))
           .ok()) {
    server->Shutdown();
    server_cq->Shutdown();
    return false;
  }

  constexpr uintptr_t kAccept = 1;
  constexpr uintptr_t kRead = 2;
  constexpr uintptr_t kWrite = 3;
  constexpr uintptr_t kFinish = 4;
  bool server_ok = false;
  std::thread server_thread([&] {
    ::grpc::GenericServerContext context;
    ::grpc::GenericServerAsyncReaderWriter stream(&context);
    service.RequestCall(&context, &stream, server_cq.get(), server_cq.get(),
                        reinterpret_cast<void*>(kAccept));
    if (!Next(server_cq.get(), reinterpret_cast<void*>(kAccept))) return;
    if (context.method() != "/dpumesh.test.Echo/Unary") return;

    ::grpc::ByteBuffer request;
    stream.Read(&request, reinterpret_cast<void*>(kRead));
    if (!Next(server_cq.get(), reinterpret_cast<void*>(kRead))) return;
    stream.Write(request, reinterpret_cast<void*>(kWrite));
    if (!Next(server_cq.get(), reinterpret_cast<void*>(kWrite))) return;
    stream.Finish(::grpc::Status::OK,
                  reinterpret_cast<void*>(kFinish));
    server_ok = Next(server_cq.get(), reinterpret_cast<void*>(kFinish));
  });

  auto channel = CreateDmeshGrpcChannel(
      std::move(client_endpoint), ::grpc::InsecureChannelCredentials());
  if (channel == nullptr) {
    server->Shutdown();
    server_cq->Shutdown();
    server_thread.join();
    return false;
  }

  ::grpc::GenericStub stub(channel);
  ::grpc::CompletionQueue client_cq;
  ::grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() +
                       std::chrono::seconds(10));
  const std::string payload(4096, 'd');
  ::grpc::ByteBuffer request = MakeBuffer(payload);
  ::grpc::ByteBuffer response;
  ::grpc::Status status;
  auto call = stub.PrepareUnaryCall(&context, "/dpumesh.test.Echo/Unary",
                                    request, &client_cq);
  constexpr uintptr_t kClientFinish = 5;
  call->StartCall();
  call->Finish(&response, &status,
               reinterpret_cast<void*>(kClientFinish));
  const bool client_ok =
      Next(&client_cq, reinterpret_cast<void*>(kClientFinish));

  server_thread.join();
  const bool payload_ok = Flatten(response) == payload;
  size_t client_bytes = 0;
  size_t server_bytes = 0;
  size_t client_posts = 0;
  size_t server_posts = 0;
  {
    std::lock_guard<std::mutex> lock(link->mu);
    client_bytes = link->bytes[0];
    server_bytes = link->bytes[1];
    client_posts = link->posts[0];
    server_posts = link->posts[1];
  }

  channel.reset();
  client_cq.Shutdown();
  server->Shutdown();
  server_cq->Shutdown();

  std::cout << "client_bytes=" << client_bytes
            << " server_bytes=" << server_bytes
            << " client_posts=" << client_posts
            << " server_posts=" << server_posts << '\n';
  return client_ok && server_ok && status.ok() && payload_ok &&
         client_bytes > payload.size() && server_bytes > payload.size() &&
         client_posts > 1 && server_posts > 1;
}

}  // namespace
}  // namespace dpumesh::grpc::testing

int main() {
  if (!dpumesh::grpc::testing::RunUnaryEcho()) {
    std::cerr << "gRPC unary RPC over DmeshEndpoint failed\n";
    return 1;
  }
  std::cout << "gRPC unary RPC over DmeshEndpoint passed\n";
  return 0;
}
