# DPUmesh 개선 계획

`bench/report/data/l4-20260728-113431` 측정이 진행되는 동안 정리한 개선 항목이다.
겨냥하는 대상별로 절을 나누고, 새 대상이 생기면 절을 덧붙인다.

각 항목은 문제(코드 위치와 근거) → 구현 방법 → 예상 효과 → 리스크와 선행 검증 →
검증 방법 순으로 적는다.

---

## 1. LD_PRELOAD 심 (`src/dmesh_preload.c`, 1-1은 `src/dmesh_core.c` 포함)

### 근거: 요청당 호스트 CPU

64B, 4 connection, open-loop, 클라이언트 코어 1개 기준의 한계 비용
(두 측정점 사이 기울기)이다.

| 경로 | 31K rps | knee | 한계 CPU/요청 |
|---|---:|---:|---:|
| dpumesh-native | 0.186 core | 0.529 @ 3.00M | **0.12 µs** |
| plain (kernel TCP) | 0.473 | 0.984 @ 477K | **1.15 µs** |
| dpumesh-preload | 0.433 | 0.979 @ 124K | **5.9 µs** |

같은 DPU 데이터 평면 위에서 심이 native API보다 요청당 약 50배, 자기가 대체하는
커널 TCP보다 약 5배 비싸다. preload의 knee는 DPU 한계가 아니라 클라이언트 코어
포화점이다(knee에서 `cli_app=0.979`).

### 원인: 상각되지 않는 요청당 wake

응답 하나가 앱에 도달하기까지의 경로다.

```
PE 스레드   write(eq notify_efd)              ← inbox empty→non-empty 에지마다
dispatcher  poll() 복귀 + read(eq_fd)
dispatcher  dmesh_poll_eq → pfd_queue_rx → write(e->sigfd)   ← RECV 이벤트마다
앱 스레드   poll(e->efd) 복귀 + efd_drain (read 2회)
```

요청 하나에 syscall 5~6회와 컨텍스트 스위치 2회(PE→dispatcher→앱)가 든다.

이것이 "eventfd를 쓰기 때문"이 아니라는 근거는 native 서버다.
`bench/apps/echo_dpumesh.c:153-179`는 심과 동일하게 `dmesh_eq_fd` + `epoll_wait`를
쓰지만, 깨어날 때마다 `dmesh_poll_eq`를 0이 될 때까지 돌려 wake 하나를 수십 개
이벤트에 상각한다. 심은 반대로 `pfd_queue_rx`가 **이벤트마다 fd별로** 한 번씩
`efd_signal`하고, 앱은 **이벤트마다** park/wake한다.

### 1-1. 대기자가 EQ를 직접 드레인 (waiter-drains-EQ) — 본 수정

이 항목만 `src/dmesh_core.c`까지 건드린다. (A) 심의 드레인 경로와 (B) 코어의 EQ
알림 게이트 두 갈래로 이루어진다. (A)만 적용하면 앱 스레드의 park는 사라지지만
PE의 eventfd write와 dispatcher의 헛된 wake는 남으므로 절반만 얻는다.

**문제.** `shim_recv`(`dmesh_preload.c:646-672`)는 `rx_once`가 EAGAIN을 내면 곧바로
`wait_ready` → `poll(e->efd)`로 park한다. 데이터가 EQ에 이미 도착해 있어도 앱
스레드는 dispatcher가 그것을 꺼내 eventfd로 깨워줄 때까지 기다린다. 파이프라인이
걸린 상태(knee에서 연결당 약 5개 in-flight)에서는 park 시점에 이미 응답이 EQ에
있는 경우가 많으므로, 이 대기는 대부분 불필요하다.

**(A) 심 측 구현.**

1. 전역 EQ 소비권을 나타내는 `pthread_mutex_t g_poll_mu`를 둔다.
   `dmesh_poll_eq`의 single-consumer 계약은 "동시에 한 스레드만"이라는 뜻이므로,
   이 뮤텍스가 계약을 그대로 유지한다.

