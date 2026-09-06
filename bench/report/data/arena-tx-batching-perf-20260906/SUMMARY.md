# Arena TX batching hardware A/B

기준일: 2026-09-06

## 판정

arena batching은 실제 gRPC 경로에서 동작했다. direct TX 대비 성공 publication
크기는 1 KiB에서 906 B→2,873 B, 8 KiB에서 3,268 B→7,018 B로 커졌고,
publication/MiB는 각각 3.17배와 2.15배 줄었다. 1 KiB capacity의 TX budget wait
중앙값도 46,021→0으로 줄었다.

CPU/RPC는 gRPC capacity 네 크기 모두 1.0~3.6% 감소했다. 핵심 64 KiB에서는
capacity가 12,505→12,905 RPC/s(+3.20%), CPU/RPC가 619.07→599.50 us
(-3.16%), p99가 8,009→7,610 us(-4.98%)였다. 전체 worker CPU 시간은
64.17→64.24초로 거의 같았고, 같은 CPU로 더 많은 RPC를 처리했다.

그러나 전체 성능 acceptance는 **OPEN**이다. gRPC 8 KiB capacity가
22,436→21,949 RPC/s(-2.17%)였고 세 반복의 범위도 direct 22,196~22,499,
batch 21,698~22,071로 겹치지 않았다. 1 KiB capacity p99 중앙값은 18.42%
늘었지만 범위가 넓게 겹치고 matched-rate 변화는 +0.65%여서, 이 표본만으로 안정적인
tail 회귀라고 판정하지 않는다. HTTP/policy hardware correctness도 이번 campaign의
범위가 아니었다.

따라서 이번 결과는 다음을 지지한다.

- write마다 생기던 reservation/publication 관리 비용은 CPU 회귀의 한 원인이었다.
  batching으로 그 횟수를 줄이자 모든 측정 크기에서 CPU/RPC가 감소했다.
- 상태 관리 instruction 자체만 세는 설명은 충분하지 않다. 64 KiB PMU에서
  instructions/RPC는 +1.77%였지만 cycles/RPC는 -3.16%, IPC는 +5.12%였다.
  새 경로가 instruction을 조금 더 실행하면서도 cycle 효율은 좋아졌다.
- 정확히 어떤 stall 또는 동기화 비용이 줄었는지는 이번 generic PMU event만으로
  분리할 수 없다. cache-misses/RPC는 -0.35%로 거의 같았다.

## clean capacity 결과

각 값은 계측하지 않은 8초 run 세 번의 중앙값이다. 괄호는 batch의 direct 대비 변화다.
CPU는 ARM worker 8개의 utime+stime 합을 scheduled RPC로 나눈 값이다.

| protocol | payload | direct→batch RPC/s | direct→batch CPU us/RPC | direct→batch p99 us |
|---|---:|---:|---:|---:|
| gRPC | 64 B | 36,134→36,163 (+0.08%) | 178.09→176.30 (-1.00%) | 2,676→2,699 (+0.86%) |
| gRPC | 1 KiB | 34,104→34,632 (+1.55%) | 190.55→183.65 (-3.62%) | 3,752→4,443 (+18.42%) |
| gRPC | 8 KiB | 22,436→21,949 (-2.17%) | 309.02→300.71 (-2.69%) | 5,636→4,240 (-24.77%) |
| gRPC | 64 KiB | 12,505→12,905 (+3.20%) | 619.07→599.50 (-3.16%) | 8,009→7,610 (-4.98%) |
| opaque | 64 B | 133,844→142,393 (+6.39%) | 23.96→22.52 (-6.00%) | 1,112→1,099 (-1.17%) |
| opaque | 1 KiB | 122,685→128,964 (+5.12%) | 24.98→24.30 (-2.72%) | 1,176→1,135 (-3.49%) |

opaque의 평균 publication 크기는 64 B에서 266.17→266.29 B, 1 KiB에서
3,451.22→3,450.52 B로 사실상 같았다. opaque의 처리량 개선을 batch 밀도 증가로
설명하지 않는다. gRPC와 arm 순서를 반대로 실행해 단순한 campaign 순서 효과는
줄였지만, 정확한 원인 분해에는 별도 profile이 필요하다.

## matched-rate 결과

gRPC는 64 B/1 KiB를 20k RPC/s, 8 KiB를 10k RPC/s, 64 KiB를 2k RPC/s로
고정했다. opaque는 두 크기 모두 20k RPC/s로 고정했다.

| protocol | payload | CPU/RPC 변화 | p99 변화 |
|---|---:|---:|---:|
| gRPC | 64 B | -0.48% | +3.56% |
| gRPC | 1 KiB | -0.43% | +0.65% |
| gRPC | 8 KiB | -1.29% | -1.53% |
| gRPC | 64 KiB | -2.16% | +0.81% |
| opaque | 64 B | -5.83% | -0.41% |
| opaque | 1 KiB | -5.22% | -4.01% |

고정 부하에서도 CPU/RPC는 여섯 조건 모두 감소했다. gRPC matched-rate p99는
대체로 작은 변화였고 방향도 섞였다.

## publication 모양

