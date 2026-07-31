#ifndef DPUMESH_GRPC_THREAD_EXECUTOR_H
#define DPUMESH_GRPC_THREAD_EXECUTOR_H

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

#include "absl/functional/any_invocable.h"
#include "absl/status/status.h"
#include "executor.h"

namespace dpumesh::grpc {

// One dedicated callback thread with a condition-variable queue. The default
// pairing is one ThreadExecutor per reactor, so callback work shards with the
// EQ shards and an idle runtime consumes no spin cycles. The worker claims the
// whole queue per wake, so a burst of completions costs one lock acquisition.
class ThreadExecutor final : public Executor {
 public:
  ThreadExecutor();
  ~ThreadExecutor() override;

  void Run(absl::AnyInvocable<void()> task) override;
  void RunCompletion(absl::AnyInvocable<void(absl::Status)> callback,
                     absl::Status status) override;

 private:
  // A queued task, or a completion callback stored next to its status. Both
  // forms share one queue, so callbacks run in enqueue order.
  struct Entry {
    absl::AnyInvocable<void()> task;
    absl::AnyInvocable<void(absl::Status)> completion;
    absl::Status status;
  };

  void Push(Entry entry);
  void ThreadMain();

  std::mutex mu_;
  std::condition_variable cv_;
  std::deque<Entry> queue_;
  bool stopping_ = false;
  std::thread worker_;
};

}  // namespace dpumesh::grpc

#endif  // DPUMESH_GRPC_THREAD_EXECUTOR_H
