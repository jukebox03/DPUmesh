# DPUmesh 개선 계획

`bench/report/data/l4-20260728-113431` 측정이 진행되는 동안 정리한 개선 항목이다.
겨냥하는 대상별로 절을 나누고, 새 대상이 생기면 절을 덧붙인다.

각 항목은 문제(코드 위치와 근거) → 구현 방법 → 예상 효과 → 리스크와 선행 검증 →
검증 방법 순으로 적는다.

---

## 1. LD_PRELOAD 심 (`src/dmesh_preload.c`, 1-1은 `src/dmesh_core.c` 포함)

### 근거: 포화 전 요청당 CPU와 포화 후 상각

64B, 4 connection, open-loop, 클라이언트 코어 1개 기준이다. 출처는
`bench/report/data/l4-20260728-113431/results.csv`.

`client_app_cores`는 코어 상한에서 **클리핑된다.** plain 64B의 실제 곡선은
0.506(31K) → 0.673(62K) → 0.870(93K) → **0.981(111K)** → 0.985(405K) →
0.985(477K)이다. 111K에서 코어가 찬 뒤 rps가 4.3배 늘어나는 동안 CPU는 고정이다.
31K↔knee 직선 fit은 분자(ΔCPU)가 0인 구간을 지나므로 **범위를 넓힐수록 0으로
수렴하는 값**이며 한계 비용이 아니다. 따라서 기울기는 코어가 아직 차지 않은
구간에서만 뽑는다. (`bench/report/L4_PLAN.md` 그래프 2가 이미 "포화 후 backlog
행으로 기울기를 계산하지 않는다"고 못 박은 규칙이다. 이 문서의 이전 판이 그
규칙을 어겼다.)

| 경로 | 포화 시작 | 미포화 구간 한계 CPU/요청 | knee | 포화 후 상각 |
|---|---:|---:|---:|---:|
| dpumesh-native | 미포화 (3.15M에서 0.535) | **0.12 µs** | ≥3.00M (censored) | — |
| plain (kernel TCP) | 111K (`cli_app` 0.981) | **5.9 µs** | 477K | **4.3배** |
| dpumesh-preload | ~111K (0.972) | **6.7 µs** | 124K | **1.1배** |

이전 판과 결론이 다르다.

- **요청당 비용은 심과 커널 TCP가 사실상 같다** (6.7 vs 5.9 µs, 1.14배).
  "커널 TCP보다 약 5배 비싸다"는 클리핑 fit이 만든 착시였다.
- **차이는 전부 포화 후 거동이다.** 코어가 찬 뒤 plain은 111K→477K로 4.3배를 더
  밀어낸다 — 앱이 뒤처지면 커널이 write를 큰 세그먼트로 합치고 `read` 한 번이 응답
  여러 개를 돌려주기 때문이다. 심은 111K→124K, 1.1배에서 무너진다.
- 따라서 목표는 "요청당 5.9 µs를 1.2 µs로"가 아니라 **"심이 상각하게 만들어라"**다.
  1-1(A)와 1-3이 상각 메커니즘이고, 1-4는 요청당 상수만 줄이므로 상각에는 기여하지
  않는다(1-4 우선순위 하향의 근거).
- native는 31K→3.00M 전 구간에서 포화하지 않으므로 0.12 µs fit이 유효하다. 다만
  rate 격자에 111K와 2.55M 사이가 비어 있어 사실상 두 점 fit이다(2-3).

payload를 바꿔 보면 무엇이 한계인지가 드러난다. knee를 rps와 Gb/s로 같이 적는다.

| 경로 | 64B | 1KB | 8KB | 한계 |
|---|---:|---:|---:|---|
| plain | 476,937 / 0.24 Gb/s | **476,937** / 3.91 | 395,369 / 25.9 | **연산 수** (~477K ops) |
| dpumesh-preload | 123,583 / 0.06 | **125,923** / 1.03 | 55,168 / 3.62 | **연산 수** (~125K ops) |
| envoy-permissive | 1,415,535 / 0.72 | 936,939 / 7.68 | 247,375 / 16.2 | **바이트** (~16 Gb/s) |
| envoy-strict | 1,389,232 / 0.71 | 656,055 / 5.37 | 121,287 / 7.95 | 바이트 + TLS/byte |

plain은 64B와 1KB의 knee가 소수점까지 같고(476,937), preload도 1.9% 차이다.
**둘 다 바이트가 아니라 연산 수가 한계다** — 그리고 심이 대체하려는 대상과 심
자신이 같은 종류의 벽에 부딪히고 있다는 뜻이다. §3-2에 해석 규칙을 적는다.

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
     이미 드레인 중이므로 기존 park 경로로 간다). **`trylock`이며 blocking lock이
     아니라는 점이 항목 6의 정당성 조건이다** — 아래 락 순서 참조.
   - 성공하면 `dispatcher_drain_eq()`를 그대로 호출한다. 이벤트 분배 로직은
     dispatcher와 완전히 동일해야 하므로 코드를 복제하지 않고 재사용한다.
   - 해제 후, `self`의 `rx_head`/`peer_closed`/`io_error`를 확인해 반환값을 정한다.

   **단, 배치 예산을 받아야 한다.** `dispatcher_drain_eq`는 지금 `dmesh_poll_eq`가
   0을 돌려줄 때까지 `for(;;)` 돈다(`:364`). 그대로 재사용하면 `read(fd_A)`를 호출한
   앱 스레드가 fd_B..fd_Z의 이벤트를 **무제한으로 대신 처리**한다. 자기 데이터가 첫
   배치에 이미 와 있어도 남의 이벤트 63개를 큐잉하고 eventfd를 쓴 뒤에야 리턴한다.
   처리량은 손해가 아니지만 꼬리 지연 분포가 바뀌는데, 그것이 이 항목이 개선하려는
   지표다.

   ```c
   /* max_batches <= 0 : 기존 dispatcher 동작(0이 될 때까지) */
   static void dispatcher_drain_eq(pfd_t *self, int max_batches);
   ```

   `shim_try_drain`은 `max_batches = 1`로 호출하고, 한 배치 뒤 `self`에 데이터가
   생겼으면 즉시 종료한다. dispatcher는 `max_batches = 0`으로 지금 그대로 둔다.

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
   **회수 기반**(예: 배치 2개)으로 둔다. 응답 왕복이 100µs 단위인 워크로드에서
   시간 기반 spin은 유휴 코어를 그대로 태운다.

   예산은 **park 결정 1회당**이지 호출당이 아니다. `MSG_WAITALL`은 조각마다 루프를
   돌므로(`:653-654`), 호출 단위 예산이면 긴 WAITALL 읽기가 조각마다 park로
   퇴화한다. 드레인이 데이터를 냈으면 예산을 리셋한다.

5. `pfd_queue_rx`(`:307`)와 `rx_once`(`:629`)의 `efd_signal`은 드레인 주체가
   대기 당사자 자신일 때 낭비다. `dispatcher_drain_eq`에 "지금 드레인하는 스레드가
   대기 중인 pfd" 포인터를 넘겨(TLS로 두는 편이 서명 변경이 적다) 그 pfd에 대해서는
   `efd_signal`을 생략한다. 단, 생략은 **그 스레드가 곧바로 `rx_once`로 돌아가
   데이터를 확인하는 경로에서만** 안전하다. park로 넘어가는 경로에서는 생략한
   토큰을 반드시 복구해야 하므로, `shim_try_drain`이 0을 반환하며 빠져나갈 때
   `efd_signal(self)`를 한 번 넣어 lost-wakeup을 막는다.

6. `shim_accept`와 `wait_writable`의 park 직전에도 같은 헬퍼를 넣는다.
   accept는 CONN_REQ, writable은 TX_READY가 EQ에 이미 와 있을 수 있다.

