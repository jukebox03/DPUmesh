# linkerd on DPUmesh — 구현 계획

DPU ARM에서 linkerd2-proxy를 DPUmesh 데이터패스 위에 올린다. 지금 `doca/dpu_l7.c`가
자리를 차지하고 있는 L7 연산을 실제 L7 프록시로 대체하는 작업이다.

이 문서만 읽고 구현할 수 있도록 쓴다. 대상 코드는 `linkerd/port` 서브모듈
(`youngmin-kaist/DPUMesh` + 중첩 `linkerd2-proxy`, 브랜치 `dpumesh`)이다.

---

## 0. 한 장 요약

| | |
|---|---|
| **핵심 판단** | linkerd는 종료형(terminating) 프록시라 `dmesh_l7_decode` 훅 자리에 들어갈 수 없다. 접합면은 **8함수 FFI 계약**이다 |
| **스레드** | 새 스레드 0개. ARM 워커마다 tokio `current_thread` 런타임 1개를 얹는다 |
| **루프 주인** | DPUmesh 워커 루프. linkerd 드라이버는 `step()`으로 펌프된다 |
| **프로세스 주인** | `doca/dpu_main.c`. linkerd는 staticlib으로 링크된다 |
| **최대 신규 구현** | ARM TX 아레나 — linkerd가 만든 새 바이트를 DMA로 보낼 소스 |
| **필수 신규 계약** | `release` (custody 반납). 없으면 데이터 손상 |
| **최종 구조** | 데이터 평면은 **하나**(DPUmesh L4). 그 위에 얹는 L7 관여 정도가 **3모드** |

---

## 1. 구조 — 데이터 평면 하나 + L7 관여 3모드

데이터 평면은 **하나뿐이다**: DPUmesh L4(도착 staging → SG-DMA → 목적지, ARM 복사 0).
그 위에 **L7이 얼마나 관여하는가**만 서비스별로 달라진다.

| 모드 | L7이 하는 일 | 바이트가 linkerd를 지나나 | ARM 복사 |
|---|---|---|---|
| **`decision`** | 인가 · 디스커버리 · 백엔드 선택 · 신원 · 텔레메트리 | ✗ (**질문만** 오감) | **0** |
| **`opaque`** | 위 + **mTLS** | ✓ | 2 |
| **`l7`** | 위 + HTTP 라우팅 · 재시도 · 타임아웃 | ✓ | 2 |

**`decision`이 기본이다.** `opaque`는 암호화가 필요한 구간(노드 간)에서만, `l7`은 요청 단위
제어가 필요한 서비스에서만 쓴다.

### 왜 단일 경로로 통합하지 않는가

기능적으로는 linkerd 하나로 `decision`이 하는 일까지 전부 덮을 수 있다(opaque 모드). 그러나
그렇게 하면 데이터 평면의 성질을 잃는다: 복사 0회, ARM CPU ≈ 0, DPA+SG-DMA 하드웨어 경로.
유저스페이스 프록시가 바이트를 만지는 순간 그 이점은 존재하지 않는다.

사이드카 메시에는 `decision` 모드가 **존재할 수 없다.** opaque 모드조차 유저스페이스 홉을
강제하기 때문이다. DPU가 데이터 평면을 소유하기 때문에만 가능하다.

### 폴백 — L7 계층이 없을 때

`decision`은 연결 수립 시 L7에 질의한다. L7 계층이 attach되지 않았거나 응답하지 못하면
데이터 평면이 **자체 LB(라운드로빈 + 커넥션 고정)로 fail-open** 한다. 이것은 선택 가능한
모드가 아니라 **가용성 폴백**이다 — 정책 없이 도는 상태이므로, 폴백에 머무는 동안은
카운터로 관측되어야 한다.

게이트가 비어 있는 상태(§8.3)도 같은 경로다. 기존 동작이 곧 폴백 동작이다.

### `decision` — "결정과 이동의 분리"

opaque 모드에서 linkerd가 실제로 하는 일을 분해하면 **전부 연결 수립 시점의 1회성 결정**이다.

| 하는 일 | 시점 | 바이트당 비용 |
|---|---|---|
| 인가 (allow/deny) | 연결 수립 | 0 |
| 디스커버리 + 백엔드 선택 | 연결 수립 | 0 |
| 신원 확인 | 연결 수립 | 0 |
| 텔레메트리 | 집계 | 카운터 |
| mTLS | 바이트마다 | **있음** (`opaque`/`l7`에서만) |

mTLS를 빼면 **바이트당 하는 일이 0**이다. 그러므로 바이트를 통과시킬 이유가 없다.

```
연결 수립 (1회)
   DPUmesh ──"pod P(워크로드 W) → 서비스 S : 허용? 어느 백엔드?"──► linkerd
   DPUmesh ◄──"허용, 백엔드 B"───────────────────────────────────┘

그 이후 모든 바이트
   staging ──SG-DMA──► 백엔드 B          복사 0, ARM CPU ≈ 0
                                          linkerd 관여 없음

연결 종료
   DPUmesh ──"bytes in/out, 기간, 종료 사유"──► linkerd (텔레메트리·부하 인지)
```

**linkerd = 제어 평면, DPUmesh = 데이터 평면.** 사용자에게는 정책·신원·관측이 전부 살아
있는 하나의 메시이고, 바이트만 하드웨어 경로로 흐른다.

### `decision`의 질의 경로 — 두 안

**안 ① 커넥터 훅 재사용 (신규 API 없음)**
포팅 코드의 `DmeshOrTcp::call(ep)`가 이미 `Remote(ServerAddr(addr))`를 받는다. 이것이
밸런서가 고른 결과다. IO를 돌려주는 대신 "이 연결은 백엔드 B로 splice"를 통보한다.
- 위험: 밸런서가 **커넥션 수명을 부하 지표로 삼는다.** 즉시 닫히는 가짜 연결을 주면
  least-request/EWMA 인지가 왜곡된다.

**안 ② 질의 API 추가 (의미상 깨끗, 협업자 몫)**
```
l7_resolve(target, client_identity) -> { allow, endpoint, identity }
l7_report(conn_id, bytes_in, bytes_out, duration_ns, reason)
```
`l7_report`가 밸런서에 부하를 되먹이므로 안 ①의 위험이 사라진다.

**권장: 안 ②.** 협업자와 조기에 합의한다. 합의 전에는 안 ①로 프로토타입한다.

### `decision`이 깨뜨리는 것 (명시할 것)

1. 연결 도중 정책이 바뀌어도 splice된 연결은 모른다. Envoy tcp_proxy도 대체로 같다.
2. opaque로 지정된 서비스가 실제로는 HTTP였을 때 잡아줄 수단이 없다.
3. 밸런서 부하 인지는 `l7_report` 없이는 무의미해진다.

### 데이터 평면 단독으로는 하지 않는 일

`decision` 모드가 존재해야 하는 이유다. 현재 `doca/dpu_proxy.c`가 제공하지 않는 것:

