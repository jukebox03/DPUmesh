#include "dmesh_reactor.h"

#include <errno.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/types/span.h"
#include "dmesh_endpoint.h"

namespace dpumesh::grpc {
namespace {

absl::Status ErrnoStatus(const char* operation, int error_number) {
  if (error_number == 0) error_number = EIO;
  const std::string message = absl::StrCat(
      operation, " failed: errno=", error_number, " (",
      std::generic_category().message(error_number), ")");
  switch (error_number) {
    case ENOENT:
      return absl::UnavailableError(message);
    case ENOMEM:
    case EMFILE:
    case ENOSPC:
      return absl::ResourceExhaustedError(message);
    case EBADMSG:
    case EIO:
      return absl::UnavailableError(message);
    case EINVAL:
      return absl::InternalError(message);
    default:
      return absl::UnknownError(message);
  }
}

void DrainCounterFd(int fd) {
  uint64_t value;
  while (::read(fd, &value, sizeof(value)) == sizeof(value)) {
  }
}

timespec ToTimespec(std::chrono::microseconds delay) {
  const auto seconds =
      std::chrono::duration_cast<std::chrono::seconds>(delay);
  const auto nanoseconds =
      std::chrono::duration_cast<std::chrono::nanoseconds>(delay - seconds);
  return timespec{static_cast<time_t>(seconds.count()),
                  static_cast<long>(nanoseconds.count())};
}

}  // namespace

class DmeshReactor::Impl final
    : public std::enable_shared_from_this<DmeshReactor::Impl> {
 public:
  struct Connection {
    struct QueuedReceive {
      std::vector<uint8_t> bytes;
    };

    enum class Terminal {
      kNone,
      kRemoteEof,
      kError,
    };

    dmesh_qp_t* qp = nullptr;
    std::weak_ptr<DmeshEndpointDriver> driver;
    std::optional<uint16_t> stream;
    std::deque<QueuedReceive> prebind_receives;
    Terminal prebind_terminal = Terminal::kNone;
    absl::Status prebind_error = absl::OkStatus();
    std::atomic<bool> close_enqueued{false};
    bool closing = false;
    bool remote_eof = false;
    bool unflushed = false;
  };

  class Transport final : public EndpointTransport {
   public:
    Transport(std::weak_ptr<Impl> impl, std::shared_ptr<Connection> connection,
              size_t post_max)
        : impl_(std::move(impl)),
          connection_(std::move(connection)),
          post_max_(post_max) {}

    ~Transport() override { Close(); }

    void BindDriver(std::weak_ptr<DmeshEndpointDriver> driver) override {
      if (auto impl = impl_.lock()) {
        impl->AttachDriver(connection_, std::move(driver));
      }
    }

    size_t MaxPostSize() const override { return post_max_; }

    PostResult Post(absl::Span<const uint8_t> bytes) override {
      if (auto impl = impl_.lock()) {
        return impl->Post(connection_, bytes);
      }
      return PostResult::Closed(
          absl::UnavailableError("DPUmesh reactor no longer exists"));
    }

    absl::Status Flush() override {
      if (auto impl = impl_.lock()) {
        return impl->Flush(connection_);
      }
      return absl::UnavailableError("DPUmesh reactor no longer exists");
    }

    void Close() override {
      if (auto impl = impl_.lock()) impl->RequestClose(connection_);
    }

   private:
    std::weak_ptr<Impl> impl_;
    std::shared_ptr<Connection> connection_;
    const size_t post_max_;
  };

  Impl(DmeshApiOps* ops, dmesh_channel_t* channel, int post_max,
       Executor* callback_executor, Options options)
      : ops_(ops),
        channel_(channel),
        post_max_(post_max),
        callback_executor_(callback_executor),
        options_(options) {}

  ~Impl() { Shutdown(); }

  void SetWorkExecutor(Executor* work_executor) {
    work_executor_ = work_executor;
  }

  absl::Status Start() {
    if (ops_ == nullptr || channel_ == nullptr || callback_executor_ == nullptr ||
        work_executor_ == nullptr) {
      return absl::InvalidArgumentError(
          "DPUmesh reactor requires ops, channel and executors");
    }
    if (post_max_ <= 0 || options_.cq_batch_size == 0 ||
        options_.cq_batch_size > static_cast<size_t>(std::numeric_limits<int>::max()) ||
        options_.tx_retry_delay <= std::chrono::microseconds::zero()) {
      return absl::InvalidArgumentError("invalid DPUmesh reactor options");
    }

    cq_ = ops_->CreateCq(channel_);
    if (cq_ == nullptr) {
      return ErrnoStatus("dmesh_create_cq", errno);
    }
    cq_fd_ = ops_->CqFd(cq_);
    if (cq_fd_ < 0) {
      const absl::Status status = ErrnoStatus("dmesh_cq_fd", errno);
      ops_->DestroyCq(cq_);
      cq_ = nullptr;
      return status;
    }

    command_fd_ = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (command_fd_ < 0) {
      const absl::Status status = ErrnoStatus("eventfd", errno);
      ops_->DestroyCq(cq_);
      cq_ = nullptr;
      return status;
    }
    retry_fd_ = ::timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    if (retry_fd_ < 0) {
      const absl::Status status = ErrnoStatus("timerfd_create", errno);
      ::close(command_fd_);
      command_fd_ = -1;
      ops_->DestroyCq(cq_);
      cq_ = nullptr;
      return status;
    }

    accepting_.store(true, std::memory_order_release);
    try {
      owner_thread_ = std::thread([self = shared_from_this()] {
        self->ThreadMain();
      });
    } catch (const std::system_error& error) {
      accepting_.store(false, std::memory_order_release);
      ::close(retry_fd_);
      ::close(command_fd_);
      retry_fd_ = -1;
      command_fd_ = -1;
      ops_->DestroyCq(cq_);
      cq_ = nullptr;
      return absl::InternalError(
          absl::StrCat("failed to start DPUmesh reactor thread: ",
                       error.what()));
    }
    return absl::OkStatus();
  }

  bool Enqueue(absl::AnyInvocable<void()> task) {
    if (!accepting_.load(std::memory_order_acquire)) return false;
    {
      std::lock_guard<std::mutex> lock(command_mu_);
      if (!accepting_.load(std::memory_order_relaxed)) return false;
      commands_.push_back(std::move(task));
    }
    WakeCommandFd();
    return true;
  }

  void Connect(std::string service, ConnectCallback callback) {
    if (service.empty()) {
      DeliverConnect(
          std::move(callback),
          absl::InvalidArgumentError("DPUmesh service name is empty"));
      return;
    }
    if (!accepting_.load(std::memory_order_acquire)) {
      DeliverConnect(
          std::move(callback),
          absl::UnavailableError("DPUmesh reactor is shutting down"));
      return;
    }
    auto callback_holder =
        std::make_shared<ConnectCallback>(std::move(callback));
    if (!Enqueue([self = shared_from_this(), service = std::move(service),
                  callback_holder]() mutable {
          self->ConnectOwner(std::move(service),
                             std::move(*callback_holder));
        })) {
      DeliverConnect(
          std::move(*callback_holder),
          absl::UnavailableError("DPUmesh reactor is shutting down"));
    }
  }

  absl::Status SetAcceptCallback(AcceptCallback callback) {
    auto applied = std::make_shared<std::promise<void>>();
    auto ready = applied->get_future();
    if (!Enqueue([self = shared_from_this(), callback = std::move(callback),
                  applied = std::move(applied)]() mutable {
          self->accept_callback_ = std::move(callback);
          applied->set_value();
        })) {
      return absl::UnavailableError("DPUmesh reactor is shutting down");
    }
    ready.wait();
    return absl::OkStatus();
  }

  void AttachDriver(std::shared_ptr<Connection> connection,
                    std::weak_ptr<DmeshEndpointDriver> driver) {
    Enqueue([self = shared_from_this(), connection = std::move(connection),
             driver = std::move(driver)]() mutable {
      self->AttachDriverOwner(connection, std::move(driver));
    });
  }

  PostResult Post(const std::shared_ptr<Connection>& connection,
                  absl::Span<const uint8_t> bytes) {
    if (!accepting_.load(std::memory_order_acquire)) {
      return PostResult::Closed(
          absl::UnavailableError("DPUmesh reactor is shutting down"));
    }
    if (std::this_thread::get_id() != owner_id_) {
      return PostResult::Error(absl::InternalError(
          "DPUmesh Post executed outside the CQ owner thread"));
    }
    if (connection->closing || connection->qp == nullptr) {
      return PostResult::Closed(
          absl::UnavailableError("DPUmesh connection is closed"));
    }
    if (bytes.empty() || bytes.size() > static_cast<size_t>(post_max_) ||
        bytes.size() > std::numeric_limits<uint32_t>::max()) {
      return PostResult::Error(
          absl::InternalError("invalid DPUmesh post length"));
    }

    void* destination =
        ops_->Alloc(connection->qp, static_cast<uint32_t>(bytes.size()));
    if (destination == nullptr) {
      const int error_number = errno;
      if (error_number == EAGAIN) {
        /* A logical write may be larger than the bounded native batch window.
         * Publish its committed prefix so TX ACKs can make room for the rest. */
        if (connection->unflushed) {
          const absl::Status status = Flush(connection);
          if (!status.ok()) return PostResult::Error(status);
        }
        pending_tx_.insert(connection.get());
        return PostResult::WouldBlock();
      }
      return PostResult::Error(ErrnoStatus("dmesh_alloc", error_number));
    }

    std::memcpy(destination, bytes.data(), bytes.size());
    if (ops_->PostSend(connection->qp, destination,
                       static_cast<uint32_t>(bytes.size()), 0, 0) != 0) {
      return PostResult::Error(ErrnoStatus("dmesh_post_send", errno));
    }
    connection->unflushed = true;
    pending_tx_.erase(connection.get());
    return PostResult::Accepted();
  }

  absl::Status Flush(const std::shared_ptr<Connection>& connection) {
    if (std::this_thread::get_id() != owner_id_) {
      return absl::InternalError(
          "DPUmesh Flush executed outside the CQ owner thread");
    }
    if (connection->closing || connection->qp == nullptr) {
      return absl::UnavailableError("DPUmesh connection is closed");
    }
    if (!connection->unflushed) return absl::OkStatus();
    if (ops_->Flush(connection->qp) != 0)
      return ErrnoStatus("dmesh_flush", errno);
    connection->unflushed = false;
    return absl::OkStatus();
  }

  void RequestClose(const std::shared_ptr<Connection>& connection) {
    if (connection->close_enqueued.exchange(true, std::memory_order_acq_rel)) {
      return;
    }
    Enqueue([self = shared_from_this(), connection] {
      self->RequestCloseOwner(connection);
    });
  }

  void Shutdown() {
    std::lock_guard<std::mutex> shutdown_lock(shutdown_mu_);
    bool expected = true;
    if (!accepting_.compare_exchange_strong(
            expected, false, std::memory_order_acq_rel)) {
      if (owner_thread_.joinable() &&
          std::this_thread::get_id() != owner_thread_.get_id()) {
        owner_thread_.join();
      }
      return;
    }

    {
      std::lock_guard<std::mutex> lock(command_mu_);
      commands_.push_back([self = shared_from_this()] {
        self->stop_requested_ = true;
      });
    }
    WakeCommandFd();
    if (owner_thread_.joinable()) owner_thread_.join();

    if (retry_fd_ >= 0) {
      ::close(retry_fd_);
      retry_fd_ = -1;
    }
    if (command_fd_ >= 0) {
      ::close(command_fd_);
      command_fd_ = -1;
    }
    if (cq_ != nullptr) {
      ops_->DestroyCq(cq_);
      cq_ = nullptr;
    }
  }

 private:
  void WakeCommandFd() {
    if (command_fd_ < 0) return;
    const uint64_t one = 1;
    const ssize_t result = ::write(command_fd_, &one, sizeof(one));
    (void)result;
  }

  void DeliverConnect(ConnectCallback callback,
                      absl::StatusOr<ConnectedTransport> result) {
    callback_executor_->Run(
        [callback = std::move(callback), result = std::move(result)]() mutable {
          callback(std::move(result));
        });
  }

  void DeliverAccept(ConnectedTransport connected) {
    AcceptCallback callback = accept_callback_;
    callback_executor_->Run(
        [callback = std::move(callback),
         connected = std::move(connected)]() mutable {
          callback(std::move(connected));
        });
  }

  void ConnectOwner(std::string service, ConnectCallback callback) {
    if (!accepting_.load(std::memory_order_acquire) || stop_requested_) {
      DeliverConnect(
          std::move(callback),
          absl::UnavailableError("DPUmesh reactor is shutting down"));
      return;
    }

    errno = 0;
    dmesh_qp_t* qp = ops_->CreateQp(cq_, service.c_str());
    if (qp == nullptr) {
      DeliverConnect(std::move(callback),
                     ErrnoStatus("dmesh_create_qp", errno));
      return;
    }

    auto connection = std::make_shared<Connection>();
    connection->qp = qp;
    connections_.emplace(qp, connection);

    ConnectedTransport connected;
    connected.transport = std::make_unique<Transport>(
        weak_from_this(), connection, static_cast<size_t>(post_max_));
    connected.work_executor = work_executor_;
    DeliverConnect(std::move(callback), std::move(connected));
  }

  void AttachDriverOwner(const std::shared_ptr<Connection>& connection,
                         std::weak_ptr<DmeshEndpointDriver> driver) {
    connection->driver = std::move(driver);
    auto bound_driver = connection->driver.lock();
    if (bound_driver == nullptr) return;

    while (!connection->prebind_receives.empty()) {
      auto receive = std::move(connection->prebind_receives.front());
      connection->prebind_receives.pop_front();
      const absl::Status status = bound_driver->OnIncomingData(
          absl::MakeConstSpan(receive.bytes));
      if (!status.ok()) {
        if (!connection->closing) FailConnectionOwner(connection, status);
        return;
      }
    }

    if (connection->prebind_terminal == Connection::Terminal::kRemoteEof) {
      bound_driver->OnRemoteEof();
    } else if (connection->prebind_terminal == Connection::Terminal::kError) {
      bound_driver->OnTransportError(connection->prebind_error);
    } else if (connection->closing || connection->qp == nullptr) {
      bound_driver->OnTransportError(
          absl::UnavailableError("DPUmesh connection closed before binding"));
    }
    connection->prebind_terminal = Connection::Terminal::kNone;
    connection->prebind_error = absl::OkStatus();
  }

  void RequestCloseOwner(const std::shared_ptr<Connection>& connection) {
    connection->close_enqueued.store(true, std::memory_order_release);
    if (connection->closing || connection->qp == nullptr) return;
    connection->closing = true;
    deferred_closes_.push_back(connection);
  }

  void FailConnectionOwner(const std::shared_ptr<Connection>& connection,
                           absl::Status status) {
    if (connection->closing || connection->qp == nullptr) return;
    if (status.ok()) status = absl::UnknownError("DPUmesh connection failed");
    connection->close_enqueued.store(true, std::memory_order_release);
    connection->closing = true;
    pending_tx_.erase(connection.get());
    if (auto driver = connection->driver.lock()) {
      driver->OnTransportError(status);
    } else {
      connection->prebind_terminal = Connection::Terminal::kError;
      connection->prebind_error = std::move(status);
    }
    deferred_closes_.push_back(connection);
  }

  void HandleCompletion(dmesh_wc_t* completion) {
    auto found = connections_.find(completion->qp);
    if (found == connections_.end()) {
      if (completion->opcode == DMESH_WC_CONN_REQ &&
          completion->qp != nullptr && accept_callback_) {
        auto connection = std::make_shared<Connection>();
        connection->qp = completion->qp;
        connections_.emplace(completion->qp, connection);

        ConnectedTransport connected;
        connected.transport = std::make_unique<Transport>(
            weak_from_this(), connection, static_cast<size_t>(post_max_));
        connected.work_executor = work_executor_;
        DeliverAccept(std::move(connected));
        return;
      }
      if (completion->opcode == DMESH_WC_RECV) {
        ops_->Release(channel_, completion);
      }
      if (completion->opcode == DMESH_WC_CONN_REQ &&
          completion->qp != nullptr) {
        deferred_unowned_qps_.insert(completion->qp);
      }
      return;
    }

    const std::shared_ptr<Connection>& connection = found->second;
    if (completion->opcode == DMESH_WC_CONN_REQ) return;

    if (completion->opcode == DMESH_WC_RECV) {
      if (connection->closing || connection->remote_eof) {
        ops_->Release(channel_, completion);
        if (!connection->closing) {
          FailConnectionOwner(
              connection,
              absl::UnavailableError(
                  "DPUmesh delivered data after remote FIN"));
        }
        return;
      }

      if (completion->len == 0 || completion->buf == nullptr) {
        ops_->Release(channel_, completion);
        FailConnectionOwner(
            connection,
            absl::InternalError("invalid DPUmesh receive completion"));
        return;
      }

      if (!connection->stream.has_value()) {
        connection->stream = completion->stream;
      } else if (*connection->stream != completion->stream) {
        ops_->Release(channel_, completion);
        FailConnectionOwner(
            connection,
            absl::UnavailableError(absl::StrCat(
                "DPUmesh passthru stream changed from ", *connection->stream,
                " to ", completion->stream)));
        return;
      }

      if (auto driver = connection->driver.lock()) {
        const absl::Status status = driver->OnIncomingData(
            absl::MakeConstSpan(completion->buf, completion->len));
        ops_->Release(channel_, completion);
        if (!status.ok()) FailConnectionOwner(connection, status);
      } else {
        Connection::QueuedReceive queued;
        try {
          queued.bytes.assign(completion->buf,
                              completion->buf + completion->len);
          connection->prebind_receives.push_back(std::move(queued));
        } catch (const std::bad_alloc&) {
          ops_->Release(channel_, completion);
          FailConnectionOwner(
              connection,
              absl::ResourceExhaustedError(
                  "failed to buffer DPUmesh receive before endpoint binding"));
          return;
        }
        ops_->Release(channel_, completion);
      }
      return;
    }

    if (completion->opcode == DMESH_WC_RECV_FIN) {
      if (connection->closing || connection->remote_eof) return;
      connection->remote_eof = true;
      if (auto driver = connection->driver.lock()) {
        driver->OnRemoteEof();
      } else {
        connection->prebind_terminal = Connection::Terminal::kRemoteEof;
      }
      return;
    }

    FailConnectionOwner(
        connection, absl::InternalError("unknown DPUmesh completion opcode"));
  }

  bool DrainCq() {
    bool did_work = false;
    std::vector<dmesh_wc_t> completions(options_.cq_batch_size);
    for (;;) {
      errno = 0;
      const int count = ops_->PollCq(
          cq_, completions.data(), static_cast<int>(completions.size()));
      if (count == 0) break;
      if (count < 0) {
        const absl::Status status = ErrnoStatus("dmesh_poll_cq", errno);
        std::vector<std::shared_ptr<Connection>> connections;
        connections.reserve(connections_.size());
        for (const auto& entry : connections_) connections.push_back(entry.second);
        for (const auto& connection : connections) {
          FailConnectionOwner(connection, status);
        }
        break;
      }
      if (count > static_cast<int>(completions.size())) {
        const absl::Status status =
            absl::InternalError("dmesh_poll_cq exceeded completion capacity");
        std::vector<std::shared_ptr<Connection>> connections;
        connections.reserve(connections_.size());
        for (const auto& entry : connections_) connections.push_back(entry.second);
        for (const auto& connection : connections) {
          FailConnectionOwner(connection, status);
        }
        break;
      }
      did_work = true;
      for (int i = 0; i < count; ++i) HandleCompletion(&completions[i]);
    }
    return did_work;
  }

  void RetryPendingWrites() {
    if (pending_tx_.empty()) return;
    std::vector<Connection*> pending(pending_tx_.begin(), pending_tx_.end());
    pending_tx_.clear();
    for (Connection* raw : pending) {
      auto found = connections_.find(raw->qp);
      if (found == connections_.end() || found->second.get() != raw ||
          raw->closing || raw->qp == nullptr) {
        continue;
      }
      if (auto driver = raw->driver.lock()) {
        driver->OnWritable();
      } else {
        pending_tx_.insert(raw);
      }
    }
  }

  void SweepDeferredCloses() {
    for (const auto& connection : deferred_closes_) {
      dmesh_qp_t* qp = connection->qp;
      if (qp == nullptr) continue;
      pending_tx_.erase(connection.get());
      connection->qp = nullptr;
      connection->driver.reset();
      connections_.erase(qp);
      ops_->DestroyQp(qp);
    }
    deferred_closes_.clear();

    for (dmesh_qp_t* qp : deferred_unowned_qps_) ops_->DestroyQp(qp);
    deferred_unowned_qps_.clear();
  }

  void ArmRetryTimer() {
    if (pending_tx_.empty() && !retry_timer_armed_) return;
    if (!pending_tx_.empty() && retry_timer_armed_) return;
    itimerspec timer{};
    if (!pending_tx_.empty()) timer.it_value = ToTimespec(options_.tx_retry_delay);
    if (timer.it_value.tv_sec == 0 && timer.it_value.tv_nsec == 0 &&
        !pending_tx_.empty()) {
      timer.it_value.tv_nsec = 1;
    }
    if (::timerfd_settime(retry_fd_, 0, &timer, nullptr) != 0) {
      const absl::Status status = ErrnoStatus("timerfd_settime", errno);
      std::vector<std::shared_ptr<Connection>> connections;
      for (Connection* raw : pending_tx_) {
        auto found = connections_.find(raw->qp);
        if (found != connections_.end() && found->second.get() == raw) {
          connections.push_back(found->second);
        }
      }
      pending_tx_.clear();
      for (const auto& connection : connections) {
        FailConnectionOwner(connection, status);
      }
      SweepDeferredCloses();
      retry_timer_armed_ = false;
      return;
    }
    retry_timer_armed_ = !pending_tx_.empty();
  }

  void DrainCommands() {
    for (;;) {
      absl::AnyInvocable<void()> task;
      {
        std::lock_guard<std::mutex> lock(command_mu_);
        if (commands_.empty()) return;
        task = std::move(commands_.front());
        commands_.pop_front();
      }
      task();
    }
  }

  void ShutdownOwner() {
    const absl::Status status =
        absl::CancelledError("DPUmesh reactor shutdown");
    std::vector<std::shared_ptr<Connection>> connections;
    connections.reserve(connections_.size());
    for (const auto& entry : connections_) connections.push_back(entry.second);
    for (const auto& connection : connections) {
      if (connection->closing) continue;
      connection->close_enqueued.store(true, std::memory_order_release);
      connection->closing = true;
      if (auto driver = connection->driver.lock()) {
        driver->OnTransportError(status);
      } else {
        connection->prebind_terminal = Connection::Terminal::kError;
        connection->prebind_error = status;
      }
      deferred_closes_.push_back(connection);
    }
    SweepDeferredCloses();
  }

  void ThreadMain() {
    owner_id_ = std::this_thread::get_id();
    pollfd descriptors[3] = {
        pollfd{command_fd_, POLLIN, 0},
        pollfd{cq_fd_, POLLIN, 0},
        pollfd{retry_fd_, POLLIN, 0},
    };

    while (!stop_requested_) {
      const int result = ::poll(descriptors, 3, -1);
      if (result < 0) {
        if (errno == EINTR) continue;
        stop_requested_ = true;
        break;
      }

      const bool command_ready = (descriptors[0].revents & POLLIN) != 0;
      const bool cq_ready = (descriptors[1].revents & POLLIN) != 0;
      const bool retry_ready = (descriptors[2].revents & POLLIN) != 0;
      if (command_ready) DrainCounterFd(command_fd_);
      if (cq_ready) DrainCounterFd(cq_fd_);
      if (retry_ready) DrainCounterFd(retry_fd_);
      if (retry_ready) retry_timer_armed_ = false;

      DrainCommands();
      if (stop_requested_) break;

      const bool cq_work = DrainCq();
      if (retry_ready || cq_work) RetryPendingWrites();
      SweepDeferredCloses();
      ArmRetryTimer();
    }

    ShutdownOwner();
    owner_id_ = std::thread::id();
  }

  DmeshApiOps* const ops_;
  dmesh_channel_t* const channel_;
  const int post_max_;
  Executor* const callback_executor_;
  const Options options_;
  Executor* work_executor_ = nullptr;

  dmesh_cq_t* cq_ = nullptr;
  int cq_fd_ = -1;
  int command_fd_ = -1;
  int retry_fd_ = -1;
  std::thread owner_thread_;
  std::thread::id owner_id_;
  std::atomic<bool> accepting_{false};
  bool stop_requested_ = false;
  bool retry_timer_armed_ = false;

  std::mutex shutdown_mu_;
  std::mutex command_mu_;
  std::deque<absl::AnyInvocable<void()>> commands_;

  std::unordered_map<dmesh_qp_t*, std::shared_ptr<Connection>> connections_;
  std::unordered_set<Connection*> pending_tx_;
  std::vector<std::shared_ptr<Connection>> deferred_closes_;
  std::unordered_set<dmesh_qp_t*> deferred_unowned_qps_;
  AcceptCallback accept_callback_;
};

absl::StatusOr<std::unique_ptr<DmeshReactor>> DmeshReactor::Create(
    DmeshApiOps* ops, dmesh_channel_t* channel, int post_max,
    Executor* callback_executor) {
  return Create(ops, channel, post_max, callback_executor, Options());
}

absl::StatusOr<std::unique_ptr<DmeshReactor>> DmeshReactor::Create(
    DmeshApiOps* ops, dmesh_channel_t* channel, int post_max,
    Executor* callback_executor, Options options) {
  auto impl = std::make_shared<Impl>(ops, channel, post_max,
                                     callback_executor, options);
  auto reactor = std::unique_ptr<DmeshReactor>(new DmeshReactor(impl));
  impl->SetWorkExecutor(reactor.get());
  const absl::Status status = impl->Start();
  if (!status.ok()) return status;
  return reactor;
}

DmeshReactor::DmeshReactor(std::shared_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

DmeshReactor::~DmeshReactor() { Shutdown(); }

void DmeshReactor::Run(absl::AnyInvocable<void()> task) {
  impl_->Enqueue(std::move(task));
}

void DmeshReactor::Connect(std::string service, ConnectCallback callback) {
  impl_->Connect(std::move(service), std::move(callback));
}

absl::Status DmeshReactor::SetAcceptCallback(AcceptCallback callback) {
  return impl_->SetAcceptCallback(std::move(callback));
}

void DmeshReactor::Shutdown() {
  if (impl_ != nullptr) impl_->Shutdown();
}

}  // namespace dpumesh::grpc
