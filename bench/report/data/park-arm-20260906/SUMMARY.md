# Driver park/arm cost — lean → lean2 → lean3

기준일: 2026-09-06

## 판정

driver 루프의 park/arm 주변 syscall을 줄인 lean3은 같은 BlueField에서 lean보다 gRPC
CPU/RPC가 capacity 1.3~3.0%, matched 1.7~3.3% 낮다. CPU/RPC 범위는 여덟 조건 모두
겹치지 않는다. capacity RPC/s는 +0.5~+1.9%로 작고 64 B/8 KiB는 범위가 겹친다. opaque
TCP에서는 lean2→lean3이 CPU/RPC −9.7~−12.0%로 더 크다. 요청당 CPU가 작아 syscall
절감이 그대로 드러난다.

lean2(driver yield의 epoll 제거, wake eventfd 1회 읽기)만으로는 epoll이 RPC당 8.30→8.18로
거의 안 줄었다. `park_yield`의 발신자는 우리 yield가 아니라 tokio coop 예산(128연산)을
소진한 h2/hyper task의 `defer`였다. lean3이 얻은 것은 tokio 타이머 waker write(RPC당 0.6)와
PE clear 안의 epoll(0)(RPC당 1.0)이다.

## 변경

| arm | 내용 | SHA256 |
|---|---|---|
| lean | 고정비 제거 빌드(직전 캠페인) | `1577c3cc…e270` |
| lean2 | runtime 루프 yield를 self-wake로 교체(`park_yield` 회피 의도), wake eventfd read 1회 | `1f4507c9…5fe9` |
| lean3 | + maintenance `Sleep` 하나를 고정하고 `reset`만 수행, `clear_notifications(fired)`로 울린 PE만 clear, `dpu_main.c` fatal signal/`atexit` trace | `d511d0c0…b543` |

ABI: `dmesh_l7_driver_clear_notifications(void *, unsigned fired)`와
`DMESH_L7_NOTIFY_{COMPLETION,DMA,WAKE}` 추가. C 데이터 경로는 무변경.

## clean capacity (3회 중앙값)

| protocol | payload | RPC/s lean / lean2 / lean3 | CPU µs/RPC lean / lean2 / lean3 | lean3 vs lean | p99 µs lean→lean3 |
|---|---:|---:|---:|---:|---:|
| gRPC | 64 B | 35,724 / 36,276 / 36,121 | 179.14 / 176.72 / 175.15 | −2.22% | 2,725→2,662 |
| gRPC | 1 KiB | 34,518 / 34,930 / 34,833 | 186.21 / 183.54 / 181.69 | −2.43% | 3,452→3,102 |
| gRPC | 8 KiB | 21,363 / 21,608 / 21,465 | 307.93 / 303.87 / 304.05 | −1.26% | 4,852→4,584 |
| gRPC | 64 KiB | 12,961 / 13,167 / 13,213 | 596.62 / 586.91 / 578.53 | −3.03% | 7,458→7,740 |
| opaque | 64 B | — / 141,854 / 141,604 | — / 21.95 / 19.83 | lean3 vs lean2 −9.68% | 1,088→1,107 |
| opaque | 1 KiB | — / 125,647 / 131,095 | — / 24.59 / 21.64 | lean3 vs lean2 −12.01% | 1,149→1,120 |

CPU/RPC 범위(3회): 64 B 178.3–179.6 vs 174.1–175.2, 1 KiB 186.2–186.6 vs 180.6–181.9,
8 KiB 307.6–308.2 vs 303.3–305.2, 64 KiB 594.9–597.7 vs 578.1–578.6. RPC/s 범위는 1 KiB와
64 KiB만 분리된다. opaque 1 KiB capacity는 lean2 122,169–131,259 vs lean3
130,805–134,756로 분리되고 64 B는 겹친다.

## matched rate

