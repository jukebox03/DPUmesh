# Linkerd direct TX / RDMA 공통 lease 배포·비교 보고서

2026-09-05. 요청한 순서대로 두 구현을 적용하고 BlueField에 직접 빌드·배포하여 비교했다.
**구현·배포·비교는 완료했지만, Linkerd 성능 acceptance와 실제 두 DPU RDMA gate는 OPEN이다.**
중간 복사는 제거됐으나 64 KiB gRPC 최대 부하가 4.25% 느려졌다. 이 결과를 성능 개선으로
표시하지 않았고 PLAN.md의 해당 완료 checkbox도 열어 두었다.

## 구현과 검증

1. **Linkerd TX**: production `DmeshIo`의 TX Vec와 driver의 Vec→arena copy를 제거했다.
   caller slice를 등록 arena에 한 번 복사한 뒤 같은 `poll_write` 호출에서 commit한다.
   `poll_write_vectored`는 동시에 제공된 header/body slices를 한 arena 예약에 모아 쓴다.
   Pending에 caller bytes/포인터를 보존하지 않는다. reserve 불가와 잘못된 handle을 구분하며,
   reserve/commit/cancel, owner thread, 처음부터 연결되는 backend/Origin/remote route,
   quota/wake, FIN/abort/slot reuse 계약을 검증했다. endpoint 64 KiB/4 attempts,
   worker 256 KiB/64 sessions 제한은 유지했다.
2. **RDMA 공통 기반**: `peer_wire_ops`에 선택적 TX reserve/commit/cancel 및 RX acquire/release
   그룹을 추가했다. connection epoch + slot generation + direction token을 검증한다.
   TX는 SEND CQ 뒤, RX는 release/repost 뒤에 재사용한다. 기존 copy API는 lease 위에서
   동작한다. TLS/BIO type을 노출하지 않아 TLS와 향후 IPsec producer/consumer가 재사용할 수 있다.
   **TLS BIO 연결(§11.20 B)과 IPsec 구현은 이번 공통 기반 범위에 포함되지 않는다.**
   따라서 현재 TLS의 production copy가 이미 제거됐다는 뜻은 아니다.

검증 통과: `make test DPUMESHD_PYTHON=/opt/dpumesh/venv/bin/python`, adapter Rust 40개,
`dmesh-doca` Rust 30개, gRPC release CTest 4/4 및 Clang ASAN+UBSAN CTest 4/4.
실제 RDMA C 구현을 포함한 mock-provider 시험은 exhaustion, CQ 지연, stale/cross-direction
release, RX 전체 FIFO/held slot, post fault, wrap 및 QP→MR teardown 순서를 검증했고
ASAN+UBSAN과 BlueField 실행도 통과했다. 실제 wire suite의 TCP는 통과, RDMA는 환경상 skip이다.

최종 gRPC deny 정책 점검: 요청 65건 실패와 process-global inbound denied=65가 일치했다.
정책 삭제 후 fail=0으로 복구했다. 8 admin endpoint에 중복 노출되는 global denied 값은 합산하지 않았다.

## 비교 조건

동일 BlueField-3, N/K/A=32/8/8, Linkerd all workers, rustc 1.90.0 release LTO/jemalloc,
C `-O2 -g` debugoptimized. 같은 두 Restricted/Device Plugin workload와 Service를 사용했다.
client는 host CPU 4–9, server는 10–17로 **프로세스 시작부터 고정**했다. 이 CPU 배치의
시스템 비교이며, host 병목이 없는 DPU 단독 최대치라고 해석하지 않는다.

64 B / 1 / 8 / 64 KiB 각각 matched-rate 및 closed-loop, 조건당 5초 × 3회 중앙값이다.
matched rate는 20k / 20k / 10k / 2k RPC/s. closed-loop는 8 threads × conc 8 = 64 outstanding.
별도 첫 warm run은 제외했다. DPU compiler와 진단 debugger는 timed 측정 중 실행하지 않았다.
총 96개 최종 표본을 모두 보존했다. gRPC 양쪽 24/24 정상; opaque 양쪽 20/24 정상이다.

CPU/RPC는 `dmesh-w0..7`의 utime+stime을 합산한 뒤 scheduled RPC로 나눈 값으로,
각 호출의 warmup/teardown CPU도 포함한다. control/admin CPU는 포함하지 않는다.
비교 CSV에 p50/p99, CPU의 절대값, 3회 min/max도 있다. 짧은 3회 측정의 차이를 보편적인
속도 향상이나 통계적 유의성으로 확대하지 않는다.

## gRPC 결과

최대 부하:

