# Fixed-overhead TX (lean) vs arena batching (batch) — hardware A/B

기준일: 2026-09-06

## 판정

write당·drain pass당 고정비를 없앤 lean 빌드는 같은 BlueField에서 batch 빌드보다
gRPC 네 크기 모두 CPU/RPC가 2.9~4.4% 낮고 capacity가 3.1~4.4% 높다. 세 반복의
RPC/s 범위가 네 크기 모두 겹치지 않는다. 같은 입력 부하(matched)에서도 CPU/RPC는
0.8~5.0% 낮고 p50/p99가 모두 낮다. publication 크기는 두 arm이 같으므로 이 차이는
batching이 아니라 고정비에서 왔다.

opaque TCP는 capacity RPC/s가 −0.7%/−3.0%지만 범위가 넓게 겹치고 matched CPU/RPC는
−2.0%/−0.2%다. opaque는 개선도 회귀도 판정하지 않는다.

이 receipt는 direct TX 이후 열려 있던 "복사를 줄였는데 CPU가 늘었다"는 문제에 대한
답이다. 2026-09-06 아침 캠페인의 Vec(before) 대비 batch가 64 KiB에서 +2%였던 CPU/RPC를
lean은 −3.8%로 뒤집었다(다른 날 캠페인 간 비교이므로 rig 편차를 포함한다).

## 변경 요약(lean)

C는 그대로이고 Rust 두 crate만 바뀌었다. writer 쪽 quota(epoch·remaining·attempts·
`TxBudget`·grant·1 ms 게이트)를 삭제하고 chunk 자체를 역압으로 쓴다. writer를
`Box<dyn TxWriter>`로 endpoint 안에 두고 C 호출을 endpoint lock 아래서 수행해 write당
lock 5쌍을 1쌍으로 줄였다. waker는 Pending 경로에서만 clone하고 owner 검사는
thread_local이다. worker당 `DriverSignal`과 endpoint별 dirty 비트로 driver가 idle
endpoint를 lock 없이 건너뛰고, prometheus 갱신은 1 ms maintenance로 옮겼다.
`poll_shutdown`은 즉시 반환하고 FIN 순서는 driver가 지킨다. `tx_budget_wait` metric은
삭제했다.

## clean capacity

8초 run 세 번의 중앙값. CPU는 ARM worker 8개의 utime+stime 합을 scheduled RPC로 나눈 값.

| protocol | payload | RPC/s batch→lean | CPU µs/RPC batch→lean | p50 µs | p99 µs |
|---|---:|---:|---:|---:|---:|
| gRPC | 64 B | 35,154→36,694 (+4.38%) | 183.01→174.92 (−4.42%) | 1,822→1,768 | 3,083→2,553 |
| gRPC | 1 KiB | 33,790→34,828 (+3.07%) | 190.14→183.61 (−3.43%) | 1,878→1,829 | 4,156→3,233 |
| gRPC | 8 KiB | 20,960→21,619 (+3.14%) | 314.08→305.08 (−2.87%) | 3,027→2,944 | 4,777→4,407 |
| gRPC | 64 KiB | 12,679→13,120 (+3.48%) | 613.24→590.00 (−3.79%) | 5,003→4,870 | 7,549→7,253 |
| opaque | 64 B | 135,303→134,353 (−0.70%) | 23.71→23.35 (−1.51%) | 441→443 | 1,103→1,116 |
| opaque | 1 KiB | 120,658→117,052 (−2.99%) | 25.57→25.98 (+1.60%) | 483→502 | 1,178→1,177 |

RPC/s 범위(3회): gRPC 64 B 34,872–35,627 vs 36,665–36,937, 1 KiB 33,536–34,227 vs
34,638–35,227, 8 KiB 20,774–21,059 vs 21,580–21,621, 64 KiB 12,645–12,713 vs
13,013–13,169로 모두 분리된다. opaque 64 B 134,772–135,901 vs 131,532–139,349, 1 KiB
118,888–125,753 vs 116,617–123,579로 겹친다.

총 worker CPU는 64 KiB에서 batch 7.78코어, lean 7.74코어로 같다. 같은 CPU로 3.5% 더
많은 RPC를 처리했다.