2. 드레인 헬퍼를 하나 만든다.

   ```c
   /* self != NULL이면 그 fd에 데이터가 들어온 순간 1을 돌려준다. */
   static int shim_try_drain(pfd_t *self);
   ```

   - `pthread_mutex_trylock(&g_poll_mu)`에 실패하면 0을 반환한다(다른 스레드가
     이미 드레인 중이므로 기존 park 경로로 간다).
   - 성공하면 `dispatcher_drain_eq()`를 그대로 호출한다. 이벤트 분배 로직은
     dispatcher와 완전히 동일해야 하므로 코드를 복제하지 않고 재사용한다.
   - 해제 후, `self`의 `rx_head`/`peer_closed`/`io_error`를 확인해 반환값을 정한다.

3. `dispatcher_main`(`:451`)의 `dispatcher_drain_eq()` 호출과 close-queue 수거
   루프를 같은 `g_poll_mu`로 감싼다. close 수거는 `dmesh_destroy_qp`를 호출하므로
   EQ 드레인과 반드시 상호배제되어야 한다(현재는 "둘 다 dispatcher 스레드"라는
   사실만으로 보장되고 있다 — `:486`의 주석이 그 가정을 명시한다).

4. `shim_recv`의 blocking 경로를 다음으로 바꾼다.

   ```
   rx_once() == EAGAIN
     → shim_try_drain(e)  : 성공하면 루프 재시작(park 없음)
     → 짧은 재시도 한도 안에서 반복
     → 그래도 없으면 기존 wait_ready(e, left)로 park
   ```

   재시도 한도는 "이미 도착한 이벤트를 줍는" 용도이므로 시간 기반 spin이 아니라
   **회수 기반**(예: 2회)으로 둔다. 응답 왕복이 100µs 단위인 워크로드에서
   시간 기반 spin은 유휴 코어를 그대로 태운다.

5. `pfd_queue_rx`(`:307`)와 `rx_once`(`:629`)의 `efd_signal`은 드레인 주체가
   대기 당사자 자신일 때 낭비다. `dispatcher_drain_eq`에 "지금 드레인하는 스레드가
   대기 중인 pfd" 포인터를 넘겨(TLS로 두는 편이 서명 변경이 적다) 그 pfd에 대해서는
   `efd_signal`을 생략한다. 단, 생략은 **그 스레드가 곧바로 `rx_once`로 돌아가
   데이터를 확인하는 경로에서만** 안전하다. park로 넘어가는 경로에서는 생략한
   토큰을 반드시 복구해야 하므로, `shim_try_drain`이 0을 반환하며 빠져나갈 때
   `efd_signal(self)`를 한 번 넣어 lost-wakeup을 막는다.

6. `shim_accept`와 `wait_writable`의 park 직전에도 같은 헬퍼를 넣는다.
   accept는 CONN_REQ, writable은 TX_READY가 EQ에 이미 와 있을 수 있다.

**(B) 코어 측 구현 — EQ 알림 게이트를 양방향으로.**

`arm_ready_after_push`(`dmesh_core.c:656-668`)는 PE 스레드에서 inbox
empty→non-empty 에지마다 `eq_notify(eq)`를 호출한다. 게이트는 `wants_notify`
하나인데, 이것은 `dmesh_eq_fd`(`:2322`)가 세우고 **내리는 경로가 없는 latch**다.
심의 dispatcher가 부팅 시 `dmesh_eq_fd`를 호출하므로 프로세스 수명 내내 1이다.
따라서 (A)로 앱이 park를 하지 않게 되어도 PE는 요청마다 eventfd를 쓰고
dispatcher는 매번 깨어나 이미 비워진 EQ를 확인한다.

- `src/dmesh_core.h`에 내부 함수를 추가한다.

  ```c
  /* EQ 알림을 켜고 끈다. 소비자가 직접 폴링하는 동안 끄고, 잠들기 직전에 켠다. */
  void dmesh_eq_set_notify(dmesh_eq_t *eq, int on);
  ```

  공개 헤더 `<dpumesh/dmesh.h>`는 건드리지 않는다. 심은 `dmesh_core.h`를 직접
  포함하므로 `libdpumesh.so.4`의 ABI도 `design/API.md`의 계약도 바뀌지 않는다.
  native 앱에 필요해지면 그때 공개 API로 승격한다.

- `eq_notify`(`:436`)의 `wants_notify` 로드 앞에 `atomic_thread_fence(seq_cst)`를
  넣고, `dmesh_eq_set_notify`의 저장도 seq_cst로 둔다. `eq_tx_ready_set`(`:487`)이
  부르는 경로도 같은 함수이므로 함께 덮인다.

