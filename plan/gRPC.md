# gRPC over DPUmesh native API: 분석, 설계, 구현 및 검증 계획

## 문서 상태

- 작성 기준 저장소: DPUmesh `af19365` (`docs: DPUmesh API bench`)
- 실제 성능 평가 코드 기준: `4beb0be` + 평가 harness 미커밋 변경 24개 파일
- gRPC 기준: gRPC Core/C++ `v1.80.0`, tag commit `f5e2d6e`
- 작성일: 2026-07-18
- 구현 상태: 설계 완료, 아직 gRPC adapter 코드는 구현하지 않음
- 이 문서에서 `native API`는 질문에서 말한 `naive API`를 뜻한다.

이 문서는 다음 작업을 별도 배경지식 없이 수행할 수 있도록 작성되었다.

1. gRPC가 실제로 socket을 사용하는 경계를 이해한다.
2. 그 경계를 DPUmesh native API로 교체할 수 있는지 판단한다.
3. 올바른 thread, memory, backpressure, shutdown 모델로 adapter를 구현한다.
4. 현재 DPU L4/L7 동작 중 gRPC와 충돌하는 부분을 수정한다.
5. 기능, 장애, 경쟁 조건, 실제 BlueField 성능을 단계적으로 검증한다.

이 문서의 결론이나 수치를 바꿀 때는 반드시 연결된 코드와 원자료도 함께 갱신한다.

---

## 1. 최종 결론

### 1.1 가능성 판정

| 질문 | 판정 | 근거 |
|---|---|---|
| gRPC C/C++의 socket 전송을 DPUmesh native API로 교체할 수 있는가? | **가능** | gRPC chttp2는 fd가 아니라 `EventEngine::Endpoint`의 byte-stream `Read`/`Write`에 의존한다. |
| protobuf, generated stub, HTTP/2 구현을 다시 작성해야 하는가? | **아니오** | chttp2와 그 위 계층은 그대로 두고 endpoint만 교체할 수 있다. |
| 현재 DPUmesh native API만으로 기능 PoC가 가능한가? | **가능** | QP, ordered full-duplex send/recv, accept, FIN, CQ가 이미 있다. |
| 현재 API를 전혀 바꾸지 않고 production-grade async transport를 만들 수 있는가? | **권장하지 않음** | TX space가 다시 생겼다는 notification이 없고 `dmesh_flush`가 드물게 오래 block할 수 있다. |
| 현재 DPU L7 hook으로 gRPC 요청 단위 LB가 가능한가? | **불가능** | 현재 hook은 16-byte bench frame parser이며 HTTP/2 connection state를 terminate/re-originate하지 않는다. |
| 가장 먼저 구현할 형태는 무엇인가? | **gRPC C++ + DPUmesh L4 passthru + custom Endpoint/EventEngine** | HTTP/2와 protobuf를 손대지 않고 transport만 교체하는 최소·정확 경로다. |

### 1.2 구현을 두 단계로 나눈다

1. **Protocol PoC**
   - `DmeshEndpoint` 구현
   - gRPC의 pre-established Endpoint 주입 API 사용
   - RX/TX copy 허용
   - `EAGAIN`은 짧은 timer retry
   - unary/streaming/TLS protocol correctness 확인

2. **Production transport**
   - hybrid `DmeshEventEngine` 구현
   - 정상 gRPC connect/listener/reconnect lifecycle 지원
   - gRPC `MemoryAllocator` quota 준수
   - DPUmesh TX writable notification 추가
   - nonblocking flush 경로 추가 또는 기존 blocking 상한 제거
   - passthru backend 사망 시 같은 byte stream을 다른 backend로 repick하지 않고 EOF 처리
   - 실제 gRPC/Envoy HCM/DPUmesh 비교 벤치 수행

### 1.3 지원 범위

이 설계의 첫 지원 대상은 gRPC C/C++이다. EventEngine은 C++ interface다.

- C++: 직접 적용 대상
- Python/Ruby: gRPC C-core를 사용하므로 장기적으로 공유할 가능성은 있지만 packaging/초기화 작업이 별도 필요
- Go/Java/.NET: 독립 transport runtime이므로 이 adapter를 그대로 사용할 수 없음

---

## 2. 근거가 되는 소스 버전과 위치

### 2.1 DPUmesh

주요 소스:

- public native API: `include/dpumesh/dmesh.h`
- native data-path 구현: `src/dmesh_api.c`
- connection, CQ, TX/RX core: `src/dmesh_core.c`, `src/dmesh_core.h`
- service registry: `src/dmesh_resolve.c`
- POSIX compatibility shim: `src/dmesh_preload.c`
- DPU L4/L7 engine: `doca/dpu_proxy.c`, `doca/dpu_proxy.h`
- 현재 L7 hook: `doca/dpu_l7.c`, `doca/dpu_l7.h`
- native bench: `bench/apps/bench_dpumesh.c`, `bench/apps/echo_dpumesh.c`
- matched socket bench: `bench/apps/bench_sock.c`, `bench/apps/echo_sock.c`
- bench report: `bench/report/REPORT.md`
- bench raw data: `bench/report/data/`

### 2.2 gRPC v1.80.0

분석에 사용한 공식 소스:

- release: <https://github.com/grpc/grpc/releases/tag/v1.80.0>
- EventEngine public interface: <https://github.com/grpc/grpc/blob/v1.80.0/include/grpc/event_engine/event_engine.h>
- Endpoint injection client API: <https://github.com/grpc/grpc/blob/v1.80.0/include/grpcpp/create_channel_posix.h>
- passive listener server API: <https://github.com/grpc/grpc/blob/v1.80.0/include/grpcpp/server_builder.h>
- passive listener endpoint API: <https://github.com/grpc/grpc/blob/v1.80.0/include/grpc/passive_listener.h>
- client chttp2 connector: <https://github.com/grpc/grpc/blob/v1.80.0/src/core/ext/transport/chttp2/client/chttp2_connector.cc>
- server chttp2 listener: <https://github.com/grpc/grpc/blob/v1.80.0/src/core/ext/transport/chttp2/server/chttp2_server.cc>
- chttp2 transport I/O: <https://github.com/grpc/grpc/blob/v1.80.0/src/core/ext/transport/chttp2/transport/chttp2_transport.cc>
- EventEngine endpoint shim: <https://github.com/grpc/grpc/blob/v1.80.0/src/core/lib/iomgr/event_engine_shims/endpoint.cc>
- EventEngine TCP client shim: <https://github.com/grpc/grpc/blob/v1.80.0/src/core/lib/iomgr/event_engine_shims/tcp_client.cc>
- wrapping EventEngine example: <https://github.com/grpc/grpc/blob/v1.80.0/examples/cpp/default_event_engine/wrapping_event_engine.h>
- gRPC over HTTP/2 protocol: <https://github.com/grpc/grpc/blob/v1.80.0/doc/PROTOCOL-HTTP2.md>

EventEngine 및 Endpoint 주입 API는 experimental이다. 정확한 gRPC 버전을 pin하고 minor/major upgrade 때 compile 및 end-to-end test를 다시 실행해야 한다.

### 2.3 빠른 코드 확인표

| 확인할 사실 | 기준 위치 |
|---|---|
| native API와 shim은 sibling surface | `include/dpumesh/dmesh.h:1-20`, `src/dmesh_api.c:1-13` |
| send completion이 없고 alloc 성공이 space 회복 신호 | `include/dpumesh/dmesh.h:58-60` |
| EAGAIN 원인 및 실제 측정 | `include/dpumesh/dmesh.h:78-97` |
| CQ single-consumer | `include/dpumesh/dmesh.h:105-115` |
| CQ fd는 inbound readiness만 알림 | `include/dpumesh/dmesh.h:273-279` |
| QP create는 local-only | `include/dpumesh/dmesh.h:283-295` |
| pending TX 때문에 echo가 poll함 | `bench/apps/echo_dpumesh.c:185-197` |
| CONN_REQ 직후 같은 batch에 RECV 가능 | `src/dmesh_api.c:119-140` |
| server QP가 첫 message를 held 상태로 accept | `src/dmesh_core.c:1883-1912` |
| flush 구현 | `src/dmesh_core.c:1954-1961` |
| QP close/FIN | `src/dmesh_core.c:1964-2002` |
| preload의 real eventfd/epoll 설계 | `src/dmesh_preload.c:28-43` |
| preload nonblocking write gap | `src/dmesh_preload.c:566-605` |
| current bench L7 header | `doca/dpu_l7.c:26-48` |
| passthru backend pin/repick | `doca/dpu_proxy.c:1045-1063` |
| DPU failure를 EOF로 변환 | `doca/dpu_proxy.c:863-921` |
| benchmark workload/수치/한계 | `bench/report/REPORT.md:1-205` |
| benchmark provenance | `bench/report/data/meta.txt` |

gRPC source에서는 다음 symbol을 우선 따라간다.

- `EventEngine::Endpoint::Read/Write`
- `event_engine_tcp_client_connect`
- `Chttp2Connector::Connect`
- `grpc_chttp2_transport_start_reading`
- `grpc_endpoint_read`, `grpc_endpoint_write`
- `CreateChannelFromEndpoint`
- `PassiveListenerImpl::AcceptConnectedEndpoint`

---

## 3. gRPC가 실제로 socket을 사용하는 경계

### 3.1 계층

```text
generated stub / protobuf
        │
        ▼
gRPC call, retry, deadline, metadata layer
        │
        ▼
gRPC chttp2 transport
        │
        ▼
grpc_endpoint
        │
        ▼
EventEngine::Endpoint
        │
        ├── POSIX EventEngine  → socket/sendmsg/recvmsg/epoll
        └── DmeshEndpoint      → dmesh QP/CQ/alloc/post/poll
```

