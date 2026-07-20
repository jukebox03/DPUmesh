#include "fake_endpoint_transport.h"

#include <thread>
#include <utility>

#include "absl/status/status.h"

namespace dpumesh::grpc::testing {

void FakeEndpointTransport::BindDriver(
    std::weak_ptr<DmeshEndpointDriver> /*driver*/) {}

void ManualExecutor::Run(absl::AnyInvocable<void()> task) {
  std::lock_guard<std::mutex> lock(mu_);
  tasks_.push_back(std::move(task));
}

bool ManualExecutor::RunOne() {
  absl::AnyInvocable<void()> task;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (tasks_.empty()) return false;
    task = std::move(tasks_.front());
    tasks_.pop_front();
  }
  task();
  return true;
}

void ManualExecutor::RunAll() {
  while (RunOne()) {
  }
}

size_t ManualExecutor::Size() const {
  std::lock_guard<std::mutex> lock(mu_);
  return tasks_.size();
}

bool ManualExecutor::WaitForSize(size_t size,
                                 std::chrono::milliseconds timeout) const {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  for (;;) {
    if (Size() >= size) return true;
    if (std::chrono::steady_clock::now() >= deadline) return false;
    std::this_thread::sleep_for(std::chrono::microseconds(100));
  }
}

size_t FakeEndpointTransport::MaxPostSize() const {
  std::lock_guard<std::mutex> lock(state_->mu);
  return state_->max_post_size;
}

PostResult FakeEndpointTransport::Post(absl::Span<const uint8_t> bytes) {
  std::lock_guard<std::mutex> lock(state_->mu);
  if (state_->close_count != 0) {
    return PostResult::Closed(
        absl::UnavailableError("fake transport is closed"));
  }

  PostResult result = PostResult::Accepted();
  if (!state_->results.empty()) {
    result = std::move(state_->results.front());
    state_->results.pop_front();
  }
  if (result.code == PostCode::kAccepted) {
    state_->posts.emplace_back(bytes.begin(), bytes.end());
  }
  return result;
}

absl::Status FakeEndpointTransport::Flush() {
  std::lock_guard<std::mutex> lock(state_->mu);
  if (state_->close_count != 0)
    return absl::UnavailableError("fake transport is closed");
  ++state_->flush_count;
  return absl::OkStatus();
}

void FakeEndpointTransport::Close() {
  std::lock_guard<std::mutex> lock(state_->mu);
  ++state_->close_count;
}

}  // namespace dpumesh::grpc::testing
