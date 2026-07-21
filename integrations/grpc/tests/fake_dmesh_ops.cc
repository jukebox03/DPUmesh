#include "fake_dmesh_ops.h"

#include <errno.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dpumesh::grpc::testing {
namespace {

template <typename Predicate>
bool WaitUntil(std::mutex* mu, std::chrono::milliseconds timeout,
               Predicate predicate) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  for (;;) {
    {
      std::lock_guard<std::mutex> lock(*mu);
      if (predicate()) return true;
    }
    if (std::chrono::steady_clock::now() >= deadline) return false;
    std::this_thread::sleep_for(std::chrono::microseconds(100));
  }
}

}  // namespace

class FakeDmeshState::Impl final {
 public:
  struct Completion {
    dmesh_wc_t value{};
  };

  struct Cq {
    int fd = -1;
    bool destroyed = false;
    bool batch_active = false;
    std::thread::id poll_thread;
    int next_poll_error = 0;
    std::deque<Completion> completions;
  };

  struct Qp {
    dmesh_qp_t value{};
    Cq* cq = nullptr;
    bool alive = true;
    size_t alloc_calls = 0;
    size_t flush_calls = 0;
    std::deque<int> alloc_failures;
    int sticky_alloc_error = 0;
    std::deque<int> post_failures;
    std::vector<uint8_t> allocation;
    std::vector<std::vector<uint8_t>> posts;
  };

  ~Impl() {
    for (auto& cq : cqs) {
      if (cq->fd >= 0) ::close(cq->fd);
    }
  }

  Cq* AsCq(dmesh_cq_t* cq) const { return reinterpret_cast<Cq*>(cq); }

  dmesh_cq_t* AsPublic(Cq* cq) const {
    return reinterpret_cast<dmesh_cq_t*>(cq);
  }

  Qp* FindQp(dmesh_qp_t* qp) const {
    auto found = qps.find(qp);
    return found == qps.end() ? nullptr : found->second.get();
  }

  Qp* NewQp(Cq* cq, int role) {
    auto qp = std::make_unique<Qp>();
    qp->cq = cq;
    qp->value.ep = &channel;
    qp->value.cq = AsPublic(cq);
    qp->value.role = role;
    qp->value.local_port = next_port++;
    qp->value.rx_slot = -1;
    dmesh_qp_t* result = &qp->value;
    qps.emplace(result, std::move(qp));
    if (role == DMESH_ROLE_CLIENT) {
      last_client_qp = result;
      client_qps.push_back(result);
    }
    return FindQp(result);
  }

  void Signal(Cq* cq) {
    if (cq == nullptr || cq->fd < 0) return;
    const uint64_t one = 1;
    const ssize_t result = ::write(cq->fd, &one, sizeof(one));
    (void)result;
  }

  void QueueReceive(Qp* qp, const std::string& bytes) {
    const int32_t slot = next_rx_slot++;
    auto payload = std::make_shared<std::vector<uint8_t>>(bytes.begin(),
                                                          bytes.end());
    rx_payloads.emplace(slot, payload);
    Completion completion;
    completion.value.qp = &qp->value;
    completion.value.opcode = DMESH_WC_RECV;
    completion.value.buf = payload->data();
    completion.value.len = static_cast<uint32_t>(payload->size());
    completion.value._rx_token = slot;
    qp->cq->completions.push_back(completion);
  }

  mutable std::mutex mu;
  dmesh_channel_t channel{nullptr, 7, 8192, 65536};
  bool channel_created = false;
  int post_max = 65536;
  int next_create_qp_error = 0;
  uint16_t next_port = 1;
  int32_t next_rx_slot = 1;
  dmesh_qp_t* last_client_qp = nullptr;
  std::vector<dmesh_qp_t*> client_qps;
  std::vector<std::string> client_targets;
  std::vector<std::unique_ptr<Cq>> cqs;
  std::unordered_map<dmesh_qp_t*, std::unique_ptr<Qp>> qps;
  std::unordered_map<int32_t, std::shared_ptr<std::vector<uint8_t>>>
      rx_payloads;
  size_t releases = 0;
  size_t destroys = 0;
  size_t mid_batch_destroys = 0;
  size_t poll_thread_violations = 0;
  size_t channel_destroys = 0;
  size_t cq_destroys = 0;
};

