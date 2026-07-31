#ifndef DPUMESH_GRPC_EXECUTOR_H
#define DPUMESH_GRPC_EXECUTOR_H

#include <utility>

#include "absl/functional/any_invocable.h"
#include "absl/status/status.h"

namespace dpumesh::grpc {

// Run() must enqueue work and must not invoke it inline. DmeshEndpoint relies
// on that rule to keep EventEngine callbacks out of endpoint locks and caller
// stack frames.
class Executor {
 public:
  virtual ~Executor() = default;
  virtual void Run(absl::AnyInvocable<void()> task) = 0;

  // Completion form of Run(). An endpoint callback and the status it completes
  // with exceed the inline capacity of one type-erased object, so the per-RPC
  // path passes the pair directly and an executor that queues it as a pair
  // schedules a completion without allocating.
  virtual void RunCompletion(absl::AnyInvocable<void(absl::Status)> callback,
                             absl::Status status) {
    Run([callback = std::move(callback),
         status = std::move(status)]() mutable { callback(std::move(status)); });
  }
};

}  // namespace dpumesh::grpc

#endif  // DPUMESH_GRPC_EXECUTOR_H
