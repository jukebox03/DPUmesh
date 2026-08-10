# linkerd on DPUmesh — 통합 계획

DPUmesh의 mock L7(`doca/dpu_l7.c`)을 실제 L7 프록시(linkerd2-proxy)로 교체하기 위한
**DPUmesh 쪽 작업 계획**이다.

이 문서의 초점은 **"포팅된 linkerd 코드를 DPUmesh 안으로 어떻게 넣을 것인가"**다.
linkerd 포팅 자체를 완성하는 일은 이 문서의 범위가 아니며, 필요한 것은 §9에 계약으로
명시만 한다.

- 대상 코드: `/home/jukebox/DPUMesh` (`youngmin-kaist/DPUMesh`), 서브모듈
  `linkerd2-proxy` @ `4f926826` (브랜치 `dpumesh`)
- 대상 지점: `doca/dpu_proxy.c` 의 L7 경로, `doca/dpu_worker.c` 의 ARM 워커 루프

---

## 1. 스코프

### 넣는 것 (DPUmesh 안으로 들어오는 것)

| 구성요소 | 파일(포팅 저장소) | 성격 |
|---|---|---|
| `DmeshIo` / `DmeshIoHandle` | `linkerd/doca/src/io.rs` | DMA 버퍼 위의 `AsyncRead+AsyncWrite+Peek+PeerAddr` |
| 드라이버 | `linkerd/doca/src/driver.rs` | PE fd ↔ tokio, 슬롯 상태 diff → 이벤트, recv/send 펌프 |
| dmesh 억셉터 | `linkerd/app/src/dmesh.rs` | TCP 리스너 대신 `DmeshEvent`를 아웃바운드 스택에 태움 |
| `DmeshOrTcp` 커넥터 | `linkerd/app/outbound/src/tcp/connect.rs` | 업스트림을 TCP 대신 DMA 채널로 |
| 부팅 | `linkerd2-proxy/src/main.rs` | DOCA 초기화 → 드라이버 spawn → `spawn_dmesh` |

### 넣지 않는 것 (버리거나 대체하는 것)

- `linkerd/doca/src/shim.c` **전체** — youngmin의 C 데이터패스(comch server + 자체 DPA
  thread pool + 자체 doca_dma)에 붙은 하부. DPUmesh는 같은 것을 이미 갖고 있으므로
  이 파일은 **DPUmesh용 어댑터로 새로 쓴다**(§3).
- `DPUMesh/DPUMesh/*.c` 12개 파일 — 위와 같은 이유로 링크 대상에서 제외.
- 호스트 측 벤치 하네스(`host_worker.c`의 h2 bridge / backend bridge) — DPUmesh는
  호스트 측에 이미 `libdpumesh` + preload shim이 있다.

### 유지하는 것

- `doca/dpu_l7.c` mock 코덱은 **지우지 않는다.** 비교 baseline(`L4 / mock-L7 / linkerd-L7 /
  Envoy`)의 한 열로 계속 쓴다. linkerd 경로는 세 번째 코덱으로 **추가**한다.

---

## 2. 왜 `dmesh_l7_decode` 자리가 아닌가

| | `dmesh_l7_decode` 훅 (현재) | linkerd 아웃바운드 스택 |
|---|---|---|
| 바이트 | 무변형. 도착 staging에서 zero-copy SG-DMA | h2 디코드→재인코딩, mTLS 레코드 = **새 바이트** |
| 상태 | 프레임 간 상태 없음 | 커넥션/스트림/HPACK/윈도우 소유 |
| 실행 | 동기 pure function, 즉시 반환 | async. 디스커버리·정책·핸드셰이크 대기 |
| 출력 | `{total_len, cluster, host}` | 바이트 스트림 |

훅에는 출력 바이트를 낼 채널이 없고, 동기 반환이 불가능하며, 출력 길이 ≠ 입력 길이라
`px_build_range(c, frame_len, dst)`의 zero-copy 전제가 무너진다. **따라서 접합면은
`dmesh_l7_decode`가 아니라 `linkerd-doca` crate의 FFI 계약이다.**

