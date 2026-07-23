#ifndef DPUMESH_GRPC_DMESH_API_OPS_H
#define DPUMESH_GRPC_DMESH_API_OPS_H

#include <cstdint>
#include <memory>

#include <dpumesh/dmesh.h>

namespace dpumesh::grpc {

// The only native-API seam used by the adapter. Keeping this interface at the
// public dmesh.h level prevents accidental dependencies on dmesh_core.h.
class DmeshApiOps {
 public:
  virtual ~DmeshApiOps() = default;

  virtual dmesh_channel_t* CreateChannel() = 0;
  virtual int DestroyChannel(dmesh_channel_t* channel) = 0;
  virtual dmesh_eq_t* CreateEq(dmesh_channel_t* channel) = 0;
  virtual int DestroyEq(dmesh_eq_t* eq) = 0;
  virtual int EqFd(dmesh_eq_t* eq) = 0;
  virtual dmesh_qp_t* CreateQp(dmesh_eq_t* eq, const char* service) = 0;
  virtual int DestroyQp(dmesh_qp_t* qp) = 0;
  virtual void* Alloc(dmesh_qp_t* qp, uint32_t len) = 0;
  virtual int PostSend(dmesh_qp_t* qp, const void* buffer, uint32_t len) = 0;
  virtual int Flush(dmesh_qp_t* qp) = 0;
  virtual int PollEq(dmesh_eq_t* eq, dmesh_event_t* events,
                     int max_events) = 0;
  virtual void ReleaseRxBuffer(dmesh_channel_t* channel,
                               dmesh_event_t* event) = 0;
  virtual int MessageMax(dmesh_channel_t* channel) = 0;
  virtual int PostMax(dmesh_channel_t* channel) = 0;
};

std::unique_ptr<DmeshApiOps> MakeNativeDmeshApiOps();

}  // namespace dpumesh::grpc

#endif  // DPUMESH_GRPC_DMESH_API_OPS_H