- 심의 park 경로는 **arm → recheck → block** 순서를 지킨다.

  ```
  드레인/재시도 동안        : set_notify(0)
  park 직전                 : set_notify(1) → 펜스 → dmesh_poll_eq 재확인
                              비어 있지 않으면 처리하고 park하지 않는다
                              비어 있으면 park
  깨어난 직후               : set_notify(0)
  ```

**함정.** `dmesh_eq_fd`가 하는 self-kick(`:2325-2329`)을 재무장 때마다 흉내내면
안 된다. kick → poll 즉시 복귀 → 드레인 empty → 재무장 → kick 으로 무한 spin이
된다. 최초 1회 arming 레이스는 kick으로 풀 수 있지만, 반복되는 arm/disarm은
반드시 recheck로 풀어야 한다.

**예상 효과.** 파이프라인이 걸린 구간에서 요청당 syscall 5~6회와 컨텍스트 스위치
2회가 0회에 수렴한다. 부하가 낮아 park가 필요한 구간의 비용은 현재와 같다.
즉 비용이 처리율이 아니라 유휴도에 비례하게 된다.

아래는 측정된 한계 비용 5.9µs를 syscall·스위치 회수로 나눈 **추정**이며 개별
항목은 검증되지 않았다.

| 상태 | 요청당(추정) | knee 추정 |
|---|---:|---:|
| 현재 | 5.9 µs | 124K |
| 1-2 적용 | ~5.6 | ~130K |
| **+1-1 (A)만** | ~3.7 | ~200K |
| **+1-1 (B)까지** | ~1.2 | ~600K |

(A)만으로 멈추면 이득의 절반을 남긴다. (B)는 (A) 없이는 의미가 없다
(폴링하는 주체가 없으면 알림을 끌 수 없다). 둘은 한 항목으로 취급한다.

**리스크와 선행 검증.**

- 앱이 `epoll`/`select`로 `e->efd`를 직접 감시하는 경우(심은 이 호출들을 가로채지
  않는다) 앱 스레드는 우리 코드 안에서 blocking하지 않는다. 이때는 드레인 주체가
  없으므로 **dispatcher 스레드가 반드시 그대로 남아 있어야 한다.** 이 항목은
  dispatcher를 제거하는 것이 아니라 "대기자가 있으면 대기자가 먼저 한다"는
  기회주의적 추가다.
- close 수거와 EQ 드레인의 상호배제(항목 3)를 빠뜨리면 `dmesh_destroy_qp`와
  `dispatcher_drain_eq`가 같은 QP를 동시에 만진다. 뮤텍스 하나로 닫힌다.
- (B)의 lost wakeup이 두 번째이자 더 다루기 까다로운 위험이다. arm과 recheck
  사이에 도착한 에지를 놓치면 앱이 영원히 park한다. 재현이 확률적이므로 코드
  검토만으로 통과시키지 않는다. `wants_notify`를 강제로 늦게 반영시키는 지연
  주입 빌드로 arm/edge 경합을 의도적으로 만들어 확인한다.
- `tests/preload_api_contract_test.c`가 단일 스레드 기준이므로, 다중 스레드에서
  한 fd를 여러 스레드가 읽는 경우와 close 경합을 덮는 케이스를 추가해야 한다.
- (B)는 `eq_notify`를 PE 핫패스에서 seq_cst 펜스 하나만큼 무겁게 만든다.
  에지당 수십 ns이므로 수용 가능하지만, native 경로(`wants_notify=0`)에서
  회귀가 없는지 `bench.sh latency`로 확인한다.

**검증.** `bench/bench.sh preload`(5,000/0), `loopback`(10,000/0), `verbs`(10,000/0)
무결성 통과 + `l4_proxy_data.sh --quick`로 preload knee 재측정.

### 1-2. dispatcher 배치당 1회 시그널

**문제.** `pfd_queue_rx`(`:333`)는 RECV 이벤트마다 `efd_signal(e)`를 호출한다.
`dispatcher_drain_eq`는 한 번에 최대 64개 이벤트를 가져오므로, 한 fd에 5개가
연속으로 들어오면 eventfd write가 5회 발생한다. 1회면 충분하다(앱은
`efd_drain`으로 전부 비운다).

**구현.**