`chttp2_transport.cc`는 만들어진 HTTP/2 bytes를 `grpc_endpoint_write()`로 전달하고, 수신도 `grpc_endpoint_read()`로 요청한다. socket syscall은 POSIX EventEngine 아래에 있을 뿐 chttp2의 필수 계약이 아니다.

따라서 바꿔야 할 것은 다음이 아니다.

- protobuf serialization
- generated stub
- gRPC call API
- chttp2 frame encoder/decoder
- deadline, retry, keepalive, flow-control 구현

바꿔야 할 것은 다음 하나다.

- `EventEngine::Endpoint`의 byte-stream transport

### 3.2 `EventEngine::Endpoint` 계약

`Endpoint`는 연결 한쪽 끝이다.

#### Read

```cpp
virtual bool Read(
    absl::AnyInvocable<void(absl::Status)> on_read,
    SliceBuffer* buffer,
    ReadArgs args) = 0;
```

- 즉시 읽을 data가 있으면 buffer에 넣고 `true` 반환
- `true` 반환 시 callback을 호출하면 안 됨
- 기다려야 하면 `false` 반환, 완료 시 callback 정확히 한 번
- endpoint당 outstanding Read는 최대 하나
- data와 error를 한 번에 같이 반환하면 안 됨
- 이미 수신한 valid data를 버리고 EOF/error부터 반환하면 안 됨
- `read_hint_bytes`는 최적화 hint이지 반드시 그 길이까지 기다리라는 요구가 아님

#### Write

```cpp
virtual bool Write(
    absl::AnyInvocable<void(absl::Status)> on_writable,
    SliceBuffer* data,
    WriteArgs args) = 0;
```

- 즉시 모든 bytes를 transport에 넘겼으면 `true`
- `true` 반환 시 callback 호출 금지
- 기다려야 하면 `false`, 나중에 callback 정확히 한 번
- endpoint당 outstanding Write는 최대 하나
- callback 전에 모든 bytes가 기록되어야 함
- endpoint는 callback 전까지 `SliceBuffer`와 slice를 보유·변형할 수 있음

#### Lifetime

- destructor는 연결을 닫는다.
- pending Read/Write callback을 error status로 완료한다.
- 모든 public method는 thread-safe해야 한다.
- callback은 동시에 실행될 수 있으므로 adapter 내부 state가 callback concurrency를 견뎌야 한다.
- endpoint buffer allocation은 제공된 gRPC `MemoryAllocator`를 따라야 한다.

### 3.3 pre-established endpoint가 socket connect를 건너뛰는 방법

`Chttp2Connector`는 channel args에 `EndpointChannelArgWrapper`가 있으면 endpoint를 꺼내고 TCP connect handshaker 설정을 건너뛴다. 그 뒤 credential handshaker와 chttp2 transport 생성은 그대로 수행한다.

그러므로 Dmesh endpoint 위에서도 다음이 유지된다.

- insecure credentials
- TLS/mTLS credentials
- HTTP/2 SETTINGS handshake
- gRPC channel readiness
- unary 및 streaming RPC
- metadata, trailers, status

client QP 생성이 local-only여도 gRPC channel이 즉시 READY가 되는 것은 아니다. chttp2 connector는 peer SETTINGS를 받을 때까지 readiness를 완료하지 않는다. 실제 backend가 없거나 stream이 깨지면 SETTINGS 단계에서 실패할 수 있다.

---

## 4. gRPC/HTTP2가 요구하는 transport 의미론

gRPC의 transport는 message transport가 아니라 ordered byte stream이어야 한다.

한 HTTP/2 connection에는 다음 state가 함께 존재한다.

- client connection preface
- SETTINGS
- HEADERS와 CONTINUATION
- HPACK dynamic/static table state
- DATA
- connection-level flow control
- stream-level flow control
- stream ID와 stream lifecycle
- trailers와 `grpc-status`
- PING 및 ACK
- GOAWAY
- RST_STREAM

또한 다음 경계는 서로 일치하지 않는다.

- DPUmesh RECV completion 경계
- HTTP/2 frame 경계
- HTTP/2 DATA frame 경계
- gRPC 5-byte length-prefixed message 경계
- protobuf object 경계

`DmeshEndpoint`는 DPUmesh completion을 단순히 순서대로 이어 붙인 byte stream으로 보여야 한다. completion 경계를 gRPC에 노출하거나 RPC message 경계로 해석하면 안 된다.

---

## 5. DPUmesh native API 계약

### 5.1 객체 대응

| 의미 | DPUmesh |
|---|---|
| process transport context | `dmesh_channel_t`, 프로세스당 하나 권장 |
| polling/ownership shard | `dmesh_cq_t`, polling thread당 하나 |
| ordered full-duplex connection | `dmesh_qp_t` |
| client connection create | `dmesh_create_qp(cq, service_name)` |
| server connection request | `DMESH_WC_CONN_REQ` |
| receive bytes | `DMESH_WC_RECV` |
| remote EOF/failure | `DMESH_WC_RECV_FIN` |
| return RX credit | `dmesh_wc_release(ch, wc)` |
| reserve registered TX memory | `dmesh_alloc(qp, len)` |
| send/commit | `dmesh_post_send(qp, buf, len, wr_id, flags)` |
| flush batched sends | `dmesh_flush(qp)` |
| CQ wake fd | `dmesh_cq_fd(cq)` |
| close and FIN | `dmesh_destroy_qp(qp)` |

### 5.2 기본 크기와 제한

현재 기본값:

- RX/TX slots per pod: 4096
- wire DMA slot size: 8 KB
- one `dmesh_alloc`/post maximum: 64 KB block
- per-QP block cap: 4 blocks, 최대 약 256 KB in flight
- shared TX block pool: 기본 512 blocks
- per-QP send-unit FIFO: 64 descriptors
- CQ cap per channel: 64
- connection port space: 1..65535

실제 값은 반드시 runtime query를 사용한다.

```c
int msg_max  = dmesh_msg_max(ch);
int post_max = dmesh_post_max(ch);
```

adapter에 8 KB나 64 KB를 hard-code하지 않는다.

### 5.3 TX 계약

- 한 QP에는 outstanding `dmesh_alloc` reserve가 최대 하나다.
- alloc order = post order = wire order다.
- post 이후 buffer ownership은 transport로 넘어간다.
- `dmesh_alloc`은 절대 block하지 않고, 공간이 없으면 `NULL`, `errno=EAGAIN`이다.
- EAGAIN 원인은 다음 둘 중 하나다.
  - 해당 QP의 block/FIFO in-flight cap
  - 다른 QP도 공유하는 process-wide block pool 고갈
- EAGAIN은 실제 정상 경로이며 fatal error가 아니다.
- `dmesh_flush`는 per-QP capacity에는 실패하지 않지만 shared host-to-DPU descriptor ring에서 backoff할 수 있다.
- descriptor ring이 장시간 wedge되면 현재 구현은 최대 5초 후 `EBADMSG`를 반환한다.

### 5.4 RX 계약

- `wc.buf`는 RX mmap 안을 가리킨다.
- `dmesh_wc_release` 전까지만 유효하다.
- release 전까지 RX credit 하나를 점유한다.
- QP가 닫힌 뒤에도 해당 WC credit release는 유효하다.
- 한 QP completion은 순서가 보장된다.
- 여러 QP completion 사이에는 순서가 없다.
- codec/per-message LB를 켜면 같은 QP에서도 서로 다른 `wc.stream` bytes가 interleave할 수 있다.
- passthru 서비스에서는 원칙적으로 한 backend stream만 존재해야 한다.

### 5.5 CQ와 thread 계약

- CQ 하나는 정확히 한 thread만 poll한다.
- QP는 생성 또는 accept한 CQ에 고정된다.
- `dmesh_cq_fd` wake 후 eventfd를 drain하고 `dmesh_poll_cq`를 0까지 호출해야 한다.
- new connection accept queue는 channel-wide SPMC다.
- `DMESH_WC_CONN_REQ`와 같은 QP의 첫 RECV들이 하나의 `dmesh_poll_cq` batch에 연속해서 나올 수 있다.
- batch 중간에 QP를 파괴하면 뒤의 WC가 freed QP를 가리킬 수 있으므로 close는 batch 뒤 sweep으로 미룬다.

### 5.6 연결과 장애 계약

- `dmesh_create_qp()`는 service name을 registry에서 찾고 local QP만 만든다.
- network round trip이나 peer liveness handshake를 하지 않는다.
- unknown name은 `ENOENT`다.
- current DPU proxy는 stream을 전달할 수 없으면 origin에 zero-length EOF를 보내도록 구현되어 있다.
- public 문서 `design/API.md:170-172`의 “dead service면 아무 응답도 오지 않는다”는 문장은 같은 문서 `design/API.md:441-448` 및 현재 `dpu_proxy.c:px_poison()`과 충돌한다. 구현자는 현재 source의 EOF 동작을 기준으로 하되 real-hardware test로 확정해야 한다.
- peer crash/FIN loss에 대한 idle reaper는 없다.

---

## 6. 정확한 API 매핑

