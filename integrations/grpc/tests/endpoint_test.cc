#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <limits>
#include <utility>

#include <grpc/event_engine/internal/memory_allocator_impl.h>
#include <grpc/event_engine/memory_allocator.h>
#include <grpc/event_engine/memory_request.h>
#include <grpc/event_engine/slice.h>
#include <grpc/event_engine/slice_buffer.h>
#include <grpc/slice.h>

#include "absl/status/status.h"
#include "dmesh_endpoint.h"
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

class TestMemoryAllocator final : public MemoryAllocatorImpl {
 public:
  size_t Reserve(MemoryRequest request) override { return request.max(); }
  grpc_slice MakeSlice(MemoryRequest request) override {
    const size_t forced = next_slice_size_.exchange(kNoForcedSize);
    return grpc_slice_malloc(forced == kNoForcedSize ? request.max() : forced);
  }
  void Release(size_t /*bytes*/) override {}
  void Shutdown() override { ++shutdowns_; }

  int shutdowns() const { return shutdowns_.load(); }
  void SetNextSliceSize(size_t size) { next_slice_size_.store(size); }

 private:
  static constexpr size_t kNoForcedSize =
      std::numeric_limits<size_t>::max();
  std::atomic<int> shutdowns_{0};
  std::atomic<size_t> next_slice_size_{kNoForcedSize};
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

struct Fixture {
  Fixture() : allocator_impl(std::make_shared<TestMemoryAllocator>()) {
    transport_state = std::make_shared<FakeTransportState>();
    endpoint = std::make_unique<DmeshEndpoint>(
        std::make_unique<FakeEndpointTransport>(transport_state),
        UnownedExecutor(&callbacks), MemoryAllocator(allocator_impl));
    driver = endpoint->driver();
  }

