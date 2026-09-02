# gRPC 평가 절차 — 2026-09-02

이 문서만으로 [`FINAL.md`](FINAL.md)(정확성·성능 요약)와 [`ANALYSIS.md`](ANALYSIS.md)(병목
분석)의 모든 수치를 다시 만들 수 있어야 한다. 각 arm은 배포 → pin → sweep → 집계의 네
단계이고, 집계는 `derive.py`와 `plot.py`가 한다.

## 질문과 판정

| 질문 | 판정값 | 근거 |
|---|---|---|
| 정확한가 | gate 전부 PASS | C |
| 용량은 얼마인가 | 전달 기준과 p99 ≤ 5 ms 기준, payload별 | P1–P3, `derive.py` |
| DPU CPU는 무엇을 따르는가 | worker core 대 in-flight, ARM µs/RPC 대 rate | P4–P6, `derive.py` |
| 어느 정도인가 | direct-TCP·Linkerd 대비 절대값과 proxy core당 | R1–R3 |
| 아직 모르는 것 | E1–E4의 통과/실패 | E |

## 고정 조건

- Kubernetes node 1개, BlueField-3 1개, cross-node 없음. DPU process는 `dpumesh_dpu`,
  data worker thread는 `dmesh-w0..A-1`이며 affinity receipt는
  [`dpu-cpu-affinity.txt`](dpu-cpu-affinity.txt)다.
- geometry는 `DPUMESH_THROUGHPUT_WORKERS=W` 하나로 A=K=W, N=32 이하 W의 최대
  배수, L7=all worker를 파생한다. 기본 W=8 → `N/K/A=32/8/8`.
- Host: client Pod CPU 18–26, server Pod CPU 27–35(PCI 94:00.0의 NUMA node),
  `bench.sh pin grpcmax`, performance governor 2.5 GHz(deploy가 설정).
- generator `bench_grpc`: thread 8, channel 8(channel 하나가 QP 하나, worker 하나),
  reactor 8, open loop는 constant arrival, 10 s, warmup 1,000 요청(retained receipt에는
  warmup이 기록되지 않아 `grpc_worker_scale.sh`의 값을 규약으로 고정). latency는 client
  histogram의 scheduled-arrival-to-completion. closed loop는 worker당 window ×
  thread 수가 총 in-flight다.
- frame 64/1,024/8,192 B는 request와 response 각각의 logical frame, body는 frame−16.
- CPU: Host는 recursive Pod cgroup(application+broker 또는 application+sidecar)의
  usage delta, 창은 시작 2.5 s 뒤에 열려 6 s. DPU는 같은 창의 `dmesh-w*` thread tick
  delta와 process 합. 8 KiB의 8-worker 합이 8.0을 넘어 보이면 main/helper 몫이다.
- `clean` = 3회 모두 achieved/offered ≥ 0.99, failure/drop/pending/worker
  failure/credit loss/EQ exhaustion/Pod restart 0. `mixed` = 같은 rate의 독립
  반복에 clean과 실패가 공존. `bad` = 첫 반복부터 실패. 첫 overload에서 그 rate 축을
  멈추고(`STOP_ON_OVERLOAD=1`) payload를 바꿀 때마다 full deploy를 다시 한다.
- campaign은 한 번에 하나(`/tmp/dpumesh-bench.lock`).

## 공통 절차

```sh
cd ~/DPUmesh
export DPUMESH_THROUGHPUT_WORKERS=8 BENCH_REACTORS=8 BENCH_NUMA_POLICY=local
BENCH_DEPLOY_SCOPE=grpc bash bench/bench.sh deploy        # DPU + Pods, 항상 full deploy
bash bench/bench.sh pin grpcmax                           # 9+9 Host CPU pin
# open loop 공통 env
export CHANNELS=8 THREADS=8 REPS=3 DUR=10 WARMUP=1000 REACTORS_TAG=8 PIN_PROFILE=grpcmax STOP_ON_OVERLOAD=1
```

