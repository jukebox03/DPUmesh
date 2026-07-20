#ifndef DPUMESH_GRPC_DMESH_REACTOR_H
#define DPUMESH_GRPC_DMESH_REACTOR_H

#include <cstddef>
#include <functional>
#include <memory>
#include <string>

#include "absl/functional/any_invocable.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "dmesh_api_ops.h"
#include "endpoint_transport.h"
#include "executor.h"

namespace dpumesh::grpc {

// One CQ shard and its single owner thread. Every QP operation is marshalled
// through Run(), so dmesh_poll_cq() has exactly one consumer and no QP is
// destroyed while a returned completion batch can still name it.
class DmeshReactor final : public Executor {
 public:
  struct Options {
    size_t cq_batch_size = 64;
  };

  struct ConnectedTransport {
    std::unique_ptr<EndpointTransport> transport;
    Executor* work_executor = nullptr;
  };

  using ConnectCallback = absl::AnyInvocable<void(
      absl::StatusOr<ConnectedTransport>)>;
  using AcceptCallback = std::function<void(ConnectedTransport)>;

  static absl::StatusOr<std::unique_ptr<DmeshReactor>> Create(
      DmeshApiOps* ops, dmesh_channel_t* channel, int post_max,
      Executor* callback_executor);
  static absl::StatusOr<std::unique_ptr<DmeshReactor>> Create(
      DmeshApiOps* ops, dmesh_channel_t* channel, int post_max,
      Executor* callback_executor, Options options);

  ~DmeshReactor() override;

  DmeshReactor(const DmeshReactor&) = delete;
  DmeshReactor& operator=(const DmeshReactor&) = delete;

  // Executor contract: enqueue only, never invoke inline.
  void Run(absl::AnyInvocable<void()> task) override;

  // QP creation and callback delivery are both asynchronous. The callback is
  // delivered on callback_executor, never on the CQ owner thread.
  void Connect(std::string service, ConnectCallback callback);

  // Installs or replaces the callback for native inbound connections. Returns
  // only after the CQ owner has applied the change, so callers may advertise
  // readiness immediately after success. The callback runs on
  // callback_executor and receives the same transport shape as Connect(). An
  // empty callback rejects future connection requests.
  absl::Status SetAcceptCallback(AcceptCallback callback);

  // Idempotent. Existing connections receive a terminal error, QPs are closed
  // on the owner thread, and the CQ is destroyed after the thread joins.
  void Shutdown();

 private:
  class Impl;

  explicit DmeshReactor(std::shared_ptr<Impl> impl);

  std::shared_ptr<Impl> impl_;
};

}  // namespace dpumesh::grpc

#endif  // DPUMESH_GRPC_DMESH_REACTOR_H
