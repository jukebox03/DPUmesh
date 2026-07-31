#include "thread_executor.h"

#include <utility>

namespace dpumesh::grpc {

ThreadExecutor::ThreadExecutor()
    : state_(std::make_shared<State>()),
      worker_([state = state_]() mutable { ThreadMain(std::move(state)); }) {}

ThreadExecutor::~ThreadExecutor() {
  {
    std::lock_guard<std::mutex> lock(state_->mu);
    state_->stopping = true;
  }
  state_->cv.notify_one();
  /* Destruction from the worker itself detaches; the worker touches only the
   * state it co-owns. */
  if (worker_.get_id() == std::this_thread::get_id()) {
    worker_.detach();
    return;
  }
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

uint64_t ThreadExecutor::dropped_task_count() const {
  return state_->dropped.load(std::memory_order_relaxed);
}

void ThreadExecutor::Push(Entry entry) {
  {
    std::lock_guard<std::mutex> lock(state_->mu);
    if (state_->stopping) {
      state_->dropped.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    state_->queue.push_back(std::move(entry));
  }
  state_->cv.notify_one();
}

void ThreadExecutor::ThreadMain(std::shared_ptr<State> state) {
  std::deque<Entry> batch;
  for (;;) {
    {
      std::unique_lock<std::mutex> lock(state->mu);
      state->cv.wait(lock,
                     [&state] { return state->stopping || !state->queue.empty(); });
      if (state->queue.empty()) return;
      batch.swap(state->queue);
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
