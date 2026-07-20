#ifndef DPUMESH_GRPC_TESTS_FAKE_DMESH_OPS_H
#define DPUMESH_GRPC_TESTS_FAKE_DMESH_OPS_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "dmesh_api_ops.h"

namespace dpumesh::grpc::testing {

class FakeDmeshApiOps;

// Thread-safe fake of the public DPUmesh API. Its CQ fd is a real eventfd, so
// reactor tests exercise the production poll/eventfd owner-thread loop rather
// than a separate test-only scheduler.
class FakeDmeshState final {
 public:
  FakeDmeshState();
  ~FakeDmeshState();

  FakeDmeshState(const FakeDmeshState&) = delete;
  FakeDmeshState& operator=(const FakeDmeshState&) = delete;

  void SetPostMax(int post_max);
  void FailNextCreateQp(int error_number);
  void FailNextAlloc(dmesh_qp_t* qp, int error_number);
  void SetAllocError(dmesh_qp_t* qp, int error_number);
  void FailNextPost(dmesh_qp_t* qp, int error_number);
  void FailNextPoll(int error_number);

  dmesh_qp_t* WaitForClientQp(std::chrono::milliseconds timeout);
  bool WaitForClientQpCount(size_t count, std::chrono::milliseconds timeout);
  std::vector<dmesh_qp_t*> ClientQps() const;
  bool WaitForPostCount(dmesh_qp_t* qp, size_t count,
                        std::chrono::milliseconds timeout);
  bool WaitForAllocCallCount(dmesh_qp_t* qp, size_t count,
                             std::chrono::milliseconds timeout);
  bool WaitForReleaseCount(size_t count, std::chrono::milliseconds timeout);
  bool WaitForDestroyCount(size_t count, std::chrono::milliseconds timeout);

  void InjectReceive(dmesh_qp_t* qp, uint16_t stream,
                     const std::string& bytes);
  void InjectReceiveBatch(
      dmesh_qp_t* qp,
      const std::vector<std::pair<uint16_t, std::string>>& receives);
  void InjectFin(dmesh_qp_t* qp);
  dmesh_qp_t* InjectConnectionRequest(const std::string& first_bytes,
                                      uint16_t stream);

  std::vector<std::vector<uint8_t>> Posts(dmesh_qp_t* qp) const;
  size_t alloc_calls(dmesh_qp_t* qp) const;
  size_t flush_calls(dmesh_qp_t* qp) const;
  size_t release_count() const;
  size_t destroy_count() const;
  size_t mid_batch_destroy_count() const;
  size_t poll_thread_violation_count() const;
  size_t channel_destroy_count() const;
  size_t cq_destroy_count() const;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;

  friend std::unique_ptr<DmeshApiOps> MakeFakeDmeshApiOps(
      std::shared_ptr<FakeDmeshState> state);
  friend class FakeDmeshApiOps;
};

std::unique_ptr<DmeshApiOps> MakeFakeDmeshApiOps(
    std::shared_ptr<FakeDmeshState> state);

}  // namespace dpumesh::grpc::testing

#endif  // DPUMESH_GRPC_TESTS_FAKE_DMESH_OPS_H
