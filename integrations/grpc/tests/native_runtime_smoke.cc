#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <deque>
#include <future>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <grpc/event_engine/internal/memory_allocator_impl.h>
#include <grpc/event_engine/memory_allocator.h>
#include <grpc/event_engine/memory_request.h>
#include <grpc/event_engine/slice.h>
#include <grpc/event_engine/slice_buffer.h>
#include <grpc/slice.h>

#include "absl/functional/any_invocable.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "dmesh_api_ops.h"
#include "dmesh_endpoint.h"
#include "dmesh_runtime.h"
#include "executor.h"

namespace dpumesh::grpc::testing {
namespace {

using EventEngine = grpc_event_engine::experimental::EventEngine;
using MemoryAllocator = grpc_event_engine::experimental::MemoryAllocator;
using MemoryAllocatorImpl =
    grpc_event_engine::experimental::internal::MemoryAllocatorImpl;
using MemoryRequest = grpc_event_engine::experimental::MemoryRequest;
using Slice = grpc_event_engine::experimental::Slice;
using SliceBuffer = grpc_event_engine::experimental::SliceBuffer;

constexpr std::chrono::seconds kOperationTimeout(10);
constexpr uint32_t kBenchRequestMagic = 0x62526571u;
constexpr uint32_t kBenchReplyMagic = 0x62526570u;
constexpr size_t kBenchHeaderLength = 16;
constexpr uint8_t kBenchReplyFill = 43;

class ThreadExecutor final : public Executor {
 public:
  ThreadExecutor() : worker_([this] { ThreadMain(); }) {}

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
  void ThreadMain() {
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
  void Shutdown() override { shutdowns_.fetch_add(1, std::memory_order_relaxed); }

 private:
  std::atomic<int> shutdowns_{0};
};

template <typename T>
absl::StatusOr<T> Await(std::future<T>& future, const char* operation) {
  if (future.wait_for(kOperationTimeout) != std::future_status::ready) {
    return absl::DeadlineExceededError(std::string(operation) + " timed out");
  }
  return future.get();
}

absl::Status AwaitStatus(std::future<absl::Status>& future,
                         const char* operation) {
  auto result = Await(future, operation);
  return result.ok() ? std::move(*result) : result.status();
}

std::vector<uint8_t> MakePayload(size_t length, size_t connection,
                                 size_t round, size_t size_index) {
  std::vector<uint8_t> payload(length);
  uint32_t state = static_cast<uint32_t>(
      0x9e3779b9u ^ (connection * 131u) ^ (round * 17u) ^ size_index);
  for (size_t i = 0; i < payload.size(); ++i) {
    state = state * 1664525u + 1013904223u;
    payload[i] = static_cast<uint8_t>(state >> 24);
  }
  return payload;
}

void PutUint32(std::vector<uint8_t>* bytes, size_t offset, uint32_t value) {
  std::memcpy(bytes->data() + offset, &value, sizeof(value));
}

uint32_t GetUint32(const std::vector<uint8_t>& bytes, size_t offset) {
  uint32_t value = 0;
  std::memcpy(&value, bytes.data() + offset, sizeof(value));
  return value;
}

std::vector<uint8_t> MakeBenchRequest(size_t payload_length, uint32_t sequence,
                                      size_t connection, size_t round,
                                      size_t size_index) {
  std::vector<uint8_t> request(kBenchHeaderLength + payload_length);
  PutUint32(&request, 0, kBenchRequestMagic);
  PutUint32(&request, 4, sequence);
  PutUint32(&request, 8, static_cast<uint32_t>(payload_length));
  PutUint32(&request, 12, static_cast<uint32_t>(payload_length));
  std::vector<uint8_t> payload =
      MakePayload(payload_length, connection, round, size_index);
  std::copy(payload.begin(), payload.end(),
            request.begin() + kBenchHeaderLength);
  return request;
}

absl::Status ValidateBenchReply(const std::vector<uint8_t>& response,
                                uint32_t sequence, size_t payload_length) {
  if (response.size() != kBenchHeaderLength + payload_length) {
    return absl::DataLossError("bench reply length mismatch");
  }
  if (GetUint32(response, 0) != kBenchReplyMagic ||
      GetUint32(response, 4) != sequence ||
      GetUint32(response, 8) != payload_length) {
    return absl::DataLossError("bench reply header mismatch");
  }
  for (size_t i = kBenchHeaderLength; i < response.size(); ++i) {
    if (response[i] != kBenchReplyFill) {
      return absl::DataLossError("bench reply payload mismatch");
    }
  }
  return absl::OkStatus();
}

absl::Status WritePayload(DmeshEndpoint* endpoint,
                          const std::vector<uint8_t>& payload) {
  SliceBuffer buffer;
  buffer.Append(Slice(grpc_slice_from_copied_buffer(
      reinterpret_cast<const char*>(payload.data()), payload.size())));

  auto promise = std::make_shared<std::promise<absl::Status>>();
  std::future<absl::Status> future = promise->get_future();
  const bool synchronous = endpoint->Write(
      [promise](absl::Status status) { promise->set_value(std::move(status)); },
      &buffer, EventEngine::Endpoint::WriteArgs());
  if (synchronous) return absl::OkStatus();
  return AwaitStatus(future, "endpoint write");
}

absl::StatusOr<std::vector<uint8_t>> ReadPayload(DmeshEndpoint* endpoint,
                                                 size_t expected_length) {
  std::vector<uint8_t> result;
  result.reserve(expected_length);

  while (result.size() < expected_length) {
    SliceBuffer buffer;
    auto promise = std::make_shared<std::promise<absl::Status>>();
    std::future<absl::Status> future = promise->get_future();
    const bool synchronous = endpoint->Read(
        [promise](absl::Status status) {
          promise->set_value(std::move(status));
        },
        &buffer, EventEngine::Endpoint::ReadArgs());
    if (!synchronous) {
      const absl::Status status = AwaitStatus(future, "endpoint read");
      if (!status.ok()) return status;
    }

    if (buffer.Length() == 0) {
      return absl::DataLossError("endpoint returned an empty successful read");
    }
    if (buffer.Length() > expected_length - result.size()) {
      return absl::DataLossError("endpoint returned more bytes than requested");
    }
    for (size_t i = 0; i < buffer.Count(); ++i) {
      result.insert(result.end(), buffer[i].data(),
                    buffer[i].data() + buffer[i].size());
    }
  }
  return result;
}

absl::StatusOr<std::unique_ptr<DmeshEndpoint>> ConnectEndpoint(
    DmeshRuntime* runtime, ThreadExecutor* callbacks, const std::string& service) {
  using ConnectedTransport = DmeshReactor::ConnectedTransport;
  using ConnectResult = absl::StatusOr<ConnectedTransport>;

  auto promise = std::make_shared<std::promise<ConnectResult>>();
  std::future<ConnectResult> future = promise->get_future();
  runtime->Connect(service, [promise](ConnectResult result) {
    promise->set_value(std::move(result));
  });
  auto awaited = Await(future, "runtime connect");
  if (!awaited.ok()) return awaited.status();

  ConnectResult connected_result = std::move(*awaited);
  if (!connected_result.ok()) return connected_result.status();
  ConnectedTransport connected = std::move(*connected_result);
  auto allocator = std::make_shared<SmokeMemoryAllocator>();
  return std::make_unique<DmeshEndpoint>(
      std::move(connected.transport), connected.work_executor, callbacks,
      MemoryAllocator(std::move(allocator)));
}

absl::Status Run(const std::string& service, size_t rounds,
                 size_t connection_count, size_t reactor_count,
                 const std::string& protocol) {
  if (protocol != "raw" && protocol != "bench") {
    return absl::InvalidArgumentError("protocol must be raw or bench");
  }
  ThreadExecutor callbacks;
  DmeshRuntime::Options options;
  options.reactor_count = reactor_count;
  auto runtime_result = DmeshRuntime::Create(MakeNativeDmeshApiOps(), &callbacks,
                                            options);
  if (!runtime_result.ok()) return runtime_result.status();
  std::unique_ptr<DmeshRuntime> runtime = std::move(*runtime_result);

  constexpr size_t kPayloadSizes[] = {1,     64,    1024,  8191,
                                      8192,  8193, 65537};
  size_t messages = 0;
  size_t bytes = 0;
  uint32_t sequence = 1;
  for (size_t connection = 0; connection < connection_count; ++connection) {
    auto endpoint_result = ConnectEndpoint(runtime.get(), &callbacks, service);
    if (!endpoint_result.ok()) return endpoint_result.status();
    std::unique_ptr<DmeshEndpoint> endpoint = std::move(*endpoint_result);

    for (size_t round = 0; round < rounds; ++round) {
      for (size_t size_index = 0;
           size_index < sizeof(kPayloadSizes) / sizeof(kPayloadSizes[0]);
           ++size_index) {
        const size_t payload_length = kPayloadSizes[size_index];
        std::vector<uint8_t> request =
            protocol == "bench"
                ? MakeBenchRequest(payload_length, sequence, connection, round,
                                   size_index)
                : MakePayload(payload_length, connection, round, size_index);
        absl::Status status = WritePayload(endpoint.get(), request);
        if (!status.ok()) return status;
        const size_t response_length =
            protocol == "bench" ? kBenchHeaderLength + payload_length
                                 : request.size();
        auto response = ReadPayload(endpoint.get(), response_length);
        if (!response.ok()) return response.status();
        if (protocol == "bench") {
          status = ValidateBenchReply(*response, sequence, payload_length);
          if (!status.ok()) return status;
        } else if (*response != request) {
          return absl::DataLossError("raw byte-exact payload comparison failed");
        }
        ++messages;
        bytes += request.size() + response->size();
        ++sequence;
      }
    }
    endpoint.reset();
  }

  std::cout << "PASS: native DPUmesh runtime smoke"
            << " service=" << service << " reactors=" << reactor_count
            << " connections=" << connection_count << " messages=" << messages
            << " wire_bytes=" << bytes << " protocol=" << protocol
            << " post_max=" << runtime->post_max()
            << '\n';
  runtime.reset();
  return absl::OkStatus();
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

}  // namespace
}  // namespace dpumesh::grpc::testing

int main(int argc, char** argv) {
  if (argc < 2 || argc > 6) {
    std::cerr << "usage: " << argv[0]
              << " SERVICE [ROUNDS=1] [CONNECTIONS=1] [REACTORS=1]"
                 " [PROTOCOL=raw]\n";
    return EXIT_FAILURE;
  }

  size_t rounds = 1;
  size_t connections = 1;
  size_t reactors = 1;
  std::string protocol = "raw";
  if (argc >= 3) {
    auto value = dpumesh::grpc::testing::ParsePositive(argv[2], "rounds");
    if (!value.ok()) {
      std::cerr << value.status() << '\n';
      return EXIT_FAILURE;
    }
    rounds = *value;
  }
  if (argc >= 4) {
    auto value =
        dpumesh::grpc::testing::ParsePositive(argv[3], "connections");
    if (!value.ok()) {
      std::cerr << value.status() << '\n';
      return EXIT_FAILURE;
    }
    connections = *value;
  }
  if (argc >= 5) {
    auto value = dpumesh::grpc::testing::ParsePositive(argv[4], "reactors");
    if (!value.ok()) {
      std::cerr << value.status() << '\n';
      return EXIT_FAILURE;
    }
    reactors = *value;
  }
  if (argc >= 6) protocol = argv[5];

  const absl::Status status =
      dpumesh::grpc::testing::Run(argv[1], rounds, connections, reactors,
                                 protocol);
  if (!status.ok()) {
    std::cerr << "FAIL: native DPUmesh runtime smoke: " << status << '\n';
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
