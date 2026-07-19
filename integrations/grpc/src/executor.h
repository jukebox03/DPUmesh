#ifndef DPUMESH_GRPC_EXECUTOR_H
#define DPUMESH_GRPC_EXECUTOR_H

#include "absl/functional/any_invocable.h"

namespace dpumesh::grpc {

// Run() must enqueue work and must not invoke it inline. DmeshEndpoint relies
// on that rule to keep EventEngine callbacks out of endpoint locks and caller
// stack frames.
class Executor {
 public:
  virtual ~Executor() = default;
  virtual void Run(absl::AnyInvocable<void()> task) = 0;
};

}  // namespace dpumesh::grpc

#endif  // DPUMESH_GRPC_EXECUTOR_H