- `dispatcher_drain_eq`의 배치 루프에 `deferred[]`와 같은 방식으로 `signalled[]`
  집합을 둔다. `pfd_queue_rx`/`pfd_rx_fin`은 링크만 하고 시그널하지 않도록
  플래그 인자를 받는다.
- 배치 처리가 끝난 뒤 `signalled[]`의 각 pfd에 대해 `efd_signal`을 한 번씩 보낸다.
- 시그널은 항상 링크 **이후**에 나가므로 "큐에 데이터가 있는데 토큰이 없는" 창은
  배치 종료 시점에 반드시 닫힌다. `rx_once`의 재시그널 규칙은 그대로 둔다.
- 집합 크기는 배치 크기(64)로 고정하고, 넘치면 즉시 시그널로 폴백한다.

**예상 효과.** 파이프라인 깊이만큼 eventfd write가 줄어든다(knee에서 약 5배).
1-1을 적용하면 대기자 경로에서는 이 호출 자체가 사라지지만, epoll 앱과
dispatcher가 남는 경로에서는 계속 유효하다. 1-1과 독립적으로 적용 가능하다.

**리스크.** 낮다. 시그널 순서가 배치 내에서 뒤로 밀릴 뿐 유실 경로가 없다.
다만 `io_error`를 설정하는 경로(`:313`, `:322`)의 시그널은 지연시키지 않고
즉시 보낸다 — 오류는 상각 대상이 아니다.

### 1-3. per-write flush 제거 (부분 유닛 지연 발행)

**문제.** `shim_send_iov`(`:777-785`)는 모든 send/write 끝에서 `dmesh_flush(c)`를
호출한다. `dmesh_post_send`는 이미 `dmesh_flush_full`로 **완성된** 8 KiB 유닛만
발행하므로(`src/dmesh_api.c:22`), 이 flush는 오직 미완성 꼬리 유닛을 강제 발행하는
역할만 한다. 결과적으로 64B 요청 하나가 디스크립터 하나, 포워드 링 엔트리 하나,
DPA 처리 하나, 되돌아오는 TX_ACK 하나를 만든다. 심이 native의 배칭을 스스로
무력화하고 있다.

`writev`/`sendmsg` 한 번 안의 여러 iov는 이미 마지막에 한 번만 flush되므로,
이 항목이 노리는 것은 **서로 다른 send 호출 사이의 병합**이다.

**구현.**

- `pfd_t`에 `tx_dirty`를 추가한다. `stream_write_locked`가 커밋했지만 꼬리 유닛이
  미발행 상태로 남으면 세운다.
- `shim_send_iov` 말미의 무조건 `dmesh_flush`를 제거한다.
- flush 지점을 다음으로 옮긴다.
  1. `shim_recv`가 park하기 직전(1-1의 드레인 시도 직전) — 요청/응답 앱을 덮는다.
  2. `shutdown(SHUT_WR)`, `close`, `dmesh_destroy_qp` 경로 — 이미 graceful close가
     `dmesh_flush`를 부른다(`dmesh_core.c:2459`).
  3. **바운드 플러셔**: dispatcher가 `poll` 타임아웃(예: 100µs)으로 깨어나
     `tx_dirty`인 pfd를 flush한다. 심은 `poll`/`select`/`epoll_wait`를 가로채지
     않으므로, 쓰기만 하고 우리 코드 안에서 절대 blocking하지 않는 앱이 존재한다.
     이 경로가 없으면 그런 앱의 바이트가 무기한 묶인다 — 성능이 아니라
     **correctness 문제**다.
- dirty 집합은 별도 리스트 대신 `g_fds` 스캔을 피하기 위해 `pfd_t`에 링크를
  달아 전역 dirty 리스트로 관리하고 `g_q_mu`를 재사용한다.

**선행 검증(구현 전 필수).** 바운드 플러셔는 dispatcher 스레드가 **소유 스레드가
아닌** QP에 대해 `dmesh_flush`를 호출한다. `dpumesh_tx_reserve`/`tx_next_send`는
주석상 owner-only이며(`dmesh_core.c:1603`, `:1546`), PE 스레드는 원자적 필드만
만진다. 실제 요구사항이 "특정 스레드"인지 "동시에 한 스레드"인지 확인해야 한다.
후자라면 `e->tx_mu` + `e->mu`를 잡고 flush하면 안전하다. 전자라면 플러셔를
dispatcher가 아니라 소유 스레드에 깨우는 방식으로 바꿔야 한다. **이 확인 전에는
구현하지 않는다.**

