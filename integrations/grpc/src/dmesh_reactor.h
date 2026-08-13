#ifndef DPUMESH_GRPC_DMESH_REACTOR_H
#define DPUMESH_GRPC_DMESH_REACTOR_H

#include <cstddef>
#include <cstdint>
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

// One EQ shard and its single owner thread. The owner thread is the sole
// consumer of dmesh_poll_eq() and owns QP lifecycle; no QP is destroyed while
// a returned event batch can still name it. Transmit operations run on the
// thread pumping the endpoint write -- the Endpoint::Write() caller or the EQ
// owner delivering TX_READY -- under a per-connection lock, which the owner
// also takes before destroying that connection's QP.
class DmeshReactor final {
 public:
  struct Options {
    size_t event_batch_size = 64;
  };

  // Each field is sampled independently.
  struct Stats {
    // Receives whose credit was returned at the per-connection retention cap
    // while the endpoint asked to hold it.
    uint64_t receive_credit_hold_dropped = 0;
    // Drains that ended on the per-iteration poll budget with the EQ non-empty.
    uint64_t eq_drain_budget_exhausted = 0;
  };

  struct ConnectedTransport {
    std::unique_ptr<EndpointTransport> transport;
    // Carries the completions a gRPC call raises itself.
    std::shared_ptr<Executor> callback_executor;
    // Native identity of the two ends. The peer pair stays unset on a client
    // QP: the DPU selects and pins a backend for that stream, so the host
    // learns no peer pod.
    int local_pod = -1;
    uint16_t local_port = 0;
    int peer_pod = -1;
    uint16_t peer_port = 0;
  };

  using ConnectCallback = absl::AnyInvocable<void(
      absl::StatusOr<ConnectedTransport>)>;
  using AcceptCallback = std::function<void(ConnectedTransport)>;

  static absl::StatusOr<std::unique_ptr<DmeshReactor>> Create(
      DmeshApiOps* ops, dmesh_channel_t* channel, int post_max,
      std::shared_ptr<Executor> callback_executor, Options options);

  ~DmeshReactor();

  DmeshReactor(const DmeshReactor&) = delete;
  DmeshReactor& operator=(const DmeshReactor&) = delete;

  // QP creation and callback delivery are both asynchronous. The callback is
  // delivered on callback_executor, never on the EQ owner thread.
  void Connect(std::string service, ConnectCallback callback);

  // Installs or replaces the callback for native inbound connections. Returns
  // only after the EQ owner has applied the change, so callers may advertise
  // readiness immediately after success. The callback runs on
  // callback_executor and receives the same transport shape as Connect(). An
  // empty callback rejects future connection requests.
  absl::Status SetAcceptCallback(AcceptCallback callback);

  // Idempotent. Existing connections receive a terminal error, QPs are closed
  // on the owner thread, and the EQ is destroyed after the thread joins.
  void Shutdown();

  Stats stats() const;

 private:
  class Impl;

  explicit DmeshReactor(std::shared_ptr<Impl> impl);

  std::shared_ptr<Impl> impl_;
};

}  // namespace dpumesh::grpc

#endif  // DPUMESH_GRPC_DMESH_REACTOR_H