7. **락 순서를 문서화한다.** 이 항목이 새 전역 뮤텍스를 추가하므로 반드시 필요하다.

   ```
   g_poll_mu → e->mu          (shim_try_drain / dispatcher_drain_eq)
   g_poll_mu → g_q_mu         (항목 3의 close-reap)
   g_q_mu    → e->mu          (기존 reap 루프, :463-477)
   e->tx_mu  → g_poll_mu      (항목 6: wait_writable park 직전)
   ```

   `g_poll_mu`를 쥔 채 `e->tx_mu`를 잡는 경로는 없으므로 사이클이 없다. 마지막 줄이
   성립하는 이유는 `shim_send_iov`가 `e->tx_mu`를 쥔 채 `wait_writable`로 park하기
   때문이며(`:728` lock → `:768` wait → `:783` unlock), **거기서 `g_poll_mu`를
   blocking으로 잡으면 `tx_mu`를 쥔 채 대기하게 된다.** 항목 2의 `trylock`이 그것을
   막는다.

**(B) 코어 측 구현 — EQ 알림 게이트를 양방향으로.**

`arm_ready_after_push`(`dmesh_core.c:656-668`)는 PE 스레드에서 inbox
empty→non-empty 에지마다 `eq_notify(eq)`를 호출한다. 게이트는 `wants_notify`
하나인데, 이것은 `dmesh_eq_fd`(`:2322`)가 세우고 **내리는 경로가 없는 latch**다.
심의 dispatcher가 부팅 시 `dmesh_eq_fd`를 호출하므로 프로세스 수명 내내 1이다.
따라서 (A)로 앱이 park를 하지 않게 되어도 PE는 요청마다 eventfd를 쓰고
dispatcher는 매번 깨어나 이미 비워진 EQ를 확인한다.

**게이트는 on/off 플래그가 아니라 카운터여야 한다.** "드레인 동안 끄고 park 직전에
켠다"는 단순 플래그 설계에는 lost-wakeup 구멍이 둘 있다.

- **구멍 1 — park하지 않고 나가는 경로에서 알림이 꺼진 채 남는다.** 스레드 T가
  드레인해서 데이터를 찾으면 park하지 않고 앱으로 리턴한다. 그러면 켜는 지점을
  지나지 않으므로 알림은 꺼진 상태다. dispatcher는 `poll(eq_fd, -1)`에서 영영 깨지
  않고, 앱이 `e->efd`를 직접 epoll하는 경우 그대로 **행**이다.
- **구멍 2 — `wants_notify`는 EQ 전역인데 드레이너 개인 소유처럼 다루게 된다.**
  심의 EQ는 `g_eq` 하나다. 스레드 U가 켜고 → recheck → park한 직후 스레드 T가
  드레인을 시작하며 끄면, **T가 U의 무장을 지운다.** U는 자기 fd로 데이터가 와도
  깨어나지 않는다.

따라서 `src/dmesh_core.h`에 다음을 추가한다.

  ```c
  /* 억제 카운터. 드레이너가 진입에 +1, 이탈에 -1. 잠드는 쪽은 만지지 않는다. */
  void dmesh_eq_suppress_notify(dmesh_eq_t *eq, int delta);
  ```

  공개 헤더 `<dpumesh/dmesh.h>`는 건드리지 않는다. 심은 `dmesh_core.h`를 직접
  포함하므로 `libdpumesh.so.4`의 ABI도 `design/API.md`의 계약도 바뀌지 않는다.
  native 앱에 필요해지면 그때 공개 API로 승격한다.

- `eq_notify`(`:436`)의 게이트를 `wants_notify && suppress == 0`으로 바꾼다. 로드
  앞에 `atomic_thread_fence(seq_cst)`를 넣고 카운터 갱신도 seq_cst로 둔다.
  `eq_tx_ready_set`(`:487`)이 부르는 경로도 같은 함수이므로 함께 덮인다.

- 규칙은 셋뿐이다.

  ```
  드레인 진입             : suppress += 1
  드레인 이탈 (모든 경로) : suppress -= 1
  0으로 떨어진 이탈       : 펜스 → dmesh_poll_eq 1회 재확인
                            비어 있지 않으면 write(eq_fd, 1)로 dispatcher를 한 번 킥
  park하는 쪽             : 카운터를 만지지 않는다
  ```

  드레이너만 카운터를 만지므로 구멍 2가 닫히고, 모든 이탈 경로에서 감소하므로
  구멍 1이 닫힌다. 마지막 재확인이 억제 구간에서 사라진 에지를 복구하는 유일한
  지점이며, 비용은 **드레인 세션당 syscall 최대 1회**이므로 이득은 유지된다.

**함정.** `dmesh_eq_fd`가 하는 self-kick(`:2325-2329`)을 재무장 때마다 흉내내면
안 된다. kick → poll 즉시 복귀 → 드레인 empty → 재무장 → kick 으로 무한 spin이
된다. 위 킥은 **재확인이 실제로 비어있지 않을 때만** 나가므로 spin이 되지 않는다.
반복되는 arm/disarm은 반드시 recheck로 풀어야 한다.

**예상 효과.** 파이프라인이 걸린 구간에서 요청당 syscall 5~6회와 컨텍스트 스위치
2회가 0회에 수렴한다. 부하가 낮아 park가 필요한 구간의 비용은 현재와 같다.
즉 비용이 처리율이 아니라 유휴도에 비례하게 된다.

근거 절이 정한 대로 **예측은 요청당 비용이 아니라 상각 축으로 적는다.** 포화
시작점은 앱의 요청당 고정비가 정하므로 이 항목들로는 거의 움직이지 않는다 —
1-1과 1-3이 바꾸는 것은 **포화 후 기울기**다. 아래는 검증되지 않은 **추정**이다.

| 상태 | 포화 시작 | knee | post_sat_amplification |
|---|---:|---:|---:|
| 현재 | ~111K | 124K | **1.1배** |
| +1-2 | ~111K | ~130K | ~1.2 |
| +1-3 | ~111K | ~180K | ~1.6 |
| **+1-1 (A)만** | ~111K | ~200K | ~1.8 |
| **+1-1 (B)까지** | ~111K | ~400K | ~3.5 |
| (목표) | — | — | **≥4.3배** (= plain 수준) |

(A)만으로 멈추면 이득의 절반을 남긴다. (B)는 (A) 없이는 의미가 없다
(폴링하는 주체가 없으면 알림을 끌 수 없다). 둘은 한 항목으로 취급한다.

이 축은 §3-1의 `analyze_saturation.py`가 `saturation.csv`로 자동 산출하므로,
커밋별 검증이 사람 판단 없이 기계적으로 된다.

**리스크와 선행 검증.**

- 앱이 `epoll`/`select`로 `e->efd`를 직접 감시하는 경우(심은 이 호출들을 가로채지
  않는다) 앱 스레드는 우리 코드 안에서 blocking하지 않는다. 이때는 드레인 주체가
  없으므로 **dispatcher 스레드가 반드시 그대로 남아 있어야 한다.** 이 항목은
  dispatcher를 제거하는 것이 아니라 "대기자가 있으면 대기자가 먼저 한다"는
  기회주의적 추가다. 다만 **dispatcher가 살아 있는 것만으로는 부족하다** — 알림이
  꺼진 채 남으면 dispatcher도 깨지 않는다. (B)의 억제 카운터가 "드레인 중에만
  꺼진다"를 보장해야 비로소 이 완화책이 성립한다.
- close 수거와 EQ 드레인의 상호배제(항목 3)를 빠뜨리면 `dmesh_destroy_qp`와
  `dispatcher_drain_eq`가 같은 QP를 동시에 만진다. 뮤텍스 하나로 닫힌다.
- (B)의 lost wakeup이 두 번째이자 더 다루기 까다로운 위험이다. 억제 해제와
  재확인 사이에 도착한 에지를 놓치면 앱이 영원히 park한다. 재현이 확률적이므로
  코드 검토만으로 통과시키지 않는다. 억제 카운터의 감소를 강제로 늦게 반영시키는
  지연 주입 빌드로 release/edge 경합을 의도적으로 만들어 확인한다. 위의 두 구멍
  (비-park 이탈, 전역 플래그 clobber)은 각각 재현 테스트를 따로 만든다 —
  구멍 1은 "드레인 성공 후 리턴 → 다른 fd로 데이터 → dispatcher가 깨는가",
  구멍 2는 "U가 park한 뒤 T가 드레인 → U에게 데이터 → U가 깨는가".
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

