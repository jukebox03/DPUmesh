#ifndef DPUMESH_GRPC_VERSION_H
#define DPUMESH_GRPC_VERSION_H

#define DPUMESH_GRPC_VERSION_MAJOR 0
#define DPUMESH_GRPC_VERSION_MINOR 2
#define DPUMESH_GRPC_VERSION_PATCH 0

// EventEngine is experimental, so the adapter intentionally has an exact
// source-level gRPC compatibility contract.
#define DPUMESH_GRPC_REQUIRED_GRPC_VERSION "1.80.0"

#endif  // DPUMESH_GRPC_VERSION_H