open-loop sweep은 `bench/suite/grpc_conns_sweep.sh`, closed-loop sweep은
`bench/suite/grpc_closed_sweep.sh`다. 둘 다 `$OUT/points.csv`(반복별 원자료)와
`$OUT/sweep.log`를 남기며, 이 디렉터리의 `*-raw.csv`는 그 `points.csv`를 합친 것이고
`*-summary.csv`는 rate별 3회 중앙값이다.

## 실험 행렬

| ID | 목적 | 명령 (공통 env 위에) | 산출 |
|---|---|---|---|
| C | 정확성 | `bash bench/suite/grpc_correctness.sh all` | [`correctness.txt`](correctness.txt), [`policy-stages.csv`](policy-stages.csv) |
| P1 | 64 B open-loop 용량 | `FRAME=64 RATES="40000 50000 60000 70000 80000 85000 90000 92500 95000 97500 100000 102500 105000" OUT=/tmp/p1 bash bench/suite/grpc_conns_sweep.sh` | [`open-raw.csv`](open-raw.csv) |
| P1r | 64 B fresh 재배포 반복 | 배포마다 하나씩: `RATES="90000 92000 94000 96000 98000"`, `RATES="98000 98500"`, `RATES="99000 101000 102000"` | [`knee-followup-raw/fresh-64-*`](knee-followup-raw/) |
| P2 | 1 KiB | `FRAME=1024 RATES="30000 40000 50000 60000 65000 70000 75000 80000"`; 재배포 후 `RATES="75250 75500 75750"`, `RATES="76000 77000 78000 79000"` | open-raw, `fresh-1k-*` |
| P3 | 8 KiB | `FRAME=8192 RATES="10000 15000 20000 25000 30000"`; 재배포 후 `"25250 25500 25750 26000"`, `"27000 28000 29000"`, `"29250 29500 29750"`, `"30000"` ×2 | open-raw, `fresh-8k-*` |
| P4 | closed loop | `THREADS=8 REPS=3 DUR=10 bash bench/suite/grpc_closed_sweep.sh --configs grpc-dpumesh --frames "64 1024 8192" --concs "1 2 4 8 16 32 64 128 256 512 1024" --out /tmp/p4` (`--concs`는 worker당 window, 총 in-flight는 ×8) | [`concurrency-raw.csv`](concurrency-raw.csv), [`closed-raw.csv`](closed-raw.csv); 8 KiB window ≥ 256은 failure로 [`saturation-rejected.csv`](saturation-rejected.csv) |
| P5 | 저부하 CPU·지연 | `FRAME=64 RATES="500 1000 2500 5000 10000"`; `FRAME=1024 RATES=10000`; `FRAME=8192 RATES=10000` | [`dpu-low-load-summary.csv`](dpu-low-load-summary.csv), [`mesh-cpu-raw.csv`](mesh-cpu-raw.csv) |
| P6 | DPU profile | 64 B 50k open loop 중 DPU에서 `pid=$(pgrep -x dpumesh_dpu); perf stat -p $pid -- sleep 8; perf record -F 49 -e cycles --call-graph fp -p $pid -- sleep 8; perf report --no-children`; 유휴는 session 없이 같은 `perf stat` | [`perf-stat.csv`](perf-stat.csv), [`perf-self.csv`](perf-self.csv) |
| P7 | worker 수 | `for w in 4 6 8 12; do WORKERS=$w OUT=/tmp/p7-a$w bash bench/suite/grpc_worker_scale.sh; done` (`threads=channels=workers`, geometry 32/4/4·30/6/6·32/8/8·24/12/12) | [`worker-scale-raw.csv`](worker-scale-raw.csv) |
| P8 | session fan-in | W=8 배포에서 `CHANNELS=24 THREADS=24 FRAME=64 RATES=80000`; W=12 배포에서 같은 명령 | [`session-scaling-summary.csv`](session-scaling-summary.csv) |
| R1 | Linkerd sidecar closed loop | Pod는 [`bench/k8s/grpc-linkerd-pods.yaml`](../../../k8s/grpc-linkerd-pods.yaml)(`linkerd.io/inject: enabled`, `skip-inbound-ports: $CTRL_PORT`, `BENCH_TRANSPORT=tcp`, `BENCH_TARGET=echo-grpc-linkerd:9091`), deploy가 함께 올린다. `bash bench/suite/grpc_closed_sweep.sh --configs grpc-linkerd --frames "64 1024 8192" --concs 128 --out /tmp/r1` | [`mesh-closed-raw.csv`](mesh-closed-raw.csv), [`linkerd-receipt.txt`](linkerd-receipt.txt) |
| R2 | matched 10k RPS Host CPU | `CLIENT_APP=bench-grpc-linkerd SERVER_APP=echo-grpc-linkerd RATES=10000` × `FRAME=64/1024/8192`; DPUmesh 쪽은 P5의 10k 점 | [`mesh-cpu-raw.csv`](mesh-cpu-raw.csv) |
| R3 | direct-TCP(mesh 없음) | 같은 두 Pod에서 sidecar를 빼고(`linkerd.io/inject: disabled`) `BENCH_TRANSPORT=tcp BENCH_TARGET=echo-grpc-linkerd:9091`, R1과 같은 closed sweep. 채택 조건: DPU process ≤ 0.05 core(경로에 DPU가 없음) | [`closed-raw.csv`](closed-raw.csv)의 `direct-tcp` 행 |
| L | 저부하 진단(E1·E2) | 아래 E1·E2의 명령 | [`lowload/`](lowload/) |