| gRPC/EventEngine | DPUmesh 구현 |
|---|---|
| `Endpoint` | QP 하나와 그 connection state |
| `Endpoint::Read` | RX queue 또는 CQ의 `DMESH_WC_RECV` |
| `Endpoint::Write` | gRPC slices를 `dmesh_alloc` buffer로 copy 후 post |
| endpoint remote close | `DMESH_WC_RECV_FIN`을 non-OK Read status로 변환 |
| endpoint local close | reactor에서 `dmesh_destroy_qp` |
| client `Connect` | service name으로 QP 생성, endpoint callback |
| server `Listener` | `CONN_REQ`를 endpoint accept callback으로 변환 |
| endpoint peer/local address | 실제/synthetic `ResolvedAddress`, routing에는 사용하지 않음 |
| EventEngine timers/tasks/DNS | 내장 EventEngine에 delegate |
| EventEngine memory | Connect allocator / Listener allocator factory 사용 |

### 6.1 가능한 이유

- QP는 full-duplex다.
- passthru QP는 ordered byte stream이다.
- HTTP/2는 transport-level half-close를 RPC마다 요구하지 않는다. 개별 RPC half-close는 HTTP/2 `END_STREAM` flag로 표현된다.
- DPUmesh whole-QP FIN이면 connection endpoint close를 표현하기에 충분하다.
- gRPC는 endpoint fd를 직접 요구하지 않는다.

### 6.2 그대로 맞지 않는 부분

- gRPC TX buffer는 DPU DMA 등록 buffer가 아니다. TX copy가 필요하다.
- CQ fd는 RX/accept/FIN만 깨우고 TX space 회복은 깨우지 않는다.
- gRPC endpoint method는 여러 thread에서 호출될 수 있지만 DPUmesh QP TX cursor는 owner-thread state다.
- Endpoint injection API는 새 endpoint에 gRPC connection allocator를 전달하지 않는다.
- DPUmesh server identity/listener는 프로세스당 사실상 하나다.
- current DPU L4는 pinned backend 사망 후 같은 stream을 다른 backend로 repick한다.
- current L7 hook은 gRPC/HTTP2 parser가 아니다.

---

## 7. production blocker 및 필수 보강

### 7.1 TX writable notification 부재

현재 API에서는 `dmesh_alloc`이 EAGAIN이었다가 공간이 생겨도 CQ fd가 깨어나지 않는다. 현재 `echo_dpumesh.c`도 pending reply가 있으면 `epoll_wait(..., timeout=0)`으로 poll한다.

가능한 임시 구현:

- 10~50 µs timer retry
- adaptive spin 후 timer
- pending write가 있을 때 busy poll

이들은 기능은 보장하지만 각각 tail latency 또는 CPU를 희생한다.

production용 권장 API:

```c
/* EAGAIN 뒤 호출한다.
 * 1: 지금 다시 시도할 수 있음
 * 0: arm 완료; space가 생기면 owning CQ fd가 wake됨
 * -1: permanent error
 */
int dmesh_arm_writable(dmesh_qp_t *qp);
```

필수 의미론:

- arm 후 condition 재검사로 missed wakeup 방지
- QP당 arm 하나에 notification 최대 하나
- per-QP ACK뿐 아니라 shared block pool 반환도 waiter를 깨워야 함
- QP close와 arm race에서 freed pointer 접근 금지
- CQ를 0까지 poll한 뒤 pending-writer set을 재시도

선택적으로 completion을 명시할 수 있다.

```c
DMESH_WC_SEND_READY
```

그러나 generic CQ wake 후 해당 shard의 exact pending-writer set만 재시도해도 충분하다.

### 7.2 `dmesh_flush`의 blocking 가능성

현재 `dmesh_flush`는 shared descriptor ring이 밀리면 내부 backoff를 수행하고 최악에는 약 5초 후 `EBADMSG`를 반환한다. 한 reactor가 여러 QP를 소유할 때 이 block은 shard 전체를 멈출 수 있다.

PoC에서는 현재 API를 사용하되 측정해야 한다.

production 권장 보강:

```c
/* 모든 committed descriptor를 지금 enqueue한다.
 * 0: 모두 enqueue
 * -1/EAGAIN: global ring pressure, 아무 thread도 오래 block하지 않음
 * -1/EBADMSG: permanent/wedged transport error
 */
int dmesh_try_flush(dmesh_qp_t *qp);
```

또는 `DMESH_SEND_NONBLOCK` flag를 추가한다. global descriptor ring pressure도 `dmesh_arm_writable`과 같은 wake/retry 체계에 포함해야 한다.

현재 `dmesh_alloc`은 `DMESH_SEND_MORE`로 commit됐지만 아직 flush되지 않은 bytes가 자기 공간을 막은 경우 deadlock을 피하기 위해 EAGAIN 경로에서 `dmesh_flush`를 호출한다. nonblocking API를 설계할 때 이 자기-deadlock 회피를 보존해야 한다.

### 7.3 backend 사망 후 byte-stream repick

현재 `doca/dpu_proxy.c:px_resolve_backend()`는 pinned backend가 죽으면 새로운 backend를 선택한다. opaque byte stream에서 이는 올바른 failover가 아니다.

gRPC에서는 새 backend가 다음 state를 갖고 있지 않다.

- TLS session/record state
- HTTP/2 preface 및 SETTINGS
- HPACK table
- 기존 stream IDs
- connection/stream flow-control window

필수 수정:

```text
처음 routing: backend 선택 후 pin
pinned backend alive: 계속 사용
pinned backend dead: origin에 EOF, connection poison/close
새 backend 선택: 오직 새 client QP에서만 수행
```

adapter도 passthru QP에서 첫 `wc.stream`을 latch하고 이후 다른 stream ID가 나타나면 `UNAVAILABLE`로 connection을 닫아 방어한다.

### 7.4 idle reaper 부재

FIN이 손실되거나 peer가 비정상 종료하면 DPU conn/upstream state가 port 재사용 전까지 남을 수 있다.

권장:

- DPU-side idle lease 또는 max connection age
- gRPC keepalive 설정
- endpoint close/FIN failure metric
- active QP, DPU upstream, client channel 수 대조

gRPC keepalive는 host가 dead connection을 감지하는 수단이지 DPU stale state reclamation 자체는 아니다.

---

## 8. 목표 아키텍처

```text
┌─────────────────────────────────────────────────────────────┐
│ Application                                                  │
│ generated Stub / Service implementation                      │
└──────────────────────────────┬──────────────────────────────┘
                               │
┌──────────────────────────────▼──────────────────────────────┐
│ gRPC C-core                                                  │
│ protobuf · calls · retry · deadline · TLS · HTTP/2 chttp2    │
└──────────────────────────────┬──────────────────────────────┘
                               │ EventEngine::Endpoint
┌──────────────────────────────▼──────────────────────────────┐
│ grpc_dpumesh                                                 │
│ DmeshEventEngine                                             │
│ DmeshEndpoint                                                │
│ DmeshRuntime + reactor shards                                │
└──────────────────────────────┬──────────────────────────────┘
                               │ native API
┌──────────────────────────────▼──────────────────────────────┐
│ libdpumesh.so                                                │
│ channel · CQ · QP · registered TX/RX · DOCA PE              │
└──────────────────────────────┬──────────────────────────────┘
                               │
┌──────────────────────────────▼──────────────────────────────┐
│ BlueField DPU/DPA                                            │
│ L4 passthru, connection pin, backend proxy                   │
└─────────────────────────────────────────────────────────────┘
```

### 8.1 코드 배치 제안

```text
integrations/grpc/
├── CMakeLists.txt
├── README.md
├── include/dpumesh/grpc/
│   ├── event_engine.h
│   └── channel.h
├── src/
│   ├── dmesh_api_ops.h
│   ├── dmesh_api_ops.cc
│   ├── dmesh_runtime.h
│   ├── dmesh_runtime.cc
│   ├── dmesh_reactor.h
│   ├── dmesh_reactor.cc
│   ├── dmesh_endpoint.h
│   ├── dmesh_endpoint.cc
│   ├── dmesh_listener.h
│   ├── dmesh_listener.cc
│   ├── dmesh_event_engine.h
│   ├── dmesh_event_engine.cc
│   └── status.cc
├── examples/helloworld/
│   ├── helloworld.proto
│   ├── client.cc
│   └── server.cc
└── tests/
    ├── fake_dmesh_ops.*
    ├── endpoint_test.cc
    ├── reactor_test.cc
    ├── event_engine_test.cc
    └── grpc_end2end_test.cc
```

adapter는 C++이어야 하지만 public DPUmesh 호출은 `<dpumesh/dmesh.h>`만 사용한다. `src/dmesh_core.h`에 의존하지 않는다.

### 8.2 build 의존성

- C++17 이상
- gRPC C++ exact version `1.80.0`
- protobuf version은 해당 gRPC release와 호환되는 버전
- Abseil은 gRPC target을 통해 링크
- `libdpumesh.so.1`
- pthread

CMake 예시:

```cmake
find_package(gRPC CONFIG REQUIRED)
find_package(Protobuf CONFIG REQUIRED)

find_path(DPUMESH_INCLUDE_DIR dpumesh/dmesh.h REQUIRED)
find_library(DPUMESH_LIBRARY NAMES dpumesh REQUIRED)

add_library(DPUmesh::dpumesh UNKNOWN IMPORTED)
set_target_properties(DPUmesh::dpumesh PROPERTIES
  IMPORTED_LOCATION "${DPUMESH_LIBRARY}"
  INTERFACE_INCLUDE_DIRECTORIES "${DPUMESH_INCLUDE_DIR}")

add_library(grpc_dpumesh ...)
target_compile_features(grpc_dpumesh PUBLIC cxx_std_17)
target_link_libraries(grpc_dpumesh
  PUBLIC gRPC::grpc++
  PRIVATE DPUmesh::dpumesh pthread)
```

