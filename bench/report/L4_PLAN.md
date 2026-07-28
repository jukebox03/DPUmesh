# L4 proxy 측정 계획

## 질문과 비교 대상

raw byte-stream echo를 다음 다섯 데이터 경로로 실행해, 요청률이 증가할 때
호스트 CPU 비용과 포화 지점을 비교한다. 앞의 네 경로는 같은 POSIX 앱을 쓰는
주 비교이고, native는 같은 protocol/workload 위에서 direct API가 낼 수 있는
구현 상한선을 보는 보조 비교다.

| ID | 데이터 경로 | 암호화 |
|---|---|---|
| `plain` | `bench_sock → echo_sock` | 없음 |
| `envoy-permissive` | app → Envoy → Envoy → app | sidecar 간 평문 |
| `envoy-strict` | app → Envoy → Envoy → app | sidecar 간 상호 TLS |
| `dpumesh-preload` | 같은 POSIX app → `libdmesh_preload` → DPUmesh L4 | 없음 |
| `dpumesh-native` | `bench_dpumesh → echo_dpumesh` direct API → DPUmesh L4 | 없음 |

여기서 PERMISSIVE/STRICT는 Istio 제어 평면 전체가 아니라, 각각 평문을 허용하는
Envoy TCP 데이터 경로와 mTLS를 강제하는 Envoy TCP 데이터 경로를 뜻한다.

## 고정 조건

- 앱: `plain`, 두 Envoy, `dpumesh-preload`는 SHA-256이 같은
  `bench_sock`과 `echo_sock`. `dpumesh-native`는 별도
  `bench_dpumesh`/`echo_dpumesh`이며 결과에서 명시적으로 다른 series로 취급한다.
- 프레임: 다섯 구성 모두 같은 요청 헤더·sequence·payload 및 8B 응답
- 페이로드: 64B, 1KB, 8KB
- 부하: 4개 연결, constant-arrival open loop
- DPU: N/K/A=32/8/8, `DPUMESH_PROXY_L7_SVC=`로 L7 비활성화
- 배포: `BENCH_DEPLOY_SCOPE=l4`; 측정하지 않는 native client/validator와 추가
  weighted-LB backend는 DPU 시작 뒤 한 번도 등록하지 않는다.
- backend: 구성마다 client 하나와 server backend 하나. 처리율은 multi-backend
  aggregate가 아니라 single-backend end-to-end 값이다.
- host budget: 구성마다 client endpoint 1코어와 server endpoint 1코어, 총 2코어.
  Envoy app과 sidecar는 endpoint 코어를 공유한다. 다섯 구성은 서로 겹치지 않는
  10개 코어를 사용하며 모든 thread affinity와 NUMA node를 실행 전에 검증한다.
- rate 발견: closed-loop 결과를 open-loop anchor로 쓰지 않는다. 같은 4-connection
  open-loop workload를 5K rps에서 1.35배씩 올린다. 2초 pilot은 후보 범위만
  찾는다. 첫 실패 후보와 그 직전 점은 본 측정과 같은 10초 창으로 각각 최소
  두 번 확인하고, 판정이 갈리면 세 번째 표본의 다수결을 쓴다. bad 표본마다
  5K recovery를 먼저 통과시킨다. 확인된 clean/bad 구간을 같은 투표 방식으로
  로그 공간에서 네 번 이분한다. 정상적으로 bracket되면 상하한 비는 최대 약
  `1.35^(1/16)=1.019`, 즉 약 2%다.
- clean 판정: `achieved/offered ≥ 0.98`, generator-side
  `drops/scheduled ≤ 0.001`, `scheduled/(offered×duration)=0.98..1.02`,
  p99 ≤10ms, `fail=reorder=overflow=0`.
- 본 rate: payload별 다섯 구성 중 가장 낮은 clean knee의
  25/50/75/90%를 공통 x 좌표로 측정하고, 각 구성 knee의
  85/100/105%를 추가한다. 중복 rate는 제거한다.
- 반복: 각 점 10초 × 3회. 홀수 반복은 rate 오름차순, 짝수 반복은 내림차순이며
  구성 순서도 회전시켜 시간·온도 편향을 분산한다.
- NUMA: BlueField PCI에 local인 NUMA node
- recovery: pilot, knee vote 또는 본 측정의 unclean 행 뒤 5K rps recovery를 실행한다.
  recovery도 실패하면 N/K/A=32/8/8로 전체 재배포하고 binary, backend 수,
  core budget을 다시 검증한 뒤 즉시 중단한다. 재배포 전후 generation을 한
  결과에 섞지 않고 새 출력 디렉터리에서 시작한다. 포화 행 자체는 삭제하지 않는다.

