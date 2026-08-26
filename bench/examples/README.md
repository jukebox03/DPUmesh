# Minimal application example

`hello_dpumesh.c` and `hello_dpumesh_server.c` are the small native-API starting
point. They show one client QP, one server accept path, byte-stream send,
zero-copy receive, backpressure, and ownership-ordered teardown.

Deploy DPUmesh, then apply the example:

```sh
./bench/bench.sh deploy
NS=test-bench IMG_BENCH_DPU=bench/bench-dpumesh:latest \
  envsubst < bench/examples/k8s.yaml | kubectl apply -f -
kubectl rollout status -n test-bench deploy/hello-dpumesh
kubectl exec -n test-bench deploy/bench-dpumesh -- \
  /usr/local/bin/hello_dpumesh hello-dpumesh 'hello'
```

The programs under `../apps/` add production concerns needed by the benchmark:
multiple connections, framing, backpressure queues, concurrency and metrics.
Start here for the API lifecycle; use those files when implementing a long-lived
or high-rate service.