| 기능 | 현재 |
|---|---|
| 인가 정책 | 없음 |
| 능동 헬스체크 · outlier detection | 없음 (`pod_data_ready` = 등록/DMA 상태만) |
| 서킷 브레이킹 · 연결 수 제한 | 없음 |
| 연결 실패 시 다른 엔드포인트로 failover | 없음 (고정 백엔드 사망 = 스트림 종료) |
| connect/idle 타임아웃 | 없음 |
| 커넥션별 텔레메트리 · access log | 집계 카운터만 |
| 고급 LB (P2C/EWMA, least-request, consistent hash, locality) | 라운드로빈 + 커넥션 고정만 |
| 프로토콜 감지 | 없음 |
| mTLS | 없음 (설계상 제외, §7) |

반대로 데이터 평면이 일반 프록시보다 엄격한 것: 레인 순서 보존, RX 크레딧 백프레셔, custody 회계,
pod 드레인 배리어(UNREGISTER→QUIESCED + DPA DEL_ACK 펜스), DMA 결함 복구.

> **측정 보고 시 이 표를 근거로 비교 범위를 못 박을 것.** 결론 문장은 "DPUmesh가 Envoy보다
> 빠르다"가 아니라 **"L4 전송·스위칭만 비교하면 N배이며 정책 계층은 포함되지 않았다"** 여야
> 한다. 빠진 기능 대부분은 연결 시점·백그라운드 비용이라 정상 상태 처리량에 영향이 없지만,
> 텔레메트리와 mTLS는 요청당 비용이므로 `Envoy permissive`가 주 비교 대상이다.

---

## 2. 왜 `dmesh_l7_decode` 자리가 아닌가

| | `dmesh_l7_decode` 훅 | linkerd 아웃바운드 스택 |
|---|---|---|
| 바이트 | 무변형. 도착 staging에서 zero-copy SG-DMA | h2 디코드→재인코딩 = **새 바이트** |
| 상태 | 프레임 간 상태 없음 | 커넥션/스트림/HPACK/윈도우 소유 |
| 실행 | 동기 pure function | async. 디스커버리·핸드셰이크 대기 |
| 출력 | `{total_len, cluster, host}` | 바이트 스트림 |

훅에는 출력 바이트를 낼 채널이 없고, 동기 반환이 불가능하며, 출력 길이 ≠ 입력 길이라
`px_build_range(c, frame_len, dst)`의 zero-copy 전제가 무너진다.

**따라서 접합면은 FFI 계약이다.** 이 판정의 실질적 이득: 포팅 쪽 Rust 약 1,100줄
(`io.rs` / `driver.rs` / `dmesh.rs` / `connect.rs`)은 데이터패스 중립이라 **손대지 않는다.**
우리가 쓰는 것은 어댑터 한 겹이다.

### 왜 linkerd가 나가는 바이트를 다시 만드는가

HTTP/2는 **커넥션마다** 상태를 갖는다.

```
클라이언트 → linkerd            linkerd → 백엔드
─────────────────────          ─────────────────────
HEADERS  stream=3              HEADERS  stream=7      ← 스트림 번호가 다름
  HPACK 테이블 A                 HPACK 테이블 B        ← 재압축 필수
                                 + l5d-* 헤더 추가
DATA     stream=3              DATA     stream=7
  [protobuf 바이트]              [protobuf 바이트]     ← 여기만 동일
```

HPACK 테이블이 커넥션마다 다르므로 헤더 바이트를 그대로 복사하면 백엔드가 오해석한다.
스트림 번호도 백엔드 커넥션에서 이미 쓰이고 있을 수 있다. **포장은 새로 만들어야 하고,
내용물(DATA 페이로드)만 동일하다.** §6의 zero-copy 아이디어가 여기서 나온다.

---

## 3. 스레드 모델

### 3.1 현재 — ARM worker thread `i` (총 A개, shared-nothing)

```
 host pod ──PCIe/DPA──► K forward rings ──► [ ARM worker thread i ]
 ══════════════════════════════════════════════════════════════════
   epoll{ own PE fd, cross-worker eventfd, DMA notify fd } + 1ms
        │
        ▼
   px_process_forward
        │
        ▼
   px_parse
        ├── L4 passthrough ────────────► px_ship_range
        │      conn-pinned LB
        └── mock L7 ───────────────────► px_build_range + 그룹화
               dmesh_l7_decode
                                              │
                                              ▼
                                   px_engine (doca_dma, 워커당 1개)
                                   SG src = arrival staging 전용
                                              │
                                              ▼
                                        dst pod host memory

 이 스레드가 단독 소유: px_conn 해시테이블 · dpu_conntrack · px_engine
 메인 스레드: comch 제어(등록/해제), 도어벨
```

워커 루프 1회전: `dpu_progress_worker_pe()` → `dpu_worker_run()` → 진전 없으면 arm 후
`epoll_wait(1ms)`.

### 3.2 이후 — 같은 스레드 위에 tokio 런타임 1개

```
 host pod ──PCIe/DPA──► K forward rings ──► [ ARM worker thread i ]
 ══════════════════════════════════════════════════════════════════
   epoll{ … } + 1ms      ← 루프 주인은 그대로 DPUmesh
        │                   1회전에 l7_worker_step() 한 항 추가
        ▼
   px_parse
        ├── L4 ────────────────────────► px_ship_range          (그대로)
        ├── mock L7 ───────────────────► px_build_range         (그대로)
        └── linkerd ⟵ 신규 분기
             px_parse_linkerd
               │  l7_conn_segment(pos,len)   ← 복사 없음, extent 그대로
               ▼
   ┌───── tokio current_thread Runtime (같은 스레드, 새 스레드 0개) ─────┐
   │   DmeshIo ──► outbound stack: detect → discovery → LB → (mTLS)     │
   │   DmeshIo ◄── h2 재인코딩된 새 바이트                               │
   └───────┬──────────────────────────────────────────┬─────────────────┘
           │ dmesh_l7_release(pos,len)                │ dmesh_l7_send(buf,len)
           ▼                                          ▼
     px_advance + arrival custody 반납        ARM TX arena (DPU-local mmap)
                                                      │
                                                      ▼
                                   px_engine (doca_dma, 워커당 1개)
                                   SG src = arrival staging │ ARM TX arena
                                              ★ px_piece 소스 2종
                                                      │
                                                      ▼
                                                dst pod host memory
```

| | 값 |
|---|---|
| 새 스레드 | **0개** |
| 바뀌는 소유권 | **없음.** `px_conn`/`ct`/`px_engine`은 계속 워커 전용, 락 0개 |
| linkerd 워커 수 | **A**. 커넥션 샤딩은 `dmesh_worker_for_port()` 그대로 상속 |

### 3.3 tokio 런타임 규칙

- **`multi_thread` 금지.** work stealing이 태스크를 스레드 간에 옮기는데, 워커 로컬 상태는
  `__thread px_cur_worker`에 묶여 있어 곧 데이터 레이스다. 워커당 `current_thread` 1개.
- **드라이버는 루프를 소유하지 않는다.** `Driver::run()`의 `loop { … select! }`를
  `Driver::step() -> bool`(진전 여부)로 분해하고, 워커 루프가 회전마다 한 번 호출한다.
- **드라이버 자체 타이머 금지.** 런타임이 독립적으로 깨어나면 워커가 park하지 못한다.
  1 ms 백스톱은 워커 루프가 이미 제공한다.
- 슬롯 수는 attach 시점 파라미터. 컴파일 상수 금지.

