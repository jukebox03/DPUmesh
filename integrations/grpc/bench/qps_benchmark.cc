#include <sys/resource.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdlib>
#include <deque>
#include <future>
#include <iomanip>
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
#include <grpc/event_engine/memory_request.h>
#include <grpcpp/create_channel.h>
#include <grpcpp/passive_listener.h>
#include <grpcpp/security/credentials.h>
#include <grpcpp/server_builder.h>

#include "absl/functional/any_invocable.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "dmesh_api_ops.h"
#include "dmesh_grpc_runtime.h"
#include "dmesh_runtime.h"
#include "executor.h"
#include "src/proto/grpc/testing/benchmark_service.grpc.pb.h"

namespace dpumesh::grpc::qps {
namespace {

using Clock = std::chrono::steady_clock;
using MemoryAllocator = grpc_event_engine::experimental::MemoryAllocator;
using MemoryAllocatorImpl =
    grpc_event_engine::experimental::internal::MemoryAllocatorImpl;
using MemoryRequest = grpc_event_engine::experimental::MemoryRequest;
using ::grpc::testing::BenchmarkService;
using ::grpc::testing::SimpleRequest;
using ::grpc::testing::SimpleResponse;
using namespace std::chrono_literals;

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

class BenchmarkMemoryAllocator final : public MemoryAllocatorImpl {
 public:
  size_t Reserve(MemoryRequest request) override { return request.max(); }
  grpc_slice MakeSlice(MemoryRequest request) override {
    return grpc_slice_malloc(request.max());
  }
  void Release(size_t /*bytes*/) override {}
  void Shutdown() override {}
};

MemoryAllocator MakeAllocator() {
  return MemoryAllocator(std::make_shared<BenchmarkMemoryAllocator>());
}

class BenchmarkServiceImpl final : public BenchmarkService::Service {
 public:
  ::grpc::Status UnaryCall(::grpc::ServerContext* /*context*/,
                           const SimpleRequest* request,
                           SimpleResponse* response) override {
    if (request->response_size() < 0) {
      return ::grpc::Status(::grpc::StatusCode::INVALID_ARGUMENT,
                            "negative response size");
    }
    if (request->response_size() > 0) {
      response->mutable_payload()->set_type(request->response_type());
      response->mutable_payload()->set_body(
          std::string(static_cast<size_t>(request->response_size()), '\0'));
    }
    calls_.fetch_add(1, std::memory_order_relaxed);
    return ::grpc::Status::OK;
  }

  uint64_t calls() const { return calls_.load(std::memory_order_relaxed); }

 private:
  std::atomic<uint64_t> calls_{0};
};

struct ThreadResult {
  uint64_t attempted = 0;
  uint64_t succeeded = 0;
  uint64_t failed = 0;
  std::vector<uint64_t> latency_ns;
  std::string first_error;
};

struct PhaseResult {
  uint64_t attempted = 0;
  uint64_t succeeded = 0;
  uint64_t failed = 0;
  std::vector<uint64_t> latency_ns;
  std::string first_error;
  double wall_seconds = 0;
  double cpu_seconds = 0;
};

double ProcessCpuSeconds() {
  rusage usage{};
  if (getrusage(RUSAGE_SELF, &usage) != 0) return 0;
  return static_cast<double>(usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) +
         static_cast<double>(usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) /
             1e6;
}

PhaseResult RunPhase(const std::shared_ptr<::grpc::Channel>& channel,
                     size_t concurrency, size_t request_size,
                     size_t response_size, std::chrono::seconds duration,
                     bool record_latency) {
  std::mutex start_mu;
  std::condition_variable start_cv;
  size_t ready = 0;
  bool start = false;
  Clock::time_point deadline;
  std::vector<ThreadResult> results(concurrency);
  std::vector<std::thread> threads;
  threads.reserve(concurrency);

  for (size_t index = 0; index < concurrency; ++index) {
    threads.emplace_back([&, index] {
      auto stub = BenchmarkService::NewStub(channel);
      SimpleRequest request;
      request.set_response_type(::grpc::testing::COMPRESSABLE);
      request.set_response_size(static_cast<int>(response_size));
      request.mutable_payload()->set_type(::grpc::testing::COMPRESSABLE);
      request.mutable_payload()->set_body(std::string(request_size, '\0'));
      ThreadResult& result = results[index];
      if (record_latency) result.latency_ns.reserve(65536);

      {
        std::unique_lock<std::mutex> lock(start_mu);
        ++ready;
        // Workers and the coordinator share this CV. notify_one can wake a
        // worker whose `start` predicate is still false and strand the
        // coordinator after the final ready increment.
        start_cv.notify_all();
        start_cv.wait(lock, [&] { return start; });
      }

      while (Clock::now() < deadline) {
        ::grpc::ClientContext context;
        // Bound every synchronous call so a server that disappears during a
        // benchmark cannot strand the process after the measurement deadline.
        context.set_deadline(std::chrono::system_clock::now() + 5s);
        SimpleResponse response;
        const auto begin = Clock::now();
        const ::grpc::Status status = stub->UnaryCall(&context, request, &response);
        const auto end = Clock::now();
        ++result.attempted;
        if (!status.ok() ||
            response.payload().body().size() != response_size) {
          ++result.failed;
          if (result.first_error.empty()) {
            result.first_error = status.ok()
                                     ? "response payload size mismatch"
                                     : status.error_message();
          }
          continue;
        }
        ++result.succeeded;
        if (record_latency) {
          result.latency_ns.push_back(static_cast<uint64_t>(
              std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin)
                  .count()));
        }
      }
    });
  }

