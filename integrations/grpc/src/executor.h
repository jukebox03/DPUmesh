#ifndef DPUMESH_GRPC_EXECUTOR_H
#define DPUMESH_GRPC_EXECUTOR_H

#include <memory>
#include <utility>

#include "absl/functional/any_invocable.h"
#include "absl/status/status.h"

namespace dpumesh::grpc {

// Run() must enqueue work and must not invoke it inline.
class Executor {
 public:
  virtual ~Executor() = default;
  virtual void Run(absl::AnyInvocable<void()> task) = 0;

  // Completion form of Run(), carrying the callback and its status as a pair.
  virtual void RunCompletion(absl::AnyInvocable<void(absl::Status)> callback,
                             absl::Status status) {
    Run([callback = std::move(callback),
         status = std::move(status)]() mutable { callback(std::move(status)); });
  }
};

// Adapts an executor the caller keeps alive itself. The caller then guarantees
// it outlives every endpoint that schedules on it.
inline std::shared_ptr<Executor> UnownedExecutor(Executor* executor) {
  return std::shared_ptr<Executor>(executor, [](Executor*) {});
}

}  // namespace dpumesh::grpc

#endif  // DPUMESH_GRPC_EXECUTOR_H