| Payload | RPC/s before → after | 변화 | p99 µs before → after | DPU CPU/RPC 변화 |
|---|---:|---:|---:|---:|
| 64 B | 35,523 → 35,784 | +0.73% | 3,456 → 2,688 | +0.18% |
| 1 KiB | 34,877 → 34,765 | -0.32% | 3,316 → 2,734 | +3.38% |
| 8 KiB | 21,291 → 21,898 | +2.85% | 4,586 → 5,137 | +2.44% |
| 64 KiB | 12,795 → 12,251 | -4.25% | 7,470 → 8,119 | +4.72% |

같은 입력 부하:

| Payload | RPC/s before → after | 변화 | p99 µs before → after | DPU CPU/RPC 변화 |
|---|---:|---:|---:|---:|
| 64 B | 19,999 → 19,999 | +0.00% | 2,180 → 1,927 | +0.91% |
| 1 KiB | 19,999 → 20,000 | +0.01% | 2,045 → 2,009 | +0.75% |
| 8 KiB | 9,999 → 9,999 | +0.00% | 2,255 → 2,305 | +0.98% |
| 64 KiB | 1,999 → 1,999 | +0.00% | 2,718 → 2,617 | +0.40% |

64 KiB capacity는 before 12,776–12,820 RPC/s, after 12,233–12,285 RPC/s로 표본 범위도
겹치지 않는다. 같은 크기 matched 부하의 p99는 2,718→2,617 µs(−3.72%)다.
즉, 복사 감소가 최대 부하 CPU/처리량 개선으로 연결되지는 않았다.

## opaque TCP 결과

최대 부하:

| Payload | RPC/s before → after | 변화 | p99 µs before → after | DPU CPU/RPC 변화 |
|---|---:|---:|---:|---:|
| 64 B | 138,770 → 139,689 | +0.66% | 1,090 → 1,102 | +1.52% |
| 1 KiB | 124,335 → 126,664 | +1.87% | 1,155 → 1,148 | +0.15% |
| 8 KiB | 실패 표본: before 1/3, after 1/3 | 제외 | 제외 | 제외 |
| 64 KiB | 실패 표본: before 3/3, after 3/3 | 제외 | 제외 | 제외 |

같은 입력 부하:

| Payload | RPC/s before → after | 변화 | p99 µs before → after | DPU CPU/RPC 변화 |
|---|---:|---:|---:|---:|
| 64 B | 19,999 → 19,999 | +0.00% | 492 → 481 | +1.07% |
| 1 KiB | 19,999 → 19,999 | +0.00% | 486 → 491 | +1.32% |
| 8 KiB | 10,000 → 10,000 | +0.00% | 1,777 → 1,788 | +2.51% |
| 64 KiB | 1,999 → 1,999 | +0.00% | 2,842 → 2,356 | +3.41% |

8 KiB capacity 양쪽 1/3, 64 KiB 양쪽 3/3에서 drain 실패가 발생했다. 일부 성공 표본만 골라
clean capacity를 만들지 않았다. 이전 예비 측정의 512 outstanding / 64 KiB baseline에는
DPU heap corruption도 있어 `raw/before-opaque-crash-dpu.log`로 보존했다. 기존 실패를
이 최적화에서 해결했다고 주장하지 않는다.

## 복사와 자원 반환

| 최종 arm | accepted bytes | arena copy bytes | retry / error | budget wait / writer wake |
|---|---:|---:|---:|---:|
| gRPC | 39,823,706,798 | 39,823,706,798 | 0 / 0 | 89,944 / 89,944 |
| opaque | 190,569,151,088 | 190,569,151,088 | 0 / 0 | 2,084 / 1,042 |

새 경로의 TX 중간 Vec copy는 0이다. 정상 accepted B bytes의 adapter copy는 **2B→B**다.
accepted는 C publication 완료량이며 destination ACK/delivery를 뜻하지 않는다.
opaque counter는 실패 표본까지 포함한 전체 arm 값이다. 표의 byte 동등성은 오류를 숨기지 않는다.

모든 최종 arm 종료 뒤 8 worker의 session/task/registration/DMA/queue residue는 0이다.
최종 opaque arm 뒤 debugger로 shared free list와 모든 thread magazine을 중복/순환 검사하여
arena **1,024/1,024 free, live 0**을 확인했다. gRPC 최종 진단 뒤에도 1,024/1,024 반환을 재확인했다. timed 측정과 분리한 검사다.
**arena high-watermark는 미수집(NA)**이며, 종료 시 전체 반환이 peak 측정을 대신하지 않는다.

## RDMA 공통 API 비용 비교

BlueField CPU 10에 고정해 보관한 이전 C source와 변경 source를 같은 mock verbs로 실행했다.
TX는 producer fill + publication + SEND CQ, RX는 acquire/copy + 첫/마지막 byte 관측 + release다.
각 크기 128 MiB, warm rep 제외 3회 중앙값. 이는 NIC/네트워크/암호화를 제외한 **API CPU 비용**이다.

