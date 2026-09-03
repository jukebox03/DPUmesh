# Per-Pod broker design receipts (2026-08-28)

`CONTROL_REPORT.md`(커밋하지 않는 설계 검토서)에서 옮겨온 부록 A·C·D다. 측정은
전부 2026-08-28 rapids4 리그(단일 노드, `N/K/A=32/8/8`)에서, broker 도입 이전의
library-owned 빌드로 했다. 원자료(`campaign.log`, `e1e2.log`,
`repeat_campaign.log`, `e3_wake.c`, `e3b_fanout.c`)는 세션 스크래치에만 있었고
남아 있지 않다 — 여기 표가 유일한 기록이다.

읽을 때: E1–E3b는 §A의 실험, V1–V6은 그 실험이 답한 실행 가능성 질문, G1–G7은
broker 구현의 acceptance gate(미실행분은 `PLAN.md` D3), M#은 구현 단계, A.1–A.5는
§A의 절 번호다. §D의 수명주기 설계는 검토 당시의 안이며 구현과 한 곳이 다르다:
broker 사망 시 라이브러리는 같은 프로세스에서 재HELLO하지 않고 `TRANSPORT_DOWN`에
SIGTERM을 올려 컨테이너를 재시작시킨다(`src/core/dmesh_core.c`;
`design/CONTROL.md` §2-1.9). 설계 근거 전체는 `design/CONTROL.md` §2-1.9·§5.4,
최종 broker 측정은
[`doorbell-relay-20260901/SUMMARY.md`](../doorbell-relay-20260901/SUMMARY.md).

---
## A. 측정 상세 (전부 2026-08-28, 같은 리그)

### A.1 REV_DOORBELL 빈도

comch 와이어 enum 11종 중 런타임 반복은 REV_DOORBELL 하나 (셋업·해체 8종
pod 수명당 1회, RESOLVE 이름당 5s 캐시 미스 1회). `dpu_flush_host_doorbells`
임시 카운터(제거됨), 부하 창 내부 5s 로그 라인쌍 6개/run:

| run | RPC/s | doorbells/s | doorbell/RPC |
|---|---|---|---|
| idle (71s + 갭 3회) | 0 | ~0.2 | — |
| conc=1 (a/b 재현) | 7,318 / 6,886 | 28,940 / 27,081 | 3.96 / 3.93 |
| conc=4 | 15,627 | 31,306 | 2.00 |
| conc=32 | 100,530 | 27,781 | 0.28 |

표의 doorbell/s는 **노드 합계**이고 이 리그의 활성 pod는 2개(bench 클라이언트
+ echo 서버)이므로 pod당 ~14-16K/s에서 포화한다 — 트래픽이 아니라 PE 스레드
wake 사이클에 묶인다는 뜻이다.

이 부하의 대부분은 신규가 아니라 **이미 pod별 PE 스레드가 내던 비용의 재배치**
다(과거 실측: PE 스레드 = 1-core pod syscall의 72%). 다만 새 구조에서
**doorbell 1건당 eventfd write 1회 + 스레드 wake 1회는 순수 증가분**이다
(§3.3 대가 ②); 위 빈도 기준 활성 pod당 ~14-16K/s이고, V5에서는 프로세스
경계 자체의 추가 wake 지연이 검출되지 않았다.

주의: DPU `-l 40`은 DOCA WARNING — stat 라인은 WARN으로 찍을 것.

### A.2 E1 — memfd 등록·DMA + 반복 캠페인

env 게이트(`DPUMESH_MEMFD_BUFFERS=1`, 제거됨)로 `alloc_buffer_and_set_mmap`
한 곳 교체 = 전 host-export 메모리가 memfd화. 단일 run: c32 97.9K rps p50
294µs, c1 6.8K rps 131µs, fail=0. 반복 ABA(단일 바이너리, env flip +
rollout + fair 재피닝, arm 확정은 ReplicaSet 이력):

| arm | n | rps | p50 |
|---|---|---|---|
| stock A+B | 8 | 97.7K ± 1.2K | 291.9µs |
| memfd (flip 직후 첫 run 제외) | 4 | 98.2K ± 1.7K | 291.8µs |
| stockA vs stockB (same config) | 4/4 | 1.0% 블록 드리프트 | — |