기본 실행의 최대 retained load는 315행, idle은 15행, perf replay는 15개다.
pilot 최대치는 360개지만 일반적으로 첫 실패에서 조기에 멈춘다. 보통 knee
확인에는 10초 표본 180개가 필요하다. timed window 예상치는 약 98분이며,
배포·recovery·profiling을 포함한 예상 wall time은 110–140분이다. 투표가 자주
갈리거나 recovery 재배포가 발생하면 더 길어진다.

## 수집 데이터

`results.csv`의 한 행은 하나의 측정 창이다.

- 성능: target/reported offered rps, achieved rps, Gb/s, p50/p95/p99/p99.9,
  scheduled·완료·미완료(pending)·실패·generator drop·overflow·reorder,
  achieved/schedule/drop 비율과 clean 실패 사유
- host attribution: 네 container cgroup의 `usage_usec`, `user_usec`,
  `system_usec`, throttling delta
- host total: 할당된 client/server 코어의 `/proc/stat` busy, IRQ, softirq delta
- system softirq: 전체 host의 softirq delta. 네트워크 softirq가 할당 코어 밖에서
  실행되는지 확인하는 보수적 보조치다.
- `mpstat -P ALL 1`: 매 측정 창의 원본을 보존해 어느 코어에서 irq/softirq가
  실행됐는지 사후 확인한다.
- DPU ARM: preload와 native 행에서 `dpumesh_dpu` 전체 process tick delta
- stack attribution: 모든 반복이 clean인 점 중 scout knee 70%에 가장 가까운 점을 별도
  재생해 49Hz `perf record`를 저장한다.
  profiling overhead가 본 측정점에 들어가지 않는다. Envoy는 sidecar
  cgroup으로 직접 분리하고, preload shim과 native `libdpumesh` 비중은 perf
  DSO 표본으로 분리한다. perf는 worker PID에 attach하지 않고 해당 경로가 독점한
  두 CPU를 `-C`로 표본화해 thread event 상속에 따른 교란을 피한다. replay는
  달성률 98% 이상과 scheduler drop 0.1% 이하를 다시 검증한다.
- idle: 각 구성의 동일 두 코어를 무부하로 3회 측정한다.
- provenance: `scout.csv`에 pilot과 개별 vote, `scout_decisions.csv`에
  rate별 2-of-3 판정, `knees.csv`에 최종 bracket을 따로 남긴다.

`host_busy_cores`에서 같은 코어의 idle median을 뺀 값이 총 host-core 주 지표다.
`host_cgroup_cores`는 app/sidecar 귀속을 설명하는 분해 지표이며,
`host_softirq_cores`와 `system_softirq_cores`로 kernel 비용의 위치를 확인한다.
DPU ARM 코어는 이기종 코어이므로 host 코어에 단순 합산하지 않고 별도 축으로
보고한다.

## 그래프 계약

그래프 스크립트는 측정 완료 뒤 별도로 작성한다.

모든 선 그래프는 개별 run을 희미한 점으로 남기고, 동일
`payload/config/offered_rps` 세 반복의 median을 선으로 연결한다. 오차막대는
세 점의 min–max다. n=3으로 정규성이나 95% CI를 주장하지 않는다. clean 행은
채운 marker, unclean 행은 빈 marker로 표시한다.
네 POSIX 경로는 실선과 동일 계열 marker를 쓰고, native는 검은 테두리의
점선/별도 marker로 표시해 앱 동등 비교가 아님을 범례와 caption에 반복한다.

1. **고정 2코어에서 achieved RPS vs offered RPS.** 이상적인 `y=x` 선과
   10초 vote의 highest-clean/lowest-bad bracket을 표시한다. retained 점은
   세 반복 중 둘 이상 clean이면 sustained-clean으로 판정하되 세 원점을 모두
   노출한다. endpoint capacity의 주 그래프이며, nominal offered가 아니라
   achieved를 y축으로 쓴다.
2. **host cores vs achieved load.** x축은 achieved RPS, y축은 endpoint에
   독점 배정된 두 CPU의 idle을 뺀 `host_busy_cores`. 이 값이 app, proxy,
   kernel, IRQ/softirq를 포함하는 고정-core 주 지표다. 네 공통 rate만 실선으로
   연결하고 구성별 knee point는
   점선으로 연장한다. 포화 후 backlog 행으로 기울기를 계산하지 않는다.
   `system_softirq_cores - host_softirq_cores`의 idle 보정 median이 0.05코어
   또는 주 지표의 5%를 넘으면 off-core softirq 경고를 패널에 표시한다.
