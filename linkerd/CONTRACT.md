# DPUmesh ↔ linkerd 포팅 — 인터페이스 계약 (제안)

두 코드를 하나로 합치기 위한 합의안. **DPUmesh의 기존 구현이 정본(normative)이고 포팅
쪽이 여기에 맞춘다**는 전제로 쓴다(포팅 담당자 제안에 따름).

- DPUmesh 쪽 근거 파일: `doca/comch_common.h`, `doca/dpa_common.h`, `doca/ring.h`,
  `doca/dpu_worker.c`, `doca/dpu_proxy.c`
- 포팅 쪽 현재 상태: `linkerd/port/DPUMesh/*.{c,h}`,
  `linkerd/port/linkerd2-proxy/linkerd/doca/src/{shim.c,driver.rs,io.rs}` @ `4f926826`

차이는 세 축이다: **① 스레드 모델 ② 프로토콜/파라미터 ③ 연산 처리 방식.**

---

## 0. 역할 분담

| | 담당 | 소유물 |
|---|---|---|
| 데이터패스 | DPUmesh (이쪽) | 호스트↔DPU 링크, 포워드/리버스 링, 크레딧, DPA EU, ARM 워커, SG-DMA |
| L7 프록시 | 포팅 (그쪽) | linkerd2-proxy, `linkerd/doca` crate, DOCA 어댑터 |
| 접합면 | 합의 대상 | 본 문서 §2의 wire ABI + §4의 함수 계약 |

`linkerd/port/DPUMesh/*.c`(포팅 쪽 자체 데이터패스)는 **최종적으로 DPUmesh 데이터패스로
대체**된다. 그 전까지는 ABI 정합의 기준점으로만 참조한다.

---

## ① 스레드 모델

### 현재 차이

| | DPUmesh (정본) | 포팅 현재 |
|---|---|---|
| 병렬 단위 | N DPA EU / K 포워드 링 / **A ARM 워커** (shared-nothing) | 워커 1개 (comch 서버 `DPUMesh0`) |
| 워커 소유물 | `px_conn` 해시 · `dpu_conntrack` · `px_engine`(doca_dma) · consumer PE | 단일 `struct objects` |
| 커넥션 상한 | 워커당 동적, `MAX_PODS` 기반 | `MAX_CONNS = 8` **컴파일 상수** |
| 루프 | epoll{PE fd, cross-worker eventfd, DMA notify} + 1 ms, park/wake 회계 | tokio `select!` + 1 ms safety tick |
| 동기화 | 워커 간 락 0, 크로스 워커는 bounded MPSC | 단일 태스크 `&mut objects` 독점 |
| 금지 사항 | busy-spin 금지, event-driven 유지 | — |

### 제안

1. **워커당 tokio `current_thread` 런타임 1개.** ARM 워커 스레드 위에서 돌린다.
   `multi_thread` 런타임은 금지 — `px_conn`/`ct`/`px_engine`이 `__thread px_cur_worker`에
   묶여 있어 work-stealing이 곧 데이터 레이스다.
2. **루프 주인은 DPUmesh 워커 루프.** 그러려면 `Driver::run()`의 `loop { … select! }`를
   **`Driver::step() -> bool`(진전 여부)로 분해**해야 한다. 워커 루프가 매 회전에 한 번
   호출하고, 반환값을 hot/park 판정에 합산한다.
   → 이 패치는 이쪽에서 만들어 보낼 수 있음. **누가 할지 결정 요청(§5-3).**
3. **`MAX_CONNS`를 런타임 파라미터로.** 슬롯 배열은 `attach` 시점에 크기를 받는다.
4. **tokio 자체 타이머 최소화.** 런타임이 독립적으로 깨어나면 워커가 park하지 못한다.
   1 ms 백스톱은 DPUmesh 워커 루프가 이미 갖고 있으므로 드라이버 쪽 `sleep(1ms)` arm은
   제거 대상이다.

```
ARM worker thread i  (i = 0 … A-1)
  ├ DPA 완료 소비 → px_parse → L4 / mock-L7            (기존)
  └ tokio current_thread RT ─ Driver::step() ─ linkerd outbound stack
```

---

## ② 프로토콜 / 파라미터 — 호스트↔DPU 연결고리

**여기가 포팅 담당자가 말한 "전에 구현했던 것들이랑 맞게 바꿀" 부분이다.**
정본은 전부 DPUmesh 쪽이고, `_Static_assert`로 오프셋이 고정되어 있다.

### 2.1 제어 메시지 집합

DPUmesh (`doca/comch_common.h`, 값은 wire ABI):

