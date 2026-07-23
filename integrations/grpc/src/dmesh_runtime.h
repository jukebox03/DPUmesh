#ifndef DPUMESH_GRPC_DMESH_RUNTIME_H
#define DPUMESH_GRPC_DMESH_RUNTIME_H

#include <atomic>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "absl/status/statusor.h"
#include "dmesh_api_ops.h"
#include "dmesh_reactor.h"
#include "executor.h"

namespace dpumesh::grpc {

// Process-level DPUmesh ownership: one channel and N independent EQ reactors.
// The callback executor must outlive the runtime and every endpoint created by
// one of its reactors. The runtime itself must also outlive those endpoints,
// because each endpoint uses its owning reactor as the work executor.
class DmeshRuntime final {
 public:
  struct Options {
    size_t reactor_count = 1;
    DmeshReactor::Options reactor;
  };

  static absl::StatusOr<std::unique_ptr<DmeshRuntime>> Create(
      std::unique_ptr<DmeshApiOps> ops, Executor* callback_executor);
  static absl::StatusOr<std::unique_ptr<DmeshRuntime>> Create(
      std::unique_ptr<DmeshApiOps> ops, Executor* callback_executor,
      Options options);

  ~DmeshRuntime();

  DmeshRuntime(const DmeshRuntime&) = delete;
  DmeshRuntime& operator=(const DmeshRuntime&) = delete;

  void Connect(std::string service, DmeshReactor::ConnectCallback callback);
  absl::Status SetAcceptCallback(DmeshReactor::AcceptCallback callback);
  int post_max() const { return post_max_; }
  size_t reactor_count() const { return reactors_.size(); }
  Executor* callback_executor() const { return callback_executor_; }

 private:
  DmeshRuntime(std::unique_ptr<DmeshApiOps> ops, dmesh_channel_t* channel,
               int post_max, Executor* callback_executor);

  std::unique_ptr<DmeshApiOps> ops_;
  dmesh_channel_t* channel_;
  int post_max_;
  Executor* const callback_executor_;
  std::vector<std::unique_ptr<DmeshReactor>> reactors_;
  std::atomic<size_t> next_reactor_{0};
};

}  // namespace dpumesh::grpc

#endif  // DPUMESH_GRPC_DMESH_RUNTIME_H
