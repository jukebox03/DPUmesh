#ifndef DPUMESH_GRPC_DMESH_ENDPOINT_H
#define DPUMESH_GRPC_DMESH_ENDPOINT_H

#include <cstdint>
#include <memory>

#include <grpc/event_engine/event_engine.h>
#include <grpc/event_engine/memory_allocator.h>

#include "absl/status/status.h"
#include "absl/types/span.h"
#include "endpoint_transport.h"
#include "executor.h"

namespace dpumesh::grpc {

class DmeshEndpointState;

// A reactor locks a weak reference to this object for each EQ event. A driver
// already locked by the reactor keeps the shared endpoint state valid for that
// event, while the public destructor still transitions it to closing immediately.
class DmeshEndpointDriver final {
 public:
  explicit DmeshEndpointDriver(std::shared_ptr<DmeshEndpointState> state);

  absl::Status OnIncomingData(absl::Span<const uint8_t> bytes);
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

  DmeshEndpoint(std::unique_ptr<EndpointTransport> transport,
                Executor* work_executor, Executor* callback_executor,
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

  // Both executors must outlive the endpoint and every retained driver. The
  // production runtime naturally satisfies this because it owns reactor and
  // callback executors above all connection state.

 private:
  std::shared_ptr<DmeshEndpointState> state_;
  std::shared_ptr<DmeshEndpointDriver> driver_;
  EventEngine::ResolvedAddress peer_address_;
  EventEngine::ResolvedAddress local_address_;
};

}  // namespace dpumesh::grpc

#endif  // DPUMESH_GRPC_DMESH_ENDPOINT_H