Linkerd proxy는 `edge-26.8.1`, `LINKERD2_PROXY_CORES=1`(표준 설치값), identity/mTLS
확인 후 측정한다. R1–R3의 client/server binary와 frame은 DPUmesh arm과 같다.

## 파생 지표 (`derive.py`)

| 지표 | 정의 | 파일 |
|---|---|---|
| 평균 in-flight | open loop: achieved × p50; closed loop: 총 window | `derived-inflight-cpu.csv` |
| 점유(occupancy) | worker core ÷ in-flight, in-flight < worker 수인 점들의 중앙값 | 같은 파일, `derive.py` 출력 |
| ARM µs/RPC | worker core ÷ achieved × 10⁶ | 같은 파일 |
| 전달 기준 용량 | 3/3 clean이고, 그 이하 rate의 fresh 반복에 mixed/bad가 없는 최대 offered | `derived-capacity.csv` |
| SLO 용량 | 위 조건에 p99 중앙값 ≤ 5 ms 추가 | 같은 파일 |
| proxy core당 처리량 | closed 1,024 처리량 ÷ 설정 proxy core(DPUmesh 8, Linkerd 2) | `derived-comparison.csv` |
| 교환비 | (Linkerd Host core − DPUmesh Host core) 대 DPUmesh ARM worker core, 10k RPS | `derived-exchange-10k.csv` |
| payload 비용 | knee ARM µs/RPC의 64 B 대비 증가 ÷ (2 × payload 증가 byte) | `derived-knee-cost.csv` |

## 그래프 (`plot.py`)

비례 비교(offered-achieved, CPU, bar)는 0에서 시작한다. latency, concurrency,
in-flight만 log 축이다. 기준선은 실선 회색, 개별 fresh 반복은 빈 원, first bad 이후는
점선이다. smoothing과 clipping은 없다.

| 파일 | 내용 |
|---|---|
| `00_summary` | FINAL.md의 유일한 그림: 최대 RPS(closed 1,024)와 10k RPS p50·p99, DPUmesh 대 Linkerd |
| `01_offered_achieved` | offered 대 achieved, `y=x` |
| `02_p50_latency` | 500 RPS부터 overload까지 p50, log-log |
| `03_p99_latency` | payload별 p99와 5 ms SLO 선 |
| `04_inflight` | closed loop 총 in-flight 대 처리량·p50 |
| `05_cpu_attribution` | achieved 대 client/server Pod·DPU worker core, 500 RPS부터 |
| `06_comparison` | direct/DPUmesh/Linkerd 처리량, proxy core당, 10k RPS core 교환 |
| `07_perf` | exclusive profile, 50k 대 session 없는 유휴 |
| `08_worker_scaling` | A=4/6/8/12 |
| `09_inflight_cpu` | worker core 대 in-flight(0.69 기준선), ARM µs/RPC 대 rate |
| `10_payload_scaling` | 64 B 대비 처리량, knee ARM µs/RPC |
| `11_per_rpc_pmu` | worker 하나의 요청당 instructions·cycles·cache miss·µs, 100 RPS 대 10k 대 50k |