repo 안에서 개발할 때는 먼저 top-level `make`로 `build/lib/libdpumesh.so`와 public header를 준비하고, adapter CMake에 `CMAKE_PREFIX_PATH`/`DPUMESH_LIBRARY`/`DPUMESH_INCLUDE_DIR`를 명시한다. DOCA가 없는 일반 CI에서는 `DmeshApiOps` fake target만 build하도록 option을 둔다.

EventEngine가 experimental이므로 `grpc_dpumesh`의 ABI도 gRPC minor version에 묶인다고 간주한다.

---

## 9. component 설계

### 9.1 `DmeshApiOps`

native C API를 얇게 감싸는 test seam이다.

```cpp
class DmeshApiOps {
 public:
  virtual ~DmeshApiOps() = default;
  virtual dmesh_channel_t* CreateChannel() = 0;
  virtual int DestroyChannel(dmesh_channel_t*) = 0;
  virtual dmesh_cq_t* CreateCq(dmesh_channel_t*) = 0;
  virtual int DestroyCq(dmesh_cq_t*) = 0;
  virtual dmesh_qp_t* CreateQp(dmesh_cq_t*, const char* service) = 0;
  virtual int PollCq(dmesh_cq_t*, dmesh_wc_t*, int) = 0;
  virtual void* Alloc(dmesh_qp_t*, uint32_t) = 0;
  virtual int PostSend(dmesh_qp_t*, const void*, uint32_t,
                       uint64_t, unsigned) = 0;
  virtual int Flush(dmesh_qp_t*) = 0;
  virtual void Release(dmesh_channel_t*, dmesh_wc_t*) = 0;
  virtual int DestroyQp(dmesh_qp_t*) = 0;
  virtual int CqFd(dmesh_cq_t*) = 0;
  virtual int MsgMax(dmesh_channel_t*) = 0;
  virtual int PostMax(dmesh_channel_t*) = 0;
};
```

production wrapper는 native 함수를 그대로 호출한다. fake는 EAGAIN, FIN, reordering, delayed ACK를 deterministic하게 주입한다.

### 9.2 `DmeshRuntime`

역할:

- channel 하나 생성 및 refcounted lifetime 관리
- reactor shard N개 생성
- client connection을 shard에 round-robin 배치
- server CONN_REQ를 accept한 shard가 connection owner가 되도록 유지
- shutdown 시 새 connection 차단
- 모든 endpoint, QP, CQ, external RX slice가 끝난 뒤 channel 파괴

초기화 실패는 조용히 TCP fallback하지 않는다. DPUmesh channel로 명시된 channel/server는 명확한 error를 반환해야 한다.

### 9.3 `DmeshReactor`

reactor당 다음을 가진다.

```text
dmesh_cq_t* cq
int cq_fd
int command_eventfd
thread reactor_thread
MPSC command queue
map<local_port, shared_ptr<ConnectionState>>
set<ConnectionState*> pending_tx
deferred-close list
```

event loop:

```text
while running:
  epoll_wait(cq_fd, command_eventfd, optional retry timer)
  drain command_eventfd
  execute queued commands
  drain cq eventfd
  while dmesh_poll_cq(...) > 0:
    process WC entries in order
    never destroy QP inside this batch
  run deferred accept/close after batch
  retry exact pending_tx set
  schedule callbacks outside state locks
```

현재 API로 pending TX가 있으면 finite timeout을 사용한다. TX notification API가 추가되면 timeout은 제거한다.

### 9.4 `ConnectionState`

endpoint object 자체보다 오래 살 수 있는 shared state다.

```cpp
struct ConnectionState {
  mutex mu;
  DmeshRuntime* runtime;
  DmeshReactor* reactor;
  dmesh_qp_t* qp;
  MemoryAllocator allocator;

  enum Life { kOpen, kRemoteEof, kClosing, kClosed } life;

  deque<RxChunk> rx_queue;
  optional<PendingRead> pending_read;
  optional<PendingWrite> pending_write;

  optional<uint16_t> passthru_stream;
  bool close_command_queued;
  bool qp_destroyed;
};
```

`qp->user_data`에는 `ConnectionState*` raw pointer를 넣을 수 있지만 reactor map이 QP 파괴까지 shared ownership을 유지해야 한다. endpoint destructor가 실행됐다고 state를 바로 free하면 안 된다.

### 9.5 `DmeshEndpoint`

역할:

- public `Endpoint` method의 thread safety 제공
- 모든 QP operation을 reactor로 marshal
- pending callback 최대 하나씩 유지
- peer/local `ResolvedAddress` lifetime 유지
- destructor에서 shared state를 closing으로 전환

가능하면 모든 Write를 async로 처리하고 `false`를 반환한다. 즉시 완료 최적화는 correctness가 완성된 뒤 추가한다.

`GetTelemetryInfo()`는 초기 버전에서 `nullptr`을 반환할 수 있다.

---

## 10. client connect 상세

### 10.1 PoC: Endpoint injection

순서:

1. `DmeshRuntime`에서 client shard 선택
2. shard command queue에 `CreateQp(service)` 요청
3. reactor thread가 `dmesh_create_qp()` 호출
4. `DmeshEndpoint` 생성
5. `grpc::experimental::CreateChannelFromEndpoint()`에 전달
6. generated stub 생성

예시:

```cpp
auto endpoint = runtime->CreateEndpoint("greeter");

grpc::ChannelArguments args;
args.SetString(GRPC_ARG_DEFAULT_AUTHORITY,
               "greeter.default.svc.cluster.local");

auto channel = grpc::experimental::CreateChannelFromEndpoint(
    std::move(endpoint),
    grpc::InsecureChannelCredentials(),
    args);

auto stub = Greeter::NewStub(channel);
```

제약:

- injected endpoint는 connector가 한 번 `TakeEndpoint()`한 뒤 재사용할 수 없다.
- reconnect 시 정상 `Connect()` lifecycle을 제공하지 않는다.
- endpoint에 gRPC connection allocator가 자동 전달되지 않는다.
- 따라서 이 경로는 protocol PoC 전용이다.

### 10.2 Production: `DmeshEventEngine::Connect`

channel 생성 전에 service를 explicit channel arg로 넣는다.

```cpp
grpc::ChannelArguments args;
args.SetString("grpc.dpumesh.service", "greeter");
args.SetString(GRPC_ARG_DEFAULT_AUTHORITY,
               "greeter.default.svc.cluster.local");

auto channel = grpc::CreateCustomChannel(
    "dns:///greeter.default.svc.cluster.local:50051",
    grpc::SslCredentials(ssl_options),
    args);
```

`Connect()` 처리:

1. `EndpointConfig.GetString("grpc.dpumesh.service")` 확인
2. 값이 없으면 wrapped EventEngine `Connect()`에 delegate
3. 값이 있으면 unique connection handle 발급
4. pending connect map에 callback 저장
5. shard에 QP 생성 command enqueue
6. reactor가 QP 생성
7. endpoint 생성
8. wrapped EventEngine `Run()`으로 `on_connect(StatusOr<Endpoint>)` 비동기 호출
9. callback을 전달하기 전 cancellation과 race 재검사

`Connect()` contract상 success/error callback은 비동기로 정확히 한 번 실행해야 한다.

`CancelConnect(handle)`:

- 아직 callback delivery 전이면 pending entry 제거, QP가 만들어졌다면 reactor close enqueue, callback을 호출하지 않고 `true`
- 이미 callback delivery를 시작했으면 `false`
- callback object는 `true` 반환 전에 destroy되어야 함

unknown service `ENOENT`는 `UNAVAILABLE`로 변환한다. registry가 load-once이므로 현재 process 안에서 자동 회복되지 않는 점도 log에 명시한다.

QP 생성은 local-only이므로 성공 callback이 peer liveness를 뜻하지 않는다. 이후 TLS/HTTP2 SETTINGS handshake가 실제 연결 유효성을 판정한다.

---

## 11. server listener 상세

### 11.1 PoC: PassiveListener

```cpp
std::unique_ptr<grpc::experimental::PassiveListener> passive;

grpc::ServerBuilder builder;
builder.RegisterService(&service);
builder.experimental().AddPassiveListener(
    grpc::InsecureServerCredentials(), passive);

auto server = builder.BuildAndStart();
```

reactor가 server QP를 accept하면 다음을 수행한다.

1. `CONN_REQ`에서 state 생성 및 `qp->user_data` 설정
2. 같은 CQ batch의 모든 RECV를 state RX queue에 넣음
3. batch가 끝난 뒤 `passive->AcceptConnectedEndpoint()` 호출

server는 `BuildAndStart()`된 뒤에만 endpoint를 accept할 수 있다.

### 11.2 Production: custom Listener

`DmeshEventEngine::CreateListener()`는 다음 객체를 반환한다.

```cpp
class DmeshListener : public EventEngine::Listener {
 public:
  StatusOr<int> Bind(const ResolvedAddress&) override;
  Status Start() override;
  ~DmeshListener() override;
};
```

제약과 처리:

- DPUmesh server identity는 `$DPUMESH_SERVICE`
- meshed listen port는 `$DPUMESH_PORT`
- 프로세스당 사실상 한 DPUmesh listener/service
- `Bind()`는 전달된 port가 `$DPUMESH_PORT`와 맞는지 검증
- 동일 port 중복 bind는 idempotent하게 허용 가능
- 두 번째 다른 DPUmesh port는 `UNIMPLEMENTED` 또는 `FAILED_PRECONDITION`
- 여러 protobuf service implementation은 하나의 gRPC server/port 위에 등록 가능하므로 문제 없음

`CreateListener()`가 받은 `MemoryAllocatorFactory`는 listener lifetime 동안 보관한다. connection마다 allocator를 **두 번** 생성한다.