class FakeDmeshApiOps final : public DmeshApiOps {
 public:
  explicit FakeDmeshApiOps(std::shared_ptr<FakeDmeshState> state)
      : state_(std::move(state)), impl_(state_->impl_.get()) {}

  dmesh_channel_t* CreateChannel() override {
    std::lock_guard<std::mutex> lock(impl_->mu);
    impl_->channel_created = true;
    return &impl_->channel;
  }

  int DestroyChannel(dmesh_channel_t* channel) override {
    std::lock_guard<std::mutex> lock(impl_->mu);
    if (channel != &impl_->channel) {
      errno = EINVAL;
      return -1;
    }
    ++impl_->channel_destroys;
    impl_->channel_created = false;
    return 0;
  }

  dmesh_cq_t* CreateCq(dmesh_channel_t* channel) override {
    std::lock_guard<std::mutex> lock(impl_->mu);
    if (channel != &impl_->channel) {
      errno = EINVAL;
      return nullptr;
    }
    auto cq = std::make_unique<FakeDmeshState::Impl::Cq>();
    cq->fd = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (cq->fd < 0) return nullptr;
    auto* result = cq.get();
    impl_->cqs.push_back(std::move(cq));
    return impl_->AsPublic(result);
  }

  int DestroyCq(dmesh_cq_t* cq) override {
    std::lock_guard<std::mutex> lock(impl_->mu);
    auto* fake = impl_->AsCq(cq);
    if (fake == nullptr || fake->destroyed) return 0;
    for (const auto& entry : impl_->qps) {
      if (entry.second->cq == fake && entry.second->alive) {
        errno = EBUSY;
        return -1;
      }
    }
    fake->destroyed = true;
    if (fake->fd >= 0) {
      ::close(fake->fd);
      fake->fd = -1;
    }
    ++impl_->cq_destroys;
    return 0;
  }

  int CqFd(dmesh_cq_t* cq) override {
    std::lock_guard<std::mutex> lock(impl_->mu);
    auto* fake = impl_->AsCq(cq);
    if (fake == nullptr || fake->destroyed) {
      errno = EINVAL;
      return -1;
    }
    return fake->fd;
  }

  dmesh_qp_t* CreateQp(dmesh_cq_t* cq, const char* service) override {
    std::lock_guard<std::mutex> lock(impl_->mu);
    if (service == nullptr || service[0] == '\0') {
      errno = EINVAL;
      return nullptr;
    }
    if (impl_->next_create_qp_error != 0) {
      errno = impl_->next_create_qp_error;
      impl_->next_create_qp_error = 0;
      return nullptr;
    }
    auto* fake_cq = impl_->AsCq(cq);
    if (fake_cq == nullptr || fake_cq->destroyed) {
      errno = EINVAL;
      return nullptr;
    }
    dmesh_qp_t* qp = &impl_->NewQp(fake_cq, DMESH_ROLE_CLIENT)->value;
    impl_->client_targets.emplace_back(service);
    return qp;
  }

  int DestroyQp(dmesh_qp_t* qp) override {
    std::lock_guard<std::mutex> lock(impl_->mu);
    auto* fake = impl_->FindQp(qp);
    if (fake == nullptr || !fake->alive) return 0;
    if (fake->cq->batch_active) ++impl_->mid_batch_destroys;
    fake->alive = false;
    fake->allocation.clear();
    ++impl_->destroys;
    return 0;
  }