워커 루프 변경 형태:

```c
while (!worker_state->stop) {
    enum px_progress_state did = dpu_progress_worker_pe(objs, worker_state);
    enum px_progress_state run = dpu_worker_run(objs, worker_state);
    int lnk = l7_worker_step(worker_state->id);      /* 신규 한 항 */

    if (did == PX_PROGRESS_PROGRESSED ||
        run == PX_PROGRESS_PROGRESSED || lnk)
        continue;                                    /* 일이 있으면 계속 hot */

    /* arm → recheck → epoll_wait(1ms) : 기존 그대로 */
}
```

### 3.4 프로세스 소유권

`doca/dpu_main.c`가 main이고 linkerd는 **staticlib**으로 링크된다. DOCA device / DPA /
comch / PE / 워커 스레드 소유권을 Rust로 넘기면 N/K/A 구조가 흔들린다. 포팅 코드
`main.rs`의 2단계 부팅을 라이브러리 진입점으로 바꾸는 것이 우리 몫이다.

---

## 4. 계약

### 4.1 FFI란

서로 다른 언어로 컴파일된 코드가 **C ABI**(호출 규약 + 심볼 이름)를 공통분모로 삼아 서로의
함수를 부르는 것이다. 같은 프로세스·같은 스레드·같은 스택 위에서 일어나므로 **비용은 일반
함수 호출과 같다.** C ABI로 표현되는 타입만 오간다 — 정수, 포인터, `#[repr(C)]` 구조체.

### 4.2 헤더 (`linkerd/include/dmesh_l7.h`)

```c
#ifndef DMESH_L7_H
#define DMESH_L7_H

#include <stdint.h>
#include <stddef.h>

/* 연결 하나의 신원. linkerd는 소켓 주소를, DPUmesh는 pod/service id를 쓰므로 둘 다 싣는다. */
struct dmesh_l7_flow {
    uint32_t src_ip,   dst_ip;      /* 호스트 바이트 순서. dst = 원래 목적지 */
    uint16_t src_port, dst_port;
    int32_t  src_pod;               /* DPUmesh 라우팅 키 */
    int32_t  dst_service;
    char     workload[64];          /* NUL 종단. 고정 배열 — 포인터 금지 */
};

/* ── DPUmesh(C)가 호출 / L7(Rust)이 구현 ───────────────────────── */

/* 워커 스레드에서 런타임 + 드라이버 생성. 실패 시 음수. */
int  l7_worker_attach(int worker_id, void *worker_ctx);

/* 런타임 1스텝. 진전이 있었으면 1, 없으면 0. */
int  l7_worker_step(int worker_id);

/* 새 연결. conn은 DPUmesh가 발급하는 불투명 핸들. */
int  l7_conn_open(int worker_id, uint64_t conn, const struct dmesh_l7_flow *flow);

/* 도착 세그먼트. base+pos 부터 len 바이트. 복사 금지 — release 전까지 유효. */
int  l7_conn_segment(int worker_id, uint64_t conn,
                     const uint8_t *base, uint32_t pos, uint32_t len);

void l7_conn_eof  (int worker_id, uint64_t conn);   /* 상대가 half-close */
void l7_conn_close(int worker_id, uint64_t conn);   /* 슬롯 회수 */
void l7_worker_detach(int worker_id);

/* ── L7(Rust)이 호출 / DPUmesh(C)가 구현 ───────────────────────── */

/* 응답 바이트 송신. 수락한 바이트 수(부분 수락 가능) 또는 음수 오류. */
int  dmesh_l7_send(int worker_id, uint64_t conn, const uint8_t *buf, size_t len);

/* 세그먼트 소비 완료 통보. 이걸 부르지 않으면 staging이 회수되지 않는다. */
void dmesh_l7_release(int worker_id, uint64_t conn, uint32_t pos, uint32_t len);

/* `decision` 모드용. §1 참조. 합의 전에는 미사용. */
struct dmesh_l7_decision { int allow; int32_t backend_pod; };
int  l7_resolve(int worker_id, const struct dmesh_l7_flow *flow,
                struct dmesh_l7_decision *out);
void l7_report (int worker_id, uint64_t conn, uint64_t bytes_in,
                uint64_t bytes_out, uint64_t duration_ns, int reason);

#endif /* DMESH_L7_H */
```

### 4.3 Rust 쪽 (`linkerd/rust/src/lib.rs`)

```rust
use std::os::raw::{c_char, c_int};

#[repr(C)]                    // C와 동일한 메모리 배치를 강제한다
pub struct DmeshL7Flow {
    pub src_ip: u32,   pub dst_ip: u32,
    pub src_port: u16, pub dst_port: u16,
    pub src_pod: i32,  pub dst_service: i32,
    pub workload: [c_char; 64],
}

// C가 구현한 것을 Rust가 부른다
extern "C" {
    fn dmesh_l7_send(worker: c_int, conn: u64, buf: *const u8, len: usize) -> c_int;
    fn dmesh_l7_release(worker: c_int, conn: u64, pos: u32, len: u32);
}

// Rust가 구현해 C에 노출한다
#[no_mangle]                                    // 심볼 이름을 뭉개지 말 것
pub extern "C" fn l7_conn_segment(worker: c_int, conn: u64,
                                  base: *const u8, pos: u32, len: u32) -> c_int {
    WORKER.with(|w| w.borrow_mut().conn(conn).push_segment(base, pos, len));
    0
}

#[no_mangle]
pub extern "C" fn l7_worker_step(worker: c_int) -> c_int {
    WORKER.with(|w| w.borrow_mut().step()) as c_int
}
```

`WORKER`는 스레드 로컬이다. 워커 스레드 밖으로 나가는 상태가 없어야 한다.

### 4.4 포팅 쪽 FFI와의 관계

`driver.rs`가 선언한 기존 15개(`ctrl_*`, `data_*`, `ctrl_advance`, `conn_state_get`,
`conn_staging_base`, `conn_recv_pop`, `conn_send`, `stats_get` …)는 이 8함수 위에 얇은
어댑터로 매핑된다 — **pull 모델 ↔ push 모델 변환**이 전부다. 어댑터는 `linkerd/shim/`에 둔다.

### 4.5 DPUmesh에 대응물이 없어 신규로 만들어야 하는 것 3개

| 항목 | 포팅 쪽 사정 | DPUmesh 사정 | 결과 |
|---|---|---|---|
| `release` | staging이 커넥션 전용 단일 버퍼 → 덮어써도 됨 | **pod 공유 링 + custody 회수** | **필수.** 없으면 읽는 중인 바이트가 재사용됨 = 데이터 손상 |
| `send` | 커넥션마다 `tx_staging` 보유 | **ARM이 만든 바이트를 담을 곳이 없음** | **최대 작업.** TX 아레나 + `px_piece` 소스 2종 |
| `flow` | 호스트 shim이 IP:port를 직접 채움 | pod/service id로만 라우팅 | 등록 경로에 아이덴티티 확장 |

나머지는 이미 있는 것에 이름을 맞추는 매핑이다.

---

## 5. 데이터 경로

### 5.1 메시지 하나가 거치는 스레드와 연산