**예상 효과.** 호스트 측 이득은 요청당 `emit_desc` + `dpumesh_enqueue`(링 티켓 +
디스크립터 쓰기) 절감으로 중간 수준이다. 큰 이득은 DPU 쪽이다: 파이프라인 깊이만큼
포워드 링 엔트리, DPA 완료 메타데이터, ARM 파싱, TX_ACK가 줄어든다. knee에서
연결당 약 5개가 병합되면 DPU 경로의 요청당 고정비가 5분의 1이 된다.
`SWEEP.md`가 지적한 ARM 코어 비용(8/8에서 667%)을 직접 낮추는 유일한 심 항목이다.

**리스크.** 지연 발행은 앱이 write 후 우리 코드 밖에서 오래 계산하는 경우 그만큼
지연을 더한다. TCP의 `TCP_NODELAY`가 꺼진 것과 유사한 계약 변화이므로,
바운드 플러셔의 주기가 곧 최악 지연의 상한이 된다. 100µs는 DPU 왕복(수십~수백 µs)
대비 무시할 수준이지만, 이 값은 문서화되어야 한다.

### 1-4. RX 큐 노드 freelist

**문제.** `pfd_queue_rx`(`:308`)는 RECV 이벤트마다 `preload_rx_t`를 `calloc`하고
`rx_once`(`:626`)는 마지막 바이트를 소비할 때 `free`한다. 요청당 malloc/free 한
쌍이 돈다. 노드는 24바이트 남짓의 고정 크기이고 수명이 짧아, 할당자를 거칠
이유가 없다.

**구현.** `pfd_t`마다 소진 노드를 담는 단일 연결 freelist를 두고, 큐잉 시 먼저
거기서 꺼낸다. 비어 있을 때만 `calloc`한다. 상한(예: 파이프라인 깊이 정도)을
두어 유휴 연결이 노드를 쌓아두지 않게 하고, 초과분은 `free`한다. 리스트는
`e->mu` 아래에서만 만지므로 새 동기화가 필요 없다. `pfd_storage_free`에서
남은 노드를 회수한다.

**예상 효과.** 요청당 약 0.2µs 추정. 단독으로는 knee를 거의 못 움직이지만,
1-1 적용 후 남는 비용에서 차지하는 비중이 커진다(1.2µs 중 0.2µs).

**리스크.** 낮다. 노드 재사용 시 `pos`와 `next`를 반드시 초기화해야 한다
(`calloc`이 하던 일을 손으로 한다).

### 적용 순서

1-2(저위험·독립) → 1-1 (A)+(B) → 재측정 → 1-4 → 1-3(선행 검증 통과 시).

1-1은 (A)와 (B)를 한 항목으로 취급하되, 커밋은 나눠서 (A)만 적용한 상태의
수치를 한 번 남긴다. (A)/(B)의 기여를 사후에 분리할 수 있는 유일한 지점이다.

1-1 적용 후 반드시 재측정한 다음 1-4와 1-3을 판단한다. 1-1이 syscall을 제거하면
남은 비용의 구성이 달라져 두 항목의 우선순위가 바뀔 수 있다.

### 이번에 하지 않는 것

- `g_tbl_mu`(`:143`) 전역 뮤텍스와 `e->mu`/`e->tx_mu`는 요청당 4~6회 잡히지만,
  무경합 뮤텍스 기준 요청당 100ns 수준으로 5.9µs 중 비중이 작다. 1-1 적용 후
  다시 측정해서 남은 비용이 유의미할 때만 손댄다. 성급한 lock-free 전환은
  `pfd_retire`/`active_ops`의 해제 경합을 다시 열 위험이 크다.
- 스레드별 EQ(`DMESH_MAX_EQ=64`이므로 코어 API는 이미 지원한다)는 1-1보다 이득이
  크지 않으면서 QP 수명과 epoll 호환성 문제를 훨씬 크게 만든다. 1-1 측정 후에도
  EQ 경합이 병목으로 남을 때만 재검토한다.

---

## 2. 측정 하네스 (`bench/suite/l4_proxy_data.sh`)

### 2-1. scout 단계에 CPU 관측치 기록

