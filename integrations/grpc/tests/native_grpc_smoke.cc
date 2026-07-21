#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <future>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <grpc/event_engine/internal/memory_allocator_impl.h>
#include <grpc/event_engine/memory_allocator.h>
#include <grpc/event_engine/memory_request.h>
#include <grpc/impl/channel_arg_names.h>
#include <grpcpp/generic/async_generic_service.h>
#include <grpcpp/generic/generic_stub.h>
#include <grpcpp/security/credentials.h>
#include <grpcpp/server_builder.h>
#include <grpcpp/support/byte_buffer.h>
#include <grpcpp/support/slice.h>

#include "absl/functional/any_invocable.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "dmesh_api_ops.h"
#include "dmesh_grpc_runtime.h"
#include "dmesh_runtime.h"
#include "executor.h"

namespace dpumesh::grpc::testing {
namespace {

using MemoryAllocator = grpc_event_engine::experimental::MemoryAllocator;
using MemoryAllocatorImpl =
    grpc_event_engine::experimental::internal::MemoryAllocatorImpl;
using MemoryRequest = grpc_event_engine::experimental::MemoryRequest;
using namespace std::chrono_literals;

constexpr char kMethod[] = "/dpumesh.smoke.Echo/Unary";
constexpr auto kTimeout = 60s;

class ThreadExecutor final : public Executor {
 public:
  ThreadExecutor() : worker_([this] { RunLoop(); }) {}
  ~ThreadExecutor() override {
    {
      std::lock_guard<std::mutex> lock(mu_);
      stopping_ = true;
    }
    cv_.notify_one();
    worker_.join();
  }