이 판정의 실질적 이득: Rust 쪽 약 1,100줄(`io.rs` / `driver.rs` / `dmesh.rs` /
`connect.rs`)은 데이터패스 중립이라 **손대지 않는다.** 우리가 쓰는 것은 어댑터 한 겹이다.

---

## 3. 통합 계약 — DPUmesh가 제공해야 할 FFI

`driver.rs`가 `extern "C"`로 선언한 함수 전부. 왼쪽이 계약, 오른쪽이 DPUmesh에서의 구현
근거다. 새 파일 `integrations/linkerd/src/dmesh_shim.c`에 구현한다.

| FFI | 의미 | DPUmesh 대응 | 상태 |
|---|---|---|---|
| `ctrl_get_fd` / `ctrl_arm` / `ctrl_drain` / `ctrl_clear_and_drain` | 컨트롤 PE 이벤트 | 메인 스레드 ctrl PE (`dpu_worker.c:812~`) | 있음 |
| `data_get_fd` / `data_arm` / `data_clear_and_drain(budget)` | 데이터 PE 이벤트 | 워커 PE + DMA PE notify fd (`dpu_data_worker_main`) | 있음 |
| `ctrl_advance(out_state)` | 셋업 상태머신 1스텝 | pod register → mmap import → DPA ADD ACK 배리어 → `dma_ready` | 있음(의미 매핑 필요) |
| `max_conns()` | 슬롯 수 | `MAX_PODS` × 포트, 또는 워커별 conn 테이블 크기 | **재정의 필요**(§4.3) |
| `conn_state_get(slot)` | 슬롯 상태 | `px_conn` 존재/`dead`/`fin_pending` | 매핑 필요 |
| `conn_flow_get(slot,…)` | `FlowId{src,dst,workload}` | `src_pod`/`src_port`/`dst_service` → **IP:port 없음** | **신규**(§5.4) |
| `conn_mode_get(slot)` | client / backend | DPUmesh엔 개념 없음. 항상 client(0) 반환 | 상수 |
| `conn_staging_base(slot)` | 읽기용 staging 베이스 | `pod->dma_buffer` (+`local_mmap`) | 있음, **연속성 주의** |
| `conn_recv_pop(slot,&pos,&len)` | 완료된 수신 세그먼트 | 도착 window extent (`px_view` / `px_head_view`) | 있음, **custody 신규**(§5.1) |
| `conn_recv_release(slot,pos,len)` | 읽기 완료 통보 | 포팅 코드에선 no-op TODO. **DPUmesh에선 필수** | **신규**(§5.1) |
| `conn_send(slot,buf,len)` | 임의 바이트 송신 | `px_piece`가 도착 staging 전용 | **신규, 최대 작업**(§5.2) |
| `stats_get(...)` | 카운터 | `px_stat_*`, 워커 통계 | 있음 |

> `conn_recv_release`가 포팅 코드에서 no-op인 것은 그쪽 staging이 커넥션 전용
> 단일 버퍼여서다. DPUmesh의 staging은 **pod 공유 링이고 arrival custody로 회수**되므로,
> 이 함수를 실제로 구현하지 않으면 linkerd가 읽는 중인 바이트가 재사용된다.

---

## 4. 스레드 모델

### 4.1 현재 (before) — ARM worker thread `i` (총 A개, shared-nothing)