| # | 스레드 | 연산 |
|---|---|---|
| 1 | **클라 앱 스레드** (호스트) | `send()` → preload가 TX slot에 memcpy → `dma_ring`에 디스크립터 publish |
| 2 | **DPA EU** (DPU HW) | 디스크립터 폴링 → 호스트 TX → **pod staging DMA** → 완료 메타데이터 |
| 3 | **ARM 워커 i** | epoll 깨어남 → 완료 큐 → 주인 워커 확인 → `px_process_forward` → `px_parse` → linkerd 분기 |
| 4 | ARM 워커 i | `px_parse_linkerd`: extent를 `l7_conn_segment()`로 전달. **custody 잡고 `px_advance` 안 함** |
| 5 | ARM 워커 i *(tokio)* | `l7_worker_step()` → 커넥션 태스크 → `poll_read`가 staging에서 **memcpy ①** → h2 디코드 |
| 6 | ARM 워커 i | 디스커버리·정책·LB로 **백엔드 결정** → 새 h2 프레임 인코딩 → `poll_write` **memcpy ②** |
| 7 | ARM 워커 i | `dmesh_l7_send()` → `px_ship_arm_bytes` → unit을 레인에 enqueue |
| 8 | ARM 워커 i | `px_engine` **SG-DMA 제출** → 백엔드 pod의 호스트 RX로 DMA |
| 9 | ARM 워커 i | 소비 완료 구간에 `dmesh_l7_release()` → `px_advance` + **custody 반납** |
| 10 | ARM 워커 i (DMA 완료 콜백) | `rev_ring`에 DONE 발행 + `REV_DOORBELL` |
| 11 | **백엔드 PE progress 스레드** | rev_ring drain → 앱 eventfd |
| 12 | **백엔드 앱 스레드** | `recv()` → RX 버퍼 → 앱 버퍼 memcpy |

응답은 같은 경로를 대칭으로 한 번 더 탄다. 워커는 conntrack이 정한 **동일 워커 i**다
(`dpu_upstream_create`가 upstream 포트에 워커 번호를 심는다).

**L7 연산은 전부 워커 스레드 하나 안에서 끝난다.** 스레드 경계는 원래 있던 3곳(앱↔DPA,
DPA↔워커, 워커↔호스트 PE 스레드)뿐이고 늘지 않는다. 3~9가 한 회전에 끝날 수도, linkerd가
`.await`로 멈추면 여러 회전에 나뉠 수도 있다. 그동안 custody는 계속 잡혀 있다.

### 5.1.1 모드는 어디서 정해지는가

`px_resolve_route()`가 커넥션 하나의 모드를 정한다. 방향에 따라 출처가 다르다.

```c
static void px_resolve_route(struct objects *objs, struct px_conn *c,
                             int is_reply, int16_t svc) {
    c->l7_mode = PX_L7_NONE;
    if (!is_reply && svc >= 0 && svc < POD_ID_SPACE) {
        c->l7_mode = px->svc_mode[svc];          /* 요청: 서비스 테이블 */
        return;
    }
    if (is_reply && c->pub.src_port >= DMESH_UPORT_BASE) {
        struct dpu_upstream *u = &px_cur_worker->ct->upstream[c->pub.src_port];
        if (u->in_use)
            c->l7_mode = u->codec_id;            /* 응답: conntrack이 기억 */
    }
}
```

- **요청 방향**: 목적지 서비스 id로 서비스별 모드 테이블을 조회한다.
- **응답 방향**: 서비스 id를 모른다. `dpu_upstream_create`가 기록해 둔 `codec_id`를
  읽는다. 즉 **모드는 요청 시 결정되고 응답은 그것을 물려받는다.**
- 따라서 `codec_id`가 `PX_CODEC_L7` 하나가 아니라 §8.3의 모드 열거형을 담아야 한다.

### 5.1.2 핸들과 컨텍스트

| 이름 | 무엇 |
|---|---|
| `conn` (`uint64_t`) | DPUmesh가 발급하는 불투명 핸들. `(pod_id << 16) \| port`로 만든다. 포인터를 쓰지 않는 이유는 `px_conn`이 풀에서 재사용되기 때문이다 — 늦게 도착한 L7 호출이 다른 커넥션을 가리키면 안 된다 |
| `worker_ctx` (`void *`) | `l7_worker_attach`에 넘기는 워커 식별 컨텍스트. `struct objects *`를 넘긴다. L7 계층은 이것을 저장만 하고 역참조하지 않으며, `dmesh_l7_send`/`release` 호출 시 되돌려 준다 |
| attach 시점 | 워커 스레드가 자기 PE·엔진 초기화를 마친 직후, 루프 진입 전. 게이트가 비어 있으면 호출하지 않는다 |
| detach 시점 | 루프를 빠져나온 뒤, 워커 자원 해제 전 |

### 5.2 수신: `px_parse` 분기 + custody 연장

`px_parse()`에 세 번째 갈래를 추가한다.

```c
switch (c->l7_mode) {
case PX_L7_OPAQUE:
case PX_L7_FULL:     px_parse_linkerd(objs, c); return;   /* 바이트가 L7을 지난다 */
case PX_L7_MOCK:     px_parse_l7(objs, c);      return;
case PX_L7_DECISION:                                      /* 결정은 conn open 시 1회, */
case PX_L7_NONE:     break;                               /* 바이트는 아래 L4 경로로 */
}
… L4 passthrough …
```

`px_parse_linkerd()`:

1. `px_view()`로 도착 window의 연속 extent를 얻는다 (프레임 파싱 없음).
2. `l7_conn_segment(worker, conn, base, pos, len)`로 넘긴다.
3. **`px_advance()`를 호출하지 않는다.** L4/mock-L7은 SG-DMA 제출과 동시에 소비가
   확정되지만 linkerd는 나중에 읽는다. custody를 읽기 완료까지 연장한다.
4. `dmesh_l7_release(pos, len)`에서 `px_advance` + custody 반납을 한다.
5. custody가 한계에 닿으면 `px_stall(c)`로 워커 백프레셔를 건다.

> `px_head_view()`는 `PX_HEAD_MAX`까지만 연속을 보장하고 `px_view()`는 하나의 staging
> extent 잔여만 준다. `DmeshIo`는 세그먼트 리스트를 받으므로 **연속화가 필요 없다** —
> extent를 있는 그대로 넘기는 것이 zero-copy를 유지하는 지점이다.

### 5.3 송신: ARM TX 아레나

현재 SG 소스는 도착 staging 전용이다.

```c
struct px_piece { struct px_arrival *arr; int32_t pod_idx;
                  uint32_t staging_off, len; … };
/* 제출부: addr = src_pod->dma_buffer + p->staging_off,
           mmap = src_pod->local_mmap */
```

linkerd가 만든 바이트는 여기 못 들어간다. 필요한 변경 4가지:

1. **워커별 TX 아레나** — DPU-local `doca_mmap` 하나(워커당 수 MiB, 슬랩/링).
   rev-ring 엔트리·credit이 이미 ARM-local 메모리를 DMA하고 있으므로
   (`px_rev_stage_append`, `px_op.kind = 1/2`) 메커니즘은 검증되어 있다.
2. **`px_piece`에 소스 종류 추가** — `arr != NULL`이면 도착 custody, `NULL`이면 아레나
   chunk. 제출부와 완료 콜백이 이 분기를 본다.