**1-1과의 상호작용.** 1-1 항목 5(드레이너 자신에 대한 시그널 생략)와 이 항목의
지연 시그널 집합은 **판정을 flush 시점에서 한 번만** 해야 한다. 링크 시점에 두
메커니즘이 각각 판단하면 "생략됐는데 집합에도 안 들어간" 토큰이 하나 사라진다.
구현 순서상 이 항목이 먼저 들어가므로, 1-1 항목 5는 집합에서 `self`를 빼는
형태로 얹는다.

**검증.** `tests/preload_api_contract_test.c`에 "한 fd로 배치 안에 RECV 5개 →
`efd_signal` 1회, 앱은 5개 조각을 순서대로 전부 읽는다" 케이스를 추가한다.
이 테스트는 native 호출을 결정적 fake로 대체하므로 **하드웨어가 필요 없다.**
`io_error` 경로가 즉시 시그널되는지도 같은 파일에서 확인한다.

### 1-3. per-write flush 제거 (부분 유닛 지연 발행)

**문제.** `shim_send_iov`(`:777-785`)는 모든 send/write 끝에서 `dmesh_flush(c)`를
호출한다. `dmesh_post_send`는 이미 `dmesh_flush_full`로 **완성된** 8 KiB 유닛만
발행하므로(`src/dmesh_api.c:22`), 이 flush는 오직 미완성 꼬리 유닛을 강제 발행하는
역할만 한다. 결과적으로 64B 요청 하나가 디스크립터 하나, 포워드 링 엔트리 하나,
DPA 처리 하나, 되돌아오는 TX_ACK 하나를 만든다. 심이 native의 배칭을 스스로
무력화하고 있다.

`writev`/`sendmsg` 한 번 안의 여러 iov는 이미 마지막에 한 번만 flush되므로,
이 항목이 노리는 것은 **서로 다른 send 호출 사이의 병합**이다.

**선행 검증 — 완료.** 이전 판은 "`dpumesh_tx_reserve`/`tx_next_send`의 owner-only가
'특정 스레드'인지 '동시에 한 스레드'인지 확인 전에는 구현하지 않는다"로 막아
두었다. 코드로 답이 난다.

- TX 커서(`tx_w/tx_c/tx_s/resv_*/pblk/recyc/blk_used/nblk_owned/nrec/tail_blk/
  head_blk_next`)는 전부 plain 필드다. PE와 공유되는 것은 `tx_f`(atomic),
  `su_head`/`su_tail`(acquire/release), `su_done[]`(FIFO 규율로 인덱스 비겹침)뿐이다.
- 스레드 ID 검사도 TLS도 없다. `dmesh_core.c:1546`의 "OWNER-only"는 **역할 이름이지
  스레드 정체성이 아니다.**

→ 요구사항은 **"동시에 한 스레드"**다. 상호배제와 happens-before만 있으면 dispatcher가
남의 QP를 flush해도 된다. **블로커 해제.**

**단, `e->tx_mu`를 잡으면 데드락이다.** 이전 판이 제안한 `e->tx_mu` + `e->mu` 조합은
쓰면 안 된다.

```
앱:          shim_send_iov :728 lock(tx_mu) → :743 unlock(mu) → :768 wait_writable
                                                        ← tx_mu를 쥔 채 park
dispatcher:  TX_READY → pfd_tx_ready(:347) → e->mu → fd_unblock_tx_locked
                                                        ← 그 park를 푸는 유일한 경로
```

dispatcher가 `tx_mu`를 기다리면 `tx_mu`를 쥔 앱은 dispatcher가 보낼 POLLOUT을
기다린다. **`e->mu`만 잡고, `trylock`으로 잡는다.** `e->mu`는 어느 경로에서도
blocking을 가로질러 잡히지 않으므로(`shim_send_iov`는 `:743`에서 풀고 park) 이것으로
충분하다. trylock 실패는 다음 틱으로 미룬다 — 그 QP는 지금 소유자가 쓰는 중이고,
소유자가 곧 자기 flush 지점을 지난다.

**구현.**

- `pfd_t`에 `tx_dirty`를 추가한다. `stream_write_locked`가 커밋했지만 꼬리 유닛이
  미발행 상태로 남으면 세운다.
- `shim_send_iov` 말미의 무조건 `dmesh_flush`를 제거한다 — **단, 아래 "지연 정책"의
  self-clocking 조건을 먼저 본다.**
- flush 지점을 다음으로 옮긴다.
  1. `shim_recv`가 park하기 직전(1-1의 드레인 시도 직전) — 요청/응답 앱을 덮는다.
     **`self`만이 아니라 dirty 리스트 전체를 flush한다.** `write(A); write(B);
     read(A); read(B)` 패턴에서 `self`만 flush하면 B의 바이트가 플러셔까지 묶인다.
     리스트가 짧아 비용이 없고 지연 서프라이즈 한 부류가 사라진다. (현재 벤치는
     스레드당 연결 1개라 이 차이가 드러나지 않는다.)
  2. `shutdown(SHUT_WR)`, `close`, `dmesh_destroy_qp` 경로 — 이미 graceful close가
     `dmesh_flush`를 부른다(`dmesh_core.c:2459`).
  3. **바운드 플러셔**: dispatcher가 `poll` 타임아웃으로 깨어나 dirty pfd를
     flush한다. 심은 `poll`/`select`/`epoll_wait`를 가로채지 않으므로, 쓰기만 하고
     우리 코드 안에서 절대 blocking하지 않는 앱이 존재한다. 이 경로가 없으면 그런
     앱의 바이트가 무기한 묶인다 — 성능이 아니라 **correctness 문제**다.
     플러셔는 `g_poll_mu` **밖에서** 돈다(`e->mu`만 필요하므로, 대기자의 trylock을
     헛되이 실패시키지 않는다).

**dirty 리스트의 락 — `g_q_mu`를 재사용하면 데드락이다.**

`stream_write_locked`는 `e->mu` 아래에서 돈다(`:739-743`). 거기서 전역 dirty
리스트에 링크하면 **`e->mu` → `g_q_mu`**가 된다. 그런데 dispatcher의 close-reap은
`g_q_mu`로 e를 꺼낸 뒤 `e->mu`를 잡는다(`:463-477`) — **`g_q_mu` → `e->mu`**.
역순이다.

전용 `g_dirty_mu`를 두고 순서를 **`e->mu` → `g_dirty_mu`** 하나로 고정한다.
플러셔는 snapshot-and-release로 그 순서를 지킨다.

```
플러셔: lock(g_dirty_mu) → 리스트 전체를 떼어냄 → unlock(g_dirty_mu)
        → 각 pfd에 trylock(e->mu) → flush → unlock
        → trylock 실패분만 다시 lock(g_dirty_mu)로 재링크 (이때 e->mu는 안 쥠)
```

**`pfd_retire`가 dirty 리스트에서 반드시 언링크해야 한다.** 빠뜨리면 플러셔가
해제된 메모리를 걷는다.

**지연 정책 — 타이머가 아니라 self-clocking(Nagle)으로.**

무조건 지연 + 고정 주기 플러셔는 지연 비용을 과소평가한다. 측정된 preload p50이
31K rps에서 **213 µs**이므로, 100 µs 주기는 최악 +100 / 평균 +50 µs, p50 기준
**+23%**다. 무시할 수준이 아니다.

더 중요한 건 대상이다. **epoll 기반 논블로킹 서버**는 `shim_recv` park에 절대
도달하지 않으므로 모든 요청이 플러셔 주기를 온전히 겪는다. 플러셔 주기가 그
앱들의 지연 하한이 된다 — 그리고 그게 실제 서버의 다수다.

따라서 조건부 지연으로 간다.

```
미완료(unacked) 유닛 없음  → 즉시 flush        (저부하: 지연 증가 0)
미완료 유닛 있음           → 지연 (병합)        (고부하: 상각 이득 그대로)
```