3. **CPU stack.** 공통 75% min-knee point에서 client app, server app,
   client sidecar, server sidecar를 cgroup으로 분해한다. `host_softirq_cores`는
   cgroup과 겹칠 수 있으므로 별도 hatch/패널로 표시하고 stack 합계에 더하지 않는다.
   preload/native library 비중은 perf 표본 비율이며 cgroup core와 같은 막대에
   절대 코어로 오인해 합산하지 않는다. 어느 구성에서든 cgroup 합과 두 물리 코어 busy가
   10% 넘게 어긋나면 막대를 “attribution, non-conserving”으로 표시하며
   물리 코어 총량처럼 해석하지 않는다.
4. **incremental cores/Mrps vs payload.** 공통 clean point만 사용해
   `host_busy_cores - idle_median`을 achieved Mrps에 대해 절편 0으로 강제하지
   않은 선형 회귀로 적합한다. slope와 R²를 같이 표시하며 R²<0.9이면 막대를
   회색 처리하고 “비선형—대표 비용 아님”으로 표시한다. 보조로 각 공통 75%
   point의 `(load-idle)/Mrps`도 점으로 표시한다.
5. **p99 vs offered load.** 모든 retained 행을 포함하고 10ms SLA 수평선,
   clean/bad bracket, generator-drop 시작점을 표시한다. y축은 log scale이다.
6. **DPU ARM cores vs achieved load.** preload와 native를 같은 별도 패널에
   표시하고 각각의 idle ARM median을 함께 표시한다. host와 ARM 코어를
   합산하거나 동급 코어로 환산하지 않는다.

주 결론은 그래프 1의 고정-core capacity와 그래프 2의 공통-rate host CPU로
분리한다. 이 결론의 apples-to-apples 범위는 네 POSIX 경로다. native는
preload 대비 shim/POSIX 호환 계층의 gap과 direct API headroom만 설명한다.
`cores/Mrps`는 고정-core 결과를 대체하지 않는 보조 효율 지표다.

## 사전 예측과 반증 조건

아래 범위는 합격 기준이 아니라 기존 10초 L4 데이터와 배포 후 quick preflight로
rate 축이 어디에 놓일지 정한 사전 예측이다. 특히 plain은 짧은 pilot과 10초
표본 사이 변동이 커서 범위를 넓게 잡았다. native quick에서는 64B와 1KB가
collector 상한 3.0 Mrps에서도 clean이어서 right-censored 가능성이 높고, 8KB는
0.50 Mrps가 clean, 0.60 Mrps에서 drop 1.26%가 관찰됐다. 이는 10초 knee
측정값이 아니라 검색 범위를 정하기 위한 3초 preflight다.

| payload | plain | Envoy plaintext | Envoy mTLS | DPUmesh preload | DPUmesh native |
|---|---:|---:|---:|---:|---:|
| 64B | 0.12–0.45 Mrps | 1.1–1.6 | 0.9–1.4 | 0.10–0.14 | 2.5–≥3.0 |
| 1KB | 0.15–0.35 | 0.8–1.1 | 0.55–0.80 | 0.10–0.14 | 2.2–≥3.0 |
| 8KB | 0.12–0.35 | 0.18–0.25 | 0.10–0.14 | 0.05–0.065 | 0.45–0.58 |

공통 75% min-knee에서는 plain이 host CPU와 p99가 가장 낮고, DPUmesh가 두
Envoy 경로보다 host CPU를 대략 10–35% 적게 쓰되 plain보다는 10–30% 더 쓸
것으로 예상한다. DPUmesh의 이 절감은 약 2–3개의 DPU ARM 코어 사용과 교환된다.
Envoy plaintext와 mTLS는 2코어에 일찍 가까워지지만 byte-stream batching으로
그 뒤에도 RPS가 증가할 수 있다. DPUmesh는 knee 직전까지 p99가 plain과 Envoy
사이에 있다가 backpressure 시 급격히 상승할 가능성이 높다.

native는 4개 worker가 직접 EQ/QP를 poll하므로 낮은 부하에서도 client 코어의
고정 polling 비용이 나타날 수 있다. 따라서 절대 host-core/Mrps가 항상 preload보다
좋다고 예측하지 않는다. 대신 2코어 고정 capacity가 크게 높다면 그 차이는
DPU data plane 자체보다 preload/POSIX 호환 경로가 현재 병목이라는 증거다.

기존 `SWEEP.md`와도 모순으로 해석하지 않는다. SWEEP은 8KB, closed-loop
`conc=32`, 세 backend, host thread 수를 바꾸는 native topology 실험이고, 본
실험은 한 backend, 고정 2 endpoint 코어, 4개 connection의 open-loop SLA
knee다. 이번 3초 native preflight의 8KB 결과는 0.50 Mrps에서 32.77 Gb/s
(p99 0.98ms, drop 0), 0.60 Mrps에서 38.83 Gb/s를 달성했지만 drop 1.26%로
unclean이었다. 이는 SWEEP의 4-thread 29.92 Gb/s와 8-thread 38.44 Gb/s
사이에 놓여 order-of-magnitude는 일치한다. 최종 비교에는 10초 다수결 knee만
사용한다.