```cpp
auto endpoint_allocator =
    factory->CreateMemoryAllocator("endpoint-" + peer_name);
auto accept_allocator =
    factory->CreateMemoryAllocator("on-accept-" + peer_name);

auto endpoint = std::make_unique<DmeshEndpoint>(
    /* connection state */, std::move(endpoint_allocator));

// 반드시 reactor/state lock 밖에서 EventEngine callback scheduler를 통해 실행한다.
on_accept(std::move(endpoint), std::move(accept_allocator));
```

두 allocator handle은 같은 resource quota를 가리킬 수 있지만 서로 다른 move-only object다. 하나를 endpoint와 callback에 두 번 move하거나, callback용 allocator를 생략하면 안 된다. gRPC v1.80의 POSIX/CF/Windows listener도 endpoint 생성용과 `on_accept` 인자용 allocator를 각각 생성한다. DPUmesh listener도 이 contract를 그대로 따라야 한다.

shutdown contract:

- listener가 shutdown되면 새 CONN_REQ는 즉시 FIN/close
- 진행 중 accept callback이 모두 반환한 뒤 `on_shutdown` 정확히 한 번
- endpoint는 listener보다 오래 살 수 있음
- listener destructor가 runtime channel을 조기에 파괴하면 안 됨

---

## 12. Read 상태기계

### 12.1 copy RX baseline

`DMESH_WC_RECV` 처리:

```text
validate qp/state/life
validate passthru stream id
allocate grpc slice using connection MemoryAllocator
copy wc.buf[0..len) into slice
dmesh_wc_release immediately
append slice to state.rx_queue
if pending Read exists:
  move queued bytes to its SliceBuffer
  detach callback
  schedule callback(OK) outside lock
```

allocation:

```cpp
using grpc_event_engine::experimental::MemoryRequest;
using grpc_event_engine::experimental::Slice;

grpc_slice raw = allocator.MakeSlice(MemoryRequest(wc.len));
if (GRPC_SLICE_LENGTH(raw) != wc.len) {
  grpc_slice_unref(raw);
  // 이 helper는 WC를 먼저 release한 뒤 endpoint를
  // INTERNAL/RESOURCE_EXHAUSTED로 실패시키고 QP close를 예약한다.
  ReleaseWcAndFailEndpoint(wc, /* status */);
  return;
}
memcpy(GRPC_SLICE_START_PTR(raw), wc.buf, wc.len);
Slice slice(raw);  // slice destructor가 raw ref를 해제한다.
read_buffer->Append(std::move(slice));
```

`MemoryRequest(n)`은 exact request지만 반환 길이는 방어적으로 확인한다. allocation 실패/길이 불일치 처리에서는 WC를 반드시 release하고 endpoint를 실패시켜 RX credit을 유실하지 않는다. quota overcommit/reclamation 의미는 gRPC version별로 다시 검증한다.

### 12.2 `Read()` 구현

권장 동작:

```text
lock state
assert no pending Read

if rx_queue has data:
  move at least one byte, optionally up to read_hint_bytes
  unlock
  return true                         // callback 금지

if remote EOF or terminal error:
  store callback temporarily
  unlock
  schedule callback(error)
  return false

store PendingRead{callback, buffer, hint}
unlock
return false
```

세부 규칙:

- data와 EOF가 모두 있으면 data를 먼저 반환한다.
- 다음 Read에서 EOF/error를 반환한다.
- hint를 만족하지 못해도 CQ를 drain했는데 data가 있으면 latency를 위해 완료할 수 있다.
- pending Read buffer pointer는 callback 시점까지 caller가 유지한다는 EventEngine 계약을 이용한다.
- callback은 state mutex나 reactor internal lock을 잡고 호출하지 않는다.

### 12.3 FIN/error 처리

`DMESH_WC_RECV_FIN`은 peer graceful close와 DPU delivery failure를 구분할 수 없다.

권장 매핑:

- local endpoint destruction: `absl::CancelledError("dpumesh endpoint shutdown")`
- remote FIN/EOF: `absl::UnavailableError("dpumesh peer closed or stream delivery failed")`
- DPU descriptor/transport fault: `absl::UnavailableError(...)`
- adapter invariant violation: `absl::InternalError(...)`

gRPC chttp2가 endpoint read failure를 connection failure로 전환하고 진행 중 RPC에는 보통 `UNAVAILABLE`을 전달한다.

### 12.4 passthru stream 검증

```text
first RECV: state.passthru_stream = wc.stream
later RECV:
  if wc.stream != latched:
    release WC
    fail endpoint UNAVAILABLE
    close QP
```

codec/per-message-LB가 잘못 활성화되거나 backend repick이 발생하면 bytes를 한 HTTP/2 stream으로 섞지 않고 즉시 연결을 실패시킨다.

---

## 13. Write 상태기계

### 13.1 기본 원칙

gRPC `SliceBuffer` memory는 DPU DMA 등록 memory가 아니므로 TX copy는 필수다.

```text
gRPC SliceBuffer
      │
      │ memcpy
      ▼
dmesh_alloc()이 반환한 registered TX ring
      │
      ▼
dmesh_post_send()/flush
```

기존 native benchmark처럼 request를 TX ring에 처음부터 생성하는 zero-copy 수치는 adapter에 그대로 적용되지 않는다.

### 13.2 PendingWrite state

```cpp
struct PendingWrite {
  SliceBuffer* input;       // callback 전까지 valid
  WriteCallback callback;
  size_t total;
  size_t done;
  size_t slice_index;
  size_t slice_offset;
  bool queued_for_retry;
};
```

`SliceBuffer`를 반드시 비울 필요는 없다. cursor로 읽고 callback 때 gRPC wrapper가 정리하게 하는 방식이 단순하다.

### 13.3 `Write()` 구현

초기 구현은 항상 async로 한다.

```text
if data length == 0:
  return true

lock state
assert no pending Write
if closing/closed:
  unlock
  schedule callback(CANCELLED/UNAVAILABLE)
  return false
store PendingWrite
unlock
enqueue reactor PumpWrite command
return false
```

### 13.4 reactor의 `PumpWrite`

```text
while write.done < write.total:
  want = min(remaining, dmesh_post_max(channel))
  dst = dmesh_alloc(qp, want)

  if dst == NULL and errno == EAGAIN:
    add state to exact pending_tx set
    arm writable or schedule retry timer
    return PENDING

  if dst == NULL:
    fail write + endpoint
    return FAILED

  copy exactly want bytes from SliceBuffer cursor to dst

  rc = dmesh_post_send(qp, dst, want, 0, flags)
  if rc != 0:
    fail write + endpoint
    return FAILED

  advance cursor only after successful post

all bytes posted/flushed:
  remove from pending_tx
  detach callback
  schedule callback(OK)
```

MVP에서는 각 chunk를 `flags=0`으로 즉시 flush해 committed-but-unsent state를 최소화한다. 성능 단계에서 여러 chunk에 `DMESH_SEND_MORE`를 사용하고 마지막에 한 번 flush하되 다음을 증명해야 한다.

- EAGAIN 시 앞선 committed bytes가 영원히 unflushed로 남지 않음
- chunk order 유지
- one outstanding alloc 계약 준수
- flush failure 시 callback이 정확히 한 번 error

Write callback은 remote delivery ACK가 아니라 bytes가 local DPUmesh transport custody로 넘어갔다는 의미다. 이는 socket write가 kernel send buffer에 받아들여졌다는 의미와 대응한다. RPC 성공 여부는 HTTP/2 response/trailers가 결정한다.

### 13.5 EAGAIN retry

PoC:

- 처음 2~8회 reactor-local immediate retry
- 이후 10 µs부터 100 µs cap까지 adaptive timer
- 한 QP를 기다리는 동안 CQ와 다른 command를 계속 처리
- 절대로 gRPC worker thread에서 nanosleep하지 않음

production:

- `dmesh_arm_writable`
- CQ wake 후 pending set만 재시도
- shared pool wake도 처리
- timer는 watchdog/backstop으로만 사용

---

## 14. close 및 lifetime 상태기계

### 14.1 상태

```text
OPEN
 ├─ remote FIN ───────────────► REMOTE_EOF
 ├─ local destructor/error ───► CLOSING
 └─ fatal write/read error ───► CLOSING

REMOTE_EOF
 └─ buffered RX delivery 완료 ► CLOSING

CLOSING
 └─ reactor destroys QP ──────► CLOSED
```

### 14.2 Endpoint destructor

destructor가 arbitrary gRPC thread에서 실행될 수 있으므로 직접 QP를 파괴하지 않는다.

```text
lock state
if already closing/closed: return
mark CLOSING
detach pending Read callback
detach pending Write callback
mark close_command_queued
unlock

schedule detached callbacks(CANCELLED)
enqueue reactor Close command
```

reactor Close command:

1. pending TX set에서 제거
2. 같은 CQ batch 처리 중이면 deferred-close list에 넣음
3. held/copy 전 RX WC가 있으면 release
4. `dmesh_destroy_qp()` 정확히 한 번
5. `qp->user_data`를 더 이상 접근하지 않도록 reactor map 제거
6. state를 CLOSED로 변경
7. reactor-held shared reference 해제

### 14.3 channel lifetime

파괴 순서:

```text
stop accepting/connects
close all endpoints/QPs
drain deferred RX release commands
destroy CQs
join reactor threads
destroy channel
```

RX zero-copy가 활성화되면 external slice가 모두 unref될 때까지 channel과 RX mmap을 유지해야 한다. runtime shutdown은 outstanding external slice count가 0이 될 때까지 기다리거나 bounded shutdown 후 process exit 전용 leak-safe 정책을 명시해야 한다.

---

## 15. memory model

