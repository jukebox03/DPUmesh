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
  virtual dmesh_cq_t* CreateCq(dmesh_channel_t* channel) = 0;
  virtual int DestroyCq(dmesh_cq_t* cq) = 0;
  virtual int CqFd(dmesh_cq_t* cq) = 0;
  virtual dmesh_qp_t* CreateQp(dmesh_cq_t* cq, const char* service) = 0;
  virtual int DestroyQp(dmesh_qp_t* qp) = 0;
  virtual void* Alloc(dmesh_qp_t* qp, uint32_t len) = 0;
  virtual int PostSend(dmesh_qp_t* qp, const void* buffer, uint32_t len) = 0;
  virtual int Flush(dmesh_qp_t* qp) = 0;
  virtual int PollCq(dmesh_cq_t* cq, dmesh_wc_t* completions,
                     int max_completions) = 0;
  virtual void Release(dmesh_channel_t* channel, dmesh_wc_t* completion) = 0;
  virtual int MessageMax(dmesh_channel_t* channel) = 0;
  virtual int PostMax(dmesh_channel_t* channel) = 0;
};

std::unique_ptr<DmeshApiOps> MakeNativeDmeshApiOps();

}  // namespace dpumesh::grpc

#endif  // DPUMESH_GRPC_DMESH_API_OPS_H