## matched rate

gRPC 64 B/1 KiB 20k, 8 KiB 10k, 64 KiB 2k RPC/s. opaque 20k RPC/s.

| protocol | payload | CPU µs/RPC batch→lean | p50 µs | p99 µs |
|---|---:|---:|---:|---:|
| gRPC | 64 B | 333.00→330.06 (−0.88%) | 998→963 | 2,003→1,941 |
| gRPC | 1 KiB | 334.75→332.06 (−0.80%) | 1,040→1,003 | 2,093→2,040 |
| gRPC | 8 KiB | 558.38→541.38 (−3.04%) | 1,704→1,695 | 2,319→2,264 |
| gRPC | 64 KiB | 1,016.25→965.00 (−5.04%) | 1,459→1,437 | 2,588→2,553 |
| opaque | 64 B | 79.31→77.75 (−1.97%) | 263→259 | 490→483 |
| opaque | 1 KiB | 80.81→80.69 (−0.15%) | 265→251 | 491→476 |

## publication 모양은 그대로

uprobe로 `dmesh_l7_tx_batch_flush`의 성공 반환만 센 별도 12초 run이다.

| protocol | payload | 평균 publication bytes batch→lean | publications/MiB batch→lean |
|---|---:|---:|---:|
| gRPC | 64 B | 320.5→321.9 | 3,272→3,257 |
| gRPC | 1 KiB | 2,879.5→2,872.7 | 364→365 |
| gRPC | 8 KiB | 7,006.9→6,999.4 | 149.7→149.8 |
| gRPC | 64 KiB | 9,490.8→9,508.4 | 110.5→110.3 |
| opaque | 64 B | 266.1→266.2 | 3,941→3,939 |
| opaque | 1 KiB | 3,449.0→3,451.8 | 304.0→303.8 |

64 KiB full publication은 두 arm 모두 0이다. clean 표본 전체에서 arena copy bytes와
accepted bytes가 같고 publication 수와 reserve attempt 수가 같다. lean의 TX retry는
clean 30개 run 합계 26회(chunk 가득 참 또는 arena 대기)이며 writer wake 26회와
일치한다. batch의 retry는 0, budget-wait wake는 60회다.

## 어디서 줄었나

64 KiB PMU(별도 12초 run 3회, 가운데 8초):

| metric | batch | lean | 변화 |
|---|---:|---:|---:|
| cycles/RPC | 1,308,634 | 1,262,728 | −3.51% |
| instructions/RPC | 573,421 | 545,137 | −4.93% |
| IPC | 0.439 | 0.432 | −1.65% |
| cache-misses/RPC | 10,008 | 9,786 | −2.22% |
| context-switches/RPC | 0.227 | 0.195 | −14.2% |

batch의 PMU 2·3회차는 처리량 11.0K RPC/s·6.8코어로 1회차(12.7K·7.8코어)와 lean 3회
(13.0K·7.7코어)보다 낮았다. RPC당 값은 이 저하에 크게 흔들리지 않았고, 저하 자체는
batch arm에서만 나타난 변동이므로 표본을 그대로 보존했다(`pmu-samples.csv`).
direct TX 때와 달리 이번에는 instruction 수 자체가 줄었다.

profile(별도 16초 run, 8 worker 합산 exclusive self %):

| payload | 원자 연산 helper | `ExternalBackend::drain` self | memcpy | kernel |
|---|---:|---:|---:|---:|
| 64 B | 10.75→10.40 | — | 2.78→2.88 | 11.07→12.54 |
| 1 KiB | 10.56→9.69 | — | 2.91→3.58 | 10.93→12.04 |
| 64 KiB | 11.09→10.17 | 1.27→0.80 | 5.23→5.94 | 5.78→5.49 |

64 KiB에서 `__aarch64_cas1_acq` 1.26→0.81, `ldadd4_acq_rel` 1.45→1.19, `ldadd8_rel`
1.13→0.89로 줄었고 `poll_write_vectored` self 0.35는 `poll_transmit` 0.23과
`DirectWriter::write` 0.22로 갈라졌다. memcpy 비중 상승은 절대량이 아니라 나머지가
줄어 생긴 비중 변화다(publication 모양과 copy bytes가 같다).

