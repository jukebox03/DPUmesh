#include "dmesh_runtime.h"

#include <errno.h>

#include <memory>
#include <utility>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "thread_executor.h"

namespace dpumesh::grpc {

DmeshRuntime::DmeshRuntime(
    std::unique_ptr<DmeshApiOps> ops, dmesh_channel_t* channel, int post_max,
    std::vector<std::unique_ptr<Executor>> owned_callback_executors,
    Executor* callback_executor)
    : ops_(std::move(ops)),
      channel_(channel),
      post_max_(post_max),
      owned_callback_executors_(std::move(owned_callback_executors)),
      callback_executor_(callback_executor) {}

absl::StatusOr<std::unique_ptr<DmeshRuntime>> DmeshRuntime::Create(
    std::unique_ptr<DmeshApiOps> ops) {
  return Create(std::move(ops), nullptr, Options());
}

absl::StatusOr<std::unique_ptr<DmeshRuntime>> DmeshRuntime::Create(
    std::unique_ptr<DmeshApiOps> ops, Options options) {
  return Create(std::move(ops), nullptr, options);
}

absl::StatusOr<std::unique_ptr<DmeshRuntime>> DmeshRuntime::Create(
    std::unique_ptr<DmeshApiOps> ops, Executor* callback_executor) {
  return Create(std::move(ops), callback_executor, Options());
}

absl::StatusOr<std::unique_ptr<DmeshRuntime>> DmeshRuntime::Create(
    std::unique_ptr<DmeshApiOps> ops, Executor* callback_executor,
    Options options) {
  if (ops == nullptr) {
    return absl::InvalidArgumentError("DPUmesh runtime requires native ops");
  }
  if (options.reactor_count == 0) {
    return absl::InvalidArgumentError(
        "DPUmesh runtime requires at least one reactor");
  }

  // Default: one dedicated callback thread paired with each reactor shard.
  std::vector<std::unique_ptr<Executor>> owned_callback_executors;
  if (callback_executor == nullptr) {
    owned_callback_executors.reserve(options.reactor_count);
    for (size_t i = 0; i < options.reactor_count; ++i) {
      owned_callback_executors.push_back(std::make_unique<ThreadExecutor>());
    }
    callback_executor = owned_callback_executors.front().get();
  }

  errno = 0;
  dmesh_channel_t* channel = ops->CreateChannel();
  if (channel == nullptr) {
    return absl::UnavailableError(absl::StrCat(
        "dmesh_create_channel failed: errno=", errno));
  }

  const int post_max = ops->PostMax(channel);
  if (post_max <= 0) {
    ops->DestroyChannel(channel);
    return absl::InternalError(absl::StrCat(
        "dmesh_post_max returned invalid value ", post_max));
  }

  auto runtime = std::unique_ptr<DmeshRuntime>(new DmeshRuntime(
      std::move(ops), channel, post_max, std::move(owned_callback_executors),
      callback_executor));
  runtime->reactors_.reserve(options.reactor_count);
  for (size_t i = 0; i < options.reactor_count; ++i) {
    Executor* reactor_callbacks =
        runtime->owned_callback_executors_.empty()
            ? callback_executor
            : runtime->owned_callback_executors_[i].get();
    auto reactor = DmeshReactor::Create(
        runtime->ops_.get(), channel, post_max, reactor_callbacks,
        options.reactor);
    if (!reactor.ok()) {
      runtime.reset();
      return reactor.status();
    }
    runtime->reactors_.push_back(std::move(*reactor));
  }
  return runtime;
}

DmeshRuntime::~DmeshRuntime() {
  reactors_.clear();
  if (channel_ != nullptr) {
    ops_->DestroyChannel(channel_);
    channel_ = nullptr;
  }
}

void DmeshRuntime::Connect(std::string service,
                           DmeshReactor::ConnectCallback callback) {
  const size_t index =
      next_reactor_.fetch_add(1, std::memory_order_relaxed) % reactors_.size();
  reactors_[index]->Connect(std::move(service), std::move(callback));
}

absl::Status DmeshRuntime::SetAcceptCallback(
    DmeshReactor::AcceptCallback callback) {
  size_t installed = 0;
  for (; installed < reactors_.size(); ++installed) {
    const absl::Status status =
        reactors_[installed]->SetAcceptCallback(callback);
    if (!status.ok()) {
      while (installed > 0) {
        --installed;
        (void)reactors_[installed]->SetAcceptCallback({});
      }
      return status;
    }
  }
  return absl::OkStatus();
}

}  // namespace dpumesh::grpc