  ManualExecutor callbacks;
  std::shared_ptr<TestMemoryAllocator> allocator_impl;
  std::shared_ptr<FakeTransportState> transport_state;
  std::unique_ptr<DmeshEndpoint> endpoint;
  std::shared_ptr<DmeshEndpointDriver> driver;
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

void TestQueuedReadCompletesSynchronously() {
  Fixture fixture;
  const std::string payload = "queued bytes";
  CHECK_TRUE(fixture.driver->OnIncomingData(absl::MakeConstSpan(
                 reinterpret_cast<const uint8_t*>(payload.data()),
                 payload.size()))
                 .status.ok());

  SliceBuffer buffer;
  bool called = false;
  const bool synchronous = fixture.endpoint->Read(
      [&called](absl::Status) { called = true; }, &buffer,
      EventEngine::Endpoint::ReadArgs());

  CHECK_TRUE(synchronous);
  CHECK_TRUE(!called);
  CHECK_EQ(Flatten(buffer), payload);
  CHECK_EQ(fixture.callbacks.Size(), size_t{0});
}

void TestPendingReadCompletesOnDelivery() {
  Fixture fixture;
  SliceBuffer buffer;
  std::optional<absl::Status> status;
  CHECK_TRUE(!fixture.endpoint->Read(
      [&status](absl::Status value) { status = std::move(value); }, &buffer,
      EventEngine::Endpoint::ReadArgs()));

  const std::string payload = "async bytes";
  CHECK_TRUE(fixture.driver->OnIncomingData(absl::MakeConstSpan(
                 reinterpret_cast<const uint8_t*>(payload.data()),
                 payload.size()))
                 .status.ok());
  CHECK_TRUE(status.has_value());
  CHECK_TRUE(status->ok());
  CHECK_EQ(Flatten(buffer), payload);
  CHECK_EQ(fixture.callbacks.Size(), size_t{0});
}

void TestRemoteEofFailsPendingAndFutureReads() {
  Fixture fixture;
  SliceBuffer first_buffer;
  std::optional<absl::Status> first_status;
  CHECK_TRUE(!fixture.endpoint->Read(
      [&first_status](absl::Status value) {
        first_status = std::move(value);
      },
      &first_buffer, EventEngine::Endpoint::ReadArgs()));

  fixture.driver->OnRemoteEof();
  CHECK_TRUE(!first_status.has_value());
  fixture.callbacks.RunAll();
  CHECK_TRUE(first_status.has_value());
  CHECK_EQ(first_status->code(), absl::StatusCode::kUnavailable);

  SliceBuffer second_buffer;
  std::optional<absl::Status> second_status;
  CHECK_TRUE(!fixture.endpoint->Read(
      [&second_status](absl::Status value) {
        second_status = std::move(value);
      },
      &second_buffer, EventEngine::Endpoint::ReadArgs()));
  CHECK_TRUE(!second_status.has_value());
  fixture.callbacks.RunAll();
  CHECK_EQ(second_status->code(), absl::StatusCode::kUnavailable);
}

void TestBufferedDataPrecedesRemoteEof() {
  Fixture fixture;
  const std::string payload = "last bytes";
  CHECK_TRUE(fixture.driver->OnIncomingData(absl::MakeConstSpan(
                 reinterpret_cast<const uint8_t*>(payload.data()),
                 payload.size()))
                 .status.ok());
  fixture.driver->OnRemoteEof();

  SliceBuffer data_buffer;
  bool data_callback_called = false;
  CHECK_TRUE(fixture.endpoint->Read(
      [&data_callback_called](absl::Status) {
        data_callback_called = true;
      },
      &data_buffer, EventEngine::Endpoint::ReadArgs()));
  CHECK_TRUE(!data_callback_called);
  CHECK_EQ(Flatten(data_buffer), payload);

  SliceBuffer eof_buffer;
  std::optional<absl::Status> eof_status;
  CHECK_TRUE(!fixture.endpoint->Read(
      [&eof_status](absl::Status value) {
        eof_status = std::move(value);
      },
      &eof_buffer, EventEngine::Endpoint::ReadArgs()));
  fixture.callbacks.RunAll();
  CHECK_EQ(eof_status->code(), absl::StatusCode::kUnavailable);
}

void TestWriteSplitsSlicesAcrossPosts() {
  Fixture fixture;
  fixture.transport_state->max_post_size = 3;

  SliceBuffer data;
  data.Append(Slice::FromCopiedString("abcde"));
  data.Append(Slice::FromCopiedString("fg"));
  std::optional<absl::Status> status;
  CHECK_TRUE(fixture.endpoint->Write(
      [&status](absl::Status value) { status = std::move(value); }, &data,
      EventEngine::Endpoint::WriteArgs()));
  CHECK_TRUE(!status.has_value());
  CHECK_EQ(data.Length(), size_t{0});
  CHECK_EQ(fixture.callbacks.Size(), size_t{0});

  std::lock_guard<std::mutex> lock(fixture.transport_state->mu);
  CHECK_EQ(fixture.transport_state->posts.size(), size_t{3});
  CHECK_EQ(std::string(fixture.transport_state->posts[0].begin(),
                       fixture.transport_state->posts[0].end()),
           std::string("abc"));
  CHECK_EQ(std::string(fixture.transport_state->posts[1].begin(),
                       fixture.transport_state->posts[1].end()),
           std::string("def"));
  CHECK_EQ(std::string(fixture.transport_state->posts[2].begin(),
                       fixture.transport_state->posts[2].end()),
           std::string("g"));
  CHECK_EQ(fixture.transport_state->flush_count, 1);
}

void TestWriteCoalescesSlicesIntoOneReservation() {
  Fixture fixture;
  const std::string first(64, 'a');
  const std::string second(64, 'b');

  SliceBuffer data;
  data.Append(Slice::FromCopiedString(first));
  data.Append(Slice::FromCopiedString(second));
  CHECK_EQ(data.Count(), size_t{2});

  std::optional<absl::Status> status;
  CHECK_TRUE(fixture.endpoint->Write(
      [&status](absl::Status value) { status = std::move(value); }, &data,
      EventEngine::Endpoint::WriteArgs()));
  CHECK_TRUE(!status.has_value());

  std::lock_guard<std::mutex> lock(fixture.transport_state->mu);
  CHECK_EQ(fixture.transport_state->posts.size(), size_t{1});
  CHECK_EQ(std::string(fixture.transport_state->posts[0].begin(),
                       fixture.transport_state->posts[0].end()),
           first + second);
  CHECK_EQ(fixture.transport_state->flush_count, 1);
}

void TestReceiveHoldsCreditAboveHighWaterUntilRead() {
  Fixture fixture;
  const std::string chunk(64 * 1024, 'r');
  const auto bytes = absl::MakeConstSpan(
      reinterpret_cast<const uint8_t*>(chunk.data()), chunk.size());
  const size_t below_mark = kReceiveHighWaterBytes / chunk.size();
  const size_t receives = below_mark + 4;

  size_t held = 0;
  for (size_t i = 0; i < receives; ++i) {
    const ReceiveOutcome outcome = fixture.driver->OnIncomingData(bytes);
    CHECK_TRUE(outcome.status.ok());
    if (outcome.hold_credit) ++held;
  }
  CHECK_EQ(held, size_t{4});
  CHECK_EQ(fixture.transport_state->resume_count, 0);

  SliceBuffer buffer;
  bool called = false;
  CHECK_TRUE(fixture.endpoint->Read(
      [&called](absl::Status) { called = true; }, &buffer,
      EventEngine::Endpoint::ReadArgs()));
  CHECK_TRUE(!called);
  CHECK_EQ(buffer.Length(), chunk.size() * receives);
  CHECK_EQ(fixture.transport_state->resume_count, 1);

  // A read waiting on the endpoint absorbs the bytes, so nothing queues and no
  // credit is held.
  SliceBuffer next;
  CHECK_TRUE(!fixture.endpoint->Read(
      [](absl::Status) {}, &next, EventEngine::Endpoint::ReadArgs()));
  CHECK_TRUE(!fixture.driver->OnIncomingData(bytes).hold_credit);
  CHECK_EQ(fixture.transport_state->resume_count, 1);
}

void TestRemoteEofFailsParkedWrite() {
  Fixture fixture;
  fixture.transport_state->results.push_back(PostResult::WouldBlock());

  SliceBuffer data;
  data.Append(Slice::FromCopiedString("parked"));
  std::optional<absl::Status> write_status;
  CHECK_TRUE(!fixture.endpoint->Write(
      [&write_status](absl::Status value) { write_status = std::move(value); },
      &data, EventEngine::Endpoint::WriteArgs()));
  CHECK_TRUE(!write_status.has_value());

  // The parked write waits for capacity backed by the departed peer's credits,
  // so EOF completes it with an error.
  fixture.driver->OnRemoteEof();
  fixture.callbacks.RunAll();
  CHECK_TRUE(write_status.has_value());
  CHECK_EQ(write_status->code(), absl::StatusCode::kUnavailable);
  CHECK_EQ(data.Length(), size_t{0});
  CHECK_EQ(fixture.transport_state->close_count, 1);
}

void TestWriteWouldBlockAndResumesExactlyOnce() {
  Fixture fixture;
  fixture.transport_state->results.push_back(PostResult::WouldBlock());
  fixture.transport_state->results.push_back(PostResult::Accepted());

  SliceBuffer data;
  data.Append(Slice::FromCopiedString("retry"));
  int callback_count = 0;
  std::optional<absl::Status> status;
  CHECK_TRUE(!fixture.endpoint->Write(
      [&callback_count, &status](absl::Status value) {
        ++callback_count;
        status = std::move(value);
      },
      &data, EventEngine::Endpoint::WriteArgs()));

  CHECK_EQ(fixture.transport_state->posts.size(), size_t{0});
  CHECK_EQ(fixture.callbacks.Size(), size_t{0});

  fixture.driver->OnWritable();
  fixture.driver->OnWritable();

  CHECK_EQ(callback_count, 1);
  CHECK_TRUE(status->ok());
  CHECK_EQ(fixture.transport_state->posts.size(), size_t{1});
  CHECK_EQ(fixture.transport_state->flush_count, 1);
}

void TestTransportErrorFailsReadAndWrite() {
  Fixture fixture;
  fixture.transport_state->results.push_back(PostResult::WouldBlock());
  SliceBuffer read_buffer;
  SliceBuffer write_buffer;
  write_buffer.Append(Slice::FromCopiedString("pending"));
  std::optional<absl::Status> read_status;
  std::optional<absl::Status> write_status;

  CHECK_TRUE(!fixture.endpoint->Read(
      [&read_status](absl::Status value) {
        read_status = std::move(value);
      },
      &read_buffer, EventEngine::Endpoint::ReadArgs()));
  CHECK_TRUE(!fixture.endpoint->Write(
      [&write_status](absl::Status value) {
        write_status = std::move(value);
      },
      &write_buffer, EventEngine::Endpoint::WriteArgs()));

  fixture.driver->OnTransportError(absl::InternalError("injected failure"));
  fixture.callbacks.RunAll();

  CHECK_EQ(read_status->code(), absl::StatusCode::kInternal);
  CHECK_EQ(write_status->code(), absl::StatusCode::kInternal);
  CHECK_EQ(write_buffer.Length(), size_t{0});
  CHECK_EQ(fixture.transport_state->close_count, 1);
}

void TestPostFailureFailsReadAndWrite() {
  Fixture fixture;
  fixture.transport_state->results.push_back(
      PostResult::Error(absl::InternalError("post failed")));

  SliceBuffer read_buffer;
  SliceBuffer write_buffer;
  write_buffer.Append(Slice::FromCopiedString("pending"));
  std::optional<absl::Status> read_status;
  std::optional<absl::Status> write_status;
  CHECK_TRUE(!fixture.endpoint->Read(
      [&read_status](absl::Status value) {
        read_status = std::move(value);
      },
      &read_buffer, EventEngine::Endpoint::ReadArgs()));
  CHECK_TRUE(!fixture.endpoint->Write(
      [&write_status](absl::Status value) {
        write_status = std::move(value);
      },
      &write_buffer, EventEngine::Endpoint::WriteArgs()));

  CHECK_EQ(read_status->code(), absl::StatusCode::kInternal);
  CHECK_EQ(write_status->code(), absl::StatusCode::kInternal);
  CHECK_EQ(write_buffer.Length(), size_t{0});
  CHECK_EQ(fixture.transport_state->close_count, 1);
}

void TestZeroMaxPostSizeFailsEndpoint() {
  Fixture fixture;
  fixture.transport_state->max_post_size = 0;

  SliceBuffer read_buffer;
  SliceBuffer write_buffer;
  write_buffer.Append(Slice::FromCopiedString("pending"));
  std::optional<absl::Status> read_status;
  std::optional<absl::Status> write_status;
  CHECK_TRUE(!fixture.endpoint->Read(
      [&read_status](absl::Status value) {
        read_status = std::move(value);
      },
      &read_buffer, EventEngine::Endpoint::ReadArgs()));
  CHECK_TRUE(!fixture.endpoint->Write(
      [&write_status](absl::Status value) {
        write_status = std::move(value);
      },
      &write_buffer, EventEngine::Endpoint::WriteArgs()));

  CHECK_EQ(read_status->code(), absl::StatusCode::kInternal);
  CHECK_EQ(write_status->code(), absl::StatusCode::kInternal);
  CHECK_EQ(fixture.transport_state->close_count, 1);
}

void TestAllocatorMismatchFailsBothOperations() {
  Fixture fixture;
  fixture.transport_state->results.push_back(PostResult::WouldBlock());
  SliceBuffer read_buffer;
  SliceBuffer write_buffer;
  write_buffer.Append(Slice::FromCopiedString("pending"));
  std::optional<absl::Status> read_status;
  std::optional<absl::Status> write_status;

  CHECK_TRUE(!fixture.endpoint->Read(
      [&read_status](absl::Status value) {
        read_status = std::move(value);
      },
      &read_buffer, EventEngine::Endpoint::ReadArgs()));
  CHECK_TRUE(!fixture.endpoint->Write(
      [&write_status](absl::Status value) {
        write_status = std::move(value);
      },
      &write_buffer, EventEngine::Endpoint::WriteArgs()));

  fixture.allocator_impl->SetNextSliceSize(1);
  const std::string payload = "too large";
  const ReceiveOutcome receive_outcome = fixture.driver->OnIncomingData(
      absl::MakeConstSpan(
          reinterpret_cast<const uint8_t*>(payload.data()), payload.size()));

  CHECK_EQ(receive_outcome.status.code(), absl::StatusCode::kResourceExhausted);
  CHECK_EQ(read_status->code(), absl::StatusCode::kResourceExhausted);
  CHECK_EQ(write_status->code(), absl::StatusCode::kResourceExhausted);
  CHECK_EQ(write_buffer.Length(), size_t{0});
  CHECK_EQ(fixture.transport_state->close_count, 1);
}

void TestDestructionCancelsPendingOperations() {
  Fixture fixture;
  fixture.transport_state->results.push_back(PostResult::WouldBlock());
  SliceBuffer read_buffer;
  SliceBuffer write_buffer;
  write_buffer.Append(Slice::FromCopiedString("pending"));
  std::optional<absl::Status> read_status;
  std::optional<absl::Status> write_status;

  CHECK_TRUE(!fixture.endpoint->Read(
      [&read_status](absl::Status value) {
        read_status = std::move(value);
      },
      &read_buffer, EventEngine::Endpoint::ReadArgs()));
  CHECK_TRUE(!fixture.endpoint->Write(
      [&write_status](absl::Status value) {
        write_status = std::move(value);
      },
      &write_buffer, EventEngine::Endpoint::WriteArgs()));

  fixture.endpoint.reset();
  CHECK_EQ(fixture.transport_state->close_count, 1);
  CHECK_TRUE(!read_status.has_value());
  CHECK_TRUE(!write_status.has_value());

  fixture.callbacks.RunAll();
  CHECK_EQ(read_status->code(), absl::StatusCode::kCancelled);
  CHECK_EQ(write_status->code(), absl::StatusCode::kCancelled);
  CHECK_EQ(write_buffer.Length(), size_t{0});
}

void TestEmptyWriteIsSynchronous() {
  Fixture fixture;
  SliceBuffer data;
  bool called = false;
  CHECK_TRUE(fixture.endpoint->Write(
      [&called](absl::Status) { called = true; }, &data,
      EventEngine::Endpoint::WriteArgs()));
  CHECK_TRUE(!called);
  CHECK_EQ(fixture.callbacks.Size(), size_t{0});
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
      {"queued read completes synchronously",
       TestQueuedReadCompletesSynchronously},
      {"pending read completes on delivery",
       TestPendingReadCompletesOnDelivery},
      {"remote EOF fails reads", TestRemoteEofFailsPendingAndFutureReads},
      {"buffered data precedes remote EOF", TestBufferedDataPrecedesRemoteEof},
      {"write splits slices across posts", TestWriteSplitsSlicesAcrossPosts},
      {"write coalesces slices into one reservation",
       TestWriteCoalescesSlicesIntoOneReservation},
      {"remote EOF fails a parked write", TestRemoteEofFailsParkedWrite},
      {"receive holds credit above high water",
       TestReceiveHoldsCreditAboveHighWaterUntilRead},
      {"write resumes after backpressure",
       TestWriteWouldBlockAndResumesExactlyOnce},
      {"transport error fails pending operations",
       TestTransportErrorFailsReadAndWrite},
      {"post failure fails pending operations",
       TestPostFailureFailsReadAndWrite},
      {"zero max post size fails endpoint", TestZeroMaxPostSizeFailsEndpoint},
      {"allocator mismatch fails pending operations",
       TestAllocatorMismatchFailsBothOperations},
      {"destruction cancels pending operations",
       TestDestructionCancelsPendingOperations},
      {"empty write is synchronous", TestEmptyWriteIsSynchronous},
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