작은 크기에서 kernel 비중이 1.5pp 올랐다. clean counter로 보면 lean은 RPC당 drain
pass가 7.9→8.9회, park/arm 전환이 1.6→2.2회로 늘었다. pass가 가벼워져 driver가 stack을
더 자주 따라잡고 더 자주 park한다. 다음 레버는 이 park/arm 횟수다.

## rig drift 확인

main A/B 뒤 batch gRPC 64 KiB를 다시 3회 측정했다: 12,659/12,626/12,602 RPC/s,
CPU/RPC 612.6/612.7/616.3 µs. main의 batch(12,645–12,713, 610.6–613.4)와 같고 lean
(13,013–13,169, 587.1–591.5)과 겹치지 않는다. 이번 campaign 안에서는 drift가 없다.

## 사고 두 건

첫 `run_matrix.sh`는 setup에서 멈췄다. sudo로 만든 scratch 디렉터리가 root 소유라
비특권 arena 레이아웃 컴파일이 쓰지 못했다. 배포 전이라 rig는 변하지 않았다
(`raw/matrix-attempt1-setup-permission.log`).

두 번째 실행은 batch/opaque 배포의 warmup에서 멈췄다. echo Pod 등록 150 ms 뒤에
첫 dial이 들어가 Linkerd가 "No dmesh backend channel … no live registration"으로
거절했고, C가 세션 8개를 poison한 직후 **batch runtime(PID 3447022)이 로그 없이
사라졌다**. dmesg에 segfault/OOM 기록이 없고 core_pattern이 apport라 core도 없다
(`raw/batch-opaque/dpu-log-3447022-died.txt`). 이 binary는 직전 캠페인의 batch와
동일하므로 lean 변경과 무관하지만, "거절된 dial → poison → 조용한 종료"는 열린
결함이다. 재현 조건은 controller feed가 새 Pod를 싣기 전의 첫 dial이며, 재시도
배포에서는 나타나지 않았다. `campaign.py deploy`는 이후 rollout 뒤 5초를 기다리고
warmup을 최대 4회 재시도한다. 6개 배포 중 나머지 5개는 첫 warmup에서 통과했다.

## 방법과 품질 gate

- binary: batch `b62194f1…36eb`(직전 캠페인이 DPU에 보존), lean `1577c3cc…e270`
  (`bench/bench.sh build`, Rust release LTO+jemalloc, C debugoptimized). 두 binary를
  `/tmp/tx-fixed-overhead-20260906/`에 두고 배포마다 `/proc/<pid>/exe` 해시를 대조했다.
- 같은 BlueField-3, N/K/A=32/8/8, Linkerd all workers, client CPU 4–9, server CPU 10–17,
  closed-loop 8 threads × conc 8, gRPC image `direct-tx`.
- gRPC는 batch→lean, opaque는 lean→batch 순서다.
- 표본 99개(clean 75, PMU 6, trace 12, profile 6) 전부 `OK`이고 fail/drop/overflow/
  worker_fail/reorder/eq_budget_exhausted 최대값이 0이다.
- 모든 L7 arm 종료 전 arena 1,024/1,024 free, live chunk 0을 7회 확인했고 L7 metric
  gauge(session/task/DMA/queue/ACK/FIN)도 0이었다.
- lean tree software gate: dmesh-doca 35/35, adapter 41/41, production `cargo check` OK.

복원은 [RESTORATION.md](RESTORATION.md)에 있다. 파생 데이터는 [comparison.csv](comparison.csv),
[summary.csv](summary.csv), [trace-summary.csv](trace-summary.csv),
[pmu-summary.csv](pmu-summary.csv), [profile-summary.csv](profile-summary.csv),
[report-tables.md](report-tables.md)이고 그림은 [comparison.png](comparison.png)이다.
원시 표본과 절차는 `raw/`, `campaign.py`, `run_matrix.sh`, `resume_matrix.sh`,
`commands.txt`에 있다.
