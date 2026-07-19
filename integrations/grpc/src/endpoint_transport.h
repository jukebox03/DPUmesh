#ifndef DPUMESH_GRPC_ENDPOINT_TRANSPORT_H
#define DPUMESH_GRPC_ENDPOINT_TRANSPORT_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

#include "absl/status/status.h"
#include "absl/types/span.h"

namespace dpumesh::grpc {

class DmeshEndpointDriver;

enum class PostCode {
  kAccepted,
  kWouldBlock,
  kClosed,
  kError,
};

struct PostResult {
  PostCode code;
  absl::Status status;

  static PostResult Accepted() { return {PostCode::kAccepted, absl::OkStatus()}; }
  static PostResult WouldBlock() {
    return {PostCode::kWouldBlock, absl::OkStatus()};
  }
  static PostResult Closed(absl::Status status) {
    return {PostCode::kClosed, std::move(status)};
  }
  static PostResult Error(absl::Status status) {
    return {PostCode::kError, std::move(status)};
  }
};

// Phase-1 seam between the EventEngine endpoint state machine and the future
// CQ reactor. Post() consumes the complete span synchronously on kAccepted;
// callers retain ownership on every other result. Post() and Close() must not
// invoke DmeshEndpointDriver inline; reactor events are delivered separately.
// BindDriver() must also defer any already-queued reactor events rather than
// calling the driver in the DmeshEndpoint constructor's stack frame.
class EndpointTransport {
 public:
  virtual ~EndpointTransport() = default;
  virtual void BindDriver(std::weak_ptr<DmeshEndpointDriver> driver) = 0;
  virtual size_t MaxPostSize() const = 0;
  virtual PostResult Post(absl::Span<const uint8_t> bytes) = 0;
  virtual void Close() = 0;
};

}  // namespace dpumesh::grpc

#endif  // DPUMESH_GRPC_ENDPOINT_TRANSPORT_H
