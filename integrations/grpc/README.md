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
auto runtime = dpumesh::grpc::DmeshRuntime::Create(
    dpumesh::grpc::MakeNativeDmeshApiOps());

grpc::ChannelArguments args;
args.SetString(GRPC_ARG_DEFAULT_AUTHORITY, "api.example.com");  // optional
args.SetInt(GRPC_ARG_INITIAL_RECONNECT_BACKOFF_MS, 100);

auto channel = dpumesh::grpc::CreateDmeshChannel(
    *runtime, "echo-dpumesh", grpc::SslCredentials(ssl_options), args);

auto stub = Echo::NewStub(*channel);
grpc::ClientContext context;
context.set_deadline(deadline);
context.set_wait_for_ready(true);
grpc::Status status = stub->Call(&context, request, &response);
```

`CreateDmeshChannel` takes the parameters of `grpc::CreateCustomChannel` after
a runtime handle and returns `absl::StatusOr`. Channel creation is lazy. Each
gRPC `Connect` creates a targeted QP, and gRPC owns reconnect backoff,
deadlines, and RPC retry policy. `DmeshRuntime::Create` returns a
`std::shared_ptr`, and the channel holds one, so the runtime outlives a channel
gRPC has not yet released. Link `grpc_dpumesh`.

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
    *runtime, listener.get());
```

`AttachDmeshGrpcServer` converts `DMESH_EVENT_CONN_REQ` events into endpoints
and injects them into the listener. Optional arguments supply a per-connection
memory-allocator factory (default: unquota'd malloc slices) and an accept-error
callback. Shutdown stops traffic, calls `Detach()`, shuts down the gRPC server,
destroys the server and listener, then destroys the runtime.

## Connection lifecycle

The registry maps configured Service names to service ids. Registered backend
instances form a separate live set, so instances may join or leave without
changing client channels or registry rows.

Each HTTP/2 connection remains pinned to one backend. Backend loss terminates
that stream; a later gRPC connection creates a QP and selects from the current
live set. In-flight RPCs retain normal gRPC deadline, retry, idempotency, and
`wait_for_ready` semantics. New Service names require registry updates.

`GetPeerAddress` and `GetLocalAddress` report loopback addresses carrying the
DPU-assigned pod id and the connection's native port. A client stream has no
peer pod until the DPU pins one, and reads as `127.0.0.0:0`.

## Runtime configuration

Each `DmeshRuntime` opens one native channel, which registers the process under
`$DPUMESH_SERVICE` and maps its RX region. Create one runtime per process and
share it across every channel and the server attachment; a second runtime
registers the process a second time. The native library does not reject the
second registration.

`DmeshRuntime::Options::reactor_count` sets the number of EQ reactor shards
over that one channel. Each shard is one EQ and one polling thread, which also
runs normal receive and TX-ready Endpoint progress for its connections.
Outbound connections are assigned round-robin; an inbound connection belongs
to whichever shard received its `DMESH_EVENT_CONN_REQ`.

One further adapter-owned thread per runtime is the default callback executor
shared by every shard and Endpoint. It delivers connect/accept results and
deferred Endpoint callbacks, including immediate terminal failures and pending
operations failed by EOF, transport error or destruction. Normal receive and
TX-ready completions do not take this hop.
`DmeshRuntime::Create` accepts a `std::shared_ptr<Executor>` that replaces it.
Endpoints share ownership of the executor they schedule on, so it outlives an
endpoint gRPC has not yet destroyed.

Transmit batching has no gRPC-specific option. `PostSend` transfers ownership
to libdpumesh, and `Flush` marks the end of one EventEngine Write. Physical
batch state and deadlines remain in the library. The reactor maps `TX_READY`
and `TX_ERROR` events into endpoint progress and failure.

Receive backpressure is automatic and has no tunable: a stalled reader stops the
transport landing further bytes on that connection. There is no busy poll, retry
timer, connection scan, or per-RPC wrapper dispatch.

`DmeshRuntime::stats()` sums each shard's `DmeshReactor::Stats`. Both counters
report a bound being reached rather than an error:
`receive_credit_hold_dropped` rises when backpressure stops applying to a
stalled connection, and `eq_drain_budget_exhausted` rises when a shard is
saturated.

Ownership, locking, and the transmit and receive state machines are specified in
[the gRPC integration whitepaper](../../design/GRPC.md).

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
syntax, the rules a retained point must satisfy, and the index of every
evaluation are in [the benchmark guide](../../bench/README.md).

The reactor suite also keeps one runtime alive while ten gRPC channels churn,
and holds four channels for the same Service concurrently. It requires every
channel to receive a distinct native QP, verifies that dropping one does not
close the others, and waits for the native runtime statistics to return to
zero. The endpoint integration suite additionally runs four concurrent unary
RPCs over independent channels to one service and exercises both client and
server FIN plus graceful server GOAWAY. Reconnect testing separately requires
a fresh targeted QP.

The integration uses L4 passthrough. It does not provide a `dpumesh:///`
resolver, HTTP/2 routing, registry reload, EndpointSlice watching, admission
control, or workload identity.
