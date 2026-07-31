#ifndef DPUMESH_GRPC_ENDPOINT_TRANSPORT_H
#define DPUMESH_GRPC_ENDPOINT_TRANSPORT_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

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

// Registered transmit space held by the transport between Reserve and Commit.
struct Reservation {
  uint8_t* data = nullptr;
  size_t length = 0;
};

// Seam between the EventEngine endpoint state machine and the EQ reactor.
// Reserve() hands out registered transmit space that the caller fills
// completely and hands back to Commit(); the caller owns nothing after Commit,
// and every other Reserve result leaves the transport untouched. Reserve,
// Commit, Flush and the reservation memory belong to the work executor thread.
// Reserve() and Close() must not invoke DmeshEndpointDriver inline; reactor
// events are delivered separately. BindDriver() must also defer any
// already-queued reactor events rather than calling the driver in the
// DmeshEndpoint constructor's stack frame.
class EndpointTransport {
 public:
  virtual ~EndpointTransport() = default;
  virtual void BindDriver(std::weak_ptr<DmeshEndpointDriver> driver) = 0;
  virtual size_t MaxPostSize() const = 0;
  virtual PostResult Reserve(size_t length, Reservation* out) = 0;
  virtual absl::Status Commit(const Reservation& reservation) = 0;
  // Publish every reservation committed since the previous Flush. The native
  // transport deliberately batches commits, and a trailing partial unit may
  // wait briefly for a successor to share it; a logical EventEngine Write is
  // the boundary at which that decision is taken.
  virtual absl::Status Flush() = 0;
  // Return the receive credit the transport withheld while the endpoint queue
  // was above its high-water mark.
  virtual void ResumeReceive() = 0;
  virtual void Close() = 0;
};

}  // namespace dpumesh::grpc

#endif  // DPUMESH_GRPC_ENDPOINT_TRANSPORT_H