**문제.** knee 판정은 `scout_clean`(`:744`)이 achieved 비율, schedule 비율,
generator drop, p99, fail/reorder/overflow만 본다. `scout.csv`에는 CPU 컬럼이
아예 없다. 그 결과 "코어가 이미 포화인데 clean으로 통과한" 후보를 사후에
구별할 수 없다.

현재 데이터가 이 문제를 실제로 드러낸다. `envoy-permissive` 64B의 knee는
1.416 Mrps인데, load 단계 기록으로는 111K rps에서 이미
`client_app 0.247 + client_sidecar 0.737 = 0.984`로 클라이언트 코어(core 22)가
포화 상태다. 두 값이 같이 참이려면 사이드카의 요청당 비용이 6.6µs에서
0.4µs 아래로 16배 상각되어야 한다. 가능한 시나리오지만(프록시가 앱의 syscall을
배치로 흡수한다), **현재 데이터로는 확인할 수 없다.** knee 근처의 retained 행이
들어오면 사후 확인은 되지만, scout 자체는 여전히 눈이 먼 상태로 남는다.

**구현.**

- `scout_one`(`:702`)에 load 단계가 이미 쓰는 `/proc/stat` 스냅샷 헬퍼(`:584`)를
  붙여 측정 창 전후 delta를 뜬다. cgroup 스냅샷(`snapshot_cgroups`, `:553`)까지
  붙이면 앱/사이드카 분해도 얻지만, scout 실행 횟수가 많으므로
  `/proc/stat` 두 코어 delta만으로 시작한다.
- `scout.csv`에 `host_busy_cores`, `host_core_budget` 두 컬럼을 추가한다.
- `scout_clean`에는 판정 조건을 **추가하지 않는다.** knee 정의를 바꾸면 이미
  수집한 데이터와 비교가 끊긴다. 대신 관측치만 남긴다.
- `knees.csv`에 `highest_clean_host_busy` 컬럼과 `core_saturated` 플래그를
  추가한다. 플래그는 `host_busy_cores >= 0.95 * host_core_budget`일 때 선다.
- 플래그가 선 knee는 그래프 1에서 "capacity, core-limited"로 표기하고,
  프록시 경로의 capacity 우위를 인과적으로 주장하지 않는다.

**예상 효과.** 성능 변화 없음. knee 숫자의 방어 가능성을 확보한다.

**병행 기록.** 이 실험의 `plain`은 "커널 TCP의 한계"가 아니다.
`bench/apps/bench_sock.c`는 요청마다 write 한 번을 하고 `TCP_NODELAY`를 켠다
(`:190`). 즉 `plain`의 knee는 이 앱의 syscall 패턴이 정하는 값이며, 프록시를
끼우면 앱의 소켓 I/O가 loopback 대용량 전송으로 바뀌어 앱 쪽 비용이 내려간다.
64B knee가 payload 64B와 1KB에서 정확히 같은 476,937이라는 사실도 payload가
아니라 요청 수가 한계를 정한다는 것과 일치한다. 이 해석은 `L4_PLAN.md`의
비교 경계 절에 명시해야 하며, 그러지 않으면 "Envoy가 direct TCP보다 3배 빠르다"가
그대로 읽힌다.

**검증.** `./bench/suite/l4_proxy_data.sh --dry-run`으로 행 수 불변 확인,
`--quick`으로 새 컬럼이 채워지는지 확인. 진행 중인 런에는 적용하지 않는다
(collector_version이 올라가면 재개 호환성이 깨진다).

---

## 부기

새 개선 대상이 생기면 이 문서에 절을 추가한다. 현재 데이터가 가리키지 않는,
그러나 알려진 대상은 다음과 같다.

- **DPU ARM 코어 비용**: `SWEEP.md` 기준 `A/K=8/8`이 41.2 Gb/s에 ARM 667%,
  `2/4`가 Gb/s당 ARM 최저(7.67). 운영점 선택과 ARM 요청당 고정비가 과제이며,
  심 수정으로는 개선되지 않는다(1-3만 부분적으로 기여한다).
- **native 상한 미측정**: `dpumesh-native`의 knee는 세 payload 모두
  `right_censored`다. 수집기 상한(3.0 Mrps)과 클라이언트 코어 1개가 먼저 걸려
  DPU 데이터 평면의 천장은 아직 측정된 적이 없다.
