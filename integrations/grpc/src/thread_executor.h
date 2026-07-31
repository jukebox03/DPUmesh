#ifndef DPUMESH_GRPC_THREAD_EXECUTOR_H
#define DPUMESH_GRPC_THREAD_EXECUTOR_H

#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

#include "absl/functional/any_invocable.h"
#include "absl/status/status.h"
#include "executor.h"

namespace dpumesh::grpc {

// One dedicated callback thread with a condition-variable queue, paired one
// per reactor by default. The worker claims the whole queue per wake.
class ThreadExecutor final : public Executor {
 public:
  ThreadExecutor();
  ~ThreadExecutor() override;

  void Run(absl::AnyInvocable<void()> task) override;
  void RunCompletion(absl::AnyInvocable<void(absl::Status)> callback,
                     absl::Status status) override;

 private:
  // A queued task, or a completion callback stored next to its status. Both
  // forms share one queue and run in enqueue order.
  struct Entry {
    absl::AnyInvocable<void()> task;
    absl::AnyInvocable<void(absl::Status)> completion;
    absl::Status status;
  };

  // Co-owned by the worker, which keeps it alive across a detached shutdown.
  struct State {
    std::mutex mu;
    std::condition_variable cv;
    std::deque<Entry> queue;
    bool stopping = false;
  };

  static void ThreadMain(std::shared_ptr<State> state);

  void Push(Entry entry);

  std::shared_ptr<State> state_;
  std::thread worker_;
};

}  // namespace dpumesh::grpc

#endif  // DPUMESH_GRPC_THREAD_EXECUTOR_H
