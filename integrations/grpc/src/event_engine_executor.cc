#include "event_engine_executor.h"

#include <utility>

namespace dpumesh::grpc {

EventEngineExecutor::EventEngineExecutor()
    : engine_(grpc_event_engine::experimental::GetDefaultEventEngine()) {}

void EventEngineExecutor::Run(absl::AnyInvocable<void()> task) {
  engine_->Run(std::move(task));
}

}  // namespace dpumesh::grpc