이것이 정확히 Nagle이고, 이 항목이 이미 그은 비유(`TCP_NODELAY`가 꺼진 것과
유사한 계약 변화)를 그대로 구현한 것이다. 신호는 코어에 이미 있다 —
`psl->su_head != psl->su_tail`이 "shipped but unacked"이며 `tx_wait_qp_retryable`이
같은 비교를 쓴다. 1-1(B)가 어차피 `dmesh_core.h`에 내부 함수를 추가하므로 같은
등급의 추가다.

```c
/* 이 QP에 아직 ACK되지 않은 전송 유닛이 있는가. 공개 ABI 불변. */
int dmesh_tx_inflight(dmesh_qp_t *c);
```

이렇게 하면 바운드 플러셔가 **지연 결정 요소가 아니라 순수 안전망**이 되므로
주기를 1 ms 이상으로 늘려도 된다. 100 µs 타이머 방식은 이 대안이 어떤 이유로
불가능할 때의 fallback으로만 남긴다.

**예상 효과.** 호스트 측 이득은 요청당 `emit_desc` + `dpumesh_enqueue`(링 티켓 +
디스크립터 쓰기) 절감으로 중간 수준이다. 큰 이득은 DPU 쪽이며, 이제 추정이 아니라
측정치가 있다. 같은 런의 `results.csv` `dpu_arm_cores`(N/K/A=32/8/8, 64B):

| 경로 | 31K rps | 상위 점 | 미포화 구간 한계 ARM/요청 |
|---|---:|---:|---:|
| dpumesh-native | 1.900 core | 3.128 @ 3.00M | **0.41 µs** |
| dpumesh-preload | 2.177 | 3.565 @ 124K | **15.0 µs** |

**심은 요청당 DPU ARM을 native보다 36배 쓴다.** 절대값으로도 preload는 124K rps에
3.57 ARM 코어를, native는 24배 많은 3.00M rps에 3.13 코어를 쓴다. 원인이 바로
per-write flush다: 64B 요청 하나가 디스크립터 하나, 포워드 링 엔트리 하나, DPA 완료
하나, ARM 파싱 한 번, TX_ACK 하나를 만든다. knee에서 연결당 약 5개가 병합되면 이
고정비가 5분의 1이 된다.

**사전 등록 예측:** 1-3 적용 후 preload의 한계 ARM/요청이 15.0 µs에서 3 µs 아래로
내려간다. 이 항목이 `SWEEP.md`의 ARM 코어 비용(8/8에서 667%)을 직접 낮추는 유일한
심 수정이며, 검증 지표는 §3-1의 재분석 스크립트가 뽑는 `dpu_arm_cores` 기울기다.

**리스크.**

- 지연 발행은 앱이 write 후 우리 코드 밖에서 오래 계산하는 경우 그만큼 지연을
  더한다. self-clocking을 쓰면 미완료 유닛이 없는 상태 — 즉 정확히 그 경우 — 에는
  즉시 발행되므로 이 리스크가 닫힌다. 고정 타이머 fallback을 쓸 때만 플러셔 주기가
  최악 지연의 상한이 되며, **그 값은 반드시 문서화한다.**
- 락 순서가 두 개 늘어난다(`e->mu` → `g_dirty_mu`, 플러셔의 `trylock(e->mu)`).
  1-1의 락 순서 표에 함께 적어 한 곳에서 관리한다.
- `dmesh_tx_inflight`는 `su_head`/`su_tail`을 acquire로 읽을 뿐이므로 PE 핫패스를
  건드리지 않는다. 다만 소유 스레드가 아닌 곳(플러셔)에서 호출되므로 두 카운터를
  각각 원자적으로 읽는다는 점을 주석에 남긴다 — 값이 한 틱 낡아도 판정은 안전하다
  (낡아서 "in-flight 있음"으로 보이면 flush를 한 번 미룰 뿐이고, 그 QP는 플러셔가
  다음 틱에 다시 본다).

**검증.** 세 가지 모두 **하드웨어 없이** 가능하다.

1. `preload_api_contract_test.c`: (a) 미완료 유닛이 없을 때 `write()` 하나가 즉시
   발행되는지, (b) 미완료가 있을 때 연속 `write()` 두 개가 유닛 하나로 합쳐지는지,
   (c) 쓰기만 하고 blocking하지 않는 앱의 바이트가 플러셔 주기 안에 반드시
   발행되는지(무기한 묶이지 않는지).
2. dirty 리스트 lifetime: `close()` 직후 `pfd_retire`가 언링크했는지 — 재링크 경합을
   유도하는 멀티스레드 케이스를 넣는다.
3. 데드락 회귀: 플러셔가 `e->tx_mu`를 잡지 않는다는 것을 정적으로 강제한다(그
   함수에서 `tx_mu`를 참조하지 않는지 grep 기반 체크를 `abi_contract_test.sh`에 추가).

### 1-4. RX 큐 노드 freelist

**문제.** `pfd_queue_rx`(`:308`)는 RECV 이벤트마다 `preload_rx_t`를 `calloc`하고
`rx_once`(`:626`)는 마지막 바이트를 소비할 때 `free`한다. 요청당 malloc/free 한
쌍이 돈다. 노드는 24바이트 남짓의 고정 크기이고 수명이 짧아, 할당자를 거칠
이유가 없다.

**구현 — per-pfd 리스트가 아니라 드레이너 TLS 매거진으로.** 이전 판은 "`pfd_t`마다
freelist를 두고 `e->mu` 아래에서만 만지므로 새 동기화가 필요 없다"고 했는데,
**현재 `calloc`은 `e->mu` 밖이다** — `:308` 할당, `:320` 락. per-pfd 리스트를 쓰려면
pop을 락 안으로 옮겨야 하고 임계구역이 그만큼 늘어난다.

그리고 1-1 적용 후에는 "dispatcher 스레드" 하나가 아니라 아무 앱 스레드나 드레이너가
되므로, 노드를 만드는 주체 자체가 여럿이 된다. **드레이너 스레드의 TLS 매거진**이
더 깔끔하다 — 락이 전혀 필요 없고, `doca/dpu_proxy.c`의 `tls_arr_mag` 패턴
(`px_arrival_alloc`, `:472-487`)을 그대로 옮기면 된다. 상한(예: 파이프라인 깊이
정도)을 두어 유휴 스레드가 노드를 쌓아두지 않게 하고 초과분은 `free`한다.
소비는 `rx_once`가 `e->mu` 아래에서 하므로, 반환은 매거진이 아니라 즉시 `free`하거나
(단순) 반환용 TLS 리스트를 따로 둔다(빠름). 전자로 시작한다.

**예상 효과.** 요청당 약 0.2 µs 추정. **상각에는 기여하지 않는다** — 요청당 상수를
줄일 뿐이고, 근거 절이 정한 대로 이 문서의 1차 지표는 `post_sat_amplification`이다.
따라서 단독 knee 이동은 거의 없다고 본다.

**우선순위.** 위 이유로 **보류**한다. 1-2/1-3/1-1을 적용해 상각이 붙은 뒤,
`saturation.csv`의 `slope_us_per_req`에서 남은 요청당 비용이 유의미할 때만 착수한다.

**리스크.** 낮다. 노드 재사용 시 `pos`와 `next`를 반드시 초기화해야 한다
(`calloc`이 하던 일을 손으로 한다). TLS 매거진은 스레드 종료 시 회수 경로가 필요하다
— `pthread_key_create`의 destructor로 남은 노드를 `free`한다.

### 적용 순서

이전 판은 `1-2 → 1-1(A)+(B) → 재측정 → 1-4 → 1-3`이었다. 두 전제가 바뀌었다.
**재측정이 불가능**하고(DPU 서버 고장), **1-3의 블로커가 해제**됐다. 상각 기여도
기준으로 다시 매긴다.

| 단계 | 항목 | 근거 | 하드웨어 |
|---|---|---|---|
| 1 | 1-2 + host-only 테스트 | 저위험·독립·상각 | 불필요 |
| 2 | **1-3** (self-clocking 방식) | 상각 기여 최대, **유일한 측정 근거(ARM 36배)**, 1-1과 독립 | 불필요 |
| 3 | 1-1(A), **단독 커밋** + 멀티스레드 테스트 | 상각 | 불필요 |
| 4 | 1-1(B), **별도 커밋** + 지연 주입 빌드 | 상각 | 불필요 |
| 5 | 1-4 | 상각 무기여 — **보류 판정** | — |
| 6 | 복구 후 커밋별 측정 | `post_sat_amplification` 비교 | 필요 |

