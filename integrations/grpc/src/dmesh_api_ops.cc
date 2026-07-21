#include "dmesh_api_ops.h"

namespace dpumesh::grpc {
namespace {

class NativeDmeshApiOps final : public DmeshApiOps {
 public:
  dmesh_channel_t* CreateChannel() override { return dmesh_create_channel(); }
  int DestroyChannel(dmesh_channel_t* channel) override {
    return dmesh_destroy_channel(channel);
  }
  dmesh_cq_t* CreateCq(dmesh_channel_t* channel) override {
    return dmesh_create_cq(channel);
  }
  int DestroyCq(dmesh_cq_t* cq) override { return dmesh_destroy_cq(cq); }
  int CqFd(dmesh_cq_t* cq) override { return dmesh_cq_fd(cq); }
  dmesh_qp_t* CreateQp(dmesh_cq_t* cq, const char* service) override {
    return dmesh_create_qp(cq, service);
  }
  int DestroyQp(dmesh_qp_t* qp) override { return dmesh_destroy_qp(qp); }
  void* Alloc(dmesh_qp_t* qp, uint32_t len) override {
    return dmesh_alloc(qp, len);
  }
  int PostSend(dmesh_qp_t* qp, const void* buffer, uint32_t len) override {
    return dmesh_post_send(qp, buffer, len);
  }
  int Flush(dmesh_qp_t* qp) override { return dmesh_flush(qp); }
  int PollCq(dmesh_cq_t* cq, dmesh_wc_t* completions,
             int max_completions) override {
    return dmesh_poll_cq(cq, completions, max_completions);
  }
  void Release(dmesh_channel_t* channel, dmesh_wc_t* completion) override {
    dmesh_wc_release(channel, completion);
  }
  int MessageMax(dmesh_channel_t* channel) override {
    return dmesh_msg_max(channel);
  }
  int PostMax(dmesh_channel_t* channel) override {
    return dmesh_post_max(channel);
  }
};

}  // namespace

std::unique_ptr<DmeshApiOps> MakeNativeDmeshApiOps() {
  return std::make_unique<NativeDmeshApiOps>();
}

}  // namespace dpumesh::grpc