```
 host pod A ──PCIe/DPA──> K forward rings ──> [ ARM worker thread i ]
 ════════════════════════════════════════════════════════════════════
   epoll{ own PE fd, cross-worker eventfd, DMA notify fd } + 1ms
        │
        ▼
   px_process_forward            (dpu_proxy.c:1583)
        │
        ▼
   px_parse                      (dpu_proxy.c:1263)
        ├── L4 passthrough ────────────► px_ship_range
        │      conn-pinned LB
        └── mock L7 ───────────────────► px_build_range + 그룹화
               dmesh_l7_decode          (dpu_l7.c: 16B 길이만 읽음)
                                              │
                                              ▼
                                   px_engine (doca_dma, 워커당 1개)
                                   SG src = ★ arrival staging 전용 ★
                                              │
                                              ▼
                                        dst pod host memory

 이 스레드가 단독 소유: px_conn 해시테이블 · dpu_conntrack · px_engine
 메인 스레드: comch 제어(등록/해제), 도어벨
```

워커 루프 1회전: `dpu_progress_worker_pe()` → `dpu_worker_run()`(완료 큐 소비 →
`px_process_forward` → `px_parse`) → 진전 없으면 arm 후 `epoll_wait(1ms)`.

### 4.2 이후 (after) — 같은 스레드 위에 tokio 런타임 1개를 얹는다

**규칙: 워커 스레드 소유권을 절대 넘기지 않는다.** `px_conn` / `ct` / `px_engine`은
`__thread px_cur_worker`에 묶여 있으므로, 그 워커의 FFI는 그 스레드에서만 호출돼야 한다.

```
 host pod A ──PCIe/DPA──> K forward rings ──> [ ARM worker thread i ]
 ════════════════════════════════════════════════════════════════════
   epoll{ … } + 1ms      ← 루프 주인은 그대로 DPUmesh (§4.3 안 B)
        │                   1회전에 dmesh_linkerd_worker_step() 한 항 추가
        ▼
   px_process_forward
        │
        ▼
   px_parse
        ├── L4 ────────────────────────► px_ship_range          (그대로)
        ├── mock L7 ────────────────────► px_build_range         (그대로, baseline)
        └── linkerd ⟵ 신규 분기
             px_parse_linkerd
               │  push_segment(pos,len)   ← 복사 없음, extent 그대로
               ▼
   ┌───── tokio current_thread Runtime (같은 스레드! 새 스레드 0개) ─────┐
   │   DmeshIo ──► outbound stack: detect → discovery → LB → mTLS      │
   │   DmeshIo ◄── h2 재인코딩된 ★새 바이트★                            │
   └───────┬──────────────────────────────────────────┬────────────────┘
           │ conn_recv_release(pos,len)               │ conn_send(buf,len)
           ▼                                          ▼
     px_advance + arrival custody 반납        ARM TX arena (DPU-local mmap)
        ⟵ 신규: 읽기 완료까지 도착 버퍼 잡아둠      ⟵ 신규
                                                      │
                                                      ▼
                                   px_engine (doca_dma, 워커당 1개)
                                   SG src = arrival staging │ ARM TX arena
                                              ★ px_piece 소스 2종으로 확장 ★
                                                      │
                                                      ▼
                                                dst pod host memory
```

linkerd 워커 수 = **A**(ARM 워커 수). 커넥션은 이미 `dmesh_worker_for_port()`로 워커에
샤딩되므로, linkerd 태스크도 같은 샤딩을 그대로 물려받는다 → 크로스 스레드 동기화 0.

### 4.2.1 이 그림이 말하는 것

| | 값 |
|---|---|
| 새로 만드는 스레드 | **0개** — 워커 스레드마다 `current_thread` 런타임 1개 (multi_thread 금지) |
| 바뀌는 소유권 | **없음** — `px_conn`/`ct`/`px_engine`은 계속 그 워커 전용, 락 0개 |
| linkerd 워커 수 | **A**. 커넥션 샤딩은 `dmesh_worker_for_port()` 그대로 상속 |
| `doca/` 안에서 바뀌는 파일 | **3개** — `dpu_proxy.c`, `dpu_worker.c`, `dpu_main.c` (§10) |
| 새로 짜는 것 | `px_parse_linkerd` · `px_ship_arm_bytes` · TX arena · FFI 어댑터 `dmesh_shim.c` |
| 없애는 것 | 없음. mock L7은 baseline으로 유지, `DPUMESH_PROXY_LINKERD_SVC` 비면 완전 off |