3. **`px_ship_arm_bytes(objs, c, buf, len, dst_pod)`** 신규 — 아레나에 쓰고 unit을 구성해
   `px_enqueue_unit`. 기존 `px_build_range` / `px_ship_range`는 건드리지 않는다.
4. `dmesh_l7_send()`는 이 함수를 호출하고, 아레나가 마르면 **부분 수락**(accepted < len)을
   반환한다. `pump_send`가 나머지를 되돌려 다음 틱에 재시도한다.

> **아레나 chunk 반납 경로는 완료 콜백 한 곳으로 모을 것.** chunk를 claim한 뒤 에러 경로에서
> 빠져나가면 아레나가 영구 누수된다. 이 코드베이스에서 같은 형태의 결함이 두 번 재발했다.

### 5.4 라우팅 소유권

`opaque`/`l7` 모드에서는 **DPUmesh가 백엔드를 고르지 않는다.**
`px_route_message()` / `px_resolve_backend()`를 우회하고, linkerd가 고른 대상이
`dmesh_l7_send`의 목적지가 된다.

- 업스트림 표현: `dpu_upstream_create(ct, cp, cport, bpod, codec_id, owner, stride)`에
  `codec_id = PX_CODEC_LINKERD`를 추가해 재사용한다. 이 함수가 upstream 포트를 워커의
  잔여류(residue class)로 발급하므로, 응답이 요청과 같은 워커로 되돌아온다.
- 응답: 백엔드 pod → DPU 도착 → **같은 워커**의 응답 방향 `DmeshIo` → `dmesh_l7_send`로
  클라이언트 pod. 왕복 두 번 모두 5.2/5.3 경로를 탄다.

`decision` 모드에서는 반대다. 결정만 linkerd에서 받고 바이트는 데이터 평면 경로(`px_resolve_backend` →
`px_ship_range`)로 흐른다.

### 5.5 아이덴티티 브리지

linkerd는 `OrigDstAddr`(소켓 주소)와 workload 문자열을 요구하고, DPUmesh는
`service_id`/`pod_id`로 라우팅한다. 둘을 잇는 표가 필요하다.

- **워크로드 신원은 별도 제어 메시지로 온다** — `POD_REGISTER`를 키우지 않는다.
  등록 메시지는 12바이트 고정 구조체이고 양쪽에서 크기를 검사하므로, 가변 길이 문자열을
  넣으면 호스트·DPU 동시 배포가 강제된다. 등록 전에 같은 연결로 보내는 별도 메시지가 그
  성질을 보존하고 재전송에도 idempotent하다. 신원은 슬롯의 DMA generation에 바인딩되어
  재사용된 슬롯이 이전 테넌트의 신원을 물려받지 않는다. (`design/CONTROL.md` §10.1)
- `service_id ↔ ClusterIP:port`는 레지스트리에서 온다.
- `l7_conn_open`이 둘을 조회해 `dmesh_l7_flow`를 채운다.
- 매핑이 없으면 그 커넥션은 L7으로 보내지 않고 폴백한다 (안전한 기본값).

### 5.6 백프레셔 일원화

두 겹이 존재한다: DPUmesh RX credit / `tx_gate`, 그리고 `DmeshIo`의 tx 상한.

- **권위는 DPUmesh.** 송신 방향은 아레나 잔량, 수신 방향은 custody 한계가 결정한다.
- `DmeshIo` tx 상한은 아레나 잔량에 연동하거나 사실상 무력화한다. 독립적인 두 번째
  브레이크가 되면 상호 대기가 생긴다.

### 5.7 커넥션 수명

| DPUmesh | L7 계층 |
|---|---|
| `px_conn` 생성 (첫 도착) | `l7_conn_open` |
| `px_try_fin` / `fin_pending` | `l7_conn_eof` → 스택이 EOF를 본다 |
| L7 스택 종료 (`DmeshIo` drop) | tx 소진 후 FIN unit 발행 |
| pod 소실 / `dma_ready` 해제 | `l7_conn_close` → 태스크 종료, 슬롯 회수 |

`px_try_fin`은 "미소비 tail = 잘린 unit이므로 드롭" 규칙을 갖는다. linkerd 경로에서는
미소비 tail이 **아직 안 읽힌 바이트**일 수 있으므로, FIN 처리 전에 **custody가 0인지
확인**해야 한다.

---

## 6. 복사 분석과 zero-copy

### 6.1 현재 설계의 복사 횟수

```
`decision` (그리고 폴백)
  host TX ──DMA──► staging ──SG-DMA──► dst host RX
  ARM memcpy = 0     (ARM은 페이로드를 만지지 않는다)

`opaque` / `l7`
  host TX ──DMA──► staging ─①─► linkerd 읽기버퍼 → 처리 → ─②─► TX arena ──SG-DMA──► dst
  ARM memcpy = 2
```

`①`은 `AsyncRead` 계약상 불가피하다(호출자 버퍼를 채워야 한다). `②`는 새 바이트를 만드는
행위 자체다. 소켓 기반 사이드카는 같은 왕복에 호스트 CPU 복사 6회 + 시스템콜 6회 +
컨텍스트 스위치 4회를 쓴다.

### 6.2 TX 복사를 1회로 — **P2에 포함**

포팅 코드는 지금 TX에 3회를 쓴다: `poll_write → tx Vec` → `take_tx가 새 Vec 할당` →
`conn_send가 memcpy`. 어댑터에서 두 가지를 한다.

1. `take_tx(max) -> Vec` 대신 **목적지를 받는 형태**로: `take_tx_into(dst, cap) -> usize`.
   중간 Vec 할당이 사라진다.
2. `DmeshIo`의 tx 버퍼 자체를 **아레나 청크로** 만든다. `poll_write`가 처음부터 DMA 가능한
   메모리에 쓰므로 복사가 1회로 줄고, 그 1회는 어떤 writer든 하는 최소값이다.

커넥션당 청크 쿼터를 둔다. 정체된 연결이 아레나를 붙잡으면 head-of-line이 생긴다.

### 6.3 TX 복사를 0회로 — 평문 한정, 추후

h2가 프레임을 내보낼 때 실제 모양은 `[프레임 헤더 9B][DATA 페이로드]`이고, **페이로드는 받은
것과 동일**하다. 우리 egress는 SG라 여러 소스를 한 전송으로 묶을 수 있다.

```
아레나:  [헤더 9B]  ┐
                    ├─ SG-DMA가 모아서 전송 → 페이로드 복사 0
staging: [페이로드─]┘
```

필요한 것:
- `DmeshIo`에 **`poll_write_vectored` 구현** + `is_write_vectored() -> true`.
  현재 미구현이라 기본 폴백이 첫 슬라이스만 복사한다.
- **출처 판별**: 슬라이스 포인터가 이 커넥션의 staging 범위 안이면 arrival piece,
  아니면 아레나 piece.
- **custody**: 그 piece가 SG-DMA를 마칠 때까지 arrival을 잡아둔다 (5.2 메커니즘 재사용).

**전제**: 페이로드가 아직 staging에 있어야 한다 = RX zero-copy(6.4)가 선행되어야 한다.
그리고 **mTLS를 켜면 페이로드가 암호문이라 성립하지 않는다** (§7).