유의차 없음. 메커니즘도 정합: anon THP=madvise(미호출)·shmem THP=never라
양 arm 모두 4K 페이지, 등록 후 런타임 경로 동일. 방법론 함정 2개: flip 직후
첫 run은 warm-up이라 폐기, rolling update 중 `kubectl logs deploy/`는 옛
pod를 읽을 수 있음(arm 확정은 RS 이력으로). 2026-08-31 재확인에서도 bench
RS rev 168/170과 echo RS rev 152/154에 `DPUMESH_MEMFD_BUFFERS=1`이 남아 있고
현재 active RS에는 없어 ABA arm 전환 근거가 보존돼 있었다.

### A.3 E2 — 등록 보유 프로세스 급사

memfd 등록을 쥔 echo 서버를 c32 60s 부하 중 t≈25s SIGKILL. DPU: `px_poison`
2건 + 버퍼 드롭 72B/3,504B가 전부 — DMA fault 폭풍·wedge 없음. 클라이언트
fail=1로 청결 종료, pod 수 초 내 재시작·재등록(직후 fail=0), preload
pod-pair 무영향. 이 실험은 등록 보유 프로세스 1개의 급사 범위만 증명한다.
agent rollout 생존·broker 자동 재등록·DPU 재시작은 새 구조의 G5에서 별도로
판정한다. 공유 broker의 "전 pod 동시 사망"은 pod별 broker 채택 후 시험 대상이
아니다.

### A.4 E3 — cross-process wake

eventfd 2개 + epoll 핑퐁, 코어 분리 고정, 200K회 ×2라운드: writer가 같은
프로세스 스레드일 때 RTT p50 17.7-18.0µs / 다른 프로세스일 때 14.7-14.9µs.

읽는 법에 주의: 교차 프로세스가 **더 빠르게** 나왔다. 이 방향은 설명되지
않으므로 "프로세스 경계가 3µs 이득"이라고 주장하면 안 되고, 결론은 **"프로세스
경계의 wake 비용은 측정 한계 안에서 0"**까지다. 커널 eventfd wake 경로가
writer의 프로세스를 보지 않는다는 것과 정합한다. (이 벤치가 재는 것은 wake
지연뿐이며, write 자체의 syscall 비용은 A.1의 빈도로 따로 계산할 것.)

### A.5 E3b — 공유 릴레이의 tail 결합 정량화 (2026-08-28, 호스트 로컬)

E3(무경합 1:1)가 못 보는 것: burst 정렬 시 공유 릴레이 1개의 직렬화. 시뮬:
source가 DPU flush처럼 130µs 주기로 N개 pod에 burst 발행(+실제 doorbell처럼
pod당 미소비 1개 coalescing), 릴레이를 pod별 전용 N개(현 구조) / 공유 1개 /
공유 2개(샤딩)로 교체, wake 지연 분포 측정. N=8은 waiter 코어 고정, 32/64는
비고정(모드 간 상대 비교용). **p50이 유효 신호** — tail(p99+)은 전 모드
공통으로 C-state/스케줄러 노이즈(수백µs)가 지배해 이 벤치로는 판정 불가.

| N (동시 wake) | perpod p50 | shared1 p50 | shared2 p50 |
|---|---|---|---|
| 8 | 15.0µs | 26.6µs (+12) | 21.9µs (+7) |
| 32 | 13.0µs | 51.4µs (+38) | 33.9µs (+21) |
| 64 | 13.2µs | 70.6µs (+57) | 52.1µs (+39) |

**판정**: 공유 릴레이의 직렬화 비용은 실재하고, 큐 모델 초과분 ≈ (N/2R)×S
(릴레이 이벤트당 S≈2µs)와 **자릿수·추세가 일치**한다 — 다만 정확히 맞지는
않는다(측정 − 모델: N=8에서 +3~4µs, N=32에서 +5~6µs, N=64에서 −7~+7µs). 모델은
샤딩 규칙을 고르는 용도로만 쓰고, 예측값으로 인용하지 말 것. 최악 정렬(N개 pod
전원 conc=1 동시 wake) 가정이며 CPU 총량은 불변이다.

