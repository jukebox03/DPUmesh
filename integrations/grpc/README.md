# gRPC C++ over DPUmesh

This integration supplies gRPC chttp2 with DPUmesh EventEngine endpoints.
Generated messages, stubs, services, RPC semantics, metadata, deadlines, HTTP/2,
credentials, and TLS remain unchanged. DPUmesh is a byte-stream transport, not
an RPC wrapper or HTTP/2 parser.

The source contract is gRPC v1.80.0, C++17, and `libdpumesh.so.4`. The endpoint
injection APIs are experimental.

## Transport model

```text
generated stub / service
          │
     gRPC chttp2 + TLS
          │ EventEngine Read / Write
          ▼
    DmeshEndpoint
          │ one QP per connection
          ▼
 DPA / BlueField ARM ── backend TCP stream
```

The client channel accepts:

| Input | Meaning |
|---|---|
| `target` | Configured DPUmesh Service name |
| `GRPC_ARG_DEFAULT_AUTHORITY` | HTTP/2 authority and credential identity |

An absent authority defaults to `target`; an explicit value is preserved. The
target is not an IP address or gRPC resolver URI. DPUmesh owns the channel's
EventEngine argument and preserves other channel arguments.

## Client

Only channel bootstrap changes:

```cpp
grpc::ChannelArguments args;
args.SetString(GRPC_ARG_DEFAULT_AUTHORITY, "api.example.com");  // optional
args.SetInt(GRPC_ARG_INITIAL_RECONNECT_BACKOFF_MS, 100);

dpumesh::grpc::ConnectDmeshGrpcChannel(
    runtime.get(),
    "echo-dpumesh",
    grpc::SslCredentials(ssl_options),
    args,
    [&](absl::StatusOr<std::shared_ptr<grpc::Channel>> result) {
      if (!result.ok()) {
        ReportBootstrapError(result.status());
        return;
      }
      channel = *result;
    });

auto stub = Echo::NewStub(channel);
grpc::ClientContext context;
context.set_deadline(deadline);
context.set_wait_for_ready(true);
grpc::Status status = stub->Call(&context, request, &response);
```

Channel creation is lazy. Each gRPC `Connect` creates a targeted QP, and gRPC
owns reconnect backoff, deadlines, and RPC retry policy. Link `grpc_dpumesh` and
keep the callback executor and `DmeshRuntime` alive longer than their channels
and endpoints.

## Server

The service implementation is unchanged. Native connections enter gRPC through
`PassiveListener`:

```cpp
grpc::ServerBuilder builder;
builder.RegisterService(&service);

std::unique_ptr<grpc::experimental::PassiveListener> listener;
builder.experimental().AddPassiveListener(server_credentials, listener);
std::unique_ptr<grpc::Server> server = builder.BuildAndStart();

auto attachment = dpumesh::grpc::AttachDmeshGrpcServer(
    runtime.get(), listener.get(),
    [] { return MakeGrpcMemoryAllocator(); },
    [](const absl::Status& error) { ReportAcceptError(error); });
```

`AttachDmeshGrpcServer` converts `DMESH_EVENT_CONN_REQ` events into endpoints
and injects them into the listener. Shutdown stops traffic, calls `Detach()`,
shuts down the gRPC server, destroys the server and listener, then destroys the
runtime.

## Connection lifecycle

The registry maps configured Service names to service ids. Registered backend
instances form a separate live set, so instances may join or leave without
changing client channels or registry rows.

Each HTTP/2 connection remains pinned to one backend. Backend loss terminates
that stream; a later gRPC connection creates a QP and selects from the current
live set. In-flight RPCs retain normal gRPC deadline, retry, idempotency, and
`wait_for_ready` semantics. New Service names require registry updates.

## Data path

Each runtime owns one native channel and configurable EQ reactor shards. A
reactor is the sole consumer of its EQ and owns its QPs. RX bytes are copied
into gRPC slices before native credit is released; TX slices are copied into
registered native reservations.

Writes flush at the EventEngine write boundary. On `EAGAIN`, the endpoint keeps
its slice cursor and resumes on `DMESH_EVENT_TX_READY`. There is no busy poll,
retry timer, connection scan, or per-RPC wrapper dispatch.

## Build and test

```sh
make lib
cmake -S integrations/grpc -B build/grpc \
  -DDPUMESH_GRPC_SOURCE_DIR=/path/to/grpc-v1.80.0 \
  -DDPUMESH_GRPC_ENABLE_SANITIZERS=ON \
  -DBUILD_TESTING=ON
cmake --build build/grpc -j2
ASAN_OPTIONS=detect_leaks=0 \
  ctest --test-dir build/grpc --output-on-failure
```

Use a separate build with `-DDPUMESH_GRPC_ENABLE_TSAN=ON`. The QPS benchmark
syntax is documented in [the benchmark guide](../../bench/README.md); measured
results are recorded in [RESULT.md](../../bench/RESULT.md).

The integration uses L4 passthrough. It does not provide a `dpumesh:///`
resolver, HTTP/2 routing, registry reload, EndpointSlice watching, admission
control, or workload identity.
