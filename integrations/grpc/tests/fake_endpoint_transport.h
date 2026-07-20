#ifndef DPUMESH_GRPC_TESTS_FAKE_ENDPOINT_TRANSPORT_H
#define DPUMESH_GRPC_TESTS_FAKE_ENDPOINT_TRANSPORT_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

#include "absl/functional/any_invocable.h"
#include "endpoint_transport.h"
#include "executor.h"

namespace dpumesh::grpc::testing {

class ManualExecutor final : public Executor {
 public:
  void Run(absl::AnyInvocable<void()> task) override;
  bool RunOne();
  void RunAll();
  size_t Size() const;
  bool WaitForSize(size_t size, std::chrono::milliseconds timeout) const;

 private:
  mutable std::mutex mu_;
  std::deque<absl::AnyInvocable<void()>> tasks_;
};

struct FakeTransportState {
  mutable std::mutex mu;
  size_t max_post_size = 4096;
  std::deque<PostResult> results;
  std::vector<std::vector<uint8_t>> posts;
  int flush_count = 0;
  int close_count = 0;
};

class FakeEndpointTransport final : public EndpointTransport {
 public:
  explicit FakeEndpointTransport(std::shared_ptr<FakeTransportState> state)
      : state_(std::move(state)) {}

  void BindDriver(std::weak_ptr<DmeshEndpointDriver> driver) override;
  size_t MaxPostSize() const override;
  PostResult Post(absl::Span<const uint8_t> bytes) override;
  absl::Status Flush() override;
  void Close() override;

 private:
  std::shared_ptr<FakeTransportState> state_;
};

}  // namespace dpumesh::grpc::testing

#endif  // DPUMESH_GRPC_TESTS_FAKE_ENDPOINT_TRANSPORT_H
