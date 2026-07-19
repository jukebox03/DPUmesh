#include <errno.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
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

std::string Flatten(SliceBuffer& buffer) {
  std::string output;
  output.reserve(buffer.Length());
  for (size_t i = 0; i < buffer.Count(); ++i) {
    output.append(reinterpret_cast<const char*>(buffer[i].data()),
                  buffer[i].size());
  }
  return output;
}

struct Fixture {
  explicit Fixture(int post_max = 65536)
      : state(std::make_shared<FakeDmeshState>()),
        allocator_impl(std::make_shared<TestMemoryAllocator>()) {
    state->SetPostMax(post_max);
    DmeshRuntime::Options options;
    options.reactor.tx_retry_delay = 100us;
    auto created = DmeshRuntime::Create(MakeFakeDmeshApiOps(state), &callbacks,
                                        options);
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
        std::move(connected->transport), connected->work_executor, &callbacks,
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
  std::unique_ptr<DmeshRuntime> runtime;
  std::optional<DmeshReactor::ConnectedTransport> connected;
  std::optional<absl::Status> connect_error;
  dmesh_qp_t* qp = nullptr;
  std::unique_ptr<DmeshEndpoint> endpoint;
};

void TestTxCopiesSplitsAndPostsOnOwnerThread() {
  Fixture fixture(3);
  SliceBuffer data;
  data.Append(Slice::FromCopiedString("abcdefg"));
  std::optional<absl::Status> status;
  CHECK_TRUE(!fixture.endpoint->Write(
      [&status](absl::Status value) { status = std::move(value); }, &data,
      EventEngine::Endpoint::WriteArgs()));

  CHECK_TRUE(fixture.state->WaitForPostCount(fixture.qp, 3, 2s));
  CHECK_TRUE(fixture.callbacks.WaitForSize(1, 2s));
  fixture.callbacks.RunAll();
  CHECK_TRUE(status.has_value());
  CHECK_TRUE(status->ok());
  CHECK_EQ(data.Length(), size_t{0});

  const auto posts = fixture.state->Posts(fixture.qp);
  CHECK_EQ(posts.size(), size_t{3});
  CHECK_EQ(std::string(posts[0].begin(), posts[0].end()), std::string("abc"));
  CHECK_EQ(std::string(posts[1].begin(), posts[1].end()), std::string("def"));
  CHECK_EQ(std::string(posts[2].begin(), posts[2].end()), std::string("g"));
  CHECK_EQ(fixture.state->poll_thread_violation_count(), size_t{0});
}

void TestTxEagainRetriesFromTimer() {
  Fixture fixture;
  fixture.state->FailNextAlloc(fixture.qp, EAGAIN);

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

  CHECK_TRUE(fixture.state->WaitForPostCount(fixture.qp, 1, 2s));
  CHECK_TRUE(fixture.callbacks.WaitForSize(1, 2s));
  fixture.callbacks.RunAll();
  CHECK_EQ(callback_count, 1);
  CHECK_TRUE(status->ok());
  CHECK_TRUE(fixture.state->alloc_calls(fixture.qp) >= size_t{2});
}

void TestRxCopiesBeforeReleasingCredit() {
  Fixture fixture;
  SliceBuffer buffer;
  std::optional<absl::Status> status;
  CHECK_TRUE(!fixture.endpoint->Read(
      [&status](absl::Status value) { status = std::move(value); }, &buffer,
      EventEngine::Endpoint::ReadArgs()));

  fixture.state->InjectReceive(fixture.qp, 41, "incoming bytes");
  CHECK_TRUE(fixture.state->WaitForReleaseCount(1, 2s));
  CHECK_TRUE(fixture.callbacks.WaitForSize(1, 2s));
  CHECK_EQ(Flatten(buffer), std::string("incoming bytes"));
  fixture.callbacks.RunAll();
  CHECK_TRUE(status->ok());
  CHECK_EQ(fixture.state->release_count(), size_t{1});
}

void TestPrebindDataAndFinAreReplayedInOrder() {
  ManualExecutor callbacks;
  auto state = std::make_shared<FakeDmeshState>();
  auto allocator_impl = std::make_shared<TestMemoryAllocator>();
  DmeshRuntime::Options options;
  options.reactor.tx_retry_delay = 100us;
  auto created = DmeshRuntime::Create(MakeFakeDmeshApiOps(state), &callbacks,
                                      options);
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

  state->InjectReceive(qp, 7, "early data");
  state->InjectFin(qp);
  CHECK_TRUE(state->WaitForReleaseCount(1, 2s));

  callbacks.RunAll();
  CHECK_TRUE(!connect_error.has_value());
  CHECK_TRUE(connected.has_value());
  auto endpoint = std::make_unique<DmeshEndpoint>(
      std::move(connected->transport), connected->work_executor, &callbacks,
      MemoryAllocator(allocator_impl));

  SliceBuffer data_buffer;
  std::optional<absl::Status> data_status;
  const bool synchronous = endpoint->Read(
      [&data_status](absl::Status value) { data_status = std::move(value); },
      &data_buffer, EventEngine::Endpoint::ReadArgs());
  if (!synchronous) {
    CHECK_TRUE(callbacks.WaitForSize(1, 2s));
    callbacks.RunAll();
    CHECK_TRUE(data_status->ok());
  }
  CHECK_EQ(Flatten(data_buffer), std::string("early data"));

  SliceBuffer eof_buffer;
  std::optional<absl::Status> eof_status;
  CHECK_TRUE(!endpoint->Read(
      [&eof_status](absl::Status value) { eof_status = std::move(value); },
      &eof_buffer, EventEngine::Endpoint::ReadArgs()));
  CHECK_TRUE(callbacks.WaitForSize(1, 2s));
  callbacks.RunAll();
  CHECK_EQ(eof_status->code(), absl::StatusCode::kUnavailable);

  endpoint.reset();
  CHECK_TRUE(state->WaitForDestroyCount(1, 2s));
  runtime.reset();
}

void TestStreamChangeFailsClosedAfterCqBatch() {
  Fixture fixture;
  fixture.state->InjectReceiveBatch(
      fixture.qp, {{9, "valid"}, {10, "wrong stream"}, {10, "tail"}});

  CHECK_TRUE(fixture.state->WaitForReleaseCount(3, 2s));
  CHECK_TRUE(fixture.state->WaitForDestroyCount(1, 2s));
  CHECK_EQ(fixture.state->mid_batch_destroy_count(), size_t{0});

  SliceBuffer data_buffer;
  bool data_callback_called = false;
  CHECK_TRUE(fixture.endpoint->Read(
      [&data_callback_called](absl::Status) {
        data_callback_called = true;
      },
      &data_buffer, EventEngine::Endpoint::ReadArgs()));
  CHECK_TRUE(!data_callback_called);
  CHECK_EQ(Flatten(data_buffer), std::string("valid"));

  SliceBuffer error_buffer;
  std::optional<absl::Status> error_status;
  CHECK_TRUE(!fixture.endpoint->Read(
      [&error_status](absl::Status value) {
        error_status = std::move(value);
      },
      &error_buffer, EventEngine::Endpoint::ReadArgs()));
  CHECK_TRUE(fixture.callbacks.WaitForSize(1, 2s));
  fixture.callbacks.RunAll();
  CHECK_EQ(error_status->code(), absl::StatusCode::kUnavailable);
}

void TestRemoteFinFailsPendingReadThenCloseIsDeferred() {
  Fixture fixture;
  SliceBuffer buffer;
  std::optional<absl::Status> status;
  CHECK_TRUE(!fixture.endpoint->Read(
      [&status](absl::Status value) { status = std::move(value); }, &buffer,
      EventEngine::Endpoint::ReadArgs()));

  fixture.state->InjectFin(fixture.qp);
  CHECK_TRUE(fixture.callbacks.WaitForSize(1, 2s));
  fixture.callbacks.RunAll();
  CHECK_EQ(status->code(), absl::StatusCode::kUnavailable);
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
  std::optional<absl::Status> status;
  CHECK_TRUE(!fixture.endpoint->Write(
      [&status](absl::Status value) { status = std::move(value); }, &data,
      EventEngine::Endpoint::WriteArgs()));

  CHECK_TRUE(fixture.callbacks.WaitForSize(1, 2s));
  fixture.callbacks.RunAll();
  CHECK_EQ(status->code(), absl::StatusCode::kUnavailable);
  CHECK_TRUE(fixture.state->WaitForDestroyCount(1, 2s));
  CHECK_EQ(fixture.state->mid_batch_destroy_count(), size_t{0});
}

void TestCloseCancelsPermanentlyBlockedWrite() {
  Fixture fixture;
  fixture.state->SetAllocError(fixture.qp, EAGAIN);

  SliceBuffer data;
  data.Append(Slice::FromCopiedString("blocked"));
  int callback_count = 0;
  std::optional<absl::Status> status;
  CHECK_TRUE(!fixture.endpoint->Write(
      [&callback_count, &status](absl::Status value) {
        ++callback_count;
        status = std::move(value);
      },
      &data, EventEngine::Endpoint::WriteArgs()));
  CHECK_TRUE(fixture.state->WaitForAllocCallCount(fixture.qp, 1, 2s));

  fixture.endpoint.reset();
  CHECK_TRUE(fixture.callbacks.WaitForSize(1, 2s));
  fixture.callbacks.RunAll();
  CHECK_EQ(callback_count, 1);
  CHECK_EQ(status->code(), absl::StatusCode::kCancelled);
  CHECK_TRUE(fixture.state->WaitForDestroyCount(1, 2s));
  const size_t calls_after_close = fixture.state->alloc_calls(fixture.qp);
  std::this_thread::sleep_for(5ms);
  CHECK_EQ(fixture.state->alloc_calls(fixture.qp), calls_after_close);
}

void TestCqPollFailureFailsAndClosesConnection() {
  Fixture fixture;
  SliceBuffer buffer;
  std::optional<absl::Status> status;
  CHECK_TRUE(!fixture.endpoint->Read(
      [&status](absl::Status value) { status = std::move(value); }, &buffer,
      EventEngine::Endpoint::ReadArgs()));

  fixture.state->FailNextPoll(EIO);
  CHECK_TRUE(fixture.callbacks.WaitForSize(1, 2s));
  fixture.callbacks.RunAll();
  CHECK_EQ(status->code(), absl::StatusCode::kUnavailable);
  CHECK_TRUE(fixture.state->WaitForDestroyCount(1, 2s));
  CHECK_EQ(fixture.state->mid_batch_destroy_count(), size_t{0});
}

void TestUnknownServiceMapsToUnavailable() {
  ManualExecutor callbacks;
  auto state = std::make_shared<FakeDmeshState>();
  state->FailNextCreateQp(ENOENT);
  auto created = DmeshRuntime::Create(MakeFakeDmeshApiOps(state), &callbacks);
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
  auto created = DmeshRuntime::Create(MakeFakeDmeshApiOps(state), &callbacks);
  CHECK_TRUE(created.ok());
  auto runtime = std::move(*created);

  dmesh_qp_t* server_qp =
      state->InjectConnectionRequest("first request", 51);
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
  auto created = DmeshRuntime::Create(MakeFakeDmeshApiOps(state), &callbacks);
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
      state->InjectConnectionRequest("gRPC client preface", 77);
  CHECK_TRUE(server_qp != nullptr);
  CHECK_TRUE(callbacks.WaitForSize(1, 2s));
  callbacks.RunAll();
  CHECK_TRUE(accepted.has_value());
  CHECK_TRUE(state->WaitForReleaseCount(1, 2s));

  auto allocator = std::make_shared<TestMemoryAllocator>();
  auto endpoint = std::make_unique<DmeshEndpoint>(
      std::move(accepted->transport), accepted->work_executor, &callbacks,
      MemoryAllocator(allocator));

  SliceBuffer received;
  std::optional<absl::Status> read_status;
  const bool read_sync = endpoint->Read(
      [&read_status](absl::Status status) {
        read_status = std::move(status);
      },
      &received, EventEngine::Endpoint::ReadArgs());
  if (!read_sync) {
    CHECK_TRUE(callbacks.WaitForSize(1, 2s));
    callbacks.RunAll();
    CHECK_TRUE(read_status.has_value());
    CHECK_TRUE(read_status->ok());
  }
  CHECK_EQ(Flatten(received), std::string("gRPC client preface"));

  SliceBuffer response;
  response.Append(Slice::FromCopiedString("gRPC server settings"));
  std::optional<absl::Status> write_status;
  CHECK_TRUE(!endpoint->Write(
      [&write_status](absl::Status status) {
        write_status = std::move(status);
      },
      &response, EventEngine::Endpoint::WriteArgs()));
  CHECK_TRUE(state->WaitForPostCount(server_qp, 1, 2s));
  CHECK_TRUE(callbacks.WaitForSize(1, 2s));
  callbacks.RunAll();
  CHECK_TRUE(write_status.has_value());
  CHECK_TRUE(write_status->ok());
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
  auto created = DmeshRuntime::Create(MakeFakeDmeshApiOps(state), &callbacks);
  CHECK_TRUE(created.ok());
  auto runtime = std::move(*created);
  CapturingPassiveListener listener;
  std::optional<absl::Status> accept_error;

  auto attachment = AttachDmeshGrpcServer(
      runtime.get(), &listener,
      [] {
        return MemoryAllocator(std::make_shared<TestMemoryAllocator>());
      },
      [&accept_error](const absl::Status& status) { accept_error = status; });
  CHECK_TRUE(attachment.ok());

  dmesh_qp_t* server_qp = state->InjectConnectionRequest("preface", 18);
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
  auto created = DmeshRuntime::Create(MakeFakeDmeshApiOps(state), &callbacks);
  CHECK_TRUE(created.ok());
  auto runtime = std::move(*created);
  std::shared_ptr<::grpc::Channel> channel;
  std::optional<absl::Status> connect_error;

  ConnectDmeshGrpcChannel(
      runtime.get(), "greeter",
      MemoryAllocator(std::make_shared<TestMemoryAllocator>()),
      ::grpc::InsecureChannelCredentials(), ::grpc::ChannelArguments(),
      [&channel, &connect_error](
          absl::StatusOr<std::shared_ptr<::grpc::Channel>> result) {
        if (result.ok()) {
          channel = std::move(*result);
        } else {
          connect_error = result.status();
        }
      });

  CHECK_TRUE(callbacks.WaitForSize(1, 2s));
  callbacks.RunAll();
  CHECK_TRUE(!connect_error.has_value());
  CHECK_TRUE(channel != nullptr);
  CHECK_EQ(state->ClientQps().size(), size_t{1});

  channel.reset();
  for (int i = 0; i < 20; ++i) {
    callbacks.RunAll();
    if (state->WaitForDestroyCount(1, 10ms)) break;
  }
  CHECK_TRUE(state->WaitForDestroyCount(1, 2s));
  runtime.reset();
}

void TestRuntimeDestroysCqBeforeChannel() {
  ManualExecutor callbacks;
  auto state = std::make_shared<FakeDmeshState>();
  auto created = DmeshRuntime::Create(MakeFakeDmeshApiOps(state), &callbacks);
  CHECK_TRUE(created.ok());
  auto runtime = std::move(*created);
  runtime.reset();
  CHECK_EQ(state->cq_destroy_count(), size_t{1});
  CHECK_EQ(state->channel_destroy_count(), size_t{1});
}

void TestRuntimeRoundRobinsAcrossCqReactors() {
  ManualExecutor callbacks;
  auto state = std::make_shared<FakeDmeshState>();
  DmeshRuntime::Options options;
  options.reactor_count = 2;
  auto created = DmeshRuntime::Create(MakeFakeDmeshApiOps(state), &callbacks,
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
  CHECK_TRUE(qps[0]->cq != qps[1]->cq);

  transports.clear();
  CHECK_TRUE(state->WaitForDestroyCount(2, 2s));
  runtime.reset();
  CHECK_EQ(state->cq_destroy_count(), size_t{2});
  CHECK_EQ(state->channel_destroy_count(), size_t{1});
  CHECK_EQ(state->poll_thread_violation_count(), size_t{0});
}

void TestConcurrentConnectUsesMpscCommandQueues() {
  ManualExecutor callbacks;
  auto state = std::make_shared<FakeDmeshState>();
  DmeshRuntime::Options options;
  options.reactor_count = 4;
  auto created = DmeshRuntime::Create(MakeFakeDmeshApiOps(state), &callbacks,
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
  CHECK_EQ(state->cq_destroy_count(), size_t{4});
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
      {"TX copies and splits on owner thread",
       TestTxCopiesSplitsAndPostsOnOwnerThread},
      {"TX EAGAIN retries from timer", TestTxEagainRetriesFromTimer},
      {"RX copies before releasing credit", TestRxCopiesBeforeReleasingCredit},
      {"pre-bind data and FIN replay in order",
       TestPrebindDataAndFinAreReplayedInOrder},
      {"stream change fails after CQ batch",
       TestStreamChangeFailsClosedAfterCqBatch},
      {"remote FIN fails read and defers close",
       TestRemoteFinFailsPendingReadThenCloseIsDeferred},
      {"post failure closes endpoint", TestPostFailureFailsEndpointAndClosesQp},
      {"close cancels permanently blocked write",
       TestCloseCancelsPermanentlyBlockedWrite},
      {"CQ poll failure closes connection",
       TestCqPollFailureFailsAndClosesConnection},
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
      {"runtime destroys CQ before channel", TestRuntimeDestroysCqBeforeChannel},
      {"runtime round-robins CQ reactors",
       TestRuntimeRoundRobinsAcrossCqReactors},
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