### 6.4 RX 복사를 0회로 — 리서치 스파이크

복사는 `poll_read`의 `put_slice`에서 발생한다. h2는 **이미 DATA 페이로드를 복사하지
않는다** — 읽기 버퍼(`BytesMut`)를 잘라 `Bytes`로 넘길 뿐이다. 따라서 **읽기 버퍼 자체가
staging이면** 페이로드는 끝까지 복사 없이 흐른다.

- 소유자를 붙일 수 있는 `Bytes` 생성자를 쓰면 drop 시점에 custody를 반납할 수 있다.
- **막는 지점**: `AsyncRead` / `hyper::rt::Read`가 전부 "내 버퍼를 채워라" 모델이라 IO가
  버퍼를 위로 건넬 방법이 없다. hyper/h2의 버퍼 획득 지점을 패치해야 한다.
- 남의 코드이고 작업량이 크다. **RX 복사가 실제로 비용인지 먼저 측정한 뒤 결정한다.**

### 6.5 우선순위

| 순서 | 항목 | RX+TX 복사 | 난이도 |
|---|---|---|---|
| 1 | 6.2 아레나를 tx 버퍼로 | 1 + 1 | 낮음, **P2 포함** |
| 2 | **`decision` 모드** — 결정만 받고 splice | **0 + 0** | 낮음 (DPUmesh 쪽), 질의 API 합의 필요 |
| 3 | 측정 — 8 KiB에서 남은 복사가 문제인가 | — | 필수 |
| 4 | 6.3 vectored + 출처 판별 | 0 + 0 (평문) | 높음, 6.4 선행 |
| 5 | 6.4 staging을 h2 읽기 버퍼로 | 0 + 0 | 매우 높음 (hyper 패치) |

판정 기준: 8 KiB에서 복사 제거 전후의 ARM 코어 사용량 차이. **노이즈(±6%) 안이면 복잡도를
늘릴 이유가 없다는 결론도 유효한 결과다.**

---

## 7. 보안 모델

**노드 내 통신은 평문, 노드 간(미구현)에만 mTLS.**

노드 내에서 바이트가 지나는 경로는 `pod 등록 메모리 ↔ PCIe ↔ DPU 메모리`뿐이다. 네트워크
스택도 케이블도 타지 않으므로 도청·MITM 위협 모델이 적용되지 않는다. 부수 효과로 §6.3의
TX zero-copy가 성립한다.

### 신원은 어디서 오는가

mTLS의 절반은 인가 정책의 근거가 되는 **클라이언트 신원**이다. 암호화를 빼면 신원을 다른
경로로 공급해야 하고, §5.5의 등록 기반 아이덴티티가 그 역할을 한다.

| | 인증서 기반 | DPU 등록 기반 |
|---|---|---|
| 신원의 출처 | pod 메모리 안의 키/인증서 | DPU가 comch 연결·등록 메모리에 바인딩해 **부여** |
| 위조 조건 | 키 탈취로 사칭 가능 | DPU 협조 없이는 불가 |

> **보증의 근거를 정확히 쓸 것.** "같은 노드라서 안전"이 아니라 **"DPU가 pod별 메모리 격리와
> 라우팅을 강제하니까"** 다. 멀티테넌시에서도 보증하는 것은 DPU 격리이지 물리적 근접성이
> 아니다.

### 노드 간

```
pod ──평문/PCIe──► DPU ──mTLS──► 원격 DPU ──평문/PCIe──► pod
     신원 = DPU 부여        AES-GCM HW, 키가 호스트 메모리에 없음
```

BlueField의 AES-GCM 가속기를 쓸 수 있고, 세션 키가 호스트에 존재하지 않으며, 암호화 구간이
실제로 노출되는 1홉으로 줄어든다. 사이드카 모델에서는 구조적으로 불가능한 성질이다.

### 모드 (서비스별 정책, 전역 상수 금지)

| 모드 | 용도 | zero-copy |
|---|---|---|
| `intra-plaintext` | 기본. 노드 내 | **가능** |
| `intra-mtls` | 컴플라이언스 요구 환경 | 불가 |
| `inter-mtls` | 노드 간 (추후) | 불가 |

모드가 바뀌면 조용히 느려지는 것이 아니라 **경로가 갈리도록** 코드에서 분기한다.

### 구현 메모

linkerd 쪽 코드 변경은 필요 없을 가능성이 높다. 아웃바운드는 디스커버리 응답에 대상
identity가 없으면 평문으로 붙으므로, **destination 서비스가 identity를 싣지 않는 것이 곧
스위치**다.

### 측정 공정성

- 주 비교: 평문 경로 ↔ **Envoy permissive**
- 부 비교: mTLS 경우 ↔ **Envoy strict** (노드 간 시나리오의 대리 지표)

### 미래 리스크

인바운드를 붙이면 linkerd 인가 정책이 인증된 클라이언트를 요구할 때 평문 연결이
**fail-closed** 될 수 있다. DPU 부여 신원을 정책 계층에 주입하는 방법이 그때 숙제가 된다.

---

## 8. 변경 범위

### 8.1 배치 규칙

```
doca/     ← px_ 내부를 만지는 모든 것
            (px_view/px_advance/px_stall/px_build_range/px_piece는 전부
             dpu_proxy.c의 static이라 다른 디렉터리에서 못 쓴다)
linkerd/  ← 8함수 계약 위만 만지는 모든 것
```

```
doca/
  dpu_proxy.c    [본문] px_parse 분기, px_piece 소스 2종, PX_CODEC_LINKERD, svc_linkerd
  dpu_linkerd.c  [신규] px_parse_linkerd · px_ship_arm_bytes · ARM TX 아레나
  dpu_worker.c   [본문] 워커 루프 한 항 + attach/detach
  dpu_main.c     [본문] 프로세스 1회 init
  dpu_l7.c       [유지] mock — P4에서 삭제
  meson.build    [본문] srcs에 dpu_linkerd.c

design/
  L7.md          [예정] CONTRACT.md가 협업자와 합의되면 규범 spec으로 승격

linkerd/
  README.md · PLAN.md · CONTRACT.md
  include/dmesh_l7.h   [신규] 8함수 계약
  shim/                [신규] 계약 ↔ 포팅 FFI 어댑터
  rust/                [신규] staticlib 래퍼 → doca/meson.build가 소비
  port/                서브모듈
  bench/               측정 하네스
```

`integrations/`가 아닌 이유: `integrations/grpc/`는 **남의 프로세스**에 링크되고 자체
`CMakeLists.txt`를 갖는다. linkerd는 **우리 DPU 바이너리**에 링크된다.

### 8.2 변경의 성격

| 범주 | 내용 |
|---|---|
| **순수 추가** | `dpu_linkerd.c`, `dmesh_l7.h`, `shim/`, `rust/`, `PX_CODEC_LINKERD`, `svc_linkerd`, meson 한 줄 |
| **본문만 변경** (시그니처 불변) | `px_parse`(분기) · `px_batch_submit_dma`(소스 분기) · `px_dma_done_cb`/`px_lane_retire`(아레나 반납) · `px_try_fin`(custody 확인) · `dpu_data_worker_main`(한 항) · `dpu_main` |
| **구조체 확장** (필드 추가만) | `px_piece` · `px_conn` · `dmesh_proxy` |
| **wire ABI 추가** | §5.5 아이덴티티 — 신원 메시지 타입 하나 추가. 기존 구조체는 불변이므로 크기 assert도 그대로이고, 동시 배포가 강제되지 않는다. 호스트 쪽 등록 경로에 전송 한 줄이 추가된다 |