| protocol | payload | CPU µs/RPC lean / lean2 / lean3 | lean3 vs lean | p50 µs lean→lean3 | p99 µs lean→lean3 |
|---|---:|---:|---:|---:|---:|
| gRPC | 64 B | 331.38 / 331.06 / 325.62 | −1.74% | 963→969 | 1,986→1,965 |
| gRPC | 1 KiB | 333.31 / 332.19 / 327.56 | −1.73% | 1,003→1,000 | 2,007→2,011 |
| gRPC | 8 KiB | 549.12 / 547.62 / 534.62 | −2.64% | 1,695→1,687 | 2,314→2,249 |
| gRPC | 64 KiB | 975.62 / 974.38 / 943.12 | −3.33% | 1,437→1,411 | 2,532→2,532 |
| opaque | 64 B | — / 73.69 / 66.50 | lean3 vs lean2 −9.75% | 249→259 | 489→484 |
| opaque | 1 KiB | — / 76.06 / 68.56 | lean3 vs lean2 −9.86% | 257→247 | 483→489 |

## syscall이 어디서 줄었나

`perf trace -s`로 워커 8개의 12초 run 가운데 6초를 셌다(RPC당, gRPC).

| payload | syscall | lean | lean2 | lean3 |
|---|---|---:|---:|---:|
| 64 B | epoll_pwait | 8.30 | 8.18 | 7.01 |
| 64 B | read (EAGAIN) | 2.37 (1.04) | 2.33 (1.03) | 2.73 (1.21) |
| 64 B | write | 1.20 | 1.18 | 0.68 |
| 64 KiB | epoll_pwait | 22.46 | 22.55 | 21.96 |
| 64 KiB | read (EAGAIN) | 1.51 (0.68) | 1.52 (0.68) | 1.65 (0.73) |
| 64 KiB | write | 1.17 | 1.16 | 0.92 |

호출 경로(`perf record -e syscalls:*` + callchain, 64 B):

- epoll_pwait: lean 61%가 tokio `Driver::turn`(그중 `park_yield` 53%, 실제 `park` 8%),
  39%가 DOCA 내부(`doca_pe_clear_notification` 22%, `doca_pe_request_notification` 9%).
  lean3에서 DOCA 몫이 RPC당 3.2→2.2로 줄었다. tokio `park_yield`는 4.4회 그대로다.
- write: lean 50%가 `Sleep::poll → Handle::reregister → mio Waker::wake`(매 select!의
  `sleep_until` 재등록), 25%가 `dpu_request_host_doorbell`(DMA 완료 콜백이 main을 깨움),
  25%가 `doca_pe_request_notification`의 eventfd write. lean3에서 tokio 몫이 0.59→0.03이다.
- read: 87%가 `mlx5dv_devx_get_event`. lean에서는 전부 clear 경로였고 lean3에서는
  clear 60%, `request_notification` 26%다. clear를 건너뛴 PE의 밀린 event를 DOCA가 다음
  arm 때 읽어 치우므로 read 총수는 줄지 않았다. 이 캠페인 이전에 wake eventfd의
  두 번째 read가 EAGAIN의 원인이라고 본 것은 틀렸다.

driver 루프 카운터(RPC당, capacity 중앙값): 64 B drain 8.87→8.04, idle 3.37→2.52,
arm 2.17→1.77; 64 KiB arm 1.43→1.31. 타이머 재등록이 깨우던 헛 pass가 사라진 결과다.

64 KiB PMU(별도 12초 run 3회, 가운데 8초): cycles/RPC 1,274,785→1,230,955(−3.4%),
instructions/RPC 546,334→536,301(−1.8%), IPC 0.429→0.435. lean3의 worker core는
7.76→6.75, context switch/RPC 0.187→0.294로 실제 스레드 park가 길어졌다(epoll 평균
7→12.7 µs).

## 남은 것

- tokio `park_yield` epoll(0) 4.4회/RPC(64 B), 약 20회/RPC(64 KiB): h2/hyper task의 coop
  예산 소진이 원인이라 driver 쪽에서는 못 없앤다. hyper가 spawn하는 H2 연결 task를
  `tokio::task::unconstrained`로 감싸면 사라지지만 task 공정성을 끄는 변경이다.