  void Run(absl::AnyInvocable<void()> task) override {
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (stopping_) return;
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
  std::thread worker_;
};

class SmokeMemoryAllocator final : public MemoryAllocatorImpl {
 public:
  size_t Reserve(MemoryRequest request) override { return request.max(); }
  grpc_slice MakeSlice(MemoryRequest request) override {
    return grpc_slice_malloc(request.max());
  }
  void Release(size_t /*bytes*/) override {}
  void Shutdown() override {}
};

MemoryAllocator MakeAllocator() {
  return MemoryAllocator(std::make_shared<SmokeMemoryAllocator>());
}

::grpc::ByteBuffer MakeBuffer(const std::string& value) {
  ::grpc::Slice slice(value);
  return ::grpc::ByteBuffer(&slice, 1);
}

absl::StatusOr<std::string> Flatten(const ::grpc::ByteBuffer& buffer) {
  std::vector<::grpc::Slice> slices;
  const ::grpc::Status status = buffer.Dump(&slices);
  if (!status.ok()) {
    return absl::DataLossError("failed to flatten gRPC ByteBuffer");
  }
  std::string value;
  for (const auto& slice : slices) {
    value.append(reinterpret_cast<const char*>(slice.begin()), slice.size());
  }
  return value;
}

bool Next(::grpc::CompletionQueue* cq, void* expected_tag) {
  void* tag = nullptr;
  bool ok = false;
  const auto result = cq->AsyncNext(
      &tag, &ok, std::chrono::system_clock::now() + kTimeout);
  return result == ::grpc::CompletionQueue::GOT_EVENT && ok &&
         tag == expected_tag;
}

absl::StatusOr<size_t> ParsePositive(const char* value, const char* name) {
  char* end = nullptr;
  const unsigned long long parsed = std::strtoull(value, &end, 10);
  if (value[0] == '\0' || end == nullptr || *end != '\0' || parsed == 0 ||
      parsed > std::numeric_limits<size_t>::max()) {
    return absl::InvalidArgumentError(std::string("invalid ") + name);
  }
  return static_cast<size_t>(parsed);
}

absl::StatusOr<std::unique_ptr<DmeshRuntime>> CreateRuntime(
    ThreadExecutor* callbacks, size_t reactors) {
  DmeshRuntime::Options options;
  options.reactor_count = reactors;
  return DmeshRuntime::Create(MakeNativeDmeshApiOps(), callbacks, options);
}

absl::Status RunServer(size_t calls, size_t reactors) {
  ThreadExecutor callbacks;
  auto runtime_result = CreateRuntime(&callbacks, reactors);
  if (!runtime_result.ok()) return runtime_result.status();
  auto runtime = std::move(*runtime_result);

  ::grpc::AsyncGenericService service;
  ::grpc::ServerBuilder builder;
  std::unique_ptr<::grpc::experimental::PassiveListener> passive_listener;
  builder.RegisterAsyncGenericService(&service);
  auto cq = builder.AddCompletionQueue();
  builder.experimental().AddPassiveListener(
      ::grpc::InsecureServerCredentials(), passive_listener);
  auto server = builder.BuildAndStart();
  if (server == nullptr || passive_listener == nullptr) {
    return absl::InternalError("failed to start passive gRPC server");
  }

  std::mutex accept_error_mu;
  std::optional<absl::Status> accept_error;
  auto attachment = AttachDmeshGrpcServer(
      runtime.get(), passive_listener.get(), [] { return MakeAllocator(); },
      [&accept_error_mu, &accept_error](const absl::Status& status) {
        std::lock_guard<std::mutex> lock(accept_error_mu);
        accept_error = status;
      });
  if (!attachment.ok()) {
    server->Shutdown();
    cq->Shutdown();
    void* tag = nullptr;
    bool ok = false;
    while (cq->Next(&tag, &ok)) {
    }
    return attachment.status();
  }

  bool shutdown = false;
  auto shutdown_server = [&] {
    if (shutdown) return;
    shutdown = true;
    (*attachment)->Detach();
    server->Shutdown();
    cq->Shutdown();
    void* tag = nullptr;
    bool ok = false;
    while (cq->Next(&tag, &ok)) {
    }
  };

  std::cout << "READY: native gRPC server calls=" << calls
            << " reactors=" << reactors << std::endl;
  constexpr uintptr_t kAccept = 1;
  constexpr uintptr_t kRead = 2;
  constexpr uintptr_t kWrite = 3;
  constexpr uintptr_t kFinish = 4;
  absl::Status outcome = absl::OkStatus();
  for (size_t i = 0; i < calls; ++i) {
    ::grpc::GenericServerContext context;
    ::grpc::GenericServerAsyncReaderWriter stream(&context);
    service.RequestCall(&context, &stream, cq.get(), cq.get(),
                        reinterpret_cast<void*>(kAccept));
    if (!Next(cq.get(), reinterpret_cast<void*>(kAccept))) {
      {
        std::lock_guard<std::mutex> lock(accept_error_mu);
        outcome = accept_error.has_value()
                      ? *accept_error
                      : absl::DeadlineExceededError("server accept timed out");
      }
      shutdown_server();
      break;
    }
    if (context.method() != kMethod) {
      outcome = absl::UnimplementedError("unexpected gRPC method");
      shutdown_server();
      break;
    }

    ::grpc::ByteBuffer request;
    stream.Read(&request, reinterpret_cast<void*>(kRead));
    if (!Next(cq.get(), reinterpret_cast<void*>(kRead))) {
      outcome = absl::DataLossError("server request read failed");
      shutdown_server();
      break;
    }
    stream.Write(request, reinterpret_cast<void*>(kWrite));
    if (!Next(cq.get(), reinterpret_cast<void*>(kWrite))) {
      outcome = absl::UnavailableError("server response write failed");
      shutdown_server();
      break;
    }
    stream.Finish(::grpc::Status::OK, reinterpret_cast<void*>(kFinish));
    if (!Next(cq.get(), reinterpret_cast<void*>(kFinish))) {
      outcome = absl::UnavailableError("server RPC finish failed");
      shutdown_server();
      break;
    }
  }

  shutdown_server();
  server.reset();
  passive_listener.reset();
  runtime.reset();
  if (!outcome.ok()) return outcome;
  std::cout << "PASS: native gRPC server calls=" << calls << '\n';
  return absl::OkStatus();
}

absl::Status RunClient(const std::string& target,
                       const std::string& authority, size_t calls,
                       size_t payload_size, size_t reactors) {
  ThreadExecutor callbacks;
  auto runtime_result = CreateRuntime(&callbacks, reactors);
  if (!runtime_result.ok()) return runtime_result.status();
  auto runtime = std::move(*runtime_result);

  auto promise = std::make_shared<std::promise<
      absl::StatusOr<std::shared_ptr<::grpc::Channel>>>>();
  auto future = promise->get_future();
  ::grpc::ChannelArguments channel_args;
  if (!authority.empty()) {
    channel_args.SetString(GRPC_ARG_DEFAULT_AUTHORITY, authority);
  }
  ConnectDmeshGrpcChannel(
      runtime.get(), target,
      ::grpc::InsecureChannelCredentials(), channel_args,
      [promise](absl::StatusOr<std::shared_ptr<::grpc::Channel>> result) {
        promise->set_value(std::move(result));
      });
  if (future.wait_for(kTimeout) != std::future_status::ready) {
    return absl::DeadlineExceededError("native gRPC channel creation timed out");
  }
  auto channel_result = future.get();
  if (!channel_result.ok()) return channel_result.status();
  auto channel = std::move(*channel_result);
  ::grpc::GenericStub stub(channel);
  ::grpc::CompletionQueue cq;

  size_t wire_payload = 0;
  for (size_t i = 0; i < calls; ++i) {
    std::string payload(payload_size,
                        static_cast<char>('a' + static_cast<int>(i % 26)));
    ::grpc::ByteBuffer request = MakeBuffer(payload);
    ::grpc::ByteBuffer response;
    ::grpc::Status status;
    ::grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + kTimeout);
    auto call = stub.PrepareUnaryCall(&context, kMethod, request, &cq);
    const uintptr_t tag_value = i + 1;
    void* tag = reinterpret_cast<void*>(tag_value);
    call->StartCall();
    call->Finish(&response, &status, tag);
    if (!Next(&cq, tag)) {
      return absl::DeadlineExceededError("client unary completion timed out");
    }
    if (!status.ok()) {
      return absl::UnavailableError(
          std::string("gRPC status: ") + status.error_message());
    }
    auto echoed = Flatten(response);
    if (!echoed.ok()) return echoed.status();
    if (*echoed != payload) {
      return absl::DataLossError("native gRPC echo payload mismatch");
    }
    wire_payload += payload.size();
  }

