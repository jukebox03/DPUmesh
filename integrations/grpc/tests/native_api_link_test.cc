#include <cstdlib>
#include <memory>

#include "absl/status/statusor.h"
#include "dmesh_api_ops.h"
#include "dmesh_runtime.h"

int main() {
  using RuntimeFactory =
      absl::StatusOr<std::unique_ptr<dpumesh::grpc::DmeshRuntime>> (*)(
          std::unique_ptr<dpumesh::grpc::DmeshApiOps>,
          dpumesh::grpc::Executor*);
  RuntimeFactory runtime_factory =
      static_cast<RuntimeFactory>(&dpumesh::grpc::DmeshRuntime::Create);
  auto api = dpumesh::grpc::MakeNativeDmeshApiOps();
  return api == nullptr || runtime_factory == nullptr ? EXIT_FAILURE
                                                       : EXIT_SUCCESS;
}