**1-3이 2번으로 올라간 이유가 셋이다.** (a) 선행 검증이 코드 리뷰로 끝났고,
(b) 이 문서에서 유일하게 사전-사후 비교 가능한 측정 근거가 있으며(한계 ARM
15.0 → 0.41 µs/req 목표), (c) 1-1과 의존 관계가 없다. 이전 판이 1-3을 마지막에
둔 것은 선행 검증이 미해결이었기 때문인데, 그 전제가 사라졌다.

1-1은 (A)와 (B)를 한 항목으로 취급하되 **커밋은 반드시 나눈다.** 지금은 중간
측정을 할 수 없으므로, 커밋 경계가 하드웨어 복구 후 되돌아가 (A)와 (B)의 기여를
분리 측정할 수 있는 **유일한 수단**이다. 1-3도 같은 이유로 단독 커밋으로 둔다.

각 단계는 코드보다 **테스트를 먼저 쓴다.** 하드웨어가 없는 동안 회귀를 잡을
수단이 `tests/preload_api_contract_test.c`뿐이기 때문이다. 그 파일은 native
호출을 결정적 fake로 대체하므로 위 1~4단계 전부를 덮을 수 있다.

§3과의 관계: 이 절은 코드 작업, §3의 적용 순서는 측정 작업이며 서로 독립이다.
다만 **§3-1(`analyze_saturation.py`)은 6단계보다 먼저 있어야 한다** — 위 표의
1차 지표 `post_sat_amplification`을 산출하는 것이 그 스크립트이기 때문이다.
둘 다 하드웨어가 필요 없으므로 병행해도 된다.

### 이번에 하지 않는 것

- `g_tbl_mu`(`:143`) 전역 뮤텍스와 `e->mu`/`e->tx_mu`는 요청당 4~6회 잡히지만,
  무경합 뮤텍스 기준 요청당 100 ns 수준이다. 미포화 구간 한계 비용 6.7 µs 중
  비중이 작고, 무엇보다 **요청당 상수라 상각에 기여하지 않는다**(1-4와 같은 이유).
  1-1/1-3 적용 후 `saturation.csv`의 `slope_us_per_req`가 유의미하게 남을 때만
  손댄다. 성급한 lock-free 전환은 `pfd_retire`/`active_ops`의 해제 경합을 다시 열
  위험이 크다.
- 스레드별 EQ(`DMESH_MAX_EQ=64`이므로 코어 API는 이미 지원한다)는 1-1보다 이득이
  크지 않으면서 QP 수명과 epoll 호환성 문제를 훨씬 크게 만든다. 1-1 측정 후에도
  EQ 경합이 병목으로 남을 때만 재검토한다.
- `tests/preload_api_contract_test.c`는 단일 스레드 기준이다. 1-1(A)가 "아무 앱
  스레드나 드레이너가 된다"로 바꾸므로 멀티스레드 케이스 추가가 **선행 조건**이지
  후속 작업이 아니다. 위 적용 순서 3단계에 포함되어 있으며 따로 절을 만들지 않는다.

---

## 2. 측정 하네스 (`bench/suite/l4_proxy_data.sh`)

### 2-1. scout 단계에 CPU 관측치 기록

**문제.** knee 판정은 `scout_clean`(`:744`)이 achieved 비율, schedule 비율,
generator drop, p99, fail/reorder/overflow만 본다. `scout.csv`에는 CPU 컬럼이
아예 없다. 그 결과 "코어가 이미 포화인데 clean으로 통과한" 후보를 사후에
구별할 수 없다.

현재 데이터가 이 문제를 실제로 드러낸다. `envoy-permissive` 64B의 knee는
1.416 Mrps인데, load 단계 기록으로는 **31K rps에서 이미**
`client_app 0.234 + client_sidecar 0.724 = 0.958`로 클라이언트 코어(core 22)가
포화 상태다. 즉 이 경로는 측정한 모든 rate에서 코어가 차 있었고, 31K→1.42M 46배
구간 내내 합계가 0.96~0.99로 고정이다.

retained 행으로 사후 확인은 된다. 사이드카는 0.724 core@31K(23.5 µs/req) →
0.415@1.42M(0.29 µs/req)로 80배 상각되고, 같은 구간에서 앱 몫은 0.234 → 0.561로
오른다. 이상 현상이 아니라 **L4 바이트 프록시의 정상 거동**이다 — Envoy는 벤치
프레임을 파싱하지 않으므로 비용이 요청당이 아니라 KB당이고, 1.42 Mrps는 Envoy에게
"요청 142만 개"가 아니라 "초당 90 MB"다.

문제는 **scout 자체는 여전히 눈이 멀어 있다**는 것이다. knee를 정하는 단계가
포화를 못 보므로, retained 행이 들어오기 전까지는 어떤 knee가 코어 제한인지
구분할 수 없다.

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
`--quick`으로 새 컬럼이 채워지는지 확인. 컬럼을 추가하는 순간 기존 `--out`
디렉터리로는 재개할 수 없다 — 스크립트가 헤더 불일치를 `die`로 막는다
(`:693`, `:518`). **`COLLECTOR_VERSION` 상향과 새 출력 디렉터리는 선택이 아니라
강제 사항이다.** `host_core_budget`은 새로 계산하지 말고 이미 출력에 있는
`core_budget.csv`를 단일 출처로 쓴다. `knees.csv`에는 highest-clean뿐 아니라
**first-bad 지점의 관측치도** 남긴다 — 그래야 "knee 아래에서 이미 코어가
포화였는가"를 사후에 판정할 수 있다.

### 2-2. knee의 retained 재현성 기록

**문제.** knee는 10초 vote 2-of-3으로 확정되는데(`scout_point`, `:770-780`),
같은 rate의 retained 3회가 그 판정과 어긋난다.

| config / payload | knee rate | retained 3회 achieved | 3회 p99 |
|---|---:|---|---|
| plain 64B | 476,937 | 322,440 / **476,937** / 301,719 | 11.8 ms / 409 µs / 9.9 ms |
| envoy-perm 64B | 1,415,535 | **1,415,056** / 1,182,543 / **1,414,934** | 943 µs / 7.9 ms / 867 µs |
| envoy-perm 64B (0.85×) | 1,203,205 | **1,203,156** / 1,181,771 / **1,203,157** | 458 µs / 3.7 ms / 439 µs |
| dpumesh-native 64B | 3,000,000 | 3,000,084 / 3,000,132 / 3,000,042 | 356 / 329 / 365 µs |

**plain은 3회 중 2회가 무너졌는데도 knee로 확정됐다.** envoy는 3회 중 1회가
knee와 그 아래 점 양쪽에서 무너진다. 반대로 native는 3회 모두 완벽하다 — 이
데이터셋에서 **유일하게 안정적인 knee가 유일하게 측정되지 않은 knee**다.

`L4_PLAN.md:180-181`이 이미 "retained 세 반복에서 clean/bad가 비단조로 반복되면
추가 3회 측정한다"고 정했지만, 자동화가 없고 `knees.csv`에 기록도 남지 않는다.

**구현.**

- load 단계 종료 후 `annotate_knee_stability()`를 한 번 돌려
  **`knee_stability.csv`(신규 파일)**를 쓴다. `knees.csv`의 스키마는 건드리지
  않는다 — `discover_knee`의 재개 체크(`:827-829`)와 `build_rate_plan`(`:910-918`)이
  그 파일을 읽으므로 컬럼 추가는 재개 호환성 위험만 늘린다.
- 컬럼: `config, payload_bytes, knee_rps, retained_reps, retained_clean,
  min_achieved_ratio, max_p99_us, knee_unstable`.
  `knee_unstable = (retained_clean < retained_reps)`.
- 판정 기준(`scout_clean`)은 **바꾸지 않는다.** 바꾸면 이미 수집한 데이터와
  비교가 끊긴다. 관측치만 남긴다.
