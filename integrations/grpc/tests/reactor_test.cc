#include <errno.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <functional>
#include <thread>
#include <utility>
#include <vector>

#include <grpc/event_engine/internal/memory_allocator_impl.h>
#include <grpc/event_engine/memory_allocator.h>
#include <grpc/event_engine/memory_request.h>
#include <grpc/event_engine/slice.h>
#include <grpc/event_engine/slice_buffer.h>
#include <grpc/slice.h>
#include <grpc/impl/channel_arg_names.h>
#include <grpcpp/security/credentials.h>

#include "absl/status/status.h"
#include "dmesh_endpoint.h"
#include "dmesh_grpc_runtime.h"
#include "dmesh_runtime.h"
#include "fake_dmesh_ops.h"
#include "fake_endpoint_transport.h"

namespace dpumesh::grpc::testing {
namespace {

using EventEngine = grpc_event_engine::experimental::EventEngine;
using MemoryAllocator = grpc_event_engine::experimental::MemoryAllocator;
using MemoryAllocatorImpl =
    grpc_event_engine::experimental::internal::MemoryAllocatorImpl;
using MemoryRequest = grpc_event_engine::experimental::MemoryRequest;
using Slice = grpc_event_engine::experimental::Slice;
using SliceBuffer = grpc_event_engine::experimental::SliceBuffer;
using namespace std::chrono_literals;

class TestMemoryAllocator final : public MemoryAllocatorImpl {
 public:
  size_t Reserve(MemoryRequest request) override { return request.max(); }
  grpc_slice MakeSlice(MemoryRequest request) override {
    return grpc_slice_malloc(request.max());
  }
  void Release(size_t /*bytes*/) override {}
  void Shutdown() override {}
};

struct TestFailure {
  std::string message;
};

#define CHECK_TRUE(condition)                                               \
  do {                                                                      \
    if (!(condition)) {                                                     \
      throw TestFailure{std::string("check failed: ") + #condition +       \
                        " at line " + std::to_string(__LINE__)};            \
    }                                                                       \
  } while (false)

#define CHECK_EQ(left, right)                                               \
  do {                                                                      \
    const auto& check_left = (left);                                        \
    const auto& check_right = (right);                                      \
    if (!(check_left == check_right)) {                                     \
      throw TestFailure{std::string("check failed: ") + #left + " == " +  \
                        #right + " at line " + std::to_string(__LINE__)};   \
    }                                                                       \
  } while (false)

class AsyncStatus {
 public:
  void Set(absl::Status value) {
    state_->status = std::move(value);
    state_->count.fetch_add(1, std::memory_order_release);
  }

  bool WaitForCount(size_t count,
                    std::chrono::milliseconds timeout = 2s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (state_->count.load(std::memory_order_acquire) < count) {
      if (std::chrono::steady_clock::now() >= deadline) return false;
      std::this_thread::sleep_for(100us);
    }
    return true;
  }

  bool HasValue() const {
    return state_->count.load(std::memory_order_acquire) != 0;
  }

  size_t Count() const {
    return state_->count.load(std::memory_order_acquire);
  }

  absl::Status Get() const {
    CHECK_TRUE(state_->count.load(std::memory_order_acquire) != 0);
    return state_->status;
  }

 private:
  struct State {
    absl::Status status;
    std::atomic<size_t> count{0};
  };

  std::shared_ptr<State> state_ = std::make_shared<State>();
};

std::string Flatten(SliceBuffer& buffer) {
  std::string output;
  output.reserve(buffer.Length());
  for (size_t i = 0; i < buffer.Count(); ++i) {
    output.append(reinterpret_cast<const char*>(buffer[i].data()),
                  buffer[i].size());
  }
  return output;
}

std::optional<std::string> StringChannelArgument(
    const ::grpc::ChannelArguments& args, const char* key) {
  const grpc_channel_args c_args = args.c_channel_args();
  for (size_t i = 0; i < c_args.num_args; ++i) {
    const grpc_arg& arg = c_args.args[i];
    if (arg.type == GRPC_ARG_STRING && std::strcmp(arg.key, key) == 0) {
      return std::string(arg.value.string);
    }
  }
  return std::nullopt;
}

struct Fixture {
  explicit Fixture(int post_max = 65536)
      : state(std::make_shared<FakeDmeshState>()),
        allocator_impl(std::make_shared<TestMemoryAllocator>()) {
    state->SetPostMax(post_max);
    auto created =
        DmeshRuntime::Create(MakeFakeDmeshApiOps(state), UnownedExecutor(&callbacks));
    CHECK_TRUE(created.ok());
    runtime = std::move(*created);

    runtime->Connect(
        "greeter",
        [this](absl::StatusOr<DmeshReactor::ConnectedTransport> result) {
          if (result.ok()) {
            connected.emplace(std::move(*result));
          } else {
            connect_error = result.status();
          }
        });
    CHECK_TRUE(callbacks.WaitForSize(1, 2s));
    const auto qps = state->ClientQps();
    CHECK_EQ(qps.size(), size_t{1});
    qp = qps.front();
    callbacks.RunAll();
    CHECK_TRUE(!connect_error.has_value());
    CHECK_TRUE(connected.has_value());

    endpoint = std::make_unique<DmeshEndpoint>(
        std::move(connected->transport), UnownedExecutor(&callbacks),
        MemoryAllocator(allocator_impl));
  }

  ~Fixture() {
    endpoint.reset();
    state->WaitForDestroyCount(1, 2s);
    runtime.reset();
  }

  ManualExecutor callbacks;
  std::shared_ptr<FakeDmeshState> state;
  std::shared_ptr<TestMemoryAllocator> allocator_impl;
  std::shared_ptr<DmeshRuntime> runtime;
  std::optional<DmeshReactor::ConnectedTransport> connected;
  std::optional<absl::Status> connect_error;
  dmesh_qp_t* qp = nullptr;
  std::unique_ptr<DmeshEndpoint> endpoint;
};

// The fixture's ManualExecutor is both the callback and the work executor, so
// a test drives write pumps and completions by pumping it until the observed
// condition holds.
template <typename Predicate>
bool PumpUntil(ManualExecutor* executor, Predicate predicate,
               std::chrono::milliseconds timeout = 2s) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  for (;;) {
    executor->RunAll();
    if (predicate()) return true;
    if (std::chrono::steady_clock::now() >= deadline) return false;
    std::this_thread::sleep_for(100us);
  }
}

// Polls a counter the fake exposes without a dedicated waiter.
template <typename Value>
bool WaitFor(const std::function<Value()>& read, Value expected,
             std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  for (;;) {
    if (read() == expected) return true;
    if (std::chrono::steady_clock::now() >= deadline) return false;
    std::this_thread::sleep_for(100us);
  }
}

void TestTxCopiesAndSplitsAcrossPosts() {
  Fixture fixture(3);
  SliceBuffer data;
  data.Append(Slice::FromCopiedString("abcdefg"));
  AsyncStatus status;
  CHECK_TRUE(fixture.endpoint->Write(
      [status](absl::Status value) mutable { status.Set(std::move(value)); },
      &data,
      EventEngine::Endpoint::WriteArgs()));

  CHECK_TRUE(fixture.state->WaitForPostCount(fixture.qp, 3, 2s));
  CHECK_TRUE(!status.HasValue());
  CHECK_EQ(data.Length(), size_t{0});

  const auto posts = fixture.state->Posts(fixture.qp);
  CHECK_EQ(posts.size(), size_t{3});
  CHECK_EQ(std::string(posts[0].begin(), posts[0].end()), std::string("abc"));
  CHECK_EQ(std::string(posts[1].begin(), posts[1].end()), std::string("def"));
  CHECK_EQ(std::string(posts[2].begin(), posts[2].end()), std::string("g"));
  CHECK_EQ(fixture.state->poll_thread_violation_count(), size_t{0});
}

void TestTxEagainRetriesFromEvent() {
  Fixture fixture;
  fixture.state->FailNextAlloc(fixture.qp, EAGAIN);

  SliceBuffer data;
  data.Append(Slice::FromCopiedString("retry"));
  AsyncStatus status;
  CHECK_TRUE(!fixture.endpoint->Write(
      [status](absl::Status value) mutable { status.Set(std::move(value)); },
      &data, EventEngine::Endpoint::WriteArgs()));

  fixture.callbacks.RunAll();
  CHECK_TRUE(fixture.state->WaitForAllocCallCount(fixture.qp, 1, 2s));
  std::this_thread::sleep_for(5ms);
  fixture.callbacks.RunAll();
  CHECK_EQ(fixture.state->alloc_calls(fixture.qp), size_t{1});
  CHECK_EQ(fixture.state->Posts(fixture.qp).size(), size_t{0});

  fixture.state->InjectTxReady(fixture.qp);
  CHECK_TRUE(status.WaitForCount(1));
  CHECK_TRUE(status.Get().ok());
  CHECK_EQ(status.Count(), size_t{1});
  CHECK_EQ(fixture.state->Posts(fixture.qp).size(), size_t{1});
  CHECK_TRUE(fixture.state->alloc_calls(fixture.qp) >= size_t{2});
}

void TestWriteBoundaryFlushesLibraryBatch() {
  Fixture fixture;

  SliceBuffer data;
  data.Append(Slice::FromCopiedString("tail"));
  AsyncStatus status;
  CHECK_TRUE(fixture.endpoint->Write(
      [status](absl::Status value) mutable { status.Set(std::move(value)); },
      &data,
      EventEngine::Endpoint::WriteArgs()));

  CHECK_TRUE(fixture.state->WaitForPostCount(fixture.qp, 1, 2s));
  CHECK_TRUE(fixture.state->WaitForFlushCount(fixture.qp, 1, 2s));
  CHECK_EQ(fixture.state->flush_calls(fixture.qp), size_t{1});
  CHECK_TRUE(!status.HasValue());  // synchronous completion withholds callback
  CHECK_EQ(data.Length(), size_t{0});
}

void TestAsyncTxErrorFailsAndClosesEndpoint() {
  Fixture fixture;
  SliceBuffer read;
  AsyncStatus status;
  CHECK_TRUE(!fixture.endpoint->Read(
      [status](absl::Status value) mutable { status.Set(std::move(value)); },
      &read,
      EventEngine::Endpoint::ReadArgs()));
  fixture.state->InjectTxError(fixture.qp);
  CHECK_TRUE(PumpUntil(&fixture.callbacks,
                       [&status] { return status.HasValue(); }));
  CHECK_EQ(status.Get().code(), absl::StatusCode::kUnavailable);
  CHECK_TRUE(fixture.state->WaitForDestroyCount(1, 2s));
}

void TestReceiveAboveHighWaterHoldsCreditUntilRead() {
  Fixture fixture;
  const std::string chunk(64 * 1024, 'r');
  const size_t below_mark = kReceiveHighWaterBytes / chunk.size();

  /* Below the high-water mark every receive returns its credit. Each chunk is
   * awaited on its own because the credit decision covers a whole coalesced
   * run: chunks that arrive together share one decision. */
  for (size_t i = 0; i < below_mark; ++i) {
    fixture.state->InjectReceive(fixture.qp, chunk);
    CHECK_TRUE(fixture.state->WaitForReleaseCount(i + 1, 2s));
  }

  /* These take the queue past the mark, so the reactor keeps their credit
   * instead of returning it. */
  const size_t receives = below_mark + 4;
  for (size_t i = below_mark; i < receives; ++i) {
    fixture.state->InjectReceive(fixture.qp, chunk);
  }
  std::this_thread::sleep_for(50ms);
  CHECK_EQ(fixture.state->release_count(), below_mark);

  SliceBuffer buffer;
  bool called = false;
  CHECK_TRUE(fixture.endpoint->Read(
      [&called](absl::Status) { called = true; }, &buffer,
      EventEngine::Endpoint::ReadArgs()));
  CHECK_TRUE(!called);
  CHECK_EQ(buffer.Length(), chunk.size() * receives);
  CHECK_TRUE(fixture.state->WaitForReleaseCount(receives, 2s));
}

// A write the transport accepts in full finishes inside Write(), which returns
// true and withholds the callback.
void TestWriteCompletesSynchronously() {
  Fixture fixture;

  SliceBuffer data;
  data.Append(Slice::FromCopiedString("inline"));
  bool called = false;
  CHECK_TRUE(fixture.endpoint->Write(
      [&called](absl::Status) { called = true; }, &data,
      EventEngine::Endpoint::WriteArgs()));
  CHECK_TRUE(!called);
  CHECK_TRUE(fixture.state->WaitForPostCount(fixture.qp, 1, 2s));
  CHECK_EQ(fixture.state->Posts(fixture.qp).size(), size_t{1});
}

void TestRxCopiesBeforeReleasingCredit() {
  Fixture fixture;
  SliceBuffer buffer;
  AsyncStatus status;
  CHECK_TRUE(!fixture.endpoint->Read(
      [status](absl::Status value) mutable { status.Set(std::move(value)); },
      &buffer,
      EventEngine::Endpoint::ReadArgs()));

  fixture.state->InjectReceive(fixture.qp, "incoming bytes");
  CHECK_TRUE(fixture.state->WaitForReleaseCount(1, 2s));
  CHECK_TRUE(status.WaitForCount(1));
  CHECK_TRUE(status.Get().ok());
  CHECK_EQ(Flatten(buffer), std::string("incoming bytes"));
  CHECK_EQ(fixture.callbacks.Size(), size_t{0});
  CHECK_EQ(fixture.state->release_count(), size_t{1});
}

void TestPrebindDataAndFinAreReplayedInOrder() {
  ManualExecutor callbacks;
  auto state = std::make_shared<FakeDmeshState>();
  auto allocator_impl = std::make_shared<TestMemoryAllocator>();
  auto created = DmeshRuntime::Create(MakeFakeDmeshApiOps(state), UnownedExecutor(&callbacks));
  CHECK_TRUE(created.ok());
  auto runtime = std::move(*created);

  std::optional<DmeshReactor::ConnectedTransport> connected;
  std::optional<absl::Status> connect_error;
  runtime->Connect(
      "greeter",
      [&connected, &connect_error](
          absl::StatusOr<DmeshReactor::ConnectedTransport> result) {
        if (result.ok()) {
          connected.emplace(std::move(*result));
        } else {
          connect_error = result.status();
        }
      });
  CHECK_TRUE(callbacks.WaitForSize(1, 2s));
  const auto qps = state->ClientQps();
  CHECK_EQ(qps.size(), size_t{1});
  dmesh_qp_t* qp = qps.front();

  state->InjectReceive(qp, "early data");
  state->InjectFin(qp);
  CHECK_TRUE(state->WaitForReleaseCount(1, 2s));

  callbacks.RunAll();
  CHECK_TRUE(!connect_error.has_value());
  CHECK_TRUE(connected.has_value());
  auto endpoint = std::make_unique<DmeshEndpoint>(
      std::move(connected->transport), UnownedExecutor(&callbacks),
      MemoryAllocator(allocator_impl));

  SliceBuffer data_buffer;
  AsyncStatus data_status;
  const bool synchronous = endpoint->Read(
      [data_status](absl::Status value) mutable {
        data_status.Set(std::move(value));
      },
      &data_buffer, EventEngine::Endpoint::ReadArgs());
  if (!synchronous) {
    CHECK_TRUE(data_status.WaitForCount(1));
    CHECK_TRUE(data_status.Get().ok());
  }
  CHECK_EQ(Flatten(data_buffer), std::string("early data"));

  SliceBuffer eof_buffer;
  AsyncStatus eof_status;
  CHECK_TRUE(!endpoint->Read(
      [eof_status](absl::Status value) mutable {
        eof_status.Set(std::move(value));
      },
      &eof_buffer, EventEngine::Endpoint::ReadArgs()));
  CHECK_TRUE(callbacks.WaitForSize(1, 2s));
  callbacks.RunAll();
  CHECK_EQ(eof_status.Get().code(), absl::StatusCode::kUnavailable);

  endpoint.reset();
  CHECK_TRUE(state->WaitForDestroyCount(1, 2s));
  runtime.reset();
}

void TestBatchedRxPreservesByteOrder() {
  Fixture fixture;
  fixture.state->InjectReceiveBatch(
      fixture.qp, {"first", "-second", "-tail"});

  CHECK_TRUE(fixture.state->WaitForReleaseCount(3, 2s));
  CHECK_EQ(fixture.state->mid_batch_destroy_count(), size_t{0});

  SliceBuffer data_buffer;
  bool data_callback_called = false;
  CHECK_TRUE(fixture.endpoint->Read(
      [&data_callback_called](absl::Status) { data_callback_called = true; },
      &data_buffer, EventEngine::Endpoint::ReadArgs()));
  CHECK_TRUE(!data_callback_called);
  CHECK_EQ(Flatten(data_buffer), std::string("first-second-tail"));
}

void TestRemoteFinFailsPendingReadThenCloseIsDeferred() {
  Fixture fixture;
  SliceBuffer buffer;
  AsyncStatus status;
  CHECK_TRUE(!fixture.endpoint->Read(
      [status](absl::Status value) mutable { status.Set(std::move(value)); },
      &buffer,
      EventEngine::Endpoint::ReadArgs()));

  fixture.state->InjectFin(fixture.qp);
  CHECK_TRUE(fixture.callbacks.WaitForSize(1, 2s));
  fixture.callbacks.RunAll();
  CHECK_EQ(status.Get().code(), absl::StatusCode::kUnavailable);
  CHECK_EQ(fixture.state->destroy_count(), size_t{0});

  fixture.endpoint.reset();
  CHECK_TRUE(fixture.state->WaitForDestroyCount(1, 2s));
  CHECK_EQ(fixture.state->mid_batch_destroy_count(), size_t{0});
}

void TestPostFailureFailsEndpointAndClosesQp() {
  Fixture fixture;
  fixture.state->FailNextPost(fixture.qp, EBADMSG);

  SliceBuffer data;
  data.Append(Slice::FromCopiedString("will fail"));
  AsyncStatus status;
  CHECK_TRUE(!fixture.endpoint->Write(
      [status](absl::Status value) mutable { status.Set(std::move(value)); },
      &data,
      EventEngine::Endpoint::WriteArgs()));

  CHECK_EQ(status.Get().code(), absl::StatusCode::kUnavailable);
  CHECK_TRUE(fixture.state->WaitForDestroyCount(1, 2s));
  CHECK_EQ(fixture.state->mid_batch_destroy_count(), size_t{0});
}

void TestCloseCancelsPermanentlyBlockedWrite() {
  Fixture fixture;
  fixture.state->SetAllocError(fixture.qp, EAGAIN);

  SliceBuffer data;
  data.Append(Slice::FromCopiedString("blocked"));
  AsyncStatus status;
  CHECK_TRUE(!fixture.endpoint->Write(
      [status](absl::Status value) mutable { status.Set(std::move(value)); },
      &data, EventEngine::Endpoint::WriteArgs()));
  fixture.callbacks.RunAll();
  CHECK_TRUE(fixture.state->WaitForAllocCallCount(fixture.qp, 1, 2s));

  fixture.endpoint.reset();
  fixture.callbacks.RunAll();
  CHECK_EQ(status.Count(), size_t{1});
  CHECK_EQ(status.Get().code(), absl::StatusCode::kCancelled);
  CHECK_TRUE(fixture.state->WaitForDestroyCount(1, 2s));
  const size_t calls_after_close = fixture.state->alloc_calls(fixture.qp);
  std::this_thread::sleep_for(5ms);
  CHECK_EQ(fixture.state->alloc_calls(fixture.qp), calls_after_close);
}

void TestPeerFinFailsParkedWrite() {
  Fixture fixture;
  fixture.state->SetAllocError(fixture.qp, EAGAIN);

  SliceBuffer data;
  data.Append(Slice::FromCopiedString("parked"));
  AsyncStatus write_status;
  CHECK_TRUE(!fixture.endpoint->Write(
      [write_status](absl::Status value) mutable {
        write_status.Set(std::move(value));
      },
      &data, EventEngine::Endpoint::WriteArgs()));
  fixture.callbacks.RunAll();
  CHECK_TRUE(fixture.state->WaitForAllocCallCount(fixture.qp, 1, 2s));

  fixture.state->InjectFin(fixture.qp);
  CHECK_TRUE(PumpUntil(&fixture.callbacks,
                       [&write_status] { return write_status.HasValue(); }));
  CHECK_EQ(write_status.Get().code(), absl::StatusCode::kUnavailable);
  CHECK_TRUE(fixture.state->WaitForDestroyCount(1, 2s));
  CHECK_EQ(fixture.state->mid_batch_destroy_count(), size_t{0});
}

void TestAllocBackpressureAfterPeerFinFailsWrite() {
  Fixture fixture;

  SliceBuffer eof_buffer;
  AsyncStatus eof_status;
  CHECK_TRUE(!fixture.endpoint->Read(
      [eof_status](absl::Status value) mutable {
        eof_status.Set(std::move(value));
      },
      &eof_buffer, EventEngine::Endpoint::ReadArgs()));
  fixture.state->InjectFin(fixture.qp);
  CHECK_TRUE(fixture.callbacks.WaitForSize(1, 2s));
  fixture.callbacks.RunAll();
  CHECK_EQ(eof_status.Get().code(), absl::StatusCode::kUnavailable);

  fixture.state->SetAllocError(fixture.qp, EAGAIN);
  SliceBuffer data;
  data.Append(Slice::FromCopiedString("after fin"));
  AsyncStatus write_status;
  CHECK_TRUE(!fixture.endpoint->Write(
      [write_status](absl::Status value) mutable {
        write_status.Set(std::move(value));
      },
      &data, EventEngine::Endpoint::WriteArgs()));
  CHECK_TRUE(PumpUntil(&fixture.callbacks,
                       [&write_status] { return write_status.HasValue(); }));
  CHECK_EQ(write_status.Get().code(), absl::StatusCode::kUnavailable);
  CHECK_TRUE(fixture.state->WaitForDestroyCount(1, 2s));
}

void TestLargeWriteCompletesAcrossPumpYields() {
  Fixture fixture(3);
  const std::string payload(60, 'x');
  SliceBuffer data;
  data.Append(Slice::FromCopiedString(payload));
  AsyncStatus status;
  CHECK_TRUE(fixture.endpoint->Write(
      [status](absl::Status value) mutable { status.Set(std::move(value)); },
      &data,
      EventEngine::Endpoint::WriteArgs()));

  CHECK_EQ(fixture.state->Posts(fixture.qp).size(), size_t{20});
  CHECK_TRUE(!status.HasValue());

  std::string sent;
  for (const auto& post : fixture.state->Posts(fixture.qp)) {
    sent.append(post.begin(), post.end());
  }
  CHECK_EQ(sent, payload);
}

void TestEqPollFailureFailsAndClosesConnection() {
  Fixture fixture;
  SliceBuffer buffer;
  AsyncStatus status;
  CHECK_TRUE(!fixture.endpoint->Read(
      [status](absl::Status value) mutable { status.Set(std::move(value)); },
      &buffer,
      EventEngine::Endpoint::ReadArgs()));

  fixture.state->FailNextPoll(EIO);
  CHECK_TRUE(fixture.callbacks.WaitForSize(1, 2s));
  fixture.callbacks.RunAll();
  CHECK_EQ(status.Get().code(), absl::StatusCode::kUnavailable);
  CHECK_TRUE(fixture.state->WaitForDestroyCount(1, 2s));
  CHECK_EQ(fixture.state->mid_batch_destroy_count(), size_t{0});
}

void TestUnknownServiceMapsToUnavailable() {
  ManualExecutor callbacks;
  auto state = std::make_shared<FakeDmeshState>();
  state->FailNextCreateQp(ENOENT);
  auto created = DmeshRuntime::Create(MakeFakeDmeshApiOps(state), UnownedExecutor(&callbacks));
  CHECK_TRUE(created.ok());
  auto runtime = std::move(*created);

  std::optional<absl::Status> status;
  runtime->Connect(
      "missing",
      [&status](absl::StatusOr<DmeshReactor::ConnectedTransport> result) {
        if (!result.ok()) status = result.status();
      });
  CHECK_TRUE(callbacks.WaitForSize(1, 2s));
  callbacks.RunAll();
  CHECK_TRUE(status.has_value());
  CHECK_EQ(status->code(), absl::StatusCode::kUnavailable);
  runtime.reset();
}

void TestUnownedConnectionRequestIsReleasedAndRejectedPostBatch() {
  ManualExecutor callbacks;
  auto state = std::make_shared<FakeDmeshState>();
  auto created = DmeshRuntime::Create(MakeFakeDmeshApiOps(state), UnownedExecutor(&callbacks));
  CHECK_TRUE(created.ok());
  auto runtime = std::move(*created);

  dmesh_qp_t* server_qp =
      state->InjectConnectionRequest("first request");
  CHECK_TRUE(server_qp != nullptr);
  CHECK_TRUE(state->WaitForReleaseCount(1, 2s));
  CHECK_TRUE(state->WaitForDestroyCount(1, 2s));
  CHECK_EQ(state->mid_batch_destroy_count(), size_t{0});
  CHECK_EQ(state->poll_thread_violation_count(), size_t{0});
  runtime.reset();
}

void TestInboundConnectionIsAcceptedAndBecomesEndpointTransport() {
  ManualExecutor callbacks;
  auto state = std::make_shared<FakeDmeshState>();
  auto created = DmeshRuntime::Create(MakeFakeDmeshApiOps(state), UnownedExecutor(&callbacks));
  CHECK_TRUE(created.ok());
  auto runtime = std::move(*created);

  std::optional<DmeshReactor::ConnectedTransport> accepted;
  CHECK_TRUE(runtime
                 ->SetAcceptCallback(
                     [&accepted](DmeshReactor::ConnectedTransport transport) {
                       accepted.emplace(std::move(transport));
                     })
                 .ok());

  dmesh_qp_t* server_qp =
      state->InjectConnectionRequest("gRPC client preface");
  CHECK_TRUE(server_qp != nullptr);
  CHECK_TRUE(callbacks.WaitForSize(1, 2s));
  callbacks.RunAll();
  CHECK_TRUE(accepted.has_value());
  CHECK_TRUE(state->WaitForReleaseCount(1, 2s));

  auto allocator = std::make_shared<TestMemoryAllocator>();
  auto endpoint = std::make_unique<DmeshEndpoint>(
      std::move(accepted->transport), UnownedExecutor(&callbacks),
      MemoryAllocator(allocator));

  SliceBuffer received;
  AsyncStatus read_status;
  const bool read_sync = endpoint->Read(
      [read_status](absl::Status status) mutable {
        read_status.Set(std::move(status));
      },
      &received, EventEngine::Endpoint::ReadArgs());
  if (!read_sync) {
    CHECK_TRUE(read_status.WaitForCount(1));
    CHECK_TRUE(read_status.Get().ok());
  }
  CHECK_EQ(Flatten(received), std::string("gRPC client preface"));

  SliceBuffer response;
  response.Append(Slice::FromCopiedString("gRPC server settings"));
  AsyncStatus write_status;
  CHECK_TRUE(endpoint->Write(
      [write_status](absl::Status status) mutable {
        write_status.Set(std::move(status));
      },
      &response, EventEngine::Endpoint::WriteArgs()));
  CHECK_TRUE(state->WaitForPostCount(server_qp, 1, 2s));
  CHECK_TRUE(!write_status.HasValue());
  const auto posts = state->Posts(server_qp);
  CHECK_EQ(posts.size(), size_t{1});
  CHECK_EQ(std::string(posts[0].begin(), posts[0].end()),
           std::string("gRPC server settings"));

  endpoint.reset();
  CHECK_TRUE(state->WaitForDestroyCount(1, 2s));
  runtime.reset();
}

class CapturingPassiveListener final
    : public ::grpc::experimental::PassiveListener {
 public:
  absl::Status AcceptConnectedEndpoint(
      std::unique_ptr<EventEngine::Endpoint> value) override {
    endpoint = std::move(value);
    return absl::OkStatus();
  }

  absl::Status AcceptConnectedFd(int /*fd*/) override {
    return absl::UnimplementedError("fd injection is not used");
  }

  std::unique_ptr<EventEngine::Endpoint> endpoint;
};

void TestGrpcServerBridgeInjectsAcceptedEndpoint() {
  ManualExecutor callbacks;
  auto state = std::make_shared<FakeDmeshState>();
  auto created = DmeshRuntime::Create(MakeFakeDmeshApiOps(state), UnownedExecutor(&callbacks));
  CHECK_TRUE(created.ok());
  auto runtime = std::move(*created);
  CapturingPassiveListener listener;
  std::optional<absl::Status> accept_error;

  auto attachment = AttachDmeshGrpcServer(
      runtime, &listener,
      [] {
        return MemoryAllocator(std::make_shared<TestMemoryAllocator>());
      },
      [&accept_error](const absl::Status& status) { accept_error = status; });
  CHECK_TRUE(attachment.ok());

  dmesh_qp_t* server_qp = state->InjectConnectionRequest("preface");
  CHECK_TRUE(server_qp != nullptr);
  CHECK_TRUE(callbacks.WaitForSize(1, 2s));
  callbacks.RunAll();
  CHECK_TRUE(!accept_error.has_value());
  CHECK_TRUE(listener.endpoint != nullptr);

  listener.endpoint.reset();
  CHECK_TRUE(state->WaitForDestroyCount(1, 2s));
  (*attachment)->Detach();
  runtime.reset();
}

void TestGrpcClientBridgeBuildsChannelFromNativeConnect() {
  ManualExecutor callbacks;
  auto state = std::make_shared<FakeDmeshState>();
  auto created = DmeshRuntime::Create(MakeFakeDmeshApiOps(state), UnownedExecutor(&callbacks));
  CHECK_TRUE(created.ok());
  auto runtime = std::move(*created);
  auto channel_result = CreateDmeshChannel(
      runtime, "greeter",
      ::grpc::InsecureChannelCredentials(), ::grpc::ChannelArguments());
  CHECK_TRUE(channel_result.ok());
  std::shared_ptr<::grpc::Channel> channel = std::move(*channel_result);
  CHECK_TRUE(channel != nullptr);
  CHECK_EQ(state->ClientQps().size(), size_t{0});

  (void)channel->GetState(true);
  CHECK_TRUE(state->WaitForClientQpCount(1, 2s));

  // Endpoint teardown runs on the callback executor, so keep draining it while
  // gRPC unwinds the channel. The bound is generous because the release is
  // paced by gRPC's own EventEngine teardown, not by the adapter.
  channel.reset();
  bool destroyed = false;
  for (int i = 0; i < 3000 && !destroyed; ++i) {
    callbacks.RunAll();
    destroyed = state->WaitForDestroyCount(1, 10ms);
  }
  CHECK_TRUE(destroyed);
  runtime.reset();
}

void TestGrpcAuthorityIsDefaultedButNeverOverwritten() {
  ::grpc::ChannelArguments defaulted;
  internal::SetDefaultAuthorityIfAbsent("greeter", &defaulted);
  CHECK_EQ(StringChannelArgument(defaulted, GRPC_ARG_DEFAULT_AUTHORITY),
           std::optional<std::string>("greeter"));

  ::grpc::ChannelArguments explicit_authority;
  explicit_authority.SetString(GRPC_ARG_DEFAULT_AUTHORITY,
                               "api.example.test");
  internal::SetDefaultAuthorityIfAbsent("greeter", &explicit_authority);
  CHECK_EQ(StringChannelArgument(explicit_authority,
                                 GRPC_ARG_DEFAULT_AUTHORITY),
           std::optional<std::string>("api.example.test"));
}

void TestGrpcClientReconnectCreatesFreshTargetedQp() {
  ManualExecutor callbacks;
  auto state = std::make_shared<FakeDmeshState>();
  auto created = DmeshRuntime::Create(MakeFakeDmeshApiOps(state), UnownedExecutor(&callbacks));
  CHECK_TRUE(created.ok());
  auto runtime = std::move(*created);

  ::grpc::ChannelArguments args;
  args.SetInt(GRPC_ARG_INITIAL_RECONNECT_BACKOFF_MS, 10);
  args.SetInt(GRPC_ARG_MIN_RECONNECT_BACKOFF_MS, 10);
  args.SetInt(GRPC_ARG_MAX_RECONNECT_BACKOFF_MS, 10);
  auto channel_result = CreateDmeshChannel(
      runtime, "greeter", ::grpc::InsecureChannelCredentials(), args);
  CHECK_TRUE(channel_result.ok());
  std::shared_ptr<::grpc::Channel> channel = std::move(*channel_result);
  CHECK_TRUE(channel != nullptr);

  (void)channel->GetState(true);
  CHECK_TRUE(state->WaitForClientQpCount(1, 2s));
  dmesh_qp_t* first = state->ClientQps().front();
  const auto first_ready_deadline = std::chrono::steady_clock::now() + 2s;
  while (std::chrono::steady_clock::now() < first_ready_deadline &&
         !state->WaitForPostCount(first, 1, 10ms)) {
    callbacks.RunAll();  // Deliver pending endpoint work.
    std::this_thread::sleep_for(1ms);
  }
  CHECK_TRUE(state->WaitForPostCount(first, 1, 100ms));

  state->InjectFin(first);
  const auto deadline = std::chrono::steady_clock::now() + 3s;
  while (std::chrono::steady_clock::now() < deadline &&
         !state->WaitForClientQpCount(2, 10ms)) {
    callbacks.RunAll();
    std::this_thread::sleep_for(1ms);
  }
  callbacks.RunAll();
  CHECK_TRUE(state->WaitForClientQpCount(2, 100ms));
  const auto targets = state->ClientTargets();
  CHECK_EQ(targets.size(), size_t{2});
  CHECK_EQ(targets[0], std::string("greeter"));
  CHECK_EQ(targets[1], std::string("greeter"));

  channel.reset();
  for (int i = 0; i < 100; ++i) {
    callbacks.RunAll();
    if (state->WaitForDestroyCount(2, 10ms)) break;
  }
  runtime.reset();
}

void TestRuntimeDestroysEqBeforeChannel() {
  ManualExecutor callbacks;
  auto state = std::make_shared<FakeDmeshState>();
  auto created = DmeshRuntime::Create(MakeFakeDmeshApiOps(state), UnownedExecutor(&callbacks));
  CHECK_TRUE(created.ok());
  auto runtime = std::move(*created);
  runtime.reset();
  CHECK_EQ(state->eq_destroy_count(), size_t{1});
  CHECK_EQ(state->channel_destroy_count(), size_t{1});
}

void TestRuntimeRoundRobinsAcrossEqReactors() {
  ManualExecutor callbacks;
  auto state = std::make_shared<FakeDmeshState>();
  DmeshRuntime::Options options;
  options.reactor_count = 2;
  auto created = DmeshRuntime::Create(MakeFakeDmeshApiOps(state), UnownedExecutor(&callbacks),
                                      options);
  CHECK_TRUE(created.ok());
  auto runtime = std::move(*created);

  std::vector<DmeshReactor::ConnectedTransport> transports;
  std::vector<absl::Status> errors;
  for (int i = 0; i < 2; ++i) {
    runtime->Connect(
        "greeter",
        [&transports, &errors](
            absl::StatusOr<DmeshReactor::ConnectedTransport> result) {
          if (result.ok()) {
            transports.push_back(std::move(*result));
          } else {
            errors.push_back(result.status());
          }
        });
  }

  CHECK_TRUE(callbacks.WaitForSize(2, 2s));
  callbacks.RunAll();
  CHECK_TRUE(errors.empty());
  CHECK_EQ(transports.size(), size_t{2});
  const auto qps = state->ClientQps();
  CHECK_EQ(qps.size(), size_t{2});
  CHECK_TRUE(qps[0]->eq != qps[1]->eq);

  transports.clear();
  CHECK_TRUE(state->WaitForDestroyCount(2, 2s));
  runtime.reset();
  CHECK_EQ(state->eq_destroy_count(), size_t{2});
  CHECK_EQ(state->channel_destroy_count(), size_t{1});
  CHECK_EQ(state->poll_thread_violation_count(), size_t{0});
}

void TestConcurrentConnectUsesMpscCommandQueues() {
  ManualExecutor callbacks;
  auto state = std::make_shared<FakeDmeshState>();
  DmeshRuntime::Options options;
  options.reactor_count = 4;
  auto created = DmeshRuntime::Create(MakeFakeDmeshApiOps(state), UnownedExecutor(&callbacks),
                                      options);
  CHECK_TRUE(created.ok());
  auto runtime = std::move(*created);

  constexpr int kConnections = 16;
  std::vector<DmeshReactor::ConnectedTransport> transports;
  std::vector<absl::Status> errors;
  std::vector<std::thread> callers;
  callers.reserve(kConnections);
  for (int i = 0; i < kConnections; ++i) {
    callers.emplace_back([&runtime, &transports, &errors] {
      runtime->Connect(
          "greeter",
          [&transports, &errors](
              absl::StatusOr<DmeshReactor::ConnectedTransport> result) {
            if (result.ok()) {
              transports.push_back(std::move(*result));
            } else {
              errors.push_back(result.status());
            }
          });
    });
  }
  for (auto& caller : callers) caller.join();

  CHECK_TRUE(callbacks.WaitForSize(kConnections, 2s));
  callbacks.RunAll();
  CHECK_TRUE(errors.empty());
  CHECK_EQ(transports.size(), size_t{kConnections});
  CHECK_EQ(state->ClientQps().size(), size_t{kConnections});

  transports.clear();
  CHECK_TRUE(state->WaitForDestroyCount(kConnections, 2s));
  runtime.reset();
  CHECK_EQ(state->eq_destroy_count(), size_t{4});
  CHECK_EQ(state->channel_destroy_count(), size_t{1});
  CHECK_EQ(state->poll_thread_violation_count(), size_t{0});
}

struct TestCase {
  const char* name;
  void (*run)();
};

}  // namespace
}  // namespace dpumesh::grpc::testing

