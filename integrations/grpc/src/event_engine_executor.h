#ifndef DPUMESH_GRPC_EVENT_ENGINE_EXECUTOR_H
#define DPUMESH_GRPC_EVENT_ENGINE_EXECUTOR_H

#include <memory>

#include <grpc/event_engine/event_engine.h>

#include "absl/functional/any_invocable.h"
#include "executor.h"

namespace dpumesh::grpc {

// Executor backed by the default gRPC EventEngine thread pool — the same
// pool that runs gRPC's own endpoint callbacks and timers. Run() schedules
// onto that pool and never executes inline.
class EventEngineExecutor final : public Executor {
 public:
  EventEngineExecutor();

  void Run(absl::AnyInvocable<void()> task) override;

 private:
  std::shared_ptr<grpc_event_engine::experimental::EventEngine> engine_;
};

}  // namespace dpumesh::grpc

#endif  // DPUMESH_GRPC_EVENT_ENGINE_EXECUTOR_H
