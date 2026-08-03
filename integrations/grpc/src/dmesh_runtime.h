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

// Process-level DPUmesh ownership: one channel and N independent EQ reactors,
// each paired with its own dedicated callback thread by default. That thread
// is both the endpoint's callback executor and its work executor. A custom
// callback executor replaces every pair. Executors are shared with the
// endpoints they serve, and the runtime outlives those endpoints.
class DmeshRuntime final {
 public:
  struct Options {
    size_t reactor_count = 1;
    DmeshReactor::Options reactor;
  };

  static absl::StatusOr<std::shared_ptr<DmeshRuntime>> Create(
      std::unique_ptr<DmeshApiOps> ops);
  static absl::StatusOr<std::shared_ptr<DmeshRuntime>> Create(
      std::unique_ptr<DmeshApiOps> ops, Options options);
  static absl::StatusOr<std::shared_ptr<DmeshRuntime>> Create(
      std::unique_ptr<DmeshApiOps> ops,
      std::shared_ptr<Executor> callback_executor);
  static absl::StatusOr<std::shared_ptr<DmeshRuntime>> Create(
      std::unique_ptr<DmeshApiOps> ops,
      std::shared_ptr<Executor> callback_executor, Options options);

  ~DmeshRuntime();

  DmeshRuntime(const DmeshRuntime&) = delete;
  DmeshRuntime& operator=(const DmeshRuntime&) = delete;

  void Connect(std::string service, DmeshReactor::ConnectCallback callback);
  absl::Status SetAcceptCallback(DmeshReactor::AcceptCallback callback);
  int post_max() const { return post_max_; }
  // Diagnostics only: lets a benchmark read native transmit counters.
  dmesh_channel_t* channel() const { return channel_; }
  const std::shared_ptr<Executor>& callback_executor() const {
    return callback_executor_;
  }
  // Summed over every reactor shard.
  DmeshReactor::Stats stats() const;

 private:
  DmeshRuntime(std::unique_ptr<DmeshApiOps> ops, dmesh_channel_t* channel,
               int post_max,
               std::shared_ptr<Executor> callback_executor);

  std::unique_ptr<DmeshApiOps> ops_;
  dmesh_channel_t* channel_;
  int post_max_;
  // Released after reactors_ (declared first), which still schedule onto it.
  const std::shared_ptr<Executor> callback_executor_;
  std::vector<std::unique_ptr<DmeshReactor>> reactors_;
  std::atomic<size_t> next_reactor_{0};
};

}  // namespace dpumesh::grpc

#endif  // DPUMESH_GRPC_DMESH_RUNTIME_H