- 그래프 1은 이미 "세 반복 중 둘 이상 clean이면 sustained-clean"
  규칙이 있으므로(`L4_PLAN.md:109-111`), `knee_unstable`이 선 knee는 caption에
  "knee unstable (n/3 clean)"로 표기한다.

**예상 효과.** 성능 변화 없음. plain의 knee 476,937을 그대로 인용할지, 안정적인
405,396(3회 모두 clean)을 함께 표기할지 결정할 근거가 생긴다.

**리스크.** 없음. 순수 후처리다.

**검증. 하드웨어 불필요** — 기존 `l4-20260728-113431/results.csv`에 돌려서 위 표를
재현하면 통과다. 지금 바로 작성·검증할 수 있다.

### 2-3. rate 격자의 구멍과 포화 시작점 해상도

**문제.** `build_rate_plan`(`:905-918`)은 공통 rate를 **다섯 구성 중 가장 낮은
knee**의 25/50/75/90%로 잡는다. 64B에서 min_knee는 preload의 123,583이므로 공통
rate는 30,896 / 61,792 / 92,687 / 111,225다. 여기에 각 config knee의
85/100/105%가 붙는다. 결과적으로 `rates.csv`는 **저부하 4점 + knee 근처 3점**이고
그 사이가 비어 있다.

| config (64B) | 공통 4점 상한 | 다음 점 | 구멍 |
|---|---:|---:|---:|
| dpumesh-native | 111,225 | 2,550,000 | **23배** |
| envoy-permissive | 111,225 | 1,203,205 | 11배 |
| plain | 111,225 | 405,396 | 3.6배 |

두 가지 결과가 따라온다. (a) **포화 시작점을 찾을 해상도가 없다.** plain은
공통 4점의 마지막(111,225)이 우연히 포화점이라 잡혔지만, envoy는 첫 점부터
포화라 시작점이 4점 격자 아래에 있고, native는 포화 자체를 안 한다.
(b) §1의 한계 CPU fit이 사실상 두 점 fit이 된다.

**구현.**

- config별 자기 knee 기준의 로그 격자를 추가한다.
  `CONFIG_SPAN_FACTORS="${CONFIG_SPAN_FACTORS:-0.05 0.15 0.30 0.50 0.70}"`.
  중복 제거는 기존 `build_rate_plan`의 정수 dedup을 그대로 쓴다.
- 비용을 줄이려면 추가 rate만 **rep 1회**로 돌리고 `rate_index`의 `source`를
  `span`으로 표시해 공통/knee 점과 섞지 않는다.
  - 전 rep(3회): 5 config × 3 payload × 5 rate × 3 rep = **225행 ≈ 38분**
  - 1 rep: **75행 ≈ 13분**
  - `L4_PLAN.md:58-62`의 예상 wall time 98분(timed window)이 111~136분이 된다.
- `source=span` 행은 그래프 2의 실선에 넣지 않고 §3-1의 회귀 입력으로만 쓴다.

**예상 효과.** 성능 변화 없음. 포화 시작점이 데이터로 결정되고, 한계 CPU 회귀의
표본 수가 config당 2~4점에서 6~9점으로 늘어 R²를 주장할 수 있게 된다.

**리스크.** wall time 증가와, 추가 점이 knee 탐색 뒤에 실행되므로 그 사이
열·드리프트가 섞일 수 있다. 기존 load 단계와 같은 `rotate_configs` 규칙으로 config
순서를 회전시키고, 홀수/짝수 rep의 rate 방향 교대도 그대로 적용해 분산한다.

**검증.** `--dry-run`이 늘어난 행 수를 출력하는지, `rates.csv`의 `source` 컬럼에
`span`이 나타나는지.

### 2-4. native right-censored 해제

**문제.** `knees.csv`의 native 세 행이 모두 `right_censored`다.

```
dpumesh-native,64,3000000,NA,NA,right_censored,10
dpumesh-native,1024,3000000,NA,NA,right_censored,10
dpumesh-native,8192,610352,NA,NA,right_censored,10
```

cap은 `discover_knee`(`:831-836`)에서 만들어진다.

```awk
wire = SCOUT_MAX_GBPS*1e9/(8*payload)
cap  = min(wire, SCOUT_MAX_RPS)
```

| payload | wire cap (40 Gb/s) | hard cap | 실제 cap | 걸린 쪽 |
|---|---:|---:|---:|---|
| 64 B | 78,125,000 | 3,000,000 | 3,000,000 | `SCOUT_MAX_RPS` |
| 1024 B | 4,882,813 | 3,000,000 | 3,000,000 | `SCOUT_MAX_RPS` |
| 8192 B | 610,352 | 3,000,000 | **610,352** | `SCOUT_MAX_GBPS` |

`right_censored`는 램프가 cap까지 올라가는 동안 bad를 한 번도 못 만나
`bad == 0`인 상태다(`:881-885`). 즉 **"knee ≥ cap, 실제 값 미지"**다.

3.0 M에서 무엇 하나 힘들지 않았다: achieved/offered = 1.0000, `generator_drops` = 0,
p99 356 µs (SLA 10 ms), `cli_app` 0.541. **시스템이 만난 벽이 아니라 스크립트가
램프를 멈춘 지점이다.**

8192 B는 성격이 다르다. 610,352(= 40 Gb/s)에서 `generator_drops` 3,944,
`drop_ratio` 0.000646으로 허용치(0.001)의 **65%**까지 찼고 p99도 2,050 µs다.
`SWEEP.md`의 관측 최대 41.2 Gb/s와 겹치므로 **8 KB의 censoring은 사실상 실제
천장에 닿은 것이고, 64 B / 1 KB만 순수 인공 상한**이다. 논문 서술에서 구분해야 한다.

**구현. 함정 먼저:** `SCOUT_MAX_RPS`만 올리면 아무 일도 일어나지 않는다. 램프는
5,000에서 1.35배씩 최대 24스텝이다(`:31-34`).

```
3.0 M 도달 : ln(600)/ln(1.35)  = 21.3 → 22스텝  (로그의 pilot23/validate23과 일치)
10  M 목표 : ln(2000)/ln(1.35) = 25.3 → 26스텝 > SCOUT_MAX_STEPS(24)
```

스텝 예산에 먼저 걸려 **여전히 `right_censored`, 다만 더 낮은 rate에서** 끝난다.
`SCOUT_MAX_STEPS`를 같이 올리거나 `SCOUT_GROWTH`를 키워야 한다.

```bash
SCOUT_MAX_RPS=10000000 SCOUT_MAX_STEPS=28 \
OUT=bench/report/data/l4-native-ceiling \
  ./bench/suite/l4_proxy_data.sh --no-deploy
```

그 다음 벽은 클라이언트 코어다. §1의 0.12 µs/req로 외삽하면
`3.00M + (1.000 − 0.541)/0.1206 µs ≈ 6.8 Mrps`에서 코어가 찬다. 그 위를 보려면
코어를 늘려야 하는데, "1 client core + 1 server core"는 L4 데이터셋 전체의 비교
기반이며 `record_and_validate_core_budget`(`:511-516`)이 강제한다.
→ **본 매트릭스를 고치지 말고 native 전용 별도 런으로 분리한다.** `CONFIGS`를
`dpumesh-native` 하나로 좁히면 `expected_cores`가 2가 되어 검사를 끄지 않고도
통과한다. 그 위에서 `THREADS`를 4→8로, 클라이언트를 2코어로 준다. 결과는 논문의
5-way 표에 넣지 않고 "native headroom" 보조선으로만 쓴다.

**예상 효과.** DPU 데이터 평면의 실제 천장이 처음으로 측정된다. 이 값이 없으면
심 최적화의 목표치도 정할 수 없다.

**리스크.** 6.8 M 근처에서는 생성기 자체가 한계에 접근한다(2-5). knee가 나오더라도
그것이 경로의 한계인지 생성기의 한계인지 `scout.csv`의 `reason` 컬럼으로 먼저
판별해야 한다 — `scheduler`/`generator_drop`이면 생성기, `achieved`면 경로,
`p99`면 지연이다.