### 15.1 TX

- gRPC → DPUmesh copy 1회
- destination은 `dmesh_alloc()` memory
- post 이후 destination 접근 금지
- gRPC input slices는 callback 전까지 유지

gRPC chttp2가 frame을 DPUmesh TX ring 위에서 직접 만들게 하는 진짜 TX zero-copy는 chttp2 allocator/frame builder 변경이 필요하므로 초기 범위에서 제외한다.

### 15.2 RX copy baseline

- DPUmesh RX mmap → gRPC quota slice copy 1회
- copy 직후 RX credit release
- credit lifecycle이 gRPC parser lifetime과 분리됨
- 가장 안전하고 resource quota와 맞음

### 15.3 RX zero-copy optimization

가능한 형태:

```cpp
struct ExternalRxToken {
  shared_ptr<DmeshRuntime> runtime;
  int32_t rx_slot;
  MemoryAllocator::Reservation quota;
};

grpc_slice s = grpc_slice_new_with_user_data(
    const_cast<uint8_t*>(wc.buf), wc.len,
    DestroyExternalRxToken,
    token);
```

destroy callback은 직접 channel을 건드리지 말고 reactor에 release command를 넣는다.

필수 조건:

- quota reservation이 slice lifetime과 정확히 일치
- channel은 token보다 오래 삶
- QP close 후 release 가능하다는 native 계약 활용
- double release 금지
- gRPC가 slice를 오래 보유할 때 RX credit starvation metric 제공
- 4096 landing slot 전체가 잡히기 전에 backpressure/경보

copy baseline을 먼저 통과시키고 실제 profile에서 RX copy가 병목으로 확인된 뒤에만 구현한다.

### 15.4 Endpoint injection의 allocator 문제

`CreateChannelFromEndpoint`와 `PassiveListener::AcceptConnectedEndpoint`는 이미 만들어진 endpoint를 받으며 connection `MemoryAllocator`를 endpoint 생성자에 전달하지 않는다.

PoC 선택지:

- quota를 정확히 연결하지 않은 copied slice: 기능 검증에만 사용
- gRPC internal `ResourceQuota` header를 사용: version coupling이 크므로 제품 코드 금지
- external DPUmesh slice: 별도 quota accounting이 없으면 역시 PoC 한정

production에서는 반드시 EventEngine `Connect()`의 allocator와 `CreateListener()`의 allocator factory를 사용한다.

---

## 16. address, service name, authority

### 16.1 service routing

`dmesh_create_qp`는 service name이 필요하지만 EventEngine `Connect`의 직접 address 인자는 resolved sockaddr다.

권장 solution:

```text
ChannelArguments["grpc.dpumesh.service"] = k8s service name
```

`Connect()`는 이 값을 authoritative route key로 사용하고 sockaddr는 logging/telemetry용으로만 사용한다.

대안:

- 기존 `IP:port name svc` registry를 adapter가 별도로 parse
- DPUmesh에 public `create_qp_addr` 추가
- custom gRPC resolver가 `dpumesh:///service` URI를 synthetic address로 변환

첫 production 구현에서는 explicit channel arg가 가장 작고 안정적이다.

### 16.2 `GetPeerAddress`/`GetLocalAddress`

- client peer: gRPC가 resolve한 target sockaddr를 보존
- client local: synthetic AF_INET address 또는 DPUmesh pod/port를 표현한 stable sockaddr
- server local: `Bind()` address
- server peer: synthetic address

반환 reference는 Endpoint lifetime 동안 유효해야 한다.

### 16.3 TLS authority/SNI

synthetic IP를 인증서 이름으로 사용하면 안 된다.

```cpp
args.SetString(GRPC_ARG_DEFAULT_AUTHORITY,
               "greeter.default.svc.cluster.local");
```

필요하면 SSL target name/SNI override도 같은 실제 DNS name으로 설정한다. TLS는 Dmesh endpoint 위에서 gRPC handshaker가 수행하고 DPU L4에는 ciphertext byte stream으로 보인다.

---

## 17. hybrid EventEngine 설계

### 17.1 delegate할 method

다음은 wrapped built-in EventEngine에 그대로 위임한다.

- `Run(Closure*)`
- `Run(AnyInvocable)`
- `RunAfter(...)`
- `Cancel(TaskHandle)`
- `IsWorkerThread()`
- `GetDNSResolver()`
- non-DPUmesh `Connect()`
- 필요 시 non-DPUmesh listener

custom 구현:

- DPUmesh `Connect()`
- `CancelConnect()` handle routing
- DPUmesh `CreateListener()`

공식 wrapping example을 시작점으로 사용한다.

### 17.2 설치

전역 hybrid engine을 gRPC object 생성 전에 설치한다.

```cpp
auto engine = std::make_shared<DmeshEventEngine>(config);
grpc_event_engine::experimental::SetDefaultEventEngine(engine);
```

server는 명시적으로 설정할 수도 있다.

```cpp
builder.SetEventEngine(engine);
```

shutdown 때는 gRPC channel/server를 먼저 종료하고 engine/runtime를 나중에 정리한다. `SetDefaultEventEngine`을 사용했으면 process shutdown 절차에서 `ShutdownDefaultEventEngine()` 또는 `SetDefaultEventEngine(nullptr)` 규약을 따른다.

### 17.3 mixed TCP/DPUmesh

`grpc.dpumesh.service` arg가 없는 client Connect는 built-in engine에 위임한다.

server mixed-listener는 더 복잡하다. `CreateListener`가 먼저 호출되고 개별 address는 이후 `Bind`로 들어오므로 listener가 composite여야 한다.

초기 production 범위:

- DPUmesh-only server EventEngine
- TCP server는 별도 process/server 또는 built-in engine 사용

후속 composite listener:

- DmeshListener와 wrapped Listener 모두 소유
- Bind address마다 어느 listener에 보낼지 결정
- accept callback을 합침
- 두 listener shutdown이 모두 끝난 뒤 on_shutdown 한 번

---

## 18. DPU L4/L7 설정

### 18.1 첫 구현은 L4 passthru만

gRPC service는 다음에서 제외한다.

- `DPUMESH_PROXY_L7_SVC`
- `DPUMESH_PROXY_FRAME_SVC`

기본 `DPUMESH_PROXY=passthru`를 사용한다.

adapter 시작 시 실제 DPU config를 조회할 public API가 없으므로 배포 validation을 별도로 둔다. 가능하면 controller/registry에 transport mode를 넣고 startup에서 mismatch를 실패시킨다.

### 18.2 current L7 hook을 켜면 즉시 실패하는 이유

`doca/dpu_l7.c`는 다음 16-byte little-endian header만 파싱한다.

```text
[u32 magic][u32 seq][u32 payload_len][u32 aux]
```

gRPC client의 첫 bytes는 HTTP/2/TLS preface다. bench magic과 맞지 않아 `dmesh_l7_route()`가 `-1`을 반환하고 DPU proxy가 connection을 poison한다.

### 18.3 HTTP/2 frame parser만 추가해도 부족한 이유

frame을 stream ID별로 다른 backend에 보내려면 다음을 모두 처리해야 한다.

- connection preface와 SETTINGS fan-out/translation
- backend별 독립 HTTP/2 session
- client stream ID ↔ backend stream ID mapping
- HPACK decode/encode 또는 table synchronization
- connection 및 stream flow control
- HEADERS/CONTINUATION assembly
- request DATA와 trailers
- response HEADERS/DATA/trailers
- RST_STREAM/cancellation
- PING/GOAWAY
- TLS termination 또는 plaintext visibility

현재 L7 interface는 `head → total_len + backend`만 반환하고 reply는 passthru한다. 이 interface로 full HTTP/2 proxy를 표현할 수 없다.

따라서 DPU per-RPC gRPC LB는 별도의 대형 프로젝트로 취급한다.

### 18.4 현재 구조에서 올바른 load distribution

한 HTTP/2 channel/QP는 한 backend에 pin한다. 여러 backend를 쓰려면 여러 QP를 만든다.

방법 A: application channel pool

```text
K gRPC channels
K Dmesh QPs
application 또는 wrapper가 RPC별 channel 선택
```

방법 B: gRPC subchannels

```text
custom resolver가 같은 service에 K synthetic addresses 반환
gRPC round_robin policy
각 subchannel Connect → 새 Dmesh QP
DPU connection-level RR이 QP별 backend 선택
```

방법 B가 gRPC retry/LB model과 더 잘 맞지만 resolver integration 작업이 추가된다.

---

## 19. status/error 매핑

| DPUmesh 상황 | EventEngine status | 처리 |
|---|---|---|
| `dmesh_alloc` EAGAIN | error 아님 | park 후 retry |
| unknown service `ENOENT` | `UNAVAILABLE` | connect callback error, registry context 포함 |
| QP/port OOM | `RESOURCE_EXHAUSTED` | connect/endpoint 실패 |
| `dmesh_post_send` EINVAL | `INTERNAL` | adapter invariant violation로 log 후 close |
| `dmesh_post_send`/flush EBADMSG | `UNAVAILABLE` | transport close |
| remote `RECV_FIN` | `UNAVAILABLE` | buffered bytes 우선 전달 후 read error |
| local endpoint destructor | `CANCELLED` | pending callbacks 완료 후 close |
| stream ID changed in passthru | `UNAVAILABLE` | mixed byte stream을 노출하지 않고 close |
| listener stopped | `UNAVAILABLE` | 새 endpoint reject |
| connect cancellation | callback 없음 | `CancelConnect`가 true인 경우 계약 준수 |

에러 메시지에는 가능한 경우 service, local port, stream, errno를 넣되 payload나 credential을 log하지 않는다.

