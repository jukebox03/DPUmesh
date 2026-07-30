#ifndef DPUMESH_GRPC_THREAD_EXECUTOR_H
#define DPUMESH_GRPC_THREAD_EXECUTOR_H

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

#include "absl/functional/any_invocable.h"
#include "executor.h"

namespace dpumesh::grpc {

// One dedicated callback thread with a condition-variable queue. The default
// pairing is one ThreadExecutor per reactor, so callback work shards with the
// EQ shards and an idle runtime consumes no spin cycles.
class ThreadExecutor final : public Executor {
 public:
  ThreadExecutor();
  ~ThreadExecutor() override;

  void Run(absl::AnyInvocable<void()> task) override;

 private:
  void ThreadMain();

  std::mutex mu_;
  std::condition_variable cv_;
  std::deque<absl::AnyInvocable<void()>> tasks_;
  bool stopping_ = false;
  std::thread worker_;
};

}  // namespace dpumesh::grpc

#endif  // DPUMESH_GRPC_THREAD_EXECUTOR_H