  void* Alloc(dmesh_qp_t* qp, uint32_t len) override {
    std::lock_guard<std::mutex> lock(impl_->mu);
    auto* fake = impl_->FindQp(qp);
    if (fake == nullptr || !fake->alive || len == 0 ||
        len > static_cast<uint32_t>(impl_->post_max)) {
      errno = EINVAL;
      return nullptr;
    }
    ++fake->alloc_calls;
    if (fake->sticky_alloc_error != 0) {
      errno = fake->sticky_alloc_error;
      return nullptr;
    }
    if (!fake->alloc_failures.empty()) {
      errno = fake->alloc_failures.front();
      fake->alloc_failures.pop_front();
      return nullptr;
    }
    fake->allocation.assign(len, 0);
    return fake->allocation.data();
  }

  int PostSend(dmesh_qp_t* qp, const void* buffer, uint32_t len) override {
    std::lock_guard<std::mutex> lock(impl_->mu);
    auto* fake = impl_->FindQp(qp);
    if (fake == nullptr || !fake->alive || buffer == nullptr ||
        len == 0 || len > fake->allocation.size() ||
        buffer != fake->allocation.data()) {
      errno = EINVAL;
      return -1;
    }
    if (!fake->post_failures.empty()) {
      errno = fake->post_failures.front();
      fake->post_failures.pop_front();
      fake->allocation.clear();
      return -1;
    }
    fake->posts.emplace_back(fake->allocation.begin(),
                             fake->allocation.begin() + len);
    fake->allocation.clear();
    return 0;
  }

  int Flush(dmesh_qp_t* qp) override {
    std::lock_guard<std::mutex> lock(impl_->mu);
    auto* fake = impl_->FindQp(qp);
    if (fake == nullptr || !fake->alive) {
      errno = EINVAL;
      return -1;
    }
    ++fake->flush_calls;
    return 0;
  }

  int PollCq(dmesh_cq_t* cq, dmesh_wc_t* completions,
             int max_completions) override {
    std::lock_guard<std::mutex> lock(impl_->mu);
    auto* fake = impl_->AsCq(cq);
    if (fake == nullptr || fake->destroyed || completions == nullptr ||
        max_completions <= 0) {
      errno = EINVAL;
      return -1;
    }
    if (fake->next_poll_error != 0) {
      errno = fake->next_poll_error;
      fake->next_poll_error = 0;
      fake->batch_active = false;
      return -1;
    }
    const std::thread::id caller = std::this_thread::get_id();
    if (fake->poll_thread == std::thread::id()) {
      fake->poll_thread = caller;
    } else if (fake->poll_thread != caller) {
      ++impl_->poll_thread_violations;
    }

    fake->batch_active = false;
    int count = 0;
    while (count < max_completions && !fake->completions.empty()) {
      completions[count++] = fake->completions.front().value;
      fake->completions.pop_front();
    }
    fake->batch_active = count != 0;
    return count;
  }

  void Release(dmesh_channel_t* channel, dmesh_wc_t* completion) override {
    std::lock_guard<std::mutex> lock(impl_->mu);
    if (channel != &impl_->channel || completion == nullptr ||
        completion->_rx_token < 0) {
      return;
    }
    impl_->rx_payloads.erase(completion->_rx_token);
    completion->_rx_token = -1;
    completion->buf = nullptr;
    ++impl_->releases;
  }

  int MessageMax(dmesh_channel_t* /*channel*/) override {
    return impl_->channel.slot_size;
  }

  int PostMax(dmesh_channel_t* /*channel*/) override {
    std::lock_guard<std::mutex> lock(impl_->mu);
    return impl_->post_max;
  }

 private:
  std::shared_ptr<FakeDmeshState> state_;
  FakeDmeshState::Impl* const impl_;
};

FakeDmeshState::FakeDmeshState() : impl_(std::make_unique<Impl>()) {}

FakeDmeshState::~FakeDmeshState() = default;

void FakeDmeshState::SetPostMax(int post_max) {
  std::lock_guard<std::mutex> lock(impl_->mu);
  impl_->post_max = post_max;
  impl_->channel.block_size = post_max;
}

void FakeDmeshState::FailNextCreateQp(int error_number) {
  std::lock_guard<std::mutex> lock(impl_->mu);
  impl_->next_create_qp_error = error_number;
}