**검증.** `knees.csv`의 `status`가 `bracketed`로 바뀌고 `bracket_ratio`가 채워지는지.

### 2-5. 생성기 자체 상한을 따로 측정

**문제.** "경로가 못 따라간 것"과 "생성기가 못 만든 것"을 구분할 근거가 지금은
`schedule_ratio`와 `generator_drop_ratio` 두 게이트뿐이다. 그리고 8 KB native는
이미 그 예산의 65%를 쓰고 있었다(2-4).

`bench_dpumesh.c:397`이 `w[i].rate = rate/threads`이므로 THREADS=4에서 3.0 M이면
스레드당 750 Krps, 도착 간격 1.33 µs다. 루프는 매 pass `bench_now_sec()`를
호출하고 할 일이 없으면 2 µs `nanosleep`한다(`:315`). 6.8 M이면 간격이 0.57 µs가
되어 두 가지 모두 유의미해진다.

**구현.** 경로를 타지 않는 self-test 제어 명령을 추가한다.

```
SELFTEST <payload> <threads> <dur> <rate> <arrival>
```

`issue()`를 호출하지 않고 도착 시각만 소비하며 `scheduled`, `drops`, 실제 경과를
보고한다. `bench_dpumesh.c`와 `bench_sock.c` 양쪽에 같은 형태로 넣는다(둘의 생성기
루프가 다르므로 각각의 상한이 필요하다). 결과를 `meta.txt`에 기록하고, 모든 knee가
그 상한의 몇 %인지 `knee_stability.csv`에 `generator_headroom_ratio`로 남긴다.

**예상 효과.** 성능 변화 없음. knee가 생성기 상한의 80%를 넘으면 그 점의 인과
주장을 보류할 수 있다.

**리스크.** 낮다. 새 명령이고 기존 경로를 건드리지 않는다.

**검증. 하드웨어 불필요** — self-test는 DPU도 k8s도 필요 없다. 아무 리눅스 박스에서
1코어에 4스레드를 핀하고 rate를 올려 `drops`가 0인 최대값을 3회 재현하면 된다.
지금 할 수 있는 항목이다.

### 2-6. DPU ARM을 scout에도 기록하고, 유휴 주장 정정

**문제 (a).** `dpu_arm_cores`는 load/idle 행에만 있고 `scout.csv`에는 없다
(`scout_one`, `:702-742`). knee를 정하는 단계에서 DPU가 얼마나 쓰였는지 모른다.

**문제 (b) — 기존 문서와 모순.** 이 런의 idle 행(N/K/A=32/8/8, 즉 **A=8**):

| config | rep 1 | rep 2 | rep 3 |
|---|---:|---:|---:|
| dpumesh-native | 0.137 | 0.130 | 0.133 |
| dpumesh-preload | 0.134 | 0.132 | 0.132 |

