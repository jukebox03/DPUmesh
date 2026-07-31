#include "dmesh_endpoint.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <optional>
#include <utility>

#include <grpc/event_engine/memory_request.h>
#include <grpc/event_engine/slice.h>
#include <grpc/event_engine/slice_buffer.h>
#include <grpc/slice.h>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"

namespace dpumesh::grpc {
namespace {

using Callback = absl::AnyInvocable<void(absl::Status)>;
using EventEngine = grpc_event_engine::experimental::EventEngine;
using MemoryRequest = grpc_event_engine::experimental::MemoryRequest;
using Slice = grpc_event_engine::experimental::Slice;
using SliceBuffer = grpc_event_engine::experimental::SliceBuffer;

// Posts one pump run may issue before yielding its thread and rescheduling
// itself for the remainder.
constexpr size_t kReservationBudget = 16;

struct Completion {
  Callback callback;
  absl::Status status;
};

void ScheduleCompletion(const std::shared_ptr<Executor>& executor,
                        Completion completion) {
  executor->RunCompletion(std::move(completion.callback),
                          std::move(completion.status));
}

}  // namespace

class DmeshEndpointState final
    : public std::enable_shared_from_this<DmeshEndpointState> {
 public:
  struct PendingRead {
    Callback callback;
    SliceBuffer* buffer;
  };

  struct PendingWrite {
    Callback callback;
    SliceBuffer* data;
    size_t slice_index = 0;
    size_t slice_offset = 0;
  };

  enum class Life {
    kOpen,
    kRemoteEof,
    kFailed,
    kClosing,
  };

  DmeshEndpointState(std::unique_ptr<EndpointTransport> transport,
                     std::shared_ptr<Executor> work_executor,
                     std::shared_ptr<Executor> callback_executor,
                     grpc_event_engine::experimental::MemoryAllocator allocator)
      : transport(std::move(transport)),
        work_executor(std::move(work_executor)),
        callback_executor(std::move(callback_executor)),
        allocator(std::move(allocator)) {}

  // allocator is declared before receive_queue so queued slices are destroyed
  // first (members are destroyed in reverse declaration order).
  std::mutex mu;
  std::unique_ptr<EndpointTransport> transport;
  const std::shared_ptr<Executor> work_executor;
  const std::shared_ptr<Executor> callback_executor;
  grpc_event_engine::experimental::MemoryAllocator allocator;
  std::deque<Slice> receive_queue;
  size_t queued_bytes = 0;
  std::optional<PendingRead> pending_read;
  std::optional<PendingWrite> pending_write;
  Life life = Life::kOpen;
  absl::Status failure = absl::OkStatus();
  bool write_pump_scheduled = false;
  // True while the pending write waits for DMESH_EVENT_TX_READY. A peer FIN
  // must fail such a write: the capacity it waits for was backed by the
  // departed peer's credits, so the event can never fire.
  bool write_parked = false;
  bool receive_paused = false;
  bool transport_closed = false;
};

namespace {

void ScheduleWritePump(const std::shared_ptr<DmeshEndpointState>& state);

void CloseTransport(const std::shared_ptr<DmeshEndpointState>& state) {
  bool close = false;
  {
    std::lock_guard<std::mutex> lock(state->mu);
    if (!state->transport_closed) {
      state->transport_closed = true;
      close = true;
    }
  }
  if (close) state->transport->Close();
}

std::optional<Completion> TakeReadFailureLocked(
    DmeshEndpointState* state, const absl::Status& status) {
  if (!state->pending_read.has_value()) return std::nullopt;
  Completion completion{std::move(state->pending_read->callback), status};
  state->pending_read.reset();
  return completion;
}

std::optional<Completion> TakeWriteFailureLocked(
    DmeshEndpointState* state, const absl::Status& status) {
  if (!state->pending_write.has_value()) return std::nullopt;
  state->pending_write->data->Clear();
  Completion completion{std::move(state->pending_write->callback), status};
  state->pending_write.reset();
  state->write_pump_scheduled = false;
  state->write_parked = false;
  return completion;
}

void ScheduleIfPresent(const std::shared_ptr<Executor>& executor,
                       std::optional<Completion> completion) {
  if (completion.has_value()) {
    ScheduleCompletion(executor, std::move(*completion));
  }
}

// Advance the cursor past slices the pump has already consumed.
void SkipConsumedSlices(DmeshEndpointState::PendingWrite* write) {
  while (write->slice_index < write->data->Count() &&
         write->slice_offset == (*write->data)[write->slice_index].size()) {
    ++write->slice_index;
    write->slice_offset = 0;
  }
}

// Bytes one post carries: everything left in the logical Write, spanning
// consecutive slices, capped by the largest post the transport accepts.
size_t ReservationLength(const DmeshEndpointState::PendingWrite& write,
                         size_t max_post_size) {
  size_t length = 0;
  size_t offset = write.slice_offset;
  for (size_t index = write.slice_index;
       index < write.data->Count() && length < max_post_size; ++index) {
    length += (*write.data)[index].size() - offset;
    offset = 0;
  }
  return std::min(length, max_post_size);
}

// Copy `length` bytes from the cursor into the reservation and advance the
// cursor past exactly those bytes.
void FillReservation(DmeshEndpointState::PendingWrite* write,
                     const Reservation& reservation) {
  size_t filled = 0;
  while (filled < reservation.length) {
    const Slice& slice = (*write->data)[write->slice_index];
    const size_t take =
        std::min(reservation.length - filled, slice.size() - write->slice_offset);
    std::memcpy(reservation.data + filled, slice.data() + write->slice_offset,
                take);
    filled += take;
    write->slice_offset += take;
    if (write->slice_offset == slice.size()) {
      ++write->slice_index;
      write->slice_offset = 0;
    }
  }
}

void PumpWrite(const std::shared_ptr<DmeshEndpointState>& state) {
  std::optional<Completion> write_completion;
  std::optional<Completion> read_completion;
  bool close_transport = false;
  bool reschedule = false;

  {
    std::lock_guard<std::mutex> lock(state->mu);
    state->write_pump_scheduled = false;
    if (!state->pending_write.has_value()) return;

    if (state->life == DmeshEndpointState::Life::kFailed ||
        state->life == DmeshEndpointState::Life::kClosing) {
      const absl::Status status =
          state->life == DmeshEndpointState::Life::kClosing
              ? absl::CancelledError("DPUmesh endpoint is closing")
              : state->failure;
      write_completion = TakeWriteFailureLocked(state.get(), status);
    } else {
      auto& write = *state->pending_write;
      const size_t max_post_size = state->transport->MaxPostSize();
      if (max_post_size == 0) {
        state->life = DmeshEndpointState::Life::kFailed;
        state->failure =
            absl::InternalError("DPUmesh transport reported max post size 0");
        write_completion =
            TakeWriteFailureLocked(state.get(), state->failure);
        read_completion = TakeReadFailureLocked(state.get(), state->failure);
        close_transport = true;
      } else {
        size_t reservations = 0;
        for (;;) {
          SkipConsumedSlices(&write);
          if (write.slice_index == write.data->Count()) break;
          if (reservations == kReservationBudget) {
            state->write_pump_scheduled = true;
            reschedule = true;
            break;
          }

          PostResult result = state->transport->Post(
              ReservationLength(write, max_post_size),
              [&write](Reservation reservation) {
                FillReservation(&write, reservation);
              });

          if (result.code == PostCode::kAccepted) {
            ++reservations;
            continue;
          } else if (result.code == PostCode::kWouldBlock) {
            state->write_parked = true;
            return;
          } else if (result.status.ok()) {
            result.status = result.code == PostCode::kClosed
                                ? absl::UnavailableError(
                                      "DPUmesh transport is closed")
                                : absl::InternalError(
                                      "DPUmesh transport post failed");
          }
          state->life = DmeshEndpointState::Life::kFailed;
          state->failure = std::move(result.status);
          write_completion =
              TakeWriteFailureLocked(state.get(), state->failure);
          read_completion = TakeReadFailureLocked(state.get(), state->failure);
          close_transport = true;
          break;
        }

        if (!write_completion.has_value() && !reschedule &&
            state->pending_write.has_value() &&
            write.slice_index == write.data->Count()) {
          absl::Status flush_status = state->transport->Flush();
          if (flush_status.ok()) {
            write.data->Clear();
            write_completion = Completion{std::move(write.callback),
                                          absl::OkStatus()};
            state->pending_write.reset();
          } else {
            state->life = DmeshEndpointState::Life::kFailed;
            state->failure = std::move(flush_status);
            write_completion =
                TakeWriteFailureLocked(state.get(), state->failure);
            read_completion =
                TakeReadFailureLocked(state.get(), state->failure);
            close_transport = true;
          }
        }
      }
    }
  }

  if (close_transport) CloseTransport(state);
  ScheduleIfPresent(state->callback_executor, std::move(read_completion));
  ScheduleIfPresent(state->callback_executor, std::move(write_completion));
  if (reschedule) ScheduleWritePump(state);
}

void ScheduleWritePump(const std::shared_ptr<DmeshEndpointState>& state) {
  state->work_executor->Run([state]() { PumpWrite(state); });
}

}  // namespace

DmeshEndpointDriver::DmeshEndpointDriver(
    std::shared_ptr<DmeshEndpointState> state)
    : state_(std::move(state)) {}

ReceiveOutcome DmeshEndpointDriver::OnIncomingData(
    absl::Span<const uint8_t> bytes) {
  return OnIncomingData(bytes.size(), [bytes](uint8_t* destination) {
    std::memcpy(destination, bytes.data(), bytes.size());
  });
}

ReceiveOutcome DmeshEndpointDriver::OnIncomingData(
    size_t length, absl::FunctionRef<void(uint8_t*)> fill) {
  if (length == 0) return ReceiveOutcome{absl::OkStatus(), false};

  std::optional<Completion> completion;
  std::optional<Completion> write_completion;
  ReceiveOutcome outcome{absl::OkStatus(), false};
  bool close_transport = false;

  {
    std::lock_guard<std::mutex> lock(state_->mu);
    if (state_->life != DmeshEndpointState::Life::kOpen) {
      return ReceiveOutcome{
          absl::FailedPreconditionError(
              "received DPUmesh data after endpoint input closed"),
          false};
    }

    grpc_slice raw = state_->allocator.MakeSlice(MemoryRequest(length));
    const size_t allocated_length = GRPC_SLICE_LENGTH(raw);
    if (allocated_length != length) {
      grpc_slice_unref(raw);
      state_->life = DmeshEndpointState::Life::kFailed;
      state_->failure = absl::ResourceExhaustedError(absl::StrCat(
          "gRPC allocator returned ", allocated_length,
          " bytes for a ", length, " byte DPUmesh receive"));
      completion = TakeReadFailureLocked(state_.get(), state_->failure);
      write_completion =
          TakeWriteFailureLocked(state_.get(), state_->failure);
      outcome.status = state_->failure;
      close_transport = true;
    } else {
      fill(GRPC_SLICE_START_PTR(raw));
      Slice slice(raw);
      if (state_->pending_read.has_value()) {
        state_->pending_read->buffer->Append(std::move(slice));
        completion = Completion{std::move(state_->pending_read->callback),
                                absl::OkStatus()};
        state_->pending_read.reset();
      } else {
        state_->queued_bytes += slice.size();
        state_->receive_queue.push_back(std::move(slice));
        if (state_->queued_bytes > kReceiveHighWaterBytes) {
          state_->receive_paused = true;
          outcome.hold_credit = true;
        }
      }
    }
  }

  if (close_transport) CloseTransport(state_);
  ScheduleIfPresent(state_->callback_executor, std::move(completion));
  ScheduleIfPresent(state_->callback_executor, std::move(write_completion));
  return outcome;
}

void DmeshEndpointDriver::OnWritable() {
  bool schedule = false;
  {
    std::lock_guard<std::mutex> lock(state_->mu);
    if (state_->pending_write.has_value() &&
        !state_->write_pump_scheduled &&
        state_->life != DmeshEndpointState::Life::kFailed &&
        state_->life != DmeshEndpointState::Life::kClosing) {
      state_->write_pump_scheduled = true;
      state_->write_parked = false;
      schedule = true;
    }
  }
  if (schedule) ScheduleWritePump(state_);
}

void DmeshEndpointDriver::OnRemoteEof() {
  std::optional<Completion> read_completion;
  std::optional<Completion> write_completion;
  bool close_transport = false;
  {
    std::lock_guard<std::mutex> lock(state_->mu);
    if (state_->life != DmeshEndpointState::Life::kOpen) return;
    read_completion = TakeReadFailureLocked(
        state_.get(), absl::UnavailableError("DPUmesh peer closed"));
    /* A parked write waits for capacity backed by the departed peer's credits,
     * which no TX_READY will resume, so it is failed here. */
    if (state_->write_parked && state_->pending_write.has_value()) {
      state_->life = DmeshEndpointState::Life::kFailed;
      state_->failure = absl::UnavailableError(
          "DPUmesh peer closed while transmit was blocked");
      write_completion = TakeWriteFailureLocked(state_.get(), state_->failure);
      close_transport = true;
    } else {
      state_->life = DmeshEndpointState::Life::kRemoteEof;
    }
  }
  if (close_transport) CloseTransport(state_);
  ScheduleIfPresent(state_->callback_executor, std::move(read_completion));
  ScheduleIfPresent(state_->callback_executor, std::move(write_completion));
}

void DmeshEndpointDriver::OnTransportError(absl::Status status) {
  if (status.ok()) {
    status = absl::UnknownError("DPUmesh transport failed without a status");
  }

  std::optional<Completion> read_completion;
  std::optional<Completion> write_completion;
  {
    std::lock_guard<std::mutex> lock(state_->mu);
    if (state_->life == DmeshEndpointState::Life::kFailed ||
        state_->life == DmeshEndpointState::Life::kClosing) {
      return;
    }
    state_->life = DmeshEndpointState::Life::kFailed;
    state_->failure = std::move(status);
    read_completion = TakeReadFailureLocked(state_.get(), state_->failure);
    write_completion = TakeWriteFailureLocked(state_.get(), state_->failure);
  }
  CloseTransport(state_);
  ScheduleIfPresent(state_->callback_executor, std::move(read_completion));
  ScheduleIfPresent(state_->callback_executor, std::move(write_completion));
}

DmeshEndpoint::DmeshEndpoint(
    std::unique_ptr<EndpointTransport> transport,
    std::shared_ptr<Executor> work_executor,
    std::shared_ptr<Executor> callback_executor, MemoryAllocator allocator,
    EventEngine::ResolvedAddress peer_address,
    EventEngine::ResolvedAddress local_address)
    : state_(std::make_shared<DmeshEndpointState>(
          std::move(transport), std::move(work_executor),
          std::move(callback_executor), std::move(allocator))),
      driver_(std::make_shared<DmeshEndpointDriver>(state_)),
      peer_address_(peer_address),
      local_address_(local_address) {
  if (state_->transport == nullptr || state_->work_executor == nullptr ||
      state_->callback_executor == nullptr || !state_->allocator.IsValid()) {
    std::abort();
  }
  state_->transport->BindDriver(driver_);
}

DmeshEndpoint::~DmeshEndpoint() {
  std::optional<Completion> read_completion;
  std::optional<Completion> write_completion;
  {
    std::lock_guard<std::mutex> lock(state_->mu);
    if (state_->life != DmeshEndpointState::Life::kClosing) {
      state_->life = DmeshEndpointState::Life::kClosing;
      const absl::Status status =
          absl::CancelledError("DPUmesh endpoint destroyed");
      read_completion = TakeReadFailureLocked(state_.get(), status);
      write_completion = TakeWriteFailureLocked(state_.get(), status);
    }
  }
  CloseTransport(state_);
  ScheduleIfPresent(state_->callback_executor, std::move(read_completion));
  ScheduleIfPresent(state_->callback_executor, std::move(write_completion));
}

bool DmeshEndpoint::Read(Callback on_read, SliceBuffer* buffer,
                         ReadArgs /*args*/) {
  std::optional<Completion> completion;
  bool resume_receive = false;
  bool queue_consumed = false;
  {
    std::lock_guard<std::mutex> lock(state_->mu);
    if (state_->pending_read.has_value()) std::abort();

    if (!state_->receive_queue.empty()) {
      while (!state_->receive_queue.empty()) {
        buffer->Append(std::move(state_->receive_queue.front()));
        state_->receive_queue.pop_front();
      }
      state_->queued_bytes = 0;
      resume_receive = std::exchange(state_->receive_paused, false);
      queue_consumed = true;
    } else if (state_->life == DmeshEndpointState::Life::kOpen) {
      state_->pending_read.emplace(
          DmeshEndpointState::PendingRead{std::move(on_read), buffer});
    } else {
      absl::Status status;
      switch (state_->life) {
        case DmeshEndpointState::Life::kRemoteEof:
          status = absl::UnavailableError("DPUmesh peer closed");
          break;
        case DmeshEndpointState::Life::kFailed:
          status = state_->failure;
          break;
        case DmeshEndpointState::Life::kClosing:
          status = absl::CancelledError("DPUmesh endpoint is closing");
          break;
        case DmeshEndpointState::Life::kOpen:
          std::abort();
      }
      completion = Completion{std::move(on_read), std::move(status)};
    }
  }
  if (resume_receive) state_->transport->ResumeReceive();
  if (queue_consumed) return true;
  ScheduleIfPresent(state_->callback_executor, std::move(completion));
  return false;
}

bool DmeshEndpoint::Write(Callback on_writable, SliceBuffer* data,
                          WriteArgs /*args*/) {
  if (data->Length() == 0) return true;

  bool schedule = false;
  std::optional<Completion> immediate_failure;
  {
    std::lock_guard<std::mutex> lock(state_->mu);
    if (state_->pending_write.has_value()) std::abort();

    if (state_->life == DmeshEndpointState::Life::kFailed ||
        state_->life == DmeshEndpointState::Life::kClosing) {
      const absl::Status status =
          state_->life == DmeshEndpointState::Life::kFailed
              ? state_->failure
              : absl::CancelledError("DPUmesh endpoint is closing");
      data->Clear();
      immediate_failure =
          Completion{std::move(on_writable), std::move(status)};
    } else {
      state_->pending_write.emplace(DmeshEndpointState::PendingWrite{
          std::move(on_writable), data, 0, 0});
      state_->write_pump_scheduled = true;
      schedule = true;
    }
  }

  if (immediate_failure.has_value()) {
    ScheduleCompletion(state_->callback_executor,
                       std::move(*immediate_failure));
  } else if (schedule) {
    ScheduleWritePump(state_);
  }
  return false;
}

const EventEngine::ResolvedAddress& DmeshEndpoint::GetPeerAddress() const {
  return peer_address_;
}

const EventEngine::ResolvedAddress& DmeshEndpoint::GetLocalAddress() const {
  return local_address_;
}

std::shared_ptr<EventEngine::Endpoint::TelemetryInfo>
DmeshEndpoint::GetTelemetryInfo() const {
  return nullptr;
}

}  // namespace dpumesh::grpc