  {
    std::unique_lock<std::mutex> lock(start_mu);
    start_cv.wait(lock, [&] { return ready == concurrency; });
    deadline = Clock::now() + duration;
    start = true;
  }
  const double cpu_begin = ProcessCpuSeconds();
  const auto wall_begin = Clock::now();
  start_cv.notify_all();
  for (auto& thread : threads) thread.join();
  const auto wall_end = Clock::now();
  const double cpu_end = ProcessCpuSeconds();

  PhaseResult aggregate;
  aggregate.wall_seconds =
      std::chrono::duration<double>(wall_end - wall_begin).count();
  aggregate.cpu_seconds = cpu_end - cpu_begin;
  for (auto& result : results) {
    aggregate.attempted += result.attempted;
    aggregate.succeeded += result.succeeded;
    aggregate.failed += result.failed;
    if (aggregate.first_error.empty() && !result.first_error.empty()) {
      aggregate.first_error = std::move(result.first_error);
    }
    aggregate.latency_ns.insert(aggregate.latency_ns.end(),
                                result.latency_ns.begin(),
                                result.latency_ns.end());
  }
  return aggregate;
}

double QuantileUs(const std::vector<uint64_t>& sorted, double quantile) {
  if (sorted.empty()) return 0;
  const size_t index = std::min(
      sorted.size() - 1,
      static_cast<size_t>(quantile * static_cast<double>(sorted.size() - 1)));
  return static_cast<double>(sorted[index]) / 1000.0;
}

absl::StatusOr<size_t> ParseSize(const char* value, const char* name,
                                 bool allow_zero = false) {
  char* end = nullptr;
  const unsigned long long parsed = std::strtoull(value, &end, 10);
  if (value[0] == '\0' || end == nullptr || *end != '\0' ||
      (!allow_zero && parsed == 0) ||
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

absl::Status RunServer(const std::string& transport,
                       const std::string& endpoint, size_t duration_seconds,
                       size_t reactors) {
  ThreadExecutor callbacks;
  std::unique_ptr<DmeshRuntime> runtime;
  if (transport == "dmesh") {
    auto runtime_result = CreateRuntime(&callbacks, reactors);
    if (!runtime_result.ok()) return runtime_result.status();
    runtime = std::move(*runtime_result);
  } else if (transport != "tcp") {
    return absl::InvalidArgumentError("transport must be tcp or dmesh");
  }

  BenchmarkServiceImpl service;
  ::grpc::ServerBuilder builder;
  int selected_tcp_port = 0;
  std::unique_ptr<::grpc::experimental::PassiveListener> passive_listener;
  builder.RegisterService(&service);
  if (transport == "tcp") {
    builder.AddListeningPort(endpoint, ::grpc::InsecureServerCredentials(),
                             &selected_tcp_port);
  } else {
    builder.experimental().AddPassiveListener(
        ::grpc::InsecureServerCredentials(), passive_listener);
  }
  auto server = builder.BuildAndStart();
  if (server == nullptr) {
    return absl::InternalError("failed to start benchmark server");
  }
  if (transport == "tcp" && selected_tcp_port == 0) {
    server->Shutdown();
    return absl::UnavailableError("failed to bind TCP benchmark endpoint");
  }

  std::optional<absl::Status> accept_error;
  std::mutex accept_error_mu;
  std::unique_ptr<DmeshGrpcServerAttachment> attachment;
  if (transport == "dmesh") {
    auto attached = AttachDmeshGrpcServer(
        runtime.get(), passive_listener.get(), [] { return MakeAllocator(); },
        [&](const absl::Status& status) {
          std::lock_guard<std::mutex> lock(accept_error_mu);
          accept_error = status;
        });
    if (!attached.ok()) {
      server->Shutdown();
      return attached.status();
    }
    attachment = std::move(*attached);
  }

  const double cpu_begin = ProcessCpuSeconds();
  std::cout << "READY: qps server transport=" << transport
            << " endpoint=" << endpoint << " duration_s=" << duration_seconds
            << " reactors=" << reactors << std::endl;
  std::this_thread::sleep_for(std::chrono::seconds(duration_seconds));
  if (attachment != nullptr) attachment->Detach();
  server->Shutdown(std::chrono::system_clock::now() + 5s);
  const double cpu_seconds = ProcessCpuSeconds() - cpu_begin;
  runtime.reset();

  {
    std::lock_guard<std::mutex> lock(accept_error_mu);
    if (accept_error.has_value()) return *accept_error;
  }
  std::cout << std::fixed << std::setprecision(3)
            << "SERVER_RESULT {\"transport\":\"" << transport
            << "\",\"calls\":" << service.calls()
            << ",\"cpu_seconds\":" << cpu_seconds << "}" << std::endl;
  return absl::OkStatus();
}

absl::Status RunClient(const std::string& transport,
                       const std::string& target, size_t warmup_seconds,
                       size_t duration_seconds, size_t concurrency,
                       size_t request_size, size_t response_size,
                       size_t reactors) {
  if (request_size > static_cast<size_t>(std::numeric_limits<int>::max()) ||
      response_size > static_cast<size_t>(std::numeric_limits<int>::max())) {
    return absl::InvalidArgumentError("payload exceeds protobuf int32 limit");
  }

  ThreadExecutor callbacks;
  std::unique_ptr<DmeshRuntime> runtime;
  std::shared_ptr<::grpc::Channel> channel;
  if (transport == "dmesh") {
    auto runtime_result = CreateRuntime(&callbacks, reactors);
    if (!runtime_result.ok()) return runtime_result.status();
    runtime = std::move(*runtime_result);
    auto promise = std::make_shared<std::promise<absl::StatusOr<
        std::shared_ptr<::grpc::Channel>>>>();
    auto future = promise->get_future();
    ConnectDmeshGrpcChannel(
        runtime.get(), target, MakeAllocator(),
        ::grpc::InsecureChannelCredentials(), ::grpc::ChannelArguments(),
        [promise](absl::StatusOr<std::shared_ptr<::grpc::Channel>> result) {
          promise->set_value(std::move(result));
        });
    if (future.wait_for(30s) != std::future_status::ready) {
      return absl::DeadlineExceededError("DPUmesh channel creation timed out");
    }
    auto channel_result = future.get();
    if (!channel_result.ok()) return channel_result.status();
    channel = std::move(*channel_result);
  } else if (transport == "tcp") {
    channel = ::grpc::CreateChannel(target,
                                    ::grpc::InsecureChannelCredentials());
  } else {
    return absl::InvalidArgumentError("transport must be tcp or dmesh");
  }

  const PhaseResult warmup =
      RunPhase(channel, concurrency, request_size, response_size,
               std::chrono::seconds(warmup_seconds), false);
  if (warmup.succeeded == 0 || warmup.failed != 0) {
    return absl::UnavailableError(
        std::string("warmup failed: ") + warmup.first_error);
  }

  PhaseResult measured =
      RunPhase(channel, concurrency, request_size, response_size,
               std::chrono::seconds(duration_seconds), true);
  channel.reset();
  runtime.reset();
  if (measured.succeeded == 0 || measured.failed != 0) {
    return absl::UnavailableError(
        std::string("measurement failed: ") + measured.first_error);
  }

  std::sort(measured.latency_ns.begin(), measured.latency_ns.end());
  const double qps =
      static_cast<double>(measured.succeeded) / measured.wall_seconds;
  const double cpu_cores = measured.cpu_seconds / measured.wall_seconds;
  std::cout << std::fixed << std::setprecision(3)
            << "RESULT {\"transport\":\"" << transport
            << "\",\"client_type\":\"SYNC_CLIENT\",\"rpc_type\":\"UNARY\""
            << ",\"channels\":1,\"concurrency\":" << concurrency
            << ",\"request_bytes\":" << request_size
            << ",\"response_bytes\":" << response_size
            << ",\"warmup_seconds\":" << warmup_seconds
            << ",\"measurement_seconds\":" << measured.wall_seconds
            << ",\"attempted\":" << measured.attempted
            << ",\"succeeded\":" << measured.succeeded
            << ",\"failed\":" << measured.failed << ",\"qps\":" << qps
            << ",\"latency_us\":{\"p50\":"
            << QuantileUs(measured.latency_ns, 0.50) << ",\"p90\":"
            << QuantileUs(measured.latency_ns, 0.90) << ",\"p99\":"
            << QuantileUs(measured.latency_ns, 0.99) << ",\"p999\":"
            << QuantileUs(measured.latency_ns, 0.999)
            << "},\"client_cpu_seconds\":" << measured.cpu_seconds
            << ",\"client_cpu_cores\":" << cpu_cores << "}" << std::endl;
  return absl::OkStatus();
}

void Usage(const char* program) {
  std::cerr
      << "usage:\n  " << program
      << " server <tcp|dmesh> ENDPOINT DURATION_S [REACTORS=1]\n  "
      << program
      << " client <tcp|dmesh> TARGET WARMUP_S DURATION_S CONCURRENCY "
         "REQUEST_BYTES RESPONSE_BYTES [REACTORS=1]\n";
}

}  // namespace
}  // namespace dpumesh::grpc::qps

int main(int argc, char** argv) {
  using dpumesh::grpc::qps::ParseSize;
  absl::Status status;
  if (argc >= 2 && std::string(argv[1]) == "server") {
    if (argc < 5 || argc > 6) {
      dpumesh::grpc::qps::Usage(argv[0]);
      return EXIT_FAILURE;
    }
    auto duration = ParseSize(argv[4], "duration");
    auto reactors = argc == 6 ? ParseSize(argv[5], "reactors")
                              : absl::StatusOr<size_t>(size_t{1});
    if (!duration.ok() || !reactors.ok()) {
      std::cerr << (!duration.ok() ? duration.status() : reactors.status())
                << '\n';
      return EXIT_FAILURE;
    }
    status = dpumesh::grpc::qps::RunServer(argv[2], argv[3], *duration,
                                           *reactors);
  } else if (argc >= 2 && std::string(argv[1]) == "client") {
    if (argc < 9 || argc > 10) {
      dpumesh::grpc::qps::Usage(argv[0]);
      return EXIT_FAILURE;
    }
    auto warmup = ParseSize(argv[4], "warmup", true);
    auto duration = ParseSize(argv[5], "duration");
    auto concurrency = ParseSize(argv[6], "concurrency");
    auto request_size = ParseSize(argv[7], "request size", true);
    auto response_size = ParseSize(argv[8], "response size", true);
    auto reactors = argc == 10 ? ParseSize(argv[9], "reactors")
                               : absl::StatusOr<size_t>(size_t{1});
    if (!warmup.ok() || !duration.ok() || !concurrency.ok() ||
        !request_size.ok() || !response_size.ok() || !reactors.ok()) {
      std::cerr << (!warmup.ok()       ? warmup.status()
                    : !duration.ok()   ? duration.status()
                    : !concurrency.ok() ? concurrency.status()
                    : !request_size.ok() ? request_size.status()
                    : !response_size.ok() ? response_size.status()
                                          : reactors.status())
                << '\n';
      return EXIT_FAILURE;
    }
    status = dpumesh::grpc::qps::RunClient(
        argv[2], argv[3], *warmup, *duration, *concurrency, *request_size,
        *response_size, *reactors);
  } else {
    dpumesh::grpc::qps::Usage(argv[0]);
    return EXIT_FAILURE;
  }

  if (!status.ok()) {
    std::cerr << "FAIL: qps benchmark: " << status << '\n';
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