| 값 | 메시지 | 방향 | 의미 |
|---|---|---|---|
| 1 | `POD_REGISTER` | H→D | `{pod_id(-1이면 DPU 할당), service_id}` |
| 2 | `MMAP_EXPORT` | H→D | 영역 1개 export (`mmap_type`으로 구분) |
| 5 | `POD_ASSIGNED` | D→H | 할당된 `pod_id`, `landing_stripes` |
| 6 | `POD_INIT_RESULT` | D→H | READY / 실패 사유 (터미널) |
| 7 | `POD_UNREGISTER` | H→D | 라우팅 중지 + 원격 참조 quiesce 요청 |
| 8 | `POD_QUIESCED` | D→H | 원격 매핑 회수 완료, 호스트가 export 파괴해도 됨 |
| 9 | `REV_DOORBELL` | D→H | 리버스 링 wake |

포팅 현재는 3종뿐(`EXPORT_METADATA`, `EXPORT_DPA_COMP`, `EXPORT_RCV_RING`)이고, 등록·할당·
READY·teardown 배리어가 없다.

**조치(포팅):**
- 단일 `dmesh_export_metadata_msg`(ring+snd+rcv 디스크립터 512B×3을 한 메시지에 몰아넣음)를
  **`MMAP_EXPORT` 4회로 분할**한다. `mmap_type ∈ {DMA_BUFFER=1, DMA_RING=2,
  DMA_HOST_RX_BUFFER=3, DMA_REV_RING=4}`.
- 핸드셰이크 순서를 다음으로 맞춘다:

```
H→D  POD_REGISTER{pod_id=-1, service_id}
D→H  POD_ASSIGNED{pod_id, landing_stripes}
H→D  MMAP_EXPORT × (DMA_RING, DMA_BUFFER, DMA_HOST_RX_BUFFER, DMA_REV_RING×stripes)
D→H  POD_INIT_RESULT{READY}          ← 이 시점 전에 트래픽 금지
     ── 데이터 경로 ──
H→D  POD_UNREGISTER
D→H  POD_QUIESCED                    ← 이 시점 전에 호스트 mmap 파괴 금지
```

- `POD_INIT_RESULT=READY`는 **K개 포워드 링 + 호스트 TX/RX mmap + 모든 대상 DPA EU의 설치
  ACK + ARM egress 엔진이 전부 준비된 뒤**에만 나간다. 포팅의 "consumer 준비되면 바로 시작"과
  다르다.
- 메시지 첫 바이트가 타입이어야 한다(호스트가 `recv_buffer[0]`으로 디스패치).
  포팅은 `enum`(4B)을 첫 필드로 쓰는데 값이 256 미만이라 LE에서는 우연히 호환되지만,
  **명시적으로 `uint8_t type`으로 바꿀 것.**

### 2.2 포워드 디스크립터 (호스트 → DPU)

DPUmesh `struct dma_desc` — **64 B 고정, 오프셋 assert 있음**(`doca/dpa_common.h:218`):

| off | 필드 | 폭 | 비고 |
|---|---|---|---|
| 0 | `mmap` | 4 | `doca_dpa_dev_mmap_t` |
| 4 | `addr` | 8 | |
| 12 | `size` | 4 | **고정폭.** 포팅의 `size_t`(8B) 아님 |
| 16 | `seq` | 2 | per-conn 시퀀스 |
| 18 | `src_port` / 20 `dst_port` | 2+2 | `PORT_BLANK=0` → accept 큐 |
| 22 | `src_service` / 23 `dst_service` | 1+1 | `SVC_NONE` 가능 |
| 24 | `dst_pod_id` | 4 | `DMESH_POD_BLANK(-1)` → DPU가 `dst_service`로 라우팅 |
| 32 | `src_pod_id` | 4 | |
| 56 | `publish_seq` | 8 | **ticket+1. 발행 규약의 핵심** |

- 발행: MPSC. `dma_ring_try_claim()`이 티켓을 받고(가득 차면 역순 반납으로 gap 방지),
  페이로드를 쓴 뒤 `publish_seq = ticket+1`을 **release 스토어**로 publish.
- 링 컨트롤: `struct dma_ring_ctrl { volatile uint64_t consumer_head; }` 64 B 정렬.

포팅 현재: `{mmap, addr, size_t size, idx, reserved[35], volatile uint8_t valid}` +
`dma_ring_ctrl{producer_tail, consumer_head}`, SPSC, `valid` 플래그 방식.

**조치(포팅):** 64 B `dma_desc`로 교체, `valid` 플래그 → `publish_seq` 세대 발행으로 교체,
`producer_tail` 제거(MPSC 티켓이 대체), 라우팅 필드(`dst_service`/`dst_pod_id`/포트/서비스)를
채운다. `size`는 `uint32_t`.

### 2.3 리버스 완료 링 (DPU → 호스트)