다음 경우에는 성능 결론보다 측정 원인을 먼저 조사한다.

- 10초 knee vote가 3회 중에도 합의하지 못하거나, retained 세 반복에서 clean/bad가
  비단조로 반복되면 해당 payload/config를 추가 3회 측정한다.
- schedule ratio가 0.98–1.02 밖이거나 generator drop이 0.1%를 넘으면 backend
  한계가 아니라 load-generator 제한이 섞인 점으로 표시한다.
- recovery가 full redeploy를 일으키면 스크립트가 중단된다. 기존 출력에
  `--no-deploy`로 이어 붙이지 않고 새 출력 디렉터리에서 다시 시작한다.
- off-core softirq 경고나 cgroup/physical closure 경고가 뜨면 해당 CPU 분해
  그래프의 인과 주장을 보류한다.

## L4 동등성과 비교 경계

비교는 “내부 구현이 동일하다”가 아니라 “같은 L4 byte-stream RPC 서비스를
제공한다”는 의미에서 동등하다.

- DPU 경로는 `DPUMESH_PROXY_L7_SVC`가 빈 상태에서 프레임을 해석하지 않는
  passthrough이며, 첫 backend 선택 뒤 연결 수명 동안 같은 backend에 고정된다.
- preload 서비스(`preload-sock`)와 native 서비스(`echo-dpumesh`)에는 각각
  backend가 하나뿐이다. 수집기는 native의 추가 backend deployment 두 개를
  0으로 내리고 base deployment의 desired/ready=1, 추가 둘의 0/0을 확인한다.
  native 응답의
  `dist=`도 정확히 한 `pod_id:count` 항목인지 모든 smoke/scout/load/profile에서
  검사한다.
- preload는 registry에 매핑된 `AF_INET/SOCK_STREAM` 데이터 socket만 가로채며,
  동일 앱의 `send/recv`, partial I/O, blocking/nonblocking 동작을 제공한다.
- 다섯 경로 모두 같은 sequence가 든 프레임을 사용하며 수집기는 `fail=0`,
  `reorder=0`을 강제한다.
- 각 측정 명령 안에서는 연결을 유지하므로 steady-state data plane이 주 대상이다.
  4개 TCP/TLS/DPU 연결은 workload clock 직전에 만든다. achieved/latency 창에서는
  제외되지만 collector의 바깥 cgroup snapshot에는 설정 CPU가 소량 포함된다.
  연결 설정 비용 자체는 별도의 churn 실험으로 분리한다.

차이도 명시해야 한다. plain은 하나의 kernel TCP leg, Envoy는 세 leg와 두 proxy,
DPUmesh는 host TCP stack 대신 DPU/DPA 및 preload shim을 쓴다. STRICT만 전송
암호화를 제공한다. 따라서 DPUmesh와 STRICT의 보안 기능이 동등하다고 주장하지
않으며, 본 실험은 L4 proxy의 처리 비용 비교다.

native는 동일 앱 비교가 아니다. `bench_dpumesh`/`echo_dpumesh`가 직접
`dmesh_alloc/post_send/poll_eq`를 호출하고 POSIX syscall/partial-I/O 호환 계층을
우회한다. 다만 수집을 위해 native에도 POSIX 벤치와 같은 `OPEN` 명령,
constant/Poisson 도착 시각, scheduled-time latency, 4개 persistent connection,
drop/pending/overflow 판정을 구현했다. 따라서 protocol·offered load·core
budget·DPU topology·backend 수는 맞지만 application API는 다르다. 논문의
주 공정 비교는 `plain`/두 Envoy/`dpumesh-preload`, native는 preload overhead와
direct-API headroom을 보이는 보조선으로만 해석한다.

## 실행과 재개

```bash
# 계획과 행 수만 확인
./bench/suite/l4_proxy_data.sh --dry-run

# 장기 측정 없이 배포와 모든 invariant만 확인
./bench/suite/l4_proxy_data.sh --preflight-only --out /tmp/l4-preflight

# 배포부터 전체 데이터 수집
./bench/suite/l4_proxy_data.sh

# 짧은 end-to-end 검증
./bench/suite/l4_proxy_data.sh --quick --out /tmp/l4-quick

# 같은 디렉터리의 완료 run_id를 건너뛰며 재개
./bench/suite/l4_proxy_data.sh --no-deploy --out <기존 출력 디렉터리>
```

출력에는 `scout.csv`, `scout_decisions.csv`, `knees.csv`, `rates.csv`,
`results.csv`, binary hash,
backend/core budget history, live pod/DPU metadata, 각 run의 cgroup·`/proc/stat`
snapshot, `mpstat`, 선택된 perf data가 포함된다.