uprobes는 clean 성능 run과 분리한 12초 run에서 성공 반환만 셌다. tracing run의
throughput과 latency는 성능 표에 사용하지 않았다. 모든 histogram drop은 0이었다.

| protocol | payload | 평균 publication bytes direct→batch | publications/MiB 감소 배수 |
|---|---:|---:|---:|
| gRPC | 64 B | 319.58→324.78 | 1.02x |
| gRPC | 1 KiB | 905.89→2,873.07 | 3.17x |
| gRPC | 8 KiB | 3,268.49→7,017.86 | 2.15x |
| gRPC | 64 KiB | 8,550.52→9,494.58 | 1.11x |
| opaque | 64 B | 266.17→266.29 | 1.00x |
| opaque | 1 KiB | 3,451.22→3,450.52 | 1.00x |

64 KiB에서도 64 KiB full publication은 두 arm 모두 0이었다. H2의 16 KiB DATA
framing과 control output을 합친 결과이며, steady state에서 64 KiB 한 덩어리로 합쳐졌다고
주장할 수 없다.

batch clean 36개 표본에서는 arena copy bytes와 실제 publication bytes가 모든 run에서
같았고 TX retry/error는 0이었다. direct clean 36개도 retry/error 0이었다. batch의
publication과 reserve-attempt counter도 각 run에서 같아 reservation leak나 중복
reservation은 관측되지 않았다.

## 64 KiB PMU

별도 12초 capacity run 세 번에서 가운데 8초를 `perf stat`으로 측정했다. per-RPC는
전체 run의 보고 RPS를 이용한 추정치다.

| metric | direct→batch | 변화 |
|---|---:|---:|
| worker cores | 7.772→7.744 | -0.36% |
| cycles/RPC | 1,322,393→1,280,630 | -3.16% |
| instructions/RPC | 562,923→572,912 | +1.77% |
| IPC | 0.426→0.448 | +5.12% |
| generic cache-misses/RPC | 10,108→10,073 | -0.35% |
| context-switches/RPC | 0.186→0.193 | +3.30% |

generic `cache-misses`를 LLC miss나 특정 memory stall로 해석하지 않는다. IPC 변화도
frontend/backend/coherence 중 어느 원인이 바뀌었는지 구분하지 않는다.

## 방법과 품질 gate

- 비교 binary: direct `2200fc9207...9422e`, batch `b62194f1...36eb`.
- campaign 뒤 production source 5개의 SHA256을 BlueField build staging과 대조했고
  모두 일치했다. 전체 값은 `raw/source-provenance.txt`에 보존했다.
- 같은 BlueField-3, N/K/A=32/8/8, client CPU 4-9, server CPU 10-17,
  release LTO와 jemalloc, concurrency 8.
- gRPC는 direct→batch, opaque는 batch→direct 순서로 측정했다.
- 주 A/B는 capacity/matched-rate 각 3회다. clean 72, PMU 6, trace 12,
  끝의 direct 64 KiB 기준선 3회를 합쳐 총 93개 유효 표본을 보존했다.
- 93/93에서 application result가 유효했고 fail/drop/overflow/worker_fail/reorder 최대값은
  모두 0이었다. 끝의 추가 direct 64 KiB 표본 하나에 `eq_budget_exhausted=1`이 있었으며
  주 A/B에는 포함하지 않았다.
- 마지막 direct 64 KiB 재측정은 12,434/12,695/11,049 RPC/s였다. 중앙값 12,434는
  주 기준선 12,505와 가깝지만 한 번의 낮은 표본은 rig 변동성을 보여 준다. 세 반복 결과를
  통계적 유의성으로 표현하지 않는다.
- 모든 L7 arm 종료에서 arena 1,024/1,024 free, live chunk 0을 확인했다. L7 metrics가
  있는 arm은 session/task/DMA/queue/ACK/FIN gauge도 0이었다.

재실행한 software gate:

| gate | 결과 |
|---|---|
| `make test` | PASS; system Python에 없던 grpcio는 repository requirements로 만든 임시 venv 사용 |
| adapter Rust | 41/41 PASS |
| `dmesh-doca` | 30/30 PASS |
| gRPC release | 4/4 PASS |
| gRPC ASAN+UBSAN | 4/4 PASS |
| C proxy-lane ASAN+UBSAN | PASS |
| non-test Rust check / diff whitespace | PASS |

## 복원

campaign 전 workload template와 Service spec을 그대로 다시 적용했다. 현재 native workload
두 개는 Ready이고 Service port는 9092다. 실행 중인 L4 DPU binary hash는 campaign 전과 같은
`2200fc9207...9422e`이며 native 64 B smoke는 180,780 RPC/s, fail/drop/overflow/worker_fail 0이었다.
temporary control relay와 trace instance/event는 제거했다. opaque의 cutoff `pending=58~64`와
native smoke의 `pending=64`는 residue 판정이 아니라 benchmark 종료 시점의 in-flight 값이다.

파생 데이터는 [comparison.csv](comparison.csv), [trace-summary.csv](trace-summary.csv),
[pmu-summary.csv](pmu-summary.csv)에 있고 전체 그림은 [comparison.png](comparison.png)에 있다.
원시 표본과 재현 script는 `raw/`, `campaign.py`, `run_matrix.sh`, `summarize.py`에 보존했다.