**기존 함수 시그니처 변경은 0이다.**

> 구조체 확장 시 주의: 이 코드베이스에는 **레이아웃 이동이 방아쇠가 된 손상 사고 전력**이
> 있다. `px_conn`/`px_piece`에 필드를 추가할 때는 카나리아를 켜 두고 진행한다.

### 8.3 게이팅 — 서비스별 모드

§1의 3모드를 서비스별로 지정한다.

```
DPUMESH_L7_DECISION_SVC=<service ids>    # 결정만 받고 splice (복사 0)
DPUMESH_L7_OPAQUE_SVC=<service ids>      # 바이트 통과 + mTLS
DPUMESH_L7_SVC=<service ids>             # 바이트 통과 + HTTP 라우팅
```

- 어느 목록에도 없는 서비스는 **폴백**(§1) — 데이터 평면 자체 LB. 기존 동작과 동일하다.
- 세 목록이 모두 비어 있으면 런타임을 attach하지 않는다. `px_parse`에 분기가 늘지만
  taken되지 않으므로 **실행 경로가 기존과 완전히 같다.**
- 한 서비스가 둘 이상의 목록에 있으면 시작 시 에러로 거부한다.
- 파싱은 기존 `px_parse_svc_csv()`를 재사용한다.

커넥션의 모드는 bool이 아니라 열거형으로 들고 있어야 한다. conntrack에 이미 `codec_id`가
있으므로 같은 형태로 맞춘다.

```c
enum px_l7_mode {
    PX_L7_NONE = 0,      /* 폴백: 데이터 평면 단독 */
    PX_L7_MOCK,          /* dpu_l7.c — P4에서 제거 */
    PX_L7_DECISION,
    PX_L7_OPAQUE,
    PX_L7_FULL,
};
/* struct px_conn: uint8_t l7_mode;  (기존 int is_l7 을 대체) */
```

---

## 9. 마일스톤

L7 소비자를 갈아끼우는 구조로 만들면 **포팅 완성을 기다리지 않고** 대부분을 진행할 수 있다.
linkerd는 소비자 중 하나일 뿐이고, 완성되면 링크 대상만 바뀐다.

| 소비자 | 언어 | 무엇을 증명하나 |
|---|---|---|
| `l7_null` | C, ~80줄 | 받은 세그먼트를 그대로 `dmesh_l7_send`. custody·아레나·SG·백프레셔가 동작 |
| `l7_framed` | C, ~150줄 | 프레임 경계 파악 → 백엔드 선택 → 다시 씀. **종료형 의미**를 검증 |
| `l7_rust_stub` | Rust | Rust↔C 경계: staticlib 링크, aarch64, jemalloc, 워커 스레드 위 런타임 |
| **linkerd** | Rust | 실제 L7. **DPUmesh 변경 0** |

| | 내용 | 완료 기준 | 포팅 의존 |
|---|---|---|---|
| **P0** | `dmesh_l7.h` · 워커 훅(attach/step/detach) · 게이팅 · `l7_null` | 게이트 off에서 기존 벤치 회귀 0 | 없음 |
| **P1** | `px_parse_linkerd` + custody 연장 + `dmesh_l7_release` | `l7_null`로 요청이 백엔드까지. 링 정체 시 `px_stall` 동작 확인 | 없음 |
| **P2** | ARM TX 아레나 + `px_piece` 소스 2종 + `dmesh_l7_send` + §6.2 | `l7_null` 왕복 성립, fail 0, 아레나 누수 0 | 없음 |
| **P3** | `l7_framed`로 종료형 검증 + §5.5 아이덴티티 브리지 | 프레임 단위 라우팅이 종료형 경로로 재현 | 없음 |
| **P4** | `l7_rust_stub` → 실제 linkerd. 워커 A개 다중화. `dpu_l7.c` 삭제 | h2 요청 end-to-end, 다중 워커에서 fail/reorder 0 | **필요** |
| **P5** | **`decision` 모드** (`l7_resolve`/`l7_report` + splice) | 정책을 받으면서 복사 0 유지 | 질의 API 합의 |
| **P6** | 측정 | `폴백 / decision / l7 / Envoy permissive / Envoy strict` + zero-copy 판정 | — |

P0~P3이 작업량의 약 70%이고 전부 지금 가능하다. 포팅이 늦어져도 P3 결과만으로 **"DPUmesh가
종료형 L7을 실을 수 있다"** 는 주장이 `l7_framed`로 성립한다.

측정 열에 mock L7은 넣지 않는다. 16B 길이 프리픽스 프레임은 실재하는 프로토콜이 아니라
설명 부담만 생긴다. 코드는 P3까지 검증 도구로 쓰고 P4에서 삭제한다.

### 9.1 P0 착수 순서

각 단계가 끝날 때마다 **게이트 off에서 기존 벤치 수치가 노이즈(±6%) 안**임을 확인한다.
회귀가 보이면 그 단계에서 멈춘다 — 뒤에서 원인을 찾는 것보다 싸다.

| 단계 | 파일 | 내용 | 확인 |
|---|---|---|---|
| **P0.1** | `linkerd/include/dmesh_l7.h` | §4.2 헤더를 그대로 작성. 구현 없음 | 컴파일만 |
| **P0.2** | `linkerd/shim/l7_null.c` | 8함수 중 L7 쪽 6개를 구현. `l7_conn_segment`는 받은 `(pos,len)`을 그대로 `dmesh_l7_send`로 되돌리고 `dmesh_l7_release` 호출. `l7_worker_step`은 항상 0 | 단독 컴파일 |
| **P0.3** | `doca/dpu_proxy.c` | `enum px_l7_mode` 추가, `px_conn.l7_mode` 필드(기존 `is_l7` 대체), §8.3 환경변수 3개 파싱, 중복 지정 시 시작 거부. **아직 아무도 mode를 보지 않는다** | 게이트 off 회귀 0 |
| **P0.4** | `doca/dpu_worker.c` · `dpu_main.c` · `meson.build` | `l7_worker_attach`/`step`/`detach`를 워커 루프에 배선. 게이트가 비면 attach하지 않음 | 게이트 off 회귀 0 |
| **P0.5** | — | 게이트를 켜고 `l7_conn_open` 로그가 찍히는지 확인. 데이터는 아직 안 흐름 | 로그 |

`dmesh_l7_send` / `dmesh_l7_release`의 C 쪽 구현은 P1·P2의 몫이다. P0에서는 **stub이
에러를 반환**하고, `l7_null`은 그 에러를 무시한다.

> P0.3~P0.5는 `doca/`를 건드린다. 그 디렉터리에 다른 작업이 진행 중이면 P0.1·P0.2를 먼저
> 끝내고 대기한다. 두 파일은 `doca/`에 의존하지 않으므로 병렬로 진행할 수 있다.

### 9.2 우리 쪽 미해결 항목

