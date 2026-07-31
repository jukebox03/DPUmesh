#include "thread_executor.h"

#include <utility>

namespace dpumesh::grpc {

ThreadExecutor::ThreadExecutor() : worker_([this] { ThreadMain(); }) {}

ThreadExecutor::~ThreadExecutor() {
  {
    std::lock_guard<std::mutex> lock(mu_);
    stopping_ = true;
  }
  cv_.notify_one();
  worker_.join();
}

void ThreadExecutor::Run(absl::AnyInvocable<void()> task) {
  Entry entry;
  entry.task = std::move(task);
  Push(std::move(entry));
}

void ThreadExecutor::RunCompletion(
    absl::AnyInvocable<void(absl::Status)> callback, absl::Status status) {
  Entry entry;
  entry.completion = std::move(callback);
  entry.status = std::move(status);
  Push(std::move(entry));
}

void ThreadExecutor::Push(Entry entry) {
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (stopping_) return;
    queue_.push_back(std::move(entry));
  }
  cv_.notify_one();
}

void ThreadExecutor::ThreadMain() {
  std::deque<Entry> batch;
  for (;;) {
    {
      std::unique_lock<std::mutex> lock(mu_);
      cv_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
      if (queue_.empty()) return;
      batch.swap(queue_);
    }
    for (auto& entry : batch) {
      if (entry.completion) {
        entry.completion(std::move(entry.status));
      } else if (entry.task) {
        entry.task();
      }
    }
    batch.clear();
  }
}

}  // namespace dpumesh::grpc