void FakeDmeshState::FailNextAlloc(dmesh_qp_t* qp, int error_number) {
  std::lock_guard<std::mutex> lock(impl_->mu);
  if (auto* fake = impl_->FindQp(qp)) {
    fake->alloc_failures.push_back(error_number);
  }
}

void FakeDmeshState::SetAllocError(dmesh_qp_t* qp, int error_number) {
  std::lock_guard<std::mutex> lock(impl_->mu);
  if (auto* fake = impl_->FindQp(qp)) {
    fake->sticky_alloc_error = error_number;
  }
}

void FakeDmeshState::FailNextPost(dmesh_qp_t* qp, int error_number) {
  std::lock_guard<std::mutex> lock(impl_->mu);
  if (auto* fake = impl_->FindQp(qp)) {
    fake->post_failures.push_back(error_number);
  }
}

void FakeDmeshState::FailNextPoll(int error_number) {
  std::lock_guard<std::mutex> lock(impl_->mu);
  if (impl_->cqs.empty()) return;
  impl_->cqs.front()->next_poll_error = error_number;
  impl_->Signal(impl_->cqs.front().get());
}

dmesh_qp_t* FakeDmeshState::WaitForClientQp(
    std::chrono::milliseconds timeout) {
  WaitUntil(&impl_->mu, timeout,
            [this] { return impl_->last_client_qp != nullptr; });
  std::lock_guard<std::mutex> lock(impl_->mu);
  return impl_->last_client_qp;
}

bool FakeDmeshState::WaitForClientQpCount(
    size_t count, std::chrono::milliseconds timeout) {
  return WaitUntil(&impl_->mu, timeout,
                   [this, count] { return impl_->client_qps.size() >= count; });
}

std::vector<dmesh_qp_t*> FakeDmeshState::ClientQps() const {
  std::lock_guard<std::mutex> lock(impl_->mu);
  return impl_->client_qps;
}

std::vector<std::string> FakeDmeshState::ClientTargets() const {
  std::lock_guard<std::mutex> lock(impl_->mu);
  return impl_->client_targets;
}

bool FakeDmeshState::WaitForPostCount(dmesh_qp_t* qp, size_t count,
                                      std::chrono::milliseconds timeout) {
  return WaitUntil(&impl_->mu, timeout, [this, qp, count] {
    auto* fake = impl_->FindQp(qp);
    return fake != nullptr && fake->posts.size() >= count;
  });
}

bool FakeDmeshState::WaitForAllocCallCount(
    dmesh_qp_t* qp, size_t count, std::chrono::milliseconds timeout) {
  return WaitUntil(&impl_->mu, timeout, [this, qp, count] {
    auto* fake = impl_->FindQp(qp);
    return fake != nullptr && fake->alloc_calls >= count;
  });
}

bool FakeDmeshState::WaitForReleaseCount(
    size_t count, std::chrono::milliseconds timeout) {
  return WaitUntil(&impl_->mu, timeout,
                   [this, count] { return impl_->releases >= count; });
}

bool FakeDmeshState::WaitForDestroyCount(
    size_t count, std::chrono::milliseconds timeout) {
  return WaitUntil(&impl_->mu, timeout,
                   [this, count] { return impl_->destroys >= count; });
}

void FakeDmeshState::InjectReceive(dmesh_qp_t* qp, const std::string& bytes) {
  std::lock_guard<std::mutex> lock(impl_->mu);
  auto* fake = impl_->FindQp(qp);
  if (fake == nullptr || !fake->alive) return;
  impl_->QueueReceive(fake, bytes);
  impl_->Signal(fake->cq);
}

void FakeDmeshState::InjectReceiveBatch(
    dmesh_qp_t* qp,
    const std::vector<std::string>& receives) {
  std::lock_guard<std::mutex> lock(impl_->mu);
  auto* fake = impl_->FindQp(qp);
  if (fake == nullptr || !fake->alive) return;
  for (const auto& receive : receives) impl_->QueueReceive(fake, receive);
  impl_->Signal(fake->cq);
}