포팅 상태와 무관하게 **측정으로 결정**할 것들이다.

| 항목 | 무엇을 재는가 | 시점 |
|---|---|---|
| **인스턴스 밑을 공유할 것인가** — 워커별 인스턴스는 데이터 경로 때문에 강제되지만, 그 아래 디스커버리·정책·신원까지 A벌로 복제할지는 별개다. 복제하면 캐시가 A벌이고 밸런서가 분열한다. Envoy는 "커넥션은 워커별, 설정은 하나"로 푼다 | 디스커버리 캐시 메모리 × A, 백엔드별 요청 분배의 균등도 | P4 이후 |
| **L7 워커를 전부 쓸 것인가** — upstream 포트를 우리가 발급하므로 워커 배정을 고를 수 있다. 일부 워커에만 L7을 몰면 인스턴스 수와 밸런서 분열이 줄고 L7의 CPU가 L4 워커를 방해하지 않는다. 대신 L7 처리량이 그만큼 제한된다 | L7 전용 워커 수를 바꿔가며 용량·L4 간섭 | P4 |
| **코어 확장성** — 호스트 사이드카 linkerd는 6+6 코어에서 1.44배에서 멈췄고 프록시 간 커넥션이 78개였다(opaque는 1,488개). 워커별 인스턴스 = 워커별 업스트림 풀이 이 직렬화를 푸는지 | 워커 A개일 때 용량이 A에 비례하는가, 업스트림 커넥션이 A배가 되는가 | **P4 완료 기준** |

---

## 10. 리스크

1. **ARM CPU가 새 천장.** DPU ARM 코어는 호스트 코어와 대등한 비용이고, L7 스택이 비용을
   지배한다(같은 트랜스포트가 native 232 ns → gRPC 74.5 µs). ARM에서 h2를 돌리면 여기가
   병목이 될 공산이 크다. **결과로 보고할 준비를 하고 시작한다.**
2. **custody 연장이 도착 링을 마르게 한다.** L7 처리 지연이 곧 forward ring 정체로 전파된다.
   `px_stall` 백프레셔가 제대로 걸리는지 P1에서 먼저 확인한다.
3. **런타임과 워커 park/wake의 상호작용.** 런타임이 자체 타이머로 깨어나면 워커가 park하지
   못한다. 타이머 사용을 최소화하고 진전 신호를 워커 hot 판정에 통합한다.
4. **아레나 누수 / 이중 반납.** §5.3 주석 참조.
5. **`decision` 모드의 밸런서 부하 인지 왜곡.** `l7_report` 없이 splice하면 least-request/EWMA가
   무의미해진다.

---

## 11. 포팅 쪽에 필요한 것

계약대로 동작해야 한다는 사실만 기록한다. 상세는 `CONTRACT.md`.

### 11.1 포팅 수정 없이는 데이터패스가 돌지 않는다

**결론부터: 포팅 코드를 고치지 않고 데이터패스를 작동시킬 수는 없다.** 필요한 수정은
`linkerd/doca/build.rs` 한 파일이지만, 그 한 파일이 없으면 링크 자체가 되지 않으므로
"무수정 동작"은 성립하지 않는다.

그 파일이 그쪽 C 데이터패스 12개와 DPA 커널 아카이브를 강제로 컴파일한다. `linkerd-app`의
`doca` 피처가 `dmesh-doca` 크레이트를 당기므로, 그 크레이트를 쓰는 순간 대체하려는
데이터패스가 링크되고 `dmesh_doca_*` 심볼이 우리 어댑터와 **중복 정의**된다. 우회로가 없다 —
크레이트를 빼면 `dmesh.rs`·`connect.rs`가 컴파일되지 않고, Cargo로 path 의존을 갈아끼울 수도
없다. C 파일 목록과 아카이브 복사를 빼는 것이 **최소 변경**이며, "그쪽 데이터패스는 대체한다"는
이미 선언한 경계와 일치한다.

그 한 파일 외에는 **손대지 않고** 아래가 성립한다.

| 필요한 것 | 방법 |
|---|---|
| `main.rs` 대신 라이브러리 진입 | `linkerd-app`·`dmesh-doca`가 일반 lib 크레이트다. 우리 staticlib이 부팅 순서를 재현한다 |
| `App::spawn_dmesh` | `doca` 피처 아래 공개 메서드 |
| 업스트림을 DMA로 | `backend::publish` / `dmesh_io_pair`가 공개 API. 우리가 채널을 등록하면 `DmeshOrTcp`가 집어간다 |
| `Driver::run()`이 루프를 소유 | `current_thread` 런타임에 spawn하고 `block_on(yield_now())`로 펌프한다. 우리 워커가 깰 때만 폴링된다 |
| 제어 평면 | 그쪽 `mock-destination` / `mock-identity` / `mock-policy` 바이너리를 그대로 실행 |

**그 패치를 얹은 뒤 되는 것**: 커넥션 1개 end-to-end. 요청이 DMA로 들어와 h2가 파싱되고
백엔드가 선택되어 응답이 DMA로 돌아간다. `opaque` / `l7` 모드. 우리 쪽 P1·P2만 끝나면
왕복이 성립한다.

패치는 우리가 만들어 서브모듈 브랜치에 한 커밋으로 들고 갈 수 있고, 협업자에게 PR로 보내
수용되면 리베이스 부담이 사라진다. **Rust가 처음 필요해지는 시점은 P4이므로, P0~P3은 이
패치와 무관하게 진행된다.**

**안 되는 것**:

| 한계 | 원인 |
|---|---|
| 커넥션 8개 초과 | `MAX_CONNS` 컴파일 상수 |
| 같은 백엔드로 가는 두 번째 커넥션 | `backend::take()`가 레지스트리에서 제거한다. 이후는 TCP dial로 폴백 — **다중 커넥션은 데모조차 불가** |
| `decision` 모드 | `l7_resolve` / `l7_report` 부재 |
| 인바운드 | 배선 없음 |
| 워커 루프 통합 | `step()` 부재 → 1 ms 지연 바닥이 남는다 |

두 번째 행 때문에 `backend::take()`의 일회성은 **합의 목록에서 우선순위가 높다.** 리버스 링
채택이나 슬롯 상수보다 먼저 풀려야 다중 커넥션 시연이 가능하다.

- **인바운드 미지원.** DMA 경로가 아웃바운드 스택에만 연결되어 있다. 이번 통합은
  아웃바운드만 다룬다.
- **`MAX_CONNS = 8` 컴파일 상수.** 런타임 파라미터로 바뀌어야 한다.
- **`Driver::run()`의 루프 형태.** `step()` 분해가 필요하다 — 패치는 이쪽에서 만들 수 있다.
- **`backend::take()` 일회성 레지스트리.** 서비스당 채널을 한 번만 꺼낼 수 있다.
- **staging 흐름 제어 부재.** `conn_recv_release`가 no-op이고 tx staging이 랩 시 덮어쓸 수
  있다. DPUmesh 어댑터에서는 §5.2/§5.6으로 메운다.
- **`Peek` 미지원** (0 반환) → 프로토콜 감지가 `read_buf` + `PrefixedIo` 폴백만 사용한다.
- **`decision` 질의 API** (`l7_resolve` / `l7_report`) — 신규 합의 사항.
