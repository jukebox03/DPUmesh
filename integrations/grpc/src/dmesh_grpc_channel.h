#ifndef DPUMESH_GRPC_DMESH_GRPC_CHANNEL_H
#define DPUMESH_GRPC_DMESH_GRPC_CHANNEL_H

#include <memory>
#include <grpcpp/channel.h>
#include <grpcpp/security/credentials.h>
#include <grpcpp/support/channel_arguments.h>

#include "dmesh_endpoint.h"

namespace dpumesh::grpc {

// Transfers ownership of an already-connected DPUmesh EventEngine endpoint to
// gRPC's chttp2 client channel implementation.
std::shared_ptr<::grpc::Channel> CreateDmeshGrpcChannel(
    std::unique_ptr<DmeshEndpoint> endpoint,
    std::shared_ptr<::grpc::ChannelCredentials> credentials,
    const ::grpc::ChannelArguments& args = {});

}  // namespace dpumesh::grpc

#endif