int main() {
  using namespace dpumesh::grpc::testing;
  const TestCase tests[] = {
      {"TX copies and splits across posts",
       TestTxCopiesAndSplitsAcrossPosts},
      {"TX EAGAIN retries from event",
       TestTxEagainRetriesFromEvent},
      {"RX copies before releasing credit", TestRxCopiesBeforeReleasingCredit},
      {"write completes synchronously", TestWriteCompletesSynchronously},
      {"write boundary flushes library batch",
       TestWriteBoundaryFlushesLibraryBatch},
      {"async TX error fails and closes endpoint",
       TestAsyncTxErrorFailsAndClosesEndpoint},
      {"receive above high water holds credit until read",
       TestReceiveAboveHighWaterHoldsCreditUntilRead},
      {"pre-bind data and FIN replay in order",
       TestPrebindDataAndFinAreReplayedInOrder},
      {"batched RX preserves byte order", TestBatchedRxPreservesByteOrder},
      {"remote FIN fails read and defers close",
       TestRemoteFinFailsPendingReadThenCloseIsDeferred},
      {"post failure closes endpoint", TestPostFailureFailsEndpointAndClosesQp},
      {"close cancels permanently blocked write",
       TestCloseCancelsPermanentlyBlockedWrite},
      {"peer FIN fails a parked write",
       TestPeerFinFailsParkedWrite},
      {"alloc backpressure after peer FIN fails write",
       TestAllocBackpressureAfterPeerFinFailsWrite},
      {"large write completes across pump yields",
       TestLargeWriteCompletesAcrossPumpYields},
      {"EQ poll failure closes connection",
       TestEqPollFailureFailsAndClosesConnection},
      {"unknown service maps to unavailable",
       TestUnknownServiceMapsToUnavailable},
      {"unowned connection is rejected post-batch",
       TestUnownedConnectionRequestIsReleasedAndRejectedPostBatch},
      {"inbound connection becomes endpoint transport",
       TestInboundConnectionIsAcceptedAndBecomesEndpointTransport},
      {"gRPC server bridge injects accepted endpoint",
       TestGrpcServerBridgeInjectsAcceptedEndpoint},
      {"gRPC client bridge builds channel",
       TestGrpcClientBridgeBuildsChannelFromNativeConnect},
      {"gRPC authority preserves explicit override",
       TestGrpcAuthorityIsDefaultedButNeverOverwritten},
      {"gRPC reconnect creates a fresh targeted QP",
       TestGrpcClientReconnectCreatesFreshTargetedQp},
      {"runtime destroys EQ before channel", TestRuntimeDestroysEqBeforeChannel},
      {"runtime round-robins EQ reactors",
       TestRuntimeRoundRobinsAcrossEqReactors},
      {"concurrent connect uses MPSC queues",
       TestConcurrentConnectUsesMpscCommandQueues},
  };

  int failures = 0;
  for (const auto& test : tests) {
    try {
      test.run();
      std::cout << "PASS: " << test.name << '\n';
    } catch (const TestFailure& failure) {
      ++failures;
      std::cerr << "FAIL: " << test.name << ": " << failure.message << '\n';
    }
  }
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
