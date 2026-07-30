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
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (stopping_) return;
    tasks_.push_back(std::move(task));
  }
  cv_.notify_one();
}

void ThreadExecutor::ThreadMain() {
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

}  // namespace dpumesh::grpc
