#include "dmesh_grpc_channel.h"

#include <grpcpp/create_channel_posix.h>

namespace dpumesh::grpc {

std::shared_ptr<::grpc::Channel> CreateDmeshGrpcChannel(
    std::unique_ptr<DmeshEndpoint> endpoint,
    std::shared_ptr<::grpc::ChannelCredentials> credentials,
    const ::grpc::ChannelArguments& args) {
  if (endpoint == nullptr || credentials == nullptr) return nullptr;
  return ::grpc::experimental::CreateChannelFromEndpoint(
      std::move(endpoint), credentials, args);
}

}  // namespace dpumesh::grpc