void FakeDmeshState::InjectFin(dmesh_qp_t* qp) {
  std::lock_guard<std::mutex> lock(impl_->mu);
  auto* fake = impl_->FindQp(qp);
  if (fake == nullptr || !fake->alive) return;
  Impl::Completion completion;
  completion.value.qp = qp;
  completion.value.opcode = DMESH_WC_RECV_FIN;
  completion.value._rx_token = -1;
  fake->cq->completions.push_back(completion);
  impl_->Signal(fake->cq);
}

void FakeDmeshState::InjectTxReady(dmesh_qp_t* qp) {
  std::lock_guard<std::mutex> lock(impl_->mu);
  auto* fake = impl_->FindQp(qp);
  if (fake == nullptr || !fake->alive) return;
  Impl::Completion completion;
  completion.value.qp = qp;
  completion.value.opcode = DMESH_WC_TX_READY;
  completion.value._rx_token = -1;
  fake->cq->completions.push_back(completion);
  impl_->Signal(fake->cq);
}

dmesh_qp_t* FakeDmeshState::InjectConnectionRequest(
    const std::string& first_bytes) {
  std::lock_guard<std::mutex> lock(impl_->mu);
  if (impl_->cqs.empty()) return nullptr;
  Impl::Cq* cq = impl_->cqs.front().get();
  Impl::Qp* fake = impl_->NewQp(cq, DMESH_ROLE_SERVER);
  Impl::Completion request;
  request.value.qp = &fake->value;
  request.value.opcode = DMESH_WC_CONN_REQ;
  request.value._rx_token = -1;
  cq->completions.push_back(request);
  if (!first_bytes.empty()) impl_->QueueReceive(fake, first_bytes);
  impl_->Signal(cq);
  return &fake->value;
}

std::vector<std::vector<uint8_t>> FakeDmeshState::Posts(
    dmesh_qp_t* qp) const {
  std::lock_guard<std::mutex> lock(impl_->mu);
  auto* fake = impl_->FindQp(qp);
  return fake == nullptr ? std::vector<std::vector<uint8_t>>() : fake->posts;
}

size_t FakeDmeshState::alloc_calls(dmesh_qp_t* qp) const {
  std::lock_guard<std::mutex> lock(impl_->mu);
  auto* fake = impl_->FindQp(qp);
  return fake == nullptr ? 0 : fake->alloc_calls;
}

size_t FakeDmeshState::flush_calls(dmesh_qp_t* qp) const {
  std::lock_guard<std::mutex> lock(impl_->mu);
  auto* fake = impl_->FindQp(qp);
  return fake == nullptr ? 0 : fake->flush_calls;
}

size_t FakeDmeshState::release_count() const {
  std::lock_guard<std::mutex> lock(impl_->mu);
  return impl_->releases;
}

size_t FakeDmeshState::destroy_count() const {
  std::lock_guard<std::mutex> lock(impl_->mu);
  return impl_->destroys;
}

size_t FakeDmeshState::mid_batch_destroy_count() const {
  std::lock_guard<std::mutex> lock(impl_->mu);
  return impl_->mid_batch_destroys;
}

size_t FakeDmeshState::poll_thread_violation_count() const {
  std::lock_guard<std::mutex> lock(impl_->mu);
  return impl_->poll_thread_violations;
}

size_t FakeDmeshState::channel_destroy_count() const {
  std::lock_guard<std::mutex> lock(impl_->mu);
  return impl_->channel_destroys;
}

size_t FakeDmeshState::cq_destroy_count() const {
  std::lock_guard<std::mutex> lock(impl_->mu);
  return impl_->cq_destroys;
}

std::unique_ptr<DmeshApiOps> MakeFakeDmeshApiOps(
    std::shared_ptr<FakeDmeshState> state) {
  return std::make_unique<FakeDmeshApiOps>(std::move(state));
}

}  // namespace dpumesh::grpc::testing