**핵심은 두 화살표다.**

- `push_segment` ↑ — 도착 window를 **연속화 없이** 그대로 넘겨 zero-copy를 유지한다.
  대가로 custody를 linkerd 읽기 완료까지 연장해야 한다(§5.1).
- `conn_send` ↓ — linkerd가 만든 **새 바이트**를 보낼 길이 지금은 없다. `px_piece`의
  소스를 arrival staging 외에 ARM TX arena까지 확장하는 것이 최대 신규 구현이다(§5.2).

### 4.3 루프 소유권 — 두 안 중 택일 (**결정 필요**)

**안 B (권장): DPUmesh 워커 루프가 주인, tokio는 펌프된다.**

```c
while (!stop) {
    did = dpu_progress_worker_pe(...);
    run = dpu_worker_run(...);              /* L4 / mock-L7 경로 */
    lnk = dmesh_linkerd_worker_step(...);   /* 신규: 런타임 1스텝 + pump_recv/pump_send */
    if (did || run || lnk) continue;
    /* arm → recheck → epoll_wait(1ms) */
}
```

- 장점: 기존 park/wake 회계, cross-worker MPSC, 1ms keepalive, busy-spin 금지 원칙이
  그대로 유지된다. 워커의 hot/park 판정에 linkerd 진전을 그냥 한 항으로 더한다.
- 비용: `driver.rs::run()`의 `loop { … select! }`를 그대로 못 쓴다. `step()` 형태로
  쪼개는 수정이 필요하다(→ §9-c, 우리 몫).

**안 A: tokio 루프가 주인.** `driver.rs::run()`을 그대로 쓰고 DPUmesh 워커 루프를
tokio 태스크로 옮긴다. 수정량은 적지만 park/wake·keepalive·완료 예산 제어를 tokio에
넘기게 되어 [[busy-spin 금지 / event-driven 유지]] 원칙과 충돌할 위험이 있다. fallback.

**안 C: 별도 프로세스 + UDS/shm.** 정합성 확인 전용 우회로. 성능 주장은 성립하지 않으나
M2 이전 리스크 헤지로만 사용한다.

### 4.4 누가 `main`인가 (**결정 필요**)

포팅 코드는 `linkerd2-proxy/src/main.rs`가 main이고 C가 라이브러리다. DPUmesh는 반대여야
한다: `doca/dpu_main.c`가 DOCA device / DPA / comch / PE / 워커 스레드 소유권을 이미
갖고 있고, 그 소유권을 Rust로 넘기면 N/K/A 구조 전체가 흔들린다.

→ **linkerd를 `staticlib` crate로 빌드해 DPU 바이너리에 링크한다.** `main.rs`의 2단계
부팅을 라이브러리 진입점 2개로 바꾸는 것이 우리 몫의 수정이다:

```c
int  dmesh_linkerd_init(const struct dmesh_linkerd_cfg *cfg);   /* 프로세스 1회: 설정/신원 로드 */
int  dmesh_linkerd_worker_attach(int worker_id, void *objs);    /* 워커 스레드에서 런타임+driver 생성 */
int  dmesh_linkerd_worker_step(int worker_id);                  /* 1스텝. 진전 있으면 1 */
void dmesh_linkerd_worker_detach(int worker_id);
```

---

## 5. 데이터 경로 삽입 지점 (코드 레벨)

### 5.1 수신: `px_parse` 분기 추가 + custody 연장

`doca/dpu_proxy.c:1263 px_parse()`는 현재 2갈래다.

```c
if (c->is_l7) { px_parse_l7(objs, c); return; }   /* mock 코덱 */
… L4 passthrough …
```

여기에 3번째 갈래를 추가한다.

```c
if (c->is_linkerd) { px_parse_linkerd(objs, c); return; }
```

`px_parse_linkerd()`가 하는 일:

