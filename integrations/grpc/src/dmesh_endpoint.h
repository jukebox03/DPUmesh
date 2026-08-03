#ifndef DPUMESH_GRPC_DMESH_ENDPOINT_H
#define DPUMESH_GRPC_DMESH_ENDPOINT_H

#include <cstddef>
#include <cstdint>
#include <memory>

#include <grpc/event_engine/event_engine.h>
#include <grpc/event_engine/memory_allocator.h>

#include "absl/functional/function_ref.h"
#include "absl/status/status.h"
#include "absl/types/span.h"
#include "endpoint_transport.h"
#include "executor.h"

namespace dpumesh::grpc {

class DmeshEndpointState;

// Queued receive bytes above which an endpoint asks its reactor to hold the
// native receive credit.
inline constexpr size_t kReceiveHighWaterBytes = 1024 * 1024;

// Result of handing one native receive to the endpoint.
struct ReceiveOutcome {
  absl::Status status;
  // True while the endpoint holds more queued bytes than its high-water mark.
  // The reactor then keeps that event's receive credit until a read drains the
  // queue, and the transport lands no further bytes on this connection.
  bool hold_credit = false;
};

// A reactor locks a weak reference to this object for each EQ event. A driver
// already locked by the reactor keeps the shared endpoint state valid for that
// event, while the public destructor still transitions it to closing immediately.
class DmeshEndpointDriver final {
 public:
  explicit DmeshEndpointDriver(std::shared_ptr<DmeshEndpointState> state);

  // Copies `length` bytes into one gRPC slice through `fill`, which writes
  // exactly `length` bytes at the pointer it receives. One reactor call
  // carries a whole batch run: one slice, one endpoint lock, one queue entry.
  ReceiveOutcome OnIncomingData(size_t length,
                                absl::FunctionRef<void(uint8_t*)> fill);
  ReceiveOutcome OnIncomingData(absl::Span<const uint8_t> bytes);
  void OnWritable();
  void OnRemoteEof();
  void OnTransportError(absl::Status status);

 private:
  std::shared_ptr<DmeshEndpointState> state_;
};

class DmeshEndpoint final
    : public grpc_event_engine::experimental::EventEngine::Endpoint {
 public:
  using EventEngine = grpc_event_engine::experimental::EventEngine;
  using MemoryAllocator = grpc_event_engine::experimental::MemoryAllocator;
  using SliceBuffer = grpc_event_engine::experimental::SliceBuffer;

  // Shares ownership of the executor for as long as the endpoint or a retained
  // driver can schedule on it.
  DmeshEndpoint(std::unique_ptr<EndpointTransport> transport,
                std::shared_ptr<Executor> callback_executor,
                MemoryAllocator allocator,
                EventEngine::ResolvedAddress peer_address = {},
                EventEngine::ResolvedAddress local_address = {});
  ~DmeshEndpoint() override;

  DmeshEndpoint(const DmeshEndpoint&) = delete;
  DmeshEndpoint& operator=(const DmeshEndpoint&) = delete;

  bool Read(absl::AnyInvocable<void(absl::Status)> on_read,
            SliceBuffer* buffer, ReadArgs args) override;
  bool Write(absl::AnyInvocable<void(absl::Status)> on_writable,
             SliceBuffer* data, WriteArgs args) override;

  const EventEngine::ResolvedAddress& GetPeerAddress() const override;
  const EventEngine::ResolvedAddress& GetLocalAddress() const override;
  std::shared_ptr<TelemetryInfo> GetTelemetryInfo() const override;

  std::shared_ptr<DmeshEndpointDriver> driver() const { return driver_; }

 private:
  std::shared_ptr<DmeshEndpointState> state_;
  std::shared_ptr<DmeshEndpointDriver> driver_;
  EventEngine::ResolvedAddress peer_address_;
  EventEngine::ResolvedAddress local_address_;
};

}  // namespace dpumesh::grpc

#endif  // DPUMESH_GRPC_DMESH_ENDPOINT_H