  channel.reset();
  cq.Shutdown();
  runtime.reset();
  std::cout << "PASS: native gRPC client target=" << target
            << " authority=" << (authority.empty() ? target : authority)
            << " calls=" << calls << " payload_bytes=" << wire_payload
            << " reactors=" << reactors << '\n';
  return absl::OkStatus();
}

}  // namespace
}  // namespace dpumesh::grpc::testing

int main(int argc, char** argv) {
  if (argc < 3 || argc > 7) {
    std::cerr
        << "usage:\n  " << argv[0]
        << " server CALLS [REACTORS=1]\n  " << argv[0]
        << " client TARGET CALLS [PAYLOAD=4096] [REACTORS=1] "
           "[AUTHORITY=TARGET]\n";
    return EXIT_FAILURE;
  }

  absl::Status status;
  const std::string mode = argv[1];
  if (mode == "server") {
    auto calls = dpumesh::grpc::testing::ParsePositive(argv[2], "calls");
    auto reactors = argc >= 4
                        ? dpumesh::grpc::testing::ParsePositive(argv[3],
                                                               "reactors")
                        : absl::StatusOr<size_t>(size_t{1});
    if (!calls.ok() || !reactors.ok()) {
      std::cerr << (!calls.ok() ? calls.status() : reactors.status()) << '\n';
      return EXIT_FAILURE;
    }
    status = dpumesh::grpc::testing::RunServer(*calls, *reactors);
  } else if (mode == "client") {
    if (argc < 4) {
      std::cerr << "client mode requires TARGET and CALLS\n";
      return EXIT_FAILURE;
    }
    auto calls = dpumesh::grpc::testing::ParsePositive(argv[3], "calls");
    auto payload = argc >= 5
                       ? dpumesh::grpc::testing::ParsePositive(argv[4],
                                                              "payload")
                       : absl::StatusOr<size_t>(size_t{4096});
    auto reactors = argc >= 6
                        ? dpumesh::grpc::testing::ParsePositive(argv[5],
                                                               "reactors")
                        : absl::StatusOr<size_t>(size_t{1});
    if (!calls.ok() || !payload.ok() || !reactors.ok()) {
      std::cerr << (!calls.ok() ? calls.status()
                                : (!payload.ok() ? payload.status()
                                                 : reactors.status()))
                << '\n';
      return EXIT_FAILURE;
    }
    const std::string authority = argc >= 7 ? argv[6] : argv[2];
    status = dpumesh::grpc::testing::RunClient(
        argv[2], authority, *calls, *payload, *reactors);
  } else {
    std::cerr << "mode must be server or client\n";
    return EXIT_FAILURE;
  }

  if (!status.ok()) {
    std::cerr << "FAIL: native gRPC smoke: " << status << '\n';
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
