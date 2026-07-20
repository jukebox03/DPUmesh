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

struct Completion {
  Callback callback;
  absl::Status status;
};

void ScheduleCompletion(Executor* executor, Completion completion) {
  executor->Run(
      [callback = std::move(completion.callback),
       status = std::move(completion.status)]() mutable {
        callback(std::move(status));
      });
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
                     Executor* work_executor, Executor* callback_executor,
                     grpc_event_engine::experimental::MemoryAllocator allocator)
      : transport(std::move(transport)),
        work_executor(work_executor),
        callback_executor(callback_executor),
        allocator(std::move(allocator)) {}

  // allocator is declared before receive_queue so queued slices are destroyed
  // first (members are destroyed in reverse declaration order).
  std::mutex mu;
  std::unique_ptr<EndpointTransport> transport;
  Executor* const work_executor;
  Executor* const callback_executor;
  grpc_event_engine::experimental::MemoryAllocator allocator;
  std::deque<Slice> receive_queue;
  std::optional<PendingRead> pending_read;
  std::optional<PendingWrite> pending_write;
  Life life = Life::kOpen;
  absl::Status failure = absl::OkStatus();
  bool write_pump_scheduled = false;
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
  return completion;
}

void ScheduleIfPresent(Executor* executor,
                       std::optional<Completion> completion) {
  if (completion.has_value()) {
    ScheduleCompletion(executor, std::move(*completion));
  }
}

void PumpWrite(const std::shared_ptr<DmeshEndpointState>& state) {
  std::optional<Completion> write_completion;
  std::optional<Completion> read_completion;
  bool close_transport = false;

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
        while (write.slice_index < write.data->Count()) {
          const Slice& slice = (*write.data)[write.slice_index];
          if (write.slice_offset == slice.size()) {
            ++write.slice_index;
            write.slice_offset = 0;
            continue;
          }

          const size_t length =
              std::min(max_post_size, slice.size() - write.slice_offset);
          const auto bytes = absl::MakeConstSpan(
              slice.data() + write.slice_offset, length);
          PostResult result = state->transport->Post(bytes);

          if (result.code == PostCode::kAccepted) {
            write.slice_offset += length;
            continue;
          }
          if (result.code == PostCode::kWouldBlock) {
            return;
          }

          if (result.status.ok()) {
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

        if (!write_completion.has_value() &&
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
}

void ScheduleWritePump(const std::shared_ptr<DmeshEndpointState>& state) {
  state->work_executor->Run([state]() { PumpWrite(state); });
}

}  // namespace

DmeshEndpointDriver::DmeshEndpointDriver(
    std::shared_ptr<DmeshEndpointState> state)
    : state_(std::move(state)) {}

absl::Status DmeshEndpointDriver::OnIncomingData(
    absl::Span<const uint8_t> bytes) {
  if (bytes.empty()) return absl::OkStatus();

  std::optional<Completion> completion;
  std::optional<Completion> write_completion;
  absl::Status result = absl::OkStatus();
  bool close_transport = false;

  {
    std::lock_guard<std::mutex> lock(state_->mu);
    if (state_->life != DmeshEndpointState::Life::kOpen) {
      return absl::FailedPreconditionError(
          "received DPUmesh data after endpoint input closed");
    }

    grpc_slice raw = state_->allocator.MakeSlice(MemoryRequest(bytes.size()));
    const size_t allocated_length = GRPC_SLICE_LENGTH(raw);
    if (allocated_length != bytes.size()) {
      grpc_slice_unref(raw);
      state_->life = DmeshEndpointState::Life::kFailed;
      state_->failure = absl::ResourceExhaustedError(absl::StrCat(
          "gRPC allocator returned ", allocated_length,
          " bytes for a ", bytes.size(), " byte DPUmesh receive"));
      completion = TakeReadFailureLocked(state_.get(), state_->failure);
      write_completion =
          TakeWriteFailureLocked(state_.get(), state_->failure);
      result = state_->failure;
      close_transport = true;
    } else {
      std::memcpy(GRPC_SLICE_START_PTR(raw), bytes.data(), bytes.size());
      Slice slice(raw);
      if (state_->pending_read.has_value()) {
        state_->pending_read->buffer->Append(std::move(slice));
        completion = Completion{std::move(state_->pending_read->callback),
                                absl::OkStatus()};
        state_->pending_read.reset();
      } else {
        state_->receive_queue.push_back(std::move(slice));
      }
    }
  }

  if (close_transport) CloseTransport(state_);
  ScheduleIfPresent(state_->callback_executor, std::move(completion));
  ScheduleIfPresent(state_->callback_executor, std::move(write_completion));
  return result;
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
      schedule = true;
    }
  }
  if (schedule) ScheduleWritePump(state_);
}

void DmeshEndpointDriver::OnRemoteEof() {
  std::optional<Completion> completion;
  {
    std::lock_guard<std::mutex> lock(state_->mu);
    if (state_->life != DmeshEndpointState::Life::kOpen) return;
    state_->life = DmeshEndpointState::Life::kRemoteEof;
    completion = TakeReadFailureLocked(
        state_.get(), absl::UnavailableError("DPUmesh peer closed"));
  }
  ScheduleIfPresent(state_->callback_executor, std::move(completion));
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
    std::unique_ptr<EndpointTransport> transport, Executor* work_executor,
    Executor* callback_executor, MemoryAllocator allocator,
    EventEngine::ResolvedAddress peer_address,
    EventEngine::ResolvedAddress local_address)
    : state_(std::make_shared<DmeshEndpointState>(
          std::move(transport), work_executor, callback_executor,
          std::move(allocator))),
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
  {
    std::lock_guard<std::mutex> lock(state_->mu);
    if (state_->pending_read.has_value()) std::abort();

    if (!state_->receive_queue.empty()) {
      while (!state_->receive_queue.empty()) {
        buffer->Append(std::move(state_->receive_queue.front()));
        state_->receive_queue.pop_front();
      }
      return true;
    }

    if (state_->life == DmeshEndpointState::Life::kOpen) {
      state_->pending_read.emplace(
          DmeshEndpointState::PendingRead{std::move(on_read), buffer});
      return false;
    }

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
  ScheduleCompletion(state_->callback_executor, std::move(*completion));
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