**이 측정이 §3의 pod별 broker 채택 근거다**: perpod arm(= pod별 broker의 구조)
은 N과 무관하게 13-15µs 평탄이라 tail 결합 문제가 구조적으로 없다. 공유
broker를 쓸 경우에만 샤딩이 필요했고, 규칙 `R ≥ N/8`은 "초과분을 8µs 이하로
누른다"는 예산을 위 모델에 넣어 나온 값이다(임의 상수가 아님). 실제 p999는
프로토타입에서 확정(이 호스트의 C-state 노이즈와 분리 필요).

## C. 공격 시나리오 비교 — 기존 메시 대비

| 공격 | sidecar | ambient (ztunnel) | 현 DPUmesh | 새 구조 |
|---|---|---|---|---|
| 탈취 pod의 자기 신원 키 반출 | **가능** (키가 pod 안) | 불가 (키는 ztunnel에) | 불가 | 불가 — **호스트 어디에도 workload 키 없음** |
| hostile workload의 동일-node 타 pod 사칭 (dataplane 정직) | 불가 | 불가 (커널이 소스 식별) | protocol상 불가지만 **privileged 장치 노출로 배포 전제 위반** | agent `SO_PEERCRED`+cgroup, broker 장벽으로 차단 목표(G5) |
| dataplane/proxy 자체 침해의 신원 blast | 해당 sidecar의 workload | node의 workload cert 전부 | DPU가 자기 node의 모든 pod를 대리 가능 | 동일 — broker 분리는 DPU blast를 줄이지 않음 |
| 공유 데이터플레인에 악성 **데이터** 주입 | 해당 없음 (파서가 pod별 = 자해) | **매 바이트가 공유 데몬 파서 통과** — 파서 버그=노드 DoS, ambient가 수용 | 매 바이트가 공유 DPU 통과 — 경화된 경계(bounded parsing, px_poison) | 동일 (DPU) — **broker는 데이터 바이트를 아예 안 봄** |
| 공유 **컨트롤** 표면 공격 | istiod (탈취 proxy의 CSR/XDS) | CNI/redirection 표면 | DPU comch 서버 (9검사·슬롯 cap; 미인증 타임아웃 갭) | + broker_i IPC — 표면 = **고정 크기 setup/RESOLVE 메시지 + shape-sealed fd**, 데이터 0바이트. crash/과부하 blast는 pod별이나 broker code execution은 host/device 신뢰 경계를 넘으므로 경화 필수 |
| 자원 고갈 DoS | 자기 cgroup만 | 연결 테이블 압박 (자체 한도) | **privileged라 raw device로 사실상 무제한 — 최악** | 장치 없음 + agent 쿼터 — **최선** |
| 공유 데몬 크래시 blast | 없음 (pod별) — 유일 우위 | 노드 데이터 플레인 전체, 재시작 복구 — 수용된 프로파일 | DPU 죽으면 이미 노드 전체 | DPU만 — **broker_i 크래시는 pod 단위** (DPU teardown 근거는 E2, broker 자동복구 판정은 G5) |
| remote network 공격자 | workload proxy mTLS | workload-identity HBONE mTLS | DPU node-pair TLS | 동일 목표, cryptographic principal은 다름 |
| 동일-node/PCIe 관찰자 | proxy 사이 mTLS, local leg는 as-is | ztunnel의 workload-identity 경로, pod↔ztunnel local leg는 as-is | **평문**, mapping/인프라 신뢰 | **평문**, mapping/인프라 신뢰 |
| host kernel/node/DPU 탈취 | node 상주 key·traffic 손실 | node 상주 workload key·traffic 손실 | node workload traffic·대리권 손실 | 동일 |

읽는 법: 새 구조는 shared-dataplane이라는 점에서 ambient와 같은 계급이고,
키 부재·host 밖 key 보관·pod 쿼터라는 장점이 있다. 그러나 ambient는 workload
identity-pair tunnel, DPUmesh는 node-pair tunnel이므로 wire scope까지 같지는
않다. broker IPC는 새 host/device 신뢰 경계 표면이고, sidecar는 개별 proxy
침해 blast가 한 workload라는 구조적 우위를 가진다. DPUmesh는 이 차이를
오프로딩과 node trust-boundary의 명시적 대가로 수용한다.