---

## 20. 구현 단계

### Phase 0: build 및 source pin

산출물:

- `integrations/grpc/CMakeLists.txt`
- gRPC v1.80.0 exact pin 문서
- hello-world proto generated build
- `libdpumesh.so` link 확인

합격:

- adapter가 public gRPC/EventEngine header와 public `dmesh.h`만으로 compile
- gRPC internal source header 의존 없음
- DPUmesh 기능을 사용하지 않은 normal gRPC example도 같은 build에서 동작

### Phase 1: fake transport 기반 Endpoint

구현:

- `DmeshApiOps`
- fake QP/CQ
- `ConnectionState`
- async-only `DmeshEndpoint::Read/Write`
- close/callback state machine

합격:

- unit tests 전부 통과
- ASAN/UBSAN/TSAN fake tests 통과
- outstanding Read/Write 위반 시 debug build가 즉시 발견
- callback double-call 0

### Phase 2: real native reactor

구현:

- process channel
- N CQ shards
- MPSC command eventfd
- CQ batch ordering
- TX copy, RX copy
- EAGAIN timer retry

합격:

- existing loopback/verbs validators 영향 없음
- real DPU byte stream 0 corruption
- QP churn 후 active QP/credit leak 없음

### Phase 3: Endpoint injection gRPC PoC

구현:

- client `CreateChannelFromEndpoint`
- server PassiveListener
- generated Greeter unary/streaming
- authority 설정

합격:

- unary/client-stream/server-stream/bidi 모두 통과
- large metadata/message fragmentation 통과
- TLS/mTLS 통과
- 현재 경로의 reconnect 미지원이 명시적으로 test/document됨

### Phase 4: full hybrid EventEngine

구현:

- delegate EventEngine
- Connect/CancelConnect
- Listener/Bind/Start/shutdown
- allocator/factory 사용
- normal `CreateCustomChannel` path
- server restart/reconnect

합격:

- injected-endpoint helper 없이 generated client/server 동작
- backend 없을 때 channel이 READY가 되지 않고 bounded failure
- server restart 뒤 새 QP로 reconnect
- cancellation race test 통과

### Phase 5: DPUmesh production 보강

구현:

- TX writable arm/wake
- nonblocking flush 또는 bounded async enqueue
- backend-death fail-close
- stream ID guard
- connection cleanup observability/idle lease

합격:

- EAGAIN load에서 reactor busy spin 없음
- pending write가 lost wakeup 없이 항상 진행 또는 명시적 failure
- backend kill 시 다른 backend에 midstream bytes 전달 0
- gRPC가 새 QP로 reconnect

### Phase 6: 최적화와 multi-QP LB

구현 후보:

- RX zero-copy external slice
- callback batching
- multiple CQs/reactors
- K subchannels/QPs
- channelz/telemetry info

최적화는 profile에서 병목이 입증된 항목만 적용한다.

---

## 21. unit test 계획

fake transport는 모든 사건 순서를 deterministic하게 제어해야 한다.

### 21.1 Read

- queued data가 있으면 `Read`가 true, callback 0회
- data가 없으면 false, RECV 후 callback 1회
- 여러 RECV completion이 순서대로 하나의 SliceBuffer에 들어감
- read hint보다 작은 data도 CQ drain 후 완료 가능
- data와 FIN이 함께 있으면 data 먼저, 다음 Read에서 error
- pending Read 중 local destructor → CANCELLED 정확히 1회
- pending Read 중 remote FIN → UNAVAILABLE 정확히 1회
- slice allocation/길이 불일치 시 WC release 후 endpoint failure
- Read callback이 즉시 새 Read를 호출하는 reentrancy
- 두 번째 outstanding Read 시 debug abort/assert

### 21.2 Write

- empty write 즉시 true
- one slice/one post
- multi-slice input
- `post_max-1`, `post_max`, `post_max+1`
- alloc EAGAIN 1회 후 성공
- EAGAIN이 영구 지속되다 endpoint close
- shared-pool EAGAIN에서 다른 QP ACK가 wake
- post EINVAL/EBADMSG
- flush failure
- pending Write 중 destructor
- callback에서 즉시 다음 Write
- 두 번째 outstanding Write 시 debug abort/assert
- bytes와 post order가 정확히 일치

### 21.3 CQ/accept/lifetime

- CONN_REQ와 첫 RECV가 같은 batch
- CONN_REQ, RECV, FIN이 같은 batch
- mid-batch close 요청이 batch 뒤 실행
- QP user_data가 close 전까지 valid
- listener stop과 CONN_REQ race
- endpoint destructor와 WC delivery race
- runtime shutdown과 external RX release race

### 21.4 EventEngine

- DPUmesh channel arg가 있으면 custom Connect
- 없으면 wrapped Connect
- Connect callback async exactly once
- CancelConnect true이면 callback 0회
- CancelConnect/complete race에서 허용된 결과만 나옴
- listener가 endpoint용/accept callback용 allocator를 각각 한 번 생성
- 두 allocator의 move/lifetime이 겹치지 않고 resource quota가 유지됨
- listener on_shutdown exactly once
- 모든 accept callback 반환 뒤 on_shutdown

---

## 22. gRPC end-to-end test 계획

### 22.1 RPC 형태

- unary
- client streaming
- server streaming
- bidi streaming
- concurrent unary over one HTTP/2 connection
- mixed RPC forms over one connection

### 22.2 payload 및 metadata

- 0-byte protobuf body
- 64 B
- 1 KB
- 8191/8192/8193 B
- 65535/65536/65537 B
- 1 MB
- configured max message size 부근
- metadata가 한 HTTP/2 HEADERS frame을 넘는 경우
- response trailers와 non-OK `grpc-status`

### 22.3 control plane/protocol

- deadline exceeded
- client cancellation → RST_STREAM
- server cancellation
- keepalive PING
- GOAWAY graceful shutdown
- max concurrent streams
- flow-control로 sender가 block되는 large streaming
- channel idle 후 재사용

### 22.4 security

- insecure
- TLS server auth
- mTLS
- correct authority/SNI
- wrong authority 실패
- certificate rotation은 후속 범위

### 22.5 장애

- unknown registry service
- service에 live backend 0개
- backend가 connection 전에 죽음
- HTTP/2 SETTINGS 전 backend 죽음
- unary request header 후 backend 죽음
- large DATA 중 backend 죽음
- response 중 backend 죽음
- server process restart
- DPU process restart
- FIN loss/forced process kill
- descriptor ring fault injection
- RX credit starvation
- QP/shared block exhaustion

모든 장애 test에는 bounded timeout이 있어야 한다. hang은 실패다.

---

## 23. real BlueField validation

### 23.1 배포 전 gate

- DPU binary와 host library commit/hash 기록
- live DPU environment 기록
- `DPUMESH_PROXY_L7_SVC`에 gRPC service가 없는지 확인
- `DPUMESH_PROXY_FRAME_SVC`에 gRPC service가 없는지 확인
- `$DPUMESH_SERVICE`, `$DPUMESH_PORT`, registry row 확인
- host/DPU clock/governor/pinning 기록

### 23.2 correctness gate

- 최소 100만 unary RPC, fail 0
- 각 streaming 형태 최소 10만 message, byte-exact
- message-size boundary 전부 0 corruption
- backend restart 100회, client hang 0
- connection churn 10만회, QP/port leak 0
- validators `loopback`, `verbs`, `stream`, `preload` 기존 회귀 0

### 23.3 backpressure gate

- `dmesh_get_tx_stats().grow_waits > 0`인 workload를 의도적으로 생성
- 그 상태에서 reactor CPU, retry latency, pending write count 측정
- TX notification 적용 전/후 비교
- writer가 영원히 pending인 경우 0
- shared block pool을 여러 QP가 경쟁해도 starvation bounded

### 23.4 resource gate

전후를 기록한다.

- active QPs
- active upstreams
- available ports
- RX credits
- TX shared blocks
- pending reads/writes
- external RX slices
- CQ eventfd wakes
- TX retry/wake counts
- FIN sent/received/lost

---

## 24. 성능 benchmark 설계

기존 pure-C L4 benchmark만으로 gRPC 결과를 예측하면 안 된다. 새 비교는 동일 proto, 동일 generated code, 동일 server business logic을 사용한다.

### 24.1 비교 경로

1. direct gRPC over kernel TCP
2. gRPC through Envoy HTTP/2 HCM
3. gRPC through DPUmesh EventEngine L4 passthru

추가 ablation:

- DPUmesh Endpoint RX copy
- DPUmesh Endpoint RX zero-copy
- Endpoint injection vs full EventEngine
- QP/channel 1, 4, 16
- DPU config 1/1, 2/2, 4/4
- TLS off/on

### 24.2 workload

- unary request 64 B, 1 KB, 8 KB, 64 KB, 1 MB
- reply 8 B 및 symmetric reply
- concurrency 1, 2, 4, 8, 16, 32, 64, 128
- one channel deep multiplexing
- multiple channels/subchannels
- streaming goodput
- application work 0, 5, 25, 50 µs
- backend N=1..3 이상
- intra-node와 가능하면 inter-host

### 24.3 측정값

- throughput RPC/s 및 bytes/s
- p50/p90/p99/p999
- client host CPU
- server host CPU
- Envoy host CPU
- DPU ARM CPU
- host+DPU total CPU/request
- context switches/syscalls
- TX copy bytes 및 time
- RX copy bytes 및 time
- `grow_waits`
- pending TX duration histogram
- CQ wake count
- reconnect count/time
- gRPC status별 failure count

### 24.4 통계