1. `px_view()`로 도착 window의 연속 extent를 얻는다(프레임 파싱 없음).
2. 그 extent를 `(pos, len)`으로 슬롯의 `DmeshIoHandle::push_segment`에 넘긴다.
3. **`px_advance()`를 즉시 호출하지 않는다.** L4/mock-L7은 SG-DMA 제출과 동시에 소비가
   확정되지만, linkerd는 나중에 읽는다. 따라서 arrival custody를 linkerd 읽기 완료까지
   연장하고, `conn_recv_release(slot,pos,len)`에서 `px_advance` + custody 반납을 한다.
4. custody가 한계에 닿으면(= linkerd가 안 읽음) `px_stall(c)`로 워커 백프레셔를 건다.

> 주의: `px_head_view()`는 `PX_HEAD_MAX`까지만 연속을 보장하고, `px_view()`는 하나의
> staging extent 잔여만 준다. `DmeshIo`는 세그먼트 리스트를 받으므로 **연속화(linearize)
> 필요 없음** — extent를 있는 그대로 push하면 된다. 이게 zero-copy를 유지하는 지점이다.

### 5.2 송신: ARM-origin egress unit (**최대 신규 구현**)

현재 SG 소스는 도착 staging 전용이다.

```c
/* dpu_proxy.c:103 */ struct px_piece { struct px_arrival *arr; int32_t pod_idx;
                                        uint32_t staging_off, len; … };
/* dpu_proxy.c:1951 */ addr = (uint8_t *)src_pod->dma_buffer + p->staging_off;
                        doca_buf_inventory_buf_get_by_addr(eng->inv, src_pod->local_mmap, addr, …)
```

linkerd가 만든 바이트는 여기에 못 들어간다. 필요한 변경:

1. **워커별 TX 아레나**: DPU-local `doca_mmap` 하나(워커당 N MiB, 슬랩/링). rev-ring
   엔트리·credit이 이미 ARM-local 메모리를 DMA하고 있으므로(`px_rev_stage_append`,
   `px_op.kind = 1/2`) 메커니즘 자체는 검증되어 있다.
2. **`px_piece`에 소스 종류 추가**: `arr`(도착 custody) | `tx_slab`(아레나 custody).
   `px_piece.arr`가 NULL이면 아레나 chunk를 참조하고, DMA 완료 콜백에서 chunk를 반납한다.
3. **`px_ship_arm_bytes(objs, c, buf, len, dst_pod)`** 신규: 아레나에 복사 → unit 구성 →
   `px_enqueue_unit`. 기존 `px_build_range` / `px_ship_range`는 건드리지 않는다.
4. `conn_send()`는 이 함수를 호출하고, 아레나가 마르면 **부분 수락**(accepted < len)을
   반환한다 — `driver.rs::pump_send`가 이미 `untake_tx`로 나머지를 되돌린다.

> 이 지점이 [[claim-then-abandon]] 계열 결함이 재발하기 쉬운 곳이다. chunk를 claim한 뒤
> 에러 경로에서 `continue`로 빠지면 아레나가 영구 누수된다. 반납 경로를 완료 콜백 한
> 곳으로 모을 것.

### 5.3 라우팅 소유권 이전

linkerd 경로에서는 **DPUmesh가 백엔드를 고르지 않는다.** `px_route_message()` /
`px_resolve_backend()`는 우회하고, linkerd `DmeshOrTcp`가 고른 대상이 `conn_send`의
목적지가 된다. 즉 이 경로에서 DPUmesh는 L4 트랜스포트로만 동작한다.

- 업스트림 표현: linkerd가 고른 backend를 DPUmesh conn/upstream으로 매핑해야 한다.
  기존 `dpu_upstream_create(ct, client_pod, client_port, dst_pod, codec_id, worker, n)`에
  `codec_id = PX_CODEC_LINKERD`를 추가해 재사용한다.
- 응답 경로: 백엔드 pod → DPU 도착 → 같은 워커의 linkerd 태스크(응답 방향 `DmeshIo`) →
  `conn_send`로 클라이언트 pod. 즉 **왕복 2회 모두** 5.1/5.2 경로를 탄다.