## E. 사전 등록 실험

통과 기준을 측정 전에 고정한다. E1·E2는 측정 완료이고 결과는 ANALYSIS.md §2·§3과
[`lowload/`](lowload/)에 있다.

**E1. worker CPU가 열린 요청 수를 따르는 이유 — 측정 완료.** W=8 배포에서
`CHANNELS=1 THREADS=1 FRAME=64 RATES="100 200 500 1000 2000" REPS=3 WARMUP=100`
(`RATES=10`은 warmup 100 요청이 측정 창을 다 써서 결과가 비므로 제외). 사전 판정
기준은 ARM µs/RPC ≤ 150이면 이벤트 구동, ≥ 400이면 결함이었고 결과는 620–700
(≥ 400)이었다. 원인은 DPU에서 다음 세 측정으로 확정했다.

```sh
# 1) channel을 든 worker thread만 PMU로 잰다 (100 RPS 1 ch, 10k/50k 8 ch에서 반복)
pid=$(pgrep -x dpumesh_dpu); busy=$(top -bH -d1 -n2 -p $pid | awk '/ PID +USER/{n++} n==2 && /dmesh-w/ {print $1, $9}' | sort -k2 -nr | head -1 | cut -d" " -f1)
perf stat -e cycles,instructions,cache-references,cache-misses,task-clock,context-switches,raw_syscalls:sys_enter -t $busy -- sleep 10
perf trace -s -t $busy -- sleep 10                       # syscall 종류별 횟수
# 2) runtime loop 패스 수: DPUMESH_PERF_STATS=1로 deploy하면 10 s마다 worker별 누적
#    drains/progressed/pending/idle가 DPU 로그(bench.sh dpulog)에 찍힌다; run 전후 차이 ÷ RPC 수
# 3) 요청당 on-CPU 모양
perf record -e sched:sched_switch -a -o /tmp/sched.data -- sleep 2
perf script -i /tmp/sched.data -F time,event,trace | grep -E "dmesh-w[0-9]"   # 4 ms 공백으로 요청 묶음
```

판정 결과: 요청당 instructions 467k(knee 173k), cache miss 4.9k(1.7k), IPC 0.30(0.54),
drain 패스 7.4–8.1회(절반 Idle), syscall 47회, wake 1.2회, 정방향 450 µs + 역방향
320 µs의 연속 on-CPU. spin이 아니라 이벤트당 고정비다. 배제 근거: 유휴 worker tick
8.7 µs, DPU에 cpufreq/cpuidle 없음(2.05–2.09 GHz 고정), session 메트릭 증가 0,
generator는 `nanosleep`.

**E2. 0.6–1 ms 지연 바닥의 위치 — 측정 완료, 기각.** `src/core/dmesh_core.c`의
`TX_TAIL_DELAY_NS`를 500000에서 50000으로 바꿔 full deploy 후 channel 1개
100/500/1,000/2,000 RPS와 channel 8개 500/1k/10k RPS를 3회. 사전 기준은 500 RPS p50
< 300 µs면 Host coalescer. 결과: 100 RPS 1,605 µs(기준 1,552–1,684), 500 RPS
1,098–1,149(기준 1,146–1,254)로 불변이고 8 channel 10k RPS는 611→1,456 µs로 악화.
편집은 되돌렸다. 바닥은 §3의 DPU 이벤트 경로(요청당 on-CPU 약 770 µs)와 Host 양쪽
wake 체인(약 800 µs)에 있다. 따뜻한 바닥은 `THREADS=1 REPS=3 DUR=10 bash
bench/suite/grpc_closed_sweep.sh --configs grpc-dpumesh --frames 64 --concs "1 2 4"`로
잰 총 1 in flight p50 936 µs다.