- warmup 제외
- 각 headline point 최소 10회
- median과 95% bootstrap CI
- one-rep 결과는 결론에 사용하지 않음
- raw CSV, live config, binary digest, commit, dirty state 보존
- generator가 포화되지 않았다는 별도 증거 확보

---

## 25. 현재 bench 결과 감사

### 25.1 실제 측정 내용

`bench/report/REPORT.md`의 headline은 다음 조건이다.

- real BlueField
- intra-node
- pure-C custom Greeter framing
- request N bytes → 8-byte reply
- Envoy L4 `tcp_proxy`
- DPUmesh L4 passthru
- headline은 thread=1, connection=1
- HTTP/2, protobuf, gRPC 없음

1 KB 결과:

| metric | direct TCP | TCP+Envoy L4 | DPUmesh native |
|---|---:|---:|---:|
| unloaded p50 | 23 µs | 62 µs | 114 µs |
| conc32 throughput | 0.933 Mrps | 0.409 Mrps | 0.195 Mrps |

native bench는 `dmesh_alloc()` buffer에 header/payload를 직접 생성한다. 즉 TX zero-copy다. gRPC adapter는 gRPC SliceBuffer에서 TX ring으로 copy해야 하므로 이 수치는 gRPC-over-DPUmesh 예상치가 아니라 낙관적 상한이다.

### 25.2 host CPU/request 주장 정정

보고서/README의 “DPUmesh host CPU/request가 Envoy보다 좋다”는 문장은 원자료와 맞지 않는다.

`bench/report/data/cpu.csv`, conc32:

- DPUmesh host efficiency: 0.435 `%core/Krps`
- Envoy path: 0.252 `%core/Krps`

config table 4/4:

- DPUmesh: 0.445
- Envoy: 0.240

DPUmesh absolute host CPU가 85.5%로 Envoy 101.3%보다 낮은 것은 처리량도 0.199 Mrps 대 0.402 Mrps로 절반이기 때문이다. 처리량으로 정규화하면 DPUmesh host cost/request가 더 높다.

“matched rate에서 DPUmesh가 낮다”는 별도 matched-rate measurement도 현재 report에 없다.

### 25.3 total CPU/request 주장 정정

`cpu_configs.csv` conc32의 host+DPU total efficiency를 같은 config의 Envoy path와 비교하면:

| DPU config | DPUmesh total `%core/Krps` | Envoy | ratio |
|---|---:|---:|---:|
| 4/4 | 2.664 | 0.240 | 11.10x |
| 2/2 | 1.837 | 0.253 | 7.26x |
| 1/1 | 2.032 | 0.254 | 8.00x |

따라서 “모든 config에서 약 7배”는 정확하지 않다. 대략 7.3~11.1배 범위다.

### 25.4 preload 설명 정정

report는 preload ablation이 “epoll hook 부재”로 막혔다고 적지만 source design은 다르다.

- shim이 private eventfd를 app fd에 `dup2`
- fd가 실제 kernel fd이므로 epoll/poll/select를 hook하지 않고 native로 사용
- dispatcher가 readability를 eventfd로 알림

실제 중요한 gap:

- write path가 `O_NONBLOCK`을 존중하지 않음
- EAGAIN 대신 nanosleep/retry로 block
- eventfd는 항상 writable이라 honest `EPOLLOUT`을 표현할 수 없음

gRPC POSIX EventEngine은 nonblocking socket I/O를 전제로 하므로 shim write가 EventEngine worker를 stall시킬 수 있다. 따라서 LD_PRELOAD는 낮은 pressure의 compatibility test에는 쓸 수 있어도 권장 gRPC transport가 아니다.

### 25.5 provenance와 반복 수

- report 평가 commit: `4beb0be`
- 평가 당시 dirty: uncommitted eval-harness edits 24 files
- current repository HEAD: `af19365`, clean
- busy-app와 N-pod sweep: 1 repetition
- publication/최종 제품 판단에는 최소 10회 필요

---

## 26. 관측성

최소 metric:

```text
grpc_dpumesh_connections_created_total
grpc_dpumesh_connections_closed_total{reason}
grpc_dpumesh_connect_failures_total{errno,status}
grpc_dpumesh_active_qps
grpc_dpumesh_pending_reads
grpc_dpumesh_pending_writes
grpc_dpumesh_tx_eagain_total
grpc_dpumesh_tx_retry_total
grpc_dpumesh_tx_retry_delay_us
grpc_dpumesh_tx_wake_total
grpc_dpumesh_flush_block_us
grpc_dpumesh_rx_bytes
grpc_dpumesh_tx_bytes
grpc_dpumesh_rx_copy_bytes
grpc_dpumesh_rx_external_slices
grpc_dpumesh_stream_change_total
grpc_dpumesh_fin_total{local,remote,error}
grpc_dpumesh_callback_cancel_total
```

log에는 connection ID, service, local port, lifecycle transition을 넣는다. payload, protobuf content, TLS credential은 log하지 않는다.

gRPC channelz와 연결하려면 후속 단계에서 `TelemetryInfo`를 구현한다. 초기 `nullptr`은 허용된다.

---

## 27. 주요 위험과 완화

| 위험 | 영향 | 완화 |
|---|---|---|
| EventEngine API 변경 | compile/ABI break | gRPC exact pin, upgrade CI |
| TX EAGAIN wake 없음 | spin 또는 latency | native writable notification |
| flush 장기 block | reactor shard stall | nonblocking try-flush |
| backend midstream repick | HTTP/2/TLS corruption | DPU fail-close, stream guard |
| RX zero-copy credit 보유 | DPA reverse stall | copy baseline, quota/credit metrics |
| Endpoint injection allocator 부재 | quota 위반 | PoC 한정, production full EE |
| CQ single consumer 위반 | race/corruption | QP ops를 reactor owner로 marshal |
| mid-batch QP destroy | UAF | deferred-close sweep |
| callback under lock | deadlock/reentrancy | callback detach 후 wrapped EE Run |
| one service/listener per process | multi-port 제한 | 명시적 validation, composite는 후속 |
| no idle reaper | stale DPU state | lease/reaper + keepalive + metrics |
| bench native zero-copy를 gRPC 결과로 오해 | 잘못된 성능 목표 | actual gRPC benchmark |
| current L7 hook을 gRPC service에 설정 | 즉시 poison | deployment validation |

---

## 28. Definition of Done

### 기능 완료

- generated gRPC C++ client/server가 socket API 직접 호출 없이 DPUmesh native QP 위에서 동작
- unary 및 모든 streaming mode 통과
- metadata, trailers, status, deadline, cancellation 통과
- TLS/mTLS 통과
- normal gRPC channel reconnect lifecycle 통과

### concurrency/lifetime 완료

- CQ당 poller 정확히 하나
- QP operation은 owner reactor에서만 실행
- outstanding Read/Write 계약 위반 없음
- callback double-call 0
- QP/channel/RX credit leak 0
- all shutdown/cancel race test 통과

### 장애 완료

- no-backend가 hang하지 않음
- backend midstream death가 다른 backend로 bytes를 이어 보내지 않음
- client가 새 QP로 reconnect 가능
- DPU/server restart가 bounded failure/recovery

### performance 완료

- TX notification 적용 후 pending write busy spin 없음
- actual gRPC vs direct/Envoy HCM 비교 완료
- 각 headline point 10 reps 이상
- host+DPU CPU/request를 포함한 결과 공개
- raw data와 provenance 보존

### 문서 완료

- public usage README
- supported gRPC version
- DPU deploy constraints
- TLS authority 설정
- known limitations
- troubleshooting/metrics
- upgrade test procedure

---

## 29. 구현자용 최종 체크리스트

구현 시작 전:

- [ ] gRPC v1.80.0 pin
- [ ] gRPC service가 DPU L7/frame service에서 제외됨
- [ ] `$DPUMESH_SERVICE`, `$DPUMESH_PORT`, registry 확인
- [ ] adapter는 public `dmesh.h`만 사용
- [ ] fake transport test seam 설계

Endpoint 구현:

- [ ] Read/Write 각 outstanding 하나
- [ ] callbacks exactly once
- [ ] all QP ops reactor marshal
- [ ] RX data before EOF
- [ ] EAGAIN은 fatal이 아님
- [ ] passthru stream ID latch
- [ ] close는 CQ batch 뒤
- [ ] callbacks outside locks
- [ ] allocator quota 준수

EventEngine 구현:

- [ ] tasks/timers/DNS delegate
- [ ] service channel arg
- [ ] Connect callback async
- [ ] CancelConnect race
- [ ] Listener allocator factory
- [ ] on_shutdown exactly once
- [ ] global engine init/shutdown order

DPU/core 보강:

- [ ] TX writable notification
- [ ] nonblocking flush
- [ ] backend-death fail-close
- [ ] idle cleanup/metrics

검증:

- [ ] fake unit tests + sanitizers
- [ ] injected endpoint PoC
- [ ] full EventEngine end-to-end
- [ ] all RPC forms
- [ ] TLS/mTLS
- [ ] failure matrix
- [ ] real BlueField stress
- [ ] actual gRPC benchmark 10+ reps

---

## 30. 한 문장 결론

gRPC C/C++는 socket fd 자체가 아니라 `EventEngine::Endpoint`의 ordered byte-stream 계약 위에 있으므로 DPUmesh native QP로 구현할 수 있다. 올바른 제품 구현은 chttp2나 protobuf를 고치는 것이 아니라 `DmeshEndpoint + reactor + hybrid EventEngine`을 만들고, DPUmesh 쪽에는 TX writable/nonblocking flush와 backend-death fail-close를 보강하며, DPU에서는 우선 L4 passthru를 사용하는 것이다.
