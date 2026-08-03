#ifndef DPUMESH_GRPC_ENDPOINT_TRANSPORT_H
#define DPUMESH_GRPC_ENDPOINT_TRANSPORT_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

#include "absl/functional/function_ref.h"
#include "absl/status/status.h"

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

// Registered transmit space, valid only for the duration of the fill callback.
struct Reservation {
  uint8_t* data = nullptr;
  size_t length = 0;
};

// Seam between the EventEngine endpoint state machine and the EQ reactor.
// Post() and Flush() run on the work executor thread. Post(), Close() and
// BindDriver() must not invoke DmeshEndpointDriver inline; reactor events are
// delivered separately.
class EndpointTransport {
 public:
  virtual ~EndpointTransport() = default;
  virtual void BindDriver(std::weak_ptr<DmeshEndpointDriver> driver) = 0;
  virtual size_t MaxPostSize() const = 0;
  // Reserve `length` bytes of registered transmit space, invoke `fill` on it,
  // and submit it, holding the connection's transmit lock throughout. `fill`
  // must write every byte of the reservation and must not re-enter the
  // transport. It runs on kAccepted only; every other result leaves the
  // transport untouched and `fill` uncalled.
  virtual PostResult Post(size_t length,
                          absl::FunctionRef<void(Reservation)> fill) = 0;
  // Mark the logical write boundary. DPUmesh transports pass the boundary to
  // libdpumesh; physical batching remains inside the library.
  virtual absl::Status Flush() = 0;
  // Return the receive credit withheld above the endpoint's high-water mark.
  virtual void ResumeReceive() = 0;
  virtual void Close() = 0;
};

}  // namespace dpumesh::grpc

#endif  // DPUMESH_GRPC_ENDPOINT_TRANSPORT_H