DPUmesh `struct dmesh_rev_ring_entry` — **32 B, `publish_seq` 오프셋 24 고정**:

```
kind(1) reserved(7) payload(16) publish_seq(8)
kind ∈ { DONE=1, TX_ACK=2 }
  DONE   : {src_pod_id, src_service, dst_service, _pad, src_port, dst_port, seq, length, pos}  = 16B
  TX_ACK : {port, seq}                                                                          =  4B
ctrl(별도 캐시라인 128B): consumer_head(호스트가 배치 드레인 후 발행), arm_epoch(호스트가 블록 전 증가)
DMA_REV_RING_SIZE = 8192
```

포팅 현재: 역방향이 **두 가지 설계로 갈라져 있다.**
- 안 1: DPU가 `rcv_ring`+`tx_staging`을 export → **호스트가 두 번째 PCI function(94:00.0)에
  자체 DPA 스레드**를 띄워 당겨감
- 안 2 (backend): DPU의 `doca_dma`가 데이터 + 16 B `dmesh_push_desc{seq,pos,len}`를 push

**조치(포팅): 두 안 모두 폐기하고 DPUmesh 리버스 링을 쓴다.** 이유는 셋:
1. 호스트 측 **두 번째 PCI function + flexio 프로세스 요구가 사라진다**(안 1의 가장 큰 제약).
2. 크레딧 회수(`TX_ACK`)와 완료 통지(`DONE`)가 **이미 같은 링에 실려 있다** — 포팅 쪽에는
   크레딧 개념 자체가 없다.
3. `arm_epoch` + `REV_DOORBELL`로 호스트가 블록/웨이크할 수 있다(안 2는 busy-poll 전제).

### 2.4 플로우 아이덴티티

포팅 `struct dmesh_flow_id { src_ip, dst_ip, src_port, dst_port, mode, char src_workload[64] }`
는 linkerd가 `OrigDstAddr` / `Remote(ClientAddr)` / workload를 얻는 유일한 출처다.
DPUmesh는 `pod_id` / `service_id`로 라우팅하고 **IP:port 아이덴티티를 아직 안 싣는다.**

**조치(DPUmesh, 이쪽 몫):** 등록 경로에 아이덴티티를 확장한다.
- `POD_REGISTER`에 `src_workload[64]` 추가(또는 신규 `POD_IDENTITY` 메시지)
- 제어 평면에 `service_id ↔ ClusterIP:port` 표를 싣는다
- 포팅 쪽은 `conn_flow_get()`으로 **읽기만** 한다. `dmesh_flow_id`를 호스트 shim이 직접
  채우는 현재 방식은 폐기.

`mode`(CLIENT/BACKEND)는 DPUmesh에 대응 개념이 없다 → 항상 CLIENT로 고정하고 필드는 유지.

### 2.5 폭 / 패킹 규칙 (전역)

- wire 구조체에 `size_t`, `enum`, 포인터를 **그대로 싣지 않는다**. 전부 고정폭
  (`uint32_t`/`uint64_t`/`int32_t`).
- 모든 wire 구조체에 `_Static_assert(sizeof(...) == N)`와 핵심 오프셋 assert를 붙인다
  (DPUmesh가 이미 하는 방식). ABI 드리프트를 컴파일 타임에 잡는 유일한 장치다.
- 엔디안: 양쪽 다 LE 고정. 변환 없음.

### 2.6 상수 합의표

| 상수 | 정본 값 | 위치 |
|---|---|---|
| `DMA_REV_RING_SIZE` | 8192 | `comch_common.h:79` |
| `CC_SEND_TASK_NUM` | 8192 | `comch_common.h:26` |
| 포워드 디스크립터 | 64 B | `dpa_common.h` assert |
| 리버스 엔트리 | 32 B | `comch_common.h` assert |
| `MAX_PODS` | 16 | `object.h` |
| 슬롯/크레딧 | RX credit 프로토콜 | `dpu_proxy.c` |

---

## ③ 연산 처리 방식

| | DPUmesh (정본) | 포팅 현재 |
|---|---|---|
| 수신 버퍼 | pod 공유 staging 링, 도착 = extent, **custody로 회수** | 커넥션 전용 단일 staging + `recv_seg` 링 |
| 소비 확정 | SG-DMA 제출 시점 | 없음 (`conn_recv_release`가 no-op TODO) |
| 전달 | zero-copy SG gather, unit/lane 순서 보존 | `push_segment`로 포인터 전달 (동일 철학) |
| 송신 | 도착 staging만 소스 | `memcpy` → `tx_staging` → 128 B 정렬 8064 B 청킹 |
| 백프레셔 | RX credit + `px_stall` | `DmeshIo` 256 KiB tx 캡 |
| 에러 | `px_poison` / FIN 규칙 / 미소비 tail 드롭 | 슬롯 파킹 |