### 5.4 아이덴티티 브리지

linkerd는 `OrigDstAddr`(소켓 주소)와 workload 문자열을 요구한다(`dmesh.rs`의
`DmeshTarget`). DPUmesh는 `service_id` / `pod_id`로 라우팅한다.

- 제어 평면(`design/CONTROL.md` 경로)에 **`service_id ↔ ClusterIP:port`,
  `pod_id ↔ workload 이름**을 싣는다. 등록 메시지에 필드를 추가하는 확장이며 신규 설계는
  아니다.
- `conn_flow_get`은 이 표를 조회해 `FlowId`를 채운다. dst가 없으면 그 커넥션은 linkerd
  경로로 보내지 않고 L4로 폴백한다(안전한 기본값).

### 5.5 백프레셔 일원화

두 겹이 존재한다: DPUmesh RX credit / `tx_gate`, 그리고 `DmeshIo`의 256 KiB tx 캡
(`io.rs: DEFAULT_TX_CAPACITY`).

- **권위는 DPUmesh RX credit.** `DmeshIo` tx 캡은 그보다 크게 잡아 사실상 무력화하거나,
  아레나 잔량에 연동해 동적으로 낮춘다.
- 수신 방향 권위는 §5.1의 custody 한계다. 두 방향 모두 "DPUmesh가 최종 결정"으로 통일.

### 5.6 커넥션 수명

| DPUmesh | linkerd |
|---|---|
| `px_conn` 생성(첫 도착) | `DmeshEvent::ConnReady(slot, flow)` |
| `px_try_fin` / `fin_pending` | `DmeshIoHandle::close_rx()` → 스택이 EOF |
| linkerd 스택 종료(`DmeshIo` drop) | `tx_finished()` → FIN unit 발행 |
| pod 소실 / `dma_ready` 해제 | `ConnClosed` → 태스크 종료, 슬롯 회수 |

`px_try_fin`은 "미소비 tail = 잘린 unit이므로 드롭" 규칙을 갖는다. linkerd 경로에서는
미소비 tail이 **아직 안 읽힌 바이트**일 수 있으므로, FIN 처리 전에 custody가 0인지
확인해야 한다.

---

## 6. 활성화 / 게이팅

기존 `DPUMESH_PROXY_L7_SVC`(`dpu_proxy.c:2917`)와 같은 방식으로 서비스별 csv:

```
DPUMESH_PROXY_LINKERD_SVC=<service ids>     # 기본 비어 있음 = 완전 off
```

- 비어 있으면 linkerd 런타임 자체를 attach하지 않는다 → 기존 L4/mock-L7 경로와
  바이너리·성능이 동일해야 한다(회귀 게이트).
- `svc_l7`와 `svc_linkerd`가 같은 서비스에 동시에 켜지면 시작 시 에러로 거부.

---

## 7. 마일스톤

| | 내용 | 완료 기준 |
|---|---|---|
| **M0** | 빌드 합류. linkerd를 `staticlib`로, DPU 바이너리에 링크. aarch64 툴체인 / DOCA pkg-config / jemalloc. `dmesh_linkerd_init` stub만. | DPU 바이너리 링크 성공, `DPUMESH_PROXY_LINKERD_SVC` 비었을 때 기존 벤치 수치 회귀 없음 |
| **M1** | 어댑터 shim 수신 반쪽. §3의 FFI 중 ctrl/data/advance/state/flow/staging/recv_pop/recv_release + `conn_send`는 stub. §5.1 custody. | DMA로 온 h2 요청을 linkerd가 파싱하고 백엔드 선택까지 로그로 확인 |
| **M2** | §5.2 ARM-origin egress + `conn_send` 실동작. §5.3 업스트림 매핑. | 1커넥션 end-to-end: 요청 → linkerd → 백엔드 pod → 응답 회귀. fail 0 |
| **M3** | 워커 A개 다중화, §5.4 아이덴티티, §5.5 백프레셔 일원화, FIN 규칙. | 다중 커넥션·다중 워커에서 fail/reorder 0, 장시간 안정 |
| **M4** | 측정. `L4 passthrough / mock-L7 / linkerd-L7 / Envoy 사이드카` 4열. | 코어 귀속 cgroup `usage_usec` 기준 표 + 그림 |

---

## 8. 리스크

1. **ARM CPU가 새 천장.** 이미 DPU ARM 코어가 host 코어와 맞먹고, 같은 트랜스포트가
   native 232 ns → gRPC 74.5 µs로 L7 스택이 비용을 지배한다. ARM에서 h2 + mTLS를 돌리면
   여기가 병목이 될 공산이 크다. M4에서 이걸 **결과로** 보고할 준비를 하고 시작한다.
2. **custody 연장이 도착 링을 마르게 함.** linkerd 처리 지연이 곧 forward ring 정체로
   전파된다. `px_stall` 백프레셔가 제대로 걸리는지 M1에서 먼저 확인.
3. **tokio 런타임과 워커 park/wake의 상호작용.** 안 B에서 런타임이 자체 타이머로
   깨어나면 워커가 park하지 못한다. `Runtime`의 타이머 사용을 최소화하고, 진전 신호를
   워커 루프의 hot 판정에 통합한다.
4. **아레나 누수 / 이중 반납** — §5.2 주석 참조.

---

## 9. 남의 몫 (명시만 하고 이 계획에서 구현하지 않음)

포팅 코드가 이 계약대로 동작해야 한다는 사실만 기록한다.

- **(a) 인바운드 미지원.** DMA 경로는 아웃바운드 스택에만 연결되어 있다. 서버 사이드
  프록시가 필요하면 포팅 쪽 작업이다.
- **(b) `MAX_CONNS = 8` 고정** (`driver.rs`). DPUmesh는 워커당 그보다 많은 커넥션을
  다루므로 상수화 해제 또는 워커당 driver 다중화가 전제된다.
- **(c) `Driver::run()`의 루프 형태.** 안 B를 택하면 `step()` 분해가 필요하다 — 이건
  우리가 수정한다(예외).
- **(d) `backend::take()` 일회성 레지스트리.** 서비스당 채널 1개만 꺼낼 수 있어
  재연결/다중 백엔드가 안 된다.
- **(e) staging 흐름 제어 TODO.** `conn_recv_release` no-op, tx_staging 랩 시 덮어쓰기
  가능. DPUmesh 어댑터에서는 §5.1/§5.5로 우리가 메운다.
- **(f) `Peek` 미지원**(0 반환) → 프로토콜 감지가 `read_buf` + `PrefixedIo` 폴백만 사용.

---

## 10. 파일 배치 (예정)

```
integrations/linkerd/
  PLAN.md                  ← 이 문서
  src/dmesh_shim.c         ← §3 FFI 어댑터 (DPUmesh objects ↔ linkerd-doca 계약)
  src/dmesh_shim.h
  src/px_linkerd.c         ← §5.1 px_parse_linkerd, §5.2 px_ship_arm_bytes
  crate/                   ← staticlib 래퍼 (linkerd2-proxy를 라이브러리로 감쌈)
  bench/                   ← M4 측정 하네스 (integrations/grpc/bench 구조를 따름)
```

`doca/` 안에서 바뀌는 것은 다음 3곳뿐이다.

| 파일 | 변경 |
|---|---|
| `doca/dpu_proxy.c` | `px_parse()`에 `is_linkerd` 분기, `px_piece` 소스 종류, `PX_CODEC_LINKERD`, `svc_linkerd` 환경변수 |
| `doca/dpu_worker.c` | 워커 루프에 `dmesh_linkerd_worker_step()` 한 항, attach/detach |
| `doca/dpu_main.c` | 프로세스 1회 `dmesh_linkerd_init()` |