## D. 설계 문답 — 생명주기·장애 엣지 (pod별 broker)

이 절은 "broker_i가 죽으면?"류의 질문을 생명주기 × 컴포넌트 × 장애 축으로
훑은 결과다. **발견된 설계 요구사항 5개**: ① pod↔broker 상시 UNIX 소켓은
선택이 아니라 필수(eventfd는 HUP 의미론이 없어 상대 죽음을 못 알림 — 감지는
소켓 EOF로), ② 현 PE 스레드의 "놓친 wake 안전망" 역할은 라이브러리의 경량
watchdog으로 존속해야 함(DOCA 역할만 소멸), ③ broker는 agent에서 detach로
spawn하고 agent 재시작 시 재입양 프로토콜 필요(아니면 agent 죽음이 노드
순단으로 회귀), ④ broker 소켓은 control thread 하나만 읽어야 함, ⑤ broker
EOF와 DPU/comch down은 서로 다른 재연결 경로여야 함.

| 질문 | 답 |
|---|---|
| broker_i 죽음을 pod가 어떻게 감지·복구? | control thread가 상시 소켓 EOF 감지(즉시) → 기존 QP 전부 ECONNRESET(자동 부활 없음) → 옛 mapping/fd 폐기 → agent listener에 **새 socket**으로 HELLO → 새 broker/새 memfd의 READY. DPU 쪽은 comch 단절이 quiescence를 돌리고, 아직 quiescing이면 등록 refuse→backoff. 끊어진 fd를 재전달하거나 버릴 mapping을 memset하지 않는다 |
| pod 죽음을 broker가 어떻게 감지? | 같은 소켓 반대쪽 HUP(+pidfd). 규칙: **HUP 시 broker exit** → comch 단절 → DPU teardown. "broker 수명 = pod 수명"을 소켓으로 강제 |
| broker wedge(죽지 않고 릴레이 정지)? | 소켓 HUP 없음 → 라이브러리 watchdog이 rev ring 미소비+무wake T 초과 시 self-wake·카운트 (wake는 힌트, ring이 진실 — 기존 arm/recheck 철학 그대로) |
| agent 죽음? | target pod 상위 cgroup으로 이동한 broker는 생존, 신규 등록만 정지(오늘의 fail-static과 parity). agent 재시작 시 root-only state의 PID/starttime+cgroup+K8s를 재검증해 registry 재입양 |
| pod 재시작·이중 broker 레이스? | 옛 broker는 HUP으로 exit 중; agent가 pod UID당 ≤1 live registry와 deny/backoff를 유지; DPU 겹침은 기존 quiescing-refuse가 흡수. 새 Pod는 UID가 다르므로 이름 재사용과 혼동하지 않음 |
| lib↔broker 버전 스큐? | lib는 webhook의 host 마운트라 broker와 함께 갱신 — 스큐 창은 "구 lib 로드된 채 도는 pod + 신 broker 재spawn"뿐. agent가 MSG_PEEK한 HELLO를 broker가 다시 읽어 version 불일치를 READY 전에 거부 |
| DPU 재시작? | 전 broker comch 단절 → TRANSPORT_DOWN → 각 broker가 ctrl/datapath와 memfd를 재생성 → 같은 pod 소켓으로 새 READY. 정해진 시간 안에 실패하면 pod가 socket을 닫아 broker-death 경로로 승격하며, 처음부터 새 broker를 만들지는 않음 |
| pod의 fd 유출? | 자기 버퍼 유출 = 오늘도 가능한 자기 메모리 유출과 동일; 신원은 fd가 아닌 SO_PEERCRED에 귀속. pod의 eventfd 자가 write는 spurious self-wake뿐 |
| pod CPU/메모리 한도와 broker 경쟁? | broker를 container scope가 아닌 pod 상위 cgroup에 넣어 PE 스레드가 오늘 겪던 의미론을 보존(자기 wake 지연 자기 부담, OOM=자기 경로 사망); supervision 재spawn은 백오프로 폭주 방지. 실제 의미론은 G5 sacrificial-pod gate로 확정 |
