#include "dmesh_api_ops.h"

namespace dpumesh::grpc {
namespace {

class NativeDmeshApiOps final : public DmeshApiOps {
 public:
  dmesh_channel_t* CreateChannel() override { return dmesh_create_channel(); }
  int DestroyChannel(dmesh_channel_t* channel) override {
    return dmesh_destroy_channel(channel);
  }
  dmesh_eq_t* CreateEq(dmesh_channel_t* channel) override {
    return dmesh_create_eq(channel);
  }
  int DestroyEq(dmesh_eq_t* eq) override { return dmesh_destroy_eq(eq); }
  int EqFd(dmesh_eq_t* eq) override { return dmesh_eq_fd(eq); }
  dmesh_qp_t* CreateQp(dmesh_eq_t* eq, const char* service) override {
    return dmesh_create_qp(eq, service);
  }
  int DestroyQp(dmesh_qp_t* qp) override { return dmesh_destroy_qp(qp); }
  void* Alloc(dmesh_qp_t* qp, uint32_t len) override {
    return dmesh_alloc(qp, len);
  }
  int PostSend(dmesh_qp_t* qp, const void* buffer, uint32_t len) override {
    return dmesh_post_send(qp, buffer, len);
  }
  int Flush(dmesh_qp_t* qp) override { return dmesh_flush(qp); }
  int PollEq(dmesh_eq_t* eq, dmesh_event_t* events,
             int max_events) override {
    return dmesh_poll_eq(eq, events, max_events);
  }
  void ReleaseRxBuffer(dmesh_channel_t* channel,
                       dmesh_event_t* event) override {
    dmesh_release_rx_buffer(channel, event);
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