`bench/report/REPORT.md:66-69`은 K=8/**A=2**에서 "무부하에서 각 폴링 워커가 약 1
ARM 코어를 쓰고 (…) 약 2코어 floor"라고 적는다. **A=8에서 0.13코어인데 A=2에서
2코어일 수는 없다.** 워커의 arm→recheck→1 ms `epoll_wait` park 경로
(`doca/dpu_worker.c:493-511`)는 정상 동작하며, REPORT.md의 2코어 floor 주장이
틀렸다.

부하가 걸리면 워커는 곧바로 hot 상태로 간다(64B, A=8):

| offered | native | preload |
|---:|---:|---:|
| idle | 0.13 | 0.13 |
| 30,896 | 1.900 | 2.177 |
| 111,225 | 2.893 | 3.357 |
| knee | 3.128 @ 3.00M | 3.565 @ 124K |

즉 "폴링 ARM 비용은 부하와 무관하게 A에 비례한다"(`REPORT.md:99`)도 다시 써야
한다. **유휴에는 park하고, 트래픽이 조금이라도 있으면 hot으로 붙는다**가 맞다.

**구현.**

- `scout_point`의 10초 vote(`:761-779`)에만 `dpu_snapshot`(`:605-612`) 전후를 붙여
  `scout.csv`에 `dpu_arm_cores` 컬럼을 추가한다. pilot(2초)에는 붙이지 않는다 —
  ssh 왕복 2회 × 최대 360회는 비용이 크다.
- `dpu_snapshot`이 빈 문자열을 돌려주면 `NA`로 기록하고 계속한다. scout에서
  `die`하면 안 된다(`measure_one`의 `dpu_snapshot_error` 경로와 다르게 다룬다).
- `REPORT.md`의 "DPU ARM" 절을 이 런의 idle/load 값으로 교체하고, 2코어 floor
  문장과 "부하 무관" 문장을 삭제한다.

**예상 효과.** 성능 변화 없음. `A`를 고를 때 유휴 비용을 과대평가하지 않게 된다
(2코어 floor가 사실이었다면 `A`를 키우는 것이 훨씬 비쌌다).

**리스크.** scout 실행 횟수가 많아 ssh 지연이 vote 창에 섞일 수 있다. 스냅샷은
측정 창 **밖**에서 찍히므로(`measure_one`의 배치와 동일) achieved/latency에는
영향이 없지만, vote 간 간격이 늘어 wall time이 증가한다.

**검증.** `--quick`으로 `scout.csv`에 컬럼이 채워지는지. 그리고 A를 1/2/4/8로 바꾼
유휴 3회 측정으로 park 비용이 A에 선형인지(0.13/8 ≈ 0.016 코어/워커) 확인한다.

---

## 3. 측정 결과 해석 (`bench/report/`, `bench/suite/`)

수집기를 고치지 않고 이미 있는 CSV만으로 할 수 있는 작업이다. **전부 하드웨어가
필요 없다.**

### 3-1. 미포화 구간 자동 판정과 재분석 스크립트

**문제.** 지금은 사람이 표를 눈으로 보고 "여기서 포화했다"를 판단한다. §1의 표가
틀렸던 이유가 정확히 그것이고, 같은 실수를 그래프 4에서도 할 수 있다.

**구현.** `bench/suite/analyze_saturation.py` (신규).

- 입력 `results.csv`. `phase=load` 행을 config × payload로 묶고 achieved_rps 정렬.
- **포화 시작점** = `client_app_cores + client_sidecar_cores`가 처음으로
  `SAT_THRESHOLD`(기본 0.95) 이상이 되는 achieved rate.
- 그 **아래** 점들만으로 `host_busy_cores − idle_median`을 achieved_rps에 대해
  선형 회귀 → `slope_us_per_req`, `intercept_cores`, `r2`.
  `dpu_arm_cores`도 같은 방식으로 `arm_slope_us_per_req`를 뽑는다.
- 미포화 점이 3개 미만이면 slope를 내지 않고 `insufficient_unsaturated_points`로
  표기한다. **두 Envoy 경로가 여기 해당한다** — 최저 공통 rate(31K)부터 이미
  포화이므로 한계 CPU/요청을 낼 수 없다. 표에 빈칸으로 두고 "모든 측정점에서
  포화"라고 적는 것이 정직한 서술이다.
- `post_sat_amplification = knee_rps / sat_start_rps`를 같이 낸다. §1이 보여주듯
  **이것이 심과 커널 TCP를 가르는 실제 지표**다.
- 출력 `saturation.csv`: `config, payload_bytes, sat_start_rps,
  n_unsaturated, slope_us_per_req, intercept_cores, r2,
  arm_slope_us_per_req, knee_rps, post_sat_amplification, note`.

**예상 효과.** §1의 표가 자동 생성되고, `L4_PLAN.md:126-130`의 "R²<0.9이면 비선형—
대표 비용 아님" 규칙이 그림 그리는 단계가 아니라 데이터 단계에서 강제된다.

**리스크.** 없음. 읽기 전용 후처리다.

**검증.** 기존 `l4-20260728-113431`에 돌려 §1의 세 숫자(5.9 / 6.7 / 0.12 µs)와
포화 시작점 111K, 상각 배율 4.3 / 1.1을 재현하면 통과.

### 3-2. rps 단일 축 비교 금지

**문제.** knee를 rps로만 보면 64 B에서 "Envoy가 direct TCP보다 3배 빠르다"로
읽힌다. §1의 두 번째 표가 보여주듯 축이 다르다 — plain/preload는 연산 제한,
두 Envoy는 바이트 제한이다. 교차점은
`477,000 × p × 8 = 16.2e9 → p ≈ 4.2 KB`이고, 실제로 1 KB에서는 Envoy가 2.0배
앞서고 8 KB에서는 plain이 1.6배 앞선다.

독립 검증이 하나 있다. envoy-strict / envoy-permissive 비는
**0.98 (64 B) → 0.70 (1 KB) → 0.49 (8 KB)** 로 payload에 단조 감소한다. TLS 비용이
순수하게 바이트당이라는 것, 따라서 이 경로가 바이트 제한이라는 것의 교차 확인이다.

커널 측 비용도 같은 방향이다(`system_softirq_cores`):

| 경로 | rate | system softirq | 연산당 |
|---|---:|---:|---:|
| plain 64 B | 111 K ~ 500 K | **1.02 ~ 1.05 코어** | 2.6 ~ 9.4 µs |
| envoy-perm 64 B | 1.42 M | 0.457 | 0.32 µs |
| dpumesh-native 64 B | 3.00 M | **0.049** | 0.016 µs |

plain은 예산 밖에서 커널 softirq 코어를 통째로 하나 더 태운다.

**구현(문서 작업).** `bench/report/L4_PLAN.md`의 "L4 동등성과 비교 경계" 절에
다음 세 문장을 명시한다.

1. 모든 knee 표는 **rps와 Gb/s를 함께** 적는다. rps 단독 표는 만들지 않는다.
2. `plain`의 knee는 커널 TCP의 한계가 아니라, `bench_sock`의 "요청당 write 1회 +
   `TCP_NODELAY`" 패턴이 1 client core에서 낼 수 있는 **연산 수**다.
   64 B와 1 KB의 knee가 소수점까지 같다는 것이 그 직접 증거다.
3. 사이드카는 앱의 소켓 상대를 pod-to-pod veth에서 **같은 pod 내 loopback**으로
   바꾼다. 이 경로 교체(드라이버·qdisc·체크섬 없음, MTU 64 KB)가 이득의 대부분이며,
   Envoy 자체가 빠른 것이 아니다. Envoy는 L4 바이트 프록시라 벤치 프레임을
   파싱하지 않고, 1.42 Mrps를 "초당 90 MB"로 처리한다.

**예상 효과.** 성능 변화 없음. `SWEEP.md`와 함께 쓸 때 결론의 방어 가능성.

**검증.** 그래프 1의 caption과 모든 knee 표에 Gb/s 열이 있는지.

### 적용 순서

**지금(하드웨어 없이):** 3-1 재분석 스크립트 → 2-2 후처리 → 3-2 문서 →
2-5 생성기 self-test. 네 항목 모두 기존 CSV나 로컬 실행만으로 완결된다.
3-1을 먼저 하는 이유는 나머지 항목의 판정 기준을 그것이 정하기 때문이다.

**하드웨어 복구 후:**

1. `--preflight-only`로 배포·코어·바이너리 invariant 확인 (새 `--out`)
2. 2-1 + 2-2 + 2-3 + 2-6을 넣은 `COLLECTOR_VERSION=8`로 **전체 재수집**.
   컬럼이 늘어 기존 디렉터리 재개가 불가하므로 어차피 새 런이다.
3. 2-4의 native 전용 런 (별도 `--out`, `CONFIGS=(dpumesh-native)`)
4. 심 수정 커밋별 재측정: 1-1(A)만 / 1-1(A)+(B) / +1-3.
   각 단계에서 `saturation.csv`를 뽑아 비교한다.
   **1차 지표는 knee가 아니라 `post_sat_amplification`과 `arm_slope_us_per_req`다.**

### 이번에 하지 않는 것

- `scout_clean`의 판정 기준(`classify_open_result`, `:649-686`)은 건드리지 않는다.
  기준을 바꾸면 이미 수집한 네 개 런과 비교가 끊긴다. 2-2는 관측치만 추가한다.
- `plain`의 앱 syscall 패턴(`bench_sock.c`의 요청당 write 1회)은 바꾸지 않는다.
  그것이 이 실험이 재현하려는 현실적인 앱 동작이고, 바꾸면 네 POSIX 경로의
  동일-앱 전제가 깨진다. 대신 3-2에서 그 사실을 명시한다.
- perf 기반 DSO 분해(`L4_PLAN.md:119-125`)는 이미 설계가 있으므로 여기서 다시
  다루지 않는다. 다만 `results.csv`의 `perf_data` 컬럼은 load 행에서 항상 `NA`이며
  (`:1080`) 실제 프로파일은 `perf_manifest.csv`와 `perf/`에만 있다는 점을 분석
  스크립트가 알아야 한다.

---

## 부기

새 개선 대상이 생기면 이 문서에 절을 추가한다.

이전 판의 두 항목은 본문으로 승격했다. **native 상한 미측정**은 2-4가, **DPU ARM
요청당 비용**은 1-3(측정치 15.0 vs 0.41 µs/req)과 2-6이 다룬다. `SWEEP.md`의
운영점 선택(`A/K=8/8`이 41.2 Gb/s에 ARM 667%, `2/4`가 Gb/s당 최저 7.67)은
2-6이 유휴 floor 주장을 정정한 뒤 다시 계산해야 한다 — 유휴가 2코어가 아니라
0.13코어라면 `A`를 키우는 비용 평가가 달라진다.

현재 데이터가 가리키지 않는, 그러나 코드 검토로 확인된 대상은 다음과 같다.
측정 없이 설계할 수 있으므로 하드웨어 복구 전에 절을 추가할 후보다.

- **호스트 per-port inbox 누적**: `dpumesh_free_port`(`dmesh_core.c:2084`)는 슬롯
  재사용을 위해 `psl->inbox`를 해제하지 않는다. 깊이는
  `configure_landing_geometry`(`:975-982`)가 `rq_depth/L`로 정하므로 L=2에서
  포트당 4096 × 24 B = 96 KB다. 서버 측 uP는 DPU가 `[32768, 65536)`을 라운드로빈
  할당하므로(`object.h:337`) 연결 churn이 있으면 32,768 슬롯을 모두 방문해 최대
  약 3.1 GB까지 누적된다. 지속 연결 벤치라 드러나지 않았고, 짧은 연결 워크로드로
  넘어가는 순간 벽이다. 추가로 `ctx->ports[]` 초기화 루프(`:1267-1277`)가 약
  40 MB를 즉시 상주화한다.
- **forward 도어벨 부재 ↔ DPA park**: 호스트는 디스크립터를 공유 링에 쓰기만 하고
  DPA에 알리지 않는다. EU가 park하면(`dpa_kernel.c:351`, 262,144 스핀 후) 다음
  디스크립터를 알리는 유일한 수단이 ARM의 1 ms 주기 `DPA_MSG_WAKE`
  (`dpu_worker.c:318-331`)다. 저부하 지연 꼬리가 1 ms이고, 그걸 피하려면 N개 EU가
  상시 스핀해야 한다. `SWEEP.md`의 conc=1 지점(DPUmesh p50 175 µs vs TCP 59 µs)과
  방향이 맞는다.
- **크로스워커 ACK 반환 wake 비대칭**: `px_lane_enqueue`는 크로스오너일 때
  `px_engine_wake`를 호출하지만(`dpu_proxy.c:1018`), `px_queue_arrival_release`
  (`:607`)와 `px_ack_retry_handoffs`(`:2346`)는 push만 한다. 정상 토폴로지에서는
  uP의 `p % A == owner` 인코딩과 `L = A` 때문에 이 경로가 실행되지 않으므로 지금은
  무해하지만, 한 줄로 닫히므로 `L ≠ A` 확장 전에 맞춰둔다. 같은 이유로
  `cross_worker` MPSC와 `ack_releases` 크로스엔진 큐는 런타임에서 한 번도 실행되지
  않는 안전망이며, `worker_mpsc_queue_test.c`의 격리 테스트가 유일한 커버리지다.