| 크기 | TX 기존 copy → direct lease (ns) | RX 기존 copy → direct lease (ns) |
|---|---:|---:|
| 64 B | 25.55 → 36.34 | 35.86 → 44.23 |
| 1 KiB | 79.18 → 56.61 | 70.44 → 44.17 |
| 8 KiB | 397.58 → 164.42 | 312.01 → 44.07 |
| 64 KiB | 3,451.92 → 2,380.05 | 2,114.89 → 44.13 |

64 B에서는 token/state 검증의 고정 비용 때문에 느려졌다. RX lease 수치는 payload 전체 처리나
TLS 복호화 비용이 아니다. 현재 TLS가 쓰는 compatibility copy 경로의 변경 비용도
`rdma-comparison.csv`의 `after_copy_ns`로 공개했다. 공통 API 추가만으로 기존 TLS 성능이
위 표처럼 빨라지는 것은 아니다.

Kubernetes는 rapids4 한 node이고 BlueField p0/p1이 DOWN이며 RDMA device에 연결된 주소가 없다.
**실제 두 DPU RDMA 전송 before/after는 실행하지 못했다.** software gate와 이 CPU 비교를
fabric/mTLS end-to-end receipt로 대체하지 않았다.

## 회귀 진단과 남은 acceptance

초기 scalar direct 구현은 gRPC capacity가 0.6–4.9% 낮아졌다. H2의 header/body를 한 번에
받도록 vectored write를 추가하고 부분 slice/재시도 시험도 추가했다. 최종 측정은 이 vectored
binary를 기준으로 baseline부터 CPU 배치를 고정해 다시 수행했다. 예비 scalar/unpinned arm은
별도 이름으로 보존했으며 최종 표에 섞지 않았다.

즉시 commit은 기존 Vec의 poll 사이 coalescing을 없애고, quota/backpressure가 caller에게
전달되는 시점도 앞당긴다. vectored write가 한 호출 안의 slices는 모으지만 서로 다른 poll의
출력까지 모으지는 않는다. 이 batching 변화와 호출별 동기 publication 비용은 64 KiB 회귀의
후보 원인이다. 실제 DPU의 처음 128개 nonempty commit을 별도 debugger 진단으로 관측했다.
before에는 65,536 B commit 3개와 16,393 B 42개, after에는 65,536 B 0개와
16,393 B 40개가 있었다. 작은 제어 출력의 합쳐진 크기도 달랐다
(`raw/{before,after}-grpc-commit-shape.txt`). startup 및 debugger 영향을 받는 진단이므로
steady-state 분포나 비용의 정량 분해로 쓰지 않는다. 위 batching 차이를 뒷받침하는 증거다.
최종 gate는 추가적인 batching/progress 개선 및 재측정 전까지 OPEN이다.

## 배포와 재현 자료

최종 실행 상태: 새 binary를 유지하고 두 workload를 원래 native image와 L4 profile로 복원했다.
L7 direct TX는 앞선 opaque/gRPC 실배포에서 검증했으며, 종료 시 profile에서는 L7이 꺼져 있다.
측정용 CPU pin/정책/임시 Linkerd control relay를 정리했다. 기존 controller relay는 유지했다.
복원 후 native smoke는 fail/drop/overflow/worker_fail=0, 두 workload는 Ready였다.
실행 중 `/proc/<pid>/exe` SHA256도 아래 최종 binary와 일치한다
(`raw/restored-native-{smoke,pods,dpu}.txt`, `restore_native.sh`).

최종 binary SHA256:
`2200fc9207e01b7e07e3aa0d45f26d55fef6f9aee07fb4a61cdfd3169389422e`

before binary SHA256:
`4273f0148b78346c83a2922db86d5a6a981fa8217fdd3cf148eb980faa76b62e`

baseline source HEAD `4e43056`. 실제 before 측정은 기존 배포 binary의 보관본을 실행했다.

- `comparison.csv/json`, `metrics-summary.json`: 최종 96개 표본 집계.
- `raw/{before,after}-{grpc,opaque}-clean/`: 원시 reply, CPU, Pod/affinity, metrics, DPU log.
- `run_points.py`, `deploy_stage.sh`, `summarize.py`, `commands.txt`: 측정·재배포·집계 절차.
- `rdma_primitive_bench.c`, `rdma-comparison.csv`, `raw/rdma-before/`: 공통 API before/after.
- `raw/*tests.log`, `raw/lease-asan.log`, `raw/dpu-wire-tests.log`: 검증 로그.
- `arena_offsets.c`, `arena_quiescence.gdb/sh`, `raw/after-opaque-clean-arena.txt`: 실제 arena 반환 검사.
- `raw/policy-*`, `raw/rejected.txt`: 정책 회귀와 제외한 예비 측정 사유.
- `raw/final-binary.txt`, `raw/vectored-dpu-build.log`: 배포 binary와 빌드 식별.

최종 변경 patch: `raw/final-implementation.patch`, `raw/final-linkerd-implementation.patch`.
