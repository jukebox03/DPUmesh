# Minimal application example

`hello_dpumesh.c` and `hello_dpumesh_server.c` are the small native-API starting
point. They show one client QP, one server accept path, byte-stream send,
zero-copy receive, backpressure, and ownership-ordered teardown.

Deploy DPUmesh, then apply the example. `RINGS` must equal the host and DPU
geometry; the native hardware profile uses eight.

```sh
./bench/native_deploy.sh deploy
NS=test-bench RINGS=8 IMG_BENCH_DPU=bench/bench-dpumesh:native \
  envsubst < bench/examples/k8s.yaml | kubectl apply -f -
kubectl rollout status -n test-bench deploy/hello-dpumesh
kubectl exec -n test-bench deploy/bench-dpumesh-native -- \
  /usr/local/bin/hello_dpumesh hello-dpumesh 'hello'
```

## gRPC

`grpc/` is the same starting point for a gRPC C++ application: `echo.proto`,
a callback-service server behind a `PassiveListener`, and a client whose
channel target is a Service name. Its `CMakeLists.txt` generates the proto,
compiles both programs and links `grpc_dpumesh`. Configure the parent adapter
build with an exact gRPC v1.80.0 source tree; the default
`DPUMESH_GRPC_BUILD_QPS_BENCHMARK=ON` build produces `hello_grpc_client` and
`hello_grpc_server`:

```sh
make lib
cmake -S integrations/grpc -B build/grpc \
  -DDPUMESH_GRPC_SOURCE_DIR=/path/to/grpc-v1.80.0
cmake --build build/grpc -j2 --target hello_grpc_client hello_grpc_server
```

Package the two binaries and `libdpumesh.so.5` into an image, then deploy them
with the explicit `dpumesh.io/channel: 1` resource and security contract in
`k8s.yaml`.
[design/GRPC.md](../../design/GRPC.md) covers the runtime options and
lifecycle.

The programs under `../apps/` add production concerns needed by the benchmark:
multiple connections, framing, backpressure queues, concurrency and metrics.
Start here for the API lifecycle; use those files when implementing a long-lived
or high-rate service.