**E3. 코어를 맞춘 비교.** 두 Pod에 `config.linkerd.io/proxy-cpu-limit: "4"`
(sidecar당 4 core)를 주고 R1·R2를 반복한다. direct-TCP는 `RATES=10000`을 3회 재서
mesh 없는 p50과 Host core를 같은 표에 넣는다. 보고는 절대값과 proxy core당을 나란히,
"n배"는 코어당 값에만 붙인다.

**E4. 단일 worker 정지와 열화.** fresh deploy 5회 × `RATES="90000 92000"` 3회.
ratio < 0.99인 반복이 나오면 즉시 `bash bench/bench.sh dpucpu`, worker별
`curl 127.0.0.1:$((4191+id))/metrics | grep ^dmesh_`, `bash bench/bench.sh dpulog 4000`을
저장한다. 별도로 80k를 1시간 간격 24회 probe한다. 판정: 90k 5/5 clean이면 90k를
용량으로 유지하고, 정지가 재현되면 정지한 worker와 정상 worker의 counter 차이를
ANALYSIS.md §5에 싣는다.

**E5. 이벤트당 고정비 절감 — 1차 측정 완료.** 채택 기준은 두 단계다. (1) 유지
기준: 64 B 90k 3/3 clean과 p99 무회귀, `grpcshutdown`과 policy 19/19 통과, channel
1개 100 RPS의 ARM µs/RPC와 syscall/RPC가 baseline보다 작을 것. (2) 목표: 100 RPS
≤ 200 µs/RPC, syscall/RPC ≤ 15, 폐루프 총 1 in flight p50 < 0.7 ms. 목표는 여러
갈래를 합쳐 도달하는 지점이고, 유지 기준을 통과한 갈래는 하나씩 채택한다.

1차 빌드(2026-09-02): `linkerd/rust/src/lib.rs` `ExternalBackend::drain`에서 Rust
`Worker::drain`을 C `dmesh_l7_driver_drain`보다 먼저 호출해 publish한 바이트가 같은
패스에서 DMA 제출되게 했고, `doca/dpu_worker.c`의 clear는 `wake_posted` 플래그가 선
경우에만 wake eventfd를 읽는다(waker는 write 뒤에 플래그를 세우므로 stale tick은
다음 readable 패스가 읽는다). 절차는 E1과 같고 deploy가 Rust를 DPU에서 다시 빌드한다
(약 13분). 결과([`lowload/e5-ab.csv`](lowload/e5-ab.csv)): worker CPU −2~6%, syscall/RPC
47→32, 지연 불변, knee 무회귀, `grpcshutdown`(opened=closed 78/78)과 policy 19/19
통과(`bash bench/suite/grpc_correctness.sh hardware`, receipt
[`policy-route-20260902-184855/`](../policy-route-20260902-184855/)). 유지 기준 통과, 목표 미달.

같은 빌드에 uprobe(`perf probe -x <bin> -a name=0x<addr>`, 주소는 실행 중 바이너리의
`nm`)를 걸어 요청당 hyper 서버 connection poll 2.0회, h2 클라이언트 connection poll
4.0회, drain 21회를 확인했다([`lowload/e5-probes-100rps.txt`](lowload/e5-probes-100rps.txt)).
inclusive profile의 태스크 64% ÷ 6 poll ≈ 80 µs/poll이므로 loop 쪽 정리의 상한은
수 %다. 다음 갈래는 (a) h2 클라이언트 poll 4→2회(요청 send와 응답 수신을 한 패스에
묶기), (b) 이벤트 뒤 bounded spin(예: 100 µs)으로 wake 체인·cold 재진입 회피,
(c) linkerd service 스택 깊이 축소이며, 각각 같은 A/B와 유지 기준을 적용한다.

## 보고 형식

FINAL.md는 결론 → 정확성 → 성능 → 재현이며 그림은 `00_summary` 하나다. ANALYSIS.md의
절 순서는 결론 → 정확성 → 용량과 지연 → DPU CPU → payload → 과부하와 재현성 → 세
transport 비교 → worker 스케일링 → 미측정 실험 → 재현으로 고정한다.
모든 표의 수치는 `*-summary.csv` 또는 `derived-*.csv`의 값이어야 한다.