- arm 1.8회/RPC 중 실제 스레드 park는 0.4~0.7회다. select!가 Pending을 낸 뒤 stack task가
  signal을 올려 바로 깨우는 "헛 park"이며, arm 하나당 DOCA syscall 약 1회가 낭비된다.
  arm 전에 self-wake yield를 한 번 더 두면 줄일 수 있다.
- `dpu_request_host_doorbell → dpu_wake_main` write 0.3회/RPC는 main 스레드의 doorbell
  소유권 설계 문제라 이번 범위 밖이다.

## 사고

- lean2 64 KiB capacity 3회(11.7~12.1K RPC/s, CPU/RPC는 낮음)는 측정 중 호스트(rapids4)에서
  lean3의 `cargo test`/`cargo check`를 돌린 시간과 겹쳤다. 호스트가 부하 생성기이므로
  `raw/lean2-grpc/*.json.contaminated`로 격리하고 reps 4–6으로 재측정했다.
- lean2 런타임(PID 3472082 이전 프로세스)이 64 B matched run 시작 직후 로그·dmesg·journal·
  core 없이 사라졌다. 이 캠페인 전 batch 런타임도 같은 방식으로 사라졌다. sudo journal에
  kill 기록이 없고 apport는 전날 crash core는 저장했으므로 시그널 crash가 아닐 수 있다.
  lean3부터 `dpu_main.c`가 fatal signal backtrace와 `atexit` 흔적을 stderr에 남긴다.
  lean3 이후 배포 7회에서는 재현되지 않았다(`raw/lean2-grpc/dpu-log-died.txt`,
  `64-matched-clean-1.json.died`).
- `raw/lean2-grpc/65536-capacity-clean-6.json`에 `eq_budget_exhausted=1`이 있다. 표본은
  유효하고 A/B에는 reps 4–6이 쓰였다.

## 최종 트리와의 차이

측정 뒤 트리를 정리했다. driver yield는 효과가 없어 `tokio::task::yield_now`로
되돌렸고, Rust가 더 이상 쓰지 않는 `dmesh_l7_tx_{try_reserve,reserve,commit,commit_remote}`
와 `px_conn.l7_tx_chunk`, `TxAttempt::Accepted`, `tx_reserve_attempts`/`tx_writer_wakes`
metric을 제거했다. lean3의 나머지(고정 maintenance 타이머, 울린 PE만 clear, wake eventfd
1회 읽기, exit trace)는 그대로다. 정리 뒤 재측정은 하지 않았다.

## 방법과 품질 gate

- 같은 BlueField-3, N/K/A=32/8/8, client CPU 4–9, server CPU 10–17, closed-loop
  8 threads × conc 8, gRPC image `direct-tx`. 배포마다 `/proc/<pid>/exe` 해시 대조.
- 표본 112개(clean 99, PMU 9, profile 4) 전부 `OK`, fail/drop/overflow/worker_fail/reorder 0.
  syscall/callchain run은 성능 표에 쓰지 않았다.
- 모든 L7 arm 종료 전 arena 1,024/1,024 free, live chunk 0을 10회 확인했다.
- lean 64 KiB 재측정(reps 4–6): 13,000/12,920/12,538 RPC/s, CPU/RPC 595.0/598.4/596.9로
  main과 같다.
- host software gate: dmesh-doca 35/35, adapter 41/41, `l7_abi_contract_test` PASS,
  `cargo check` OK.

복원은 [RESTORATION.md](RESTORATION.md)에, 파생 데이터는 [comparison3.csv](comparison3.csv),
[syscalls.csv](syscalls.csv), [pmu-summary.csv](pmu-summary.csv), [summary.csv](summary.csv),
그림은 [comparison.png](comparison.png)에 있다. 절차는 `commands.txt`, `campaign.py`,
`run_matrix.sh`, `run_matrix3.sh`다.