**조치:**

1. **`conn_recv_release()`를 실제로 구현해야 한다(양쪽 합의).** DPUmesh staging은 pod 공유
   링이라 custody를 반납하지 않으면 **linkerd가 읽는 중인 바이트가 재사용된다.** 포팅 쪽
   staging이 커넥션 전용이라 no-op으로 둔 것이 여기서는 곧 데이터 손상이다.
   → 계약: `l7`은 세그먼트를 다 읽으면 반드시 release한다. release 전까지 DPUmesh는 그
   바이트를 살려둔다.
2. **128 B 정렬 8064 B 청킹은 제거.** 그건 `producer_dma_copy`의 완료 규칙 때문에 생긴
   제약인데, DPUmesh egress는 SG-DMA라 임의 길이를 다룬다. 청킹은 오히려 디스크립터 수를
   늘린다.
3. **응답 바이트의 소스는 DPUmesh가 제공하는 ARM TX 아레나.** 포팅이 자기 `tx_staging`을
   들고 있을 필요가 없다 — `dmesh_l7_send(buf, len)` 한 번이면 된다.
4. **백프레셔 권위는 DPUmesh.** `DmeshIo` tx 캡은 아레나 잔량에 연동하거나 사실상 무력화한다.

---

## ④ 이쪽이 제공하는 함수 계약

포팅 쪽 `shim.c`는 아래 8개만 상대하면 된다. DOCA 리소스·PE·DMA·링은 전부 DPUmesh가 소유한다.

```c
/* DPUmesh → L7 (그쪽이 구현) */
int  l7_worker_attach(int worker_id, void *worker_ctx);
int  l7_worker_step(int worker_id);                       /* 진전 있으면 1 */
int  l7_conn_open(int worker_id, uint64_t conn, const struct dmesh_l7_flow *);
int  l7_conn_segment(int worker_id, uint64_t conn,
                     const uint8_t *base, uint32_t pos, uint32_t len);  /* zero-copy */
void l7_conn_eof(int worker_id, uint64_t conn);
void l7_conn_close(int worker_id, uint64_t conn);

/* L7 → DPUmesh (이쪽이 구현) */
int  dmesh_l7_send(int worker_id, uint64_t conn, const uint8_t *buf, size_t len);
void dmesh_l7_release(int worker_id, uint64_t conn, uint32_t pos, uint32_t len);
```

`driver.rs`의 기존 15개 FFI(`conn_recv_pop`, `conn_send`, `ctrl_advance`, …)는 이 8개 위에
얇은 어댑터로 매핑된다(pop 모델 ↔ push 모델 변환).

---

## ⑤ 결정 요청 (포팅 담당자에게)

1. **리버스 경로를 DPUmesh 리버스 링으로 통일해도 되는가?** (§2.3) — 그쪽 안 1(호스트 DPA)과
   안 2(push desc)를 둘 다 버리는 제안이라 가장 큰 변경이다.
2. **`MAX_CONNS = 8` 상수를 런타임 파라미터로 바꿀 수 있는가?** (§①-3)
3. **`Driver::run()` → `step()` 분해는 누가 하는가?** 이쪽에서 패치를 만들어 보낼 수 있다.
4. **인바운드(서버 사이드) 계획이 있는가?** 현재 아웃바운드 전용. 없으면 이번 통합은
   아웃바운드만으로 진행한다.
5. **플로우 아이덴티티를 DPUmesh 등록 경로에서 받는 것에 동의하는가?** (§2.4) — 동의 시
   호스트 shim의 `dmesh_flow_id` 생성 코드는 삭제 대상이 된다.

---

## ⑥ 작업 분담 제안

| | 담당 | 내용 |
|---|---|---|
| A | 포팅 | §2.1 핸드셰이크 7메시지 + `MMAP_EXPORT` 분할 |
| B | 포팅 | §2.2 64 B `dma_desc` + `publish_seq` 발행 전환 |
| C | 포팅 | §2.3 리버스 링 채택 (안 1/2 폐기) |
| D | DPUmesh | §2.4 아이덴티티 확장 (등록 메시지 + service↔ClusterIP 표) |
| E | DPUmesh | §④ 8함수 계약 구현 + ARM TX 아레나 + custody |
| F | 합의 후 | §①-2 `step()` 분해, `MAX_CONNS` 파라미터화 |

A~C와 D~E는 **서로를 기다리지 않는다.** 접점은 본 문서의 ABI뿐이므로 병렬로 진행한 뒤
`linkerd/port` 서브모듈을 pull해서 맞춘다.
