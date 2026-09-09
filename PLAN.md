# DPUmesh 실행 계획

기준일: 2026-09-05

**2026-09-08 배치 갱신:** DPU를 같은 Kubernetes cluster의 별도 node로 등록하고
`dpumesh_dpu`를 DaemonSet Pod로 운영한다. Host `dpumeshd`의 신뢰 가정과 별도 broker
process는 유지한다. 아래 본문의 과거 배치·WorkloadGrant 서술과 충돌하면 §0.4가 우선한다.
현재 계약은 [design/CONTROL.md](design/CONTROL.md), 배포 절차는
[packaging/README-dpu-kubernetes.md](packaging/README-dpu-kubernetes.md)에 있다.

코드 기준선: main / d2b42a2

이 문서는 DPUmesh에서 아직 끝나지 않은 일을 정하는 단일 작업 문서다.

설계의 현재 계약은 [design/API.md](design/API.md),
[design/DATA.md](design/DATA.md), [design/CONTROL.md](design/CONTROL.md),
[design/GRPC.md](design/GRPC.md)에 있다. 측정 방법과 기존 결과는
[bench/report/REPORT.md](bench/report/REPORT.md)에 있다. 이 문서는 그 위에서
무엇을 어떤 순서로 고치고, 무엇을 확인한 뒤 다음 단계로 넘어갈지를 정한다.

---

# 0. 이 문서를 읽는 법

## 0.1 한 문장 결론

DPUmesh의 단일 노드 데이터 경로, per-Pod broker, Linkerd L7 경로, native API,
POSIX preload, gRPC C++ 통합은 구현되어 실제 BlueField-3에서 동작한다. 그러나
현재 구현은 다음 다섯 이유로 아직 구조가 동결된 완성 상태가 아니다.

1. broker 장애와 격리 경로는 코드가 있지만 완전한 hardware receipt가 없다.
2. ARM worker가 오래된 배포뿐 아니라 fresh deploy에서도 간헐적으로 progress를
   잃는 현상이 설명되지 않았다.
3. cross-node 코드는 node-to-node TLS 모델로 구현되어 있지만, 새 요구사항은
   node 밖으로 나가는 트래픽에 대한 workload/pod-to-pod mTLS다.
4. public API와 내부 경계의 ownership, 오류, teardown 계약을 한 번 더 검토하고
   고정해야 그 위의 Go와 gRPC 통합을 장기적으로 유지할 수 있다.
5. 현재 node-agent는 Kubernetes API credential과 host PID/network, systemd,
   cgroup, iptables 권한을 한 container에 모으고, 일반 workload에도 `hostPath`를
   주입한다. 이 배치는 Kubernetes Pod Security와 resource lifecycle에 맞게
   분리되어 있지 않다.

따라서 실행 순서는 아래와 같다.

~~~text
P0  현재 결함과 기준선을 보존한다
    ├─ worker progress 변동 원인 제거
    └─ 현재 broker lifecycle의 최소 회귀 기준 기록

P1  확정한 Kubernetes 구조를 구현하고 계약을 동결한다
    ├─ host dpumeshd + Device Plugin
    ├─ per-Pod broker worker와 Pod lifecycle
    ├─ 명시적인 workload resource 계약
    └─ control/data/public API

P2  workload mTLS를 포함한 cross-node RDMA를 구현하고 실증한다

P3  동결된 API 위에 Go 및 추가 언어 통합을 올린다

P4  기능과 정확성을 바꾸지 않는 성능·규모 최적화를 진행한다
~~~

결함은 기능보다 먼저, 기능은 비용보다 먼저다. 측정하지 않은 코드는 완료가
아니며, traffic이 한 번 흘렀다는 사실만으로 정책, 보안, 복구가 증명되지는 않는다.

## 0.2 상태 표기

| 표기 | 의미 |
|---|---|
| DONE | 코드와 유지되는 시험 또는 hardware receipt가 모두 있다 |
| VERIFY | 코드는 있으나 요구되는 장애·hardware·장시간 증거가 부족하다 |
| OPEN | 설계 또는 구현이 남아 있다 |
| BLOCKED-HW | software 준비 뒤 실제 장비가 있어야 닫힌다 |
| DEFERRED | 선행 계약이 동결될 때까지 의도적으로 하지 않는다 |

체크박스는 작업량이 아니라 판정 결과를 뜻한다. 부분 성공은 체크하지 않는다.

## 0.3 이번 계획에서 하지 않을 일

- 결함 재현과 구조 변경을 한 A/B에 섞지 않는다.
- 현재 node-to-node TLS를 workload mTLS라고 이름만 바꾸지 않는다.
- hardware가 없다는 이유로 TCP carrier에서 가능한 crypto/fault test까지 미루지
  않는다.
- Go 지원을 위해 아직 동결되지 않은 C API를 그대로 포장하지 않는다.
- benchmark 최고값 하나를 운영 용량으로 발표하지 않는다.
- workload container에 privilege 또는 /dev/infiniband를 다시 노출하지 않는다.
- DRA와 Device Plugin을 동시에 운영하지 않는다. 현재 v1.31 기준 구현은 Device
  Plugin 하나만 사용한다.
- broker를 보이게 만들기 위해 privileged sidecar나 broker Pod를 추가하지 않는다.
- Host `dpumeshd`와 broker는 OS process로 유지한다. DPU runtime은 §0.4대로 Pod로 운영한다.
- 목표 구조에는 mutating webhook, library init container와 DPUmesh 전용
  RuntimeClass를 두지 않는다.
- `dpumeshd`와 별도 node-agent DaemonSet에 같은 registry와 lifecycle 판단을
  중복하지 않는다.

## 0.4 DPU Kubernetes 배치와 direct 등록

두 요구사항은 workload identity와 실제 DPU channel의 trusted binding, 그리고 workload에서
DOCA/Comch/memory registration 및 infrastructure credential 제어권을 분리하는 것이다.
DPU Pod 배치는 여기에 배포·설정·로그·업데이트의 사용자 편의를 더한다.

- **배치:** 동일 cluster의 별도 DPU node + `dpumesh_dpu` DaemonSet. Host의 trusted
  `dpumeshd`와 per-workload broker를 유지한다. App에 DOCA device를 주지 않는다.
- **등록:** `direct`에서 dpumeshd가 host evidence·kubelet allocation·Kubernetes 정보를
  직접 대조하고, paired DPU의 authenticated session으로 실제 Comch 연결을 REGISTER한다.
  `grant`는 controller-signed 경로로 유지하며 둘 사이의 자동 fallback은 없다. Controller는
  DPU 공개키·signed topology와 cross-node 역할을 유지하고, workload mTLS 목표도 그대로다.
- **범위 밖:** custom DPU enrollment, 새 ResolveWorkload API, 등록 시 DPU의 controller
  revision 일치 대기, 새 membership lease 체계. Infrastructure credential은 설치 절차로 공급한다.

구현·실기 배포 결과와 남은 제한은
[배포 기록](bench/report/data/k8s-registration-20260908/SUMMARY.md), 이후 발견한 TX tail
누락·Comch 오류 callback use-after-free·종료 task 누적·wrapper/cgroup 종료 판정·slot 정체의
원인과 회귀 검증은 [오류 조사](bench/report/data/registration-fault-investigation-20260908/SUMMARY.md)에 있다.

남은 일:

- [ ] 두 DPU 실기와 controller 장애 전체 조합, 장시간 soak. peer 인증·topology·policy
      CPU 회귀 테스트는 통과했다.
- [ ] Direct state/policy feed 경로 전환.

---

# 1. 프로젝트가 해결하려는 문제

## 1.1 목표

DPUmesh는 Kubernetes Service를 대상으로 한 service mesh의 데이터 플레인을
BlueField DPU로 옮긴다. application은 backend Pod 주소를 직접 고르지 않고
Service를 요청한다. DPU가 다음을 담당한다.

- workload 등록과 신원 바인딩
- Service 및 endpoint 발견
- 정책 판정
- backend 선택
- opaque TCP, HTTP/1, HTTP/2, gRPC 처리
- 같은 노드의 Pod 사이 DMA
- 다른 노드의 DPU 사이 보호된 전송
- 연결 수명, backpressure, custody와 오류 계수

application 옆에는 byte stream을 파싱하거나 정책을 실행하는 sidecar가 없다.
호스트에는 장치와 등록 메모리의 수명을 소유하는 작은 per-Pod broker가 있지만,
broker는 application payload를 읽거나 복사하거나 라우팅하지 않는다.

## 1.2 반드시 유지할 불변식

1. workload Pod는 Pod Security `Restricted`를 만족하고 `hostPath`, privilege와
   호스트 장치를 직접 갖지 않는다.
2. workload가 자신의 Pod UID, ServiceAccount, node, Service를 자기 주장만으로
   결정하지 못한다.
3. node infrastructure는 자기 node에 실제 배치된 Pod만 대리할 수 있다.
4. 정책 principal은 per-workload다.
5. 같은 node의 local leg는 등록된 DMA mapping의 격리에 의존할 수 있다.
6. node 밖으로 나가는 application payload는 암호화되고 상대 workload identity가
   상호 인증되어야 한다.
7. 설정, 인증, routing 또는 TLS 실패는 kernel TCP나 plaintext로 fallback하지
   않는다.
8. bytes의 ownership과 credit은 실제 전달 단계보다 먼저 반환되지 않는다.
9. 한 Pod 또는 한 broker의 실패가 다른 Pod의 데이터 경로를 중단하지 않는다.
10. host node runtime(`dpumeshd`)과 broker에 payload parsing, L7 policy,
    application byte queue를 넣지 않는다.
11. Kubernetes API bearer credential과 host 관리자 권한을 한 component가 동시에
    갖지 않는다.
12. `dpumeshd`는 node가 broker의 최악 조건 CPU/memory를 미리 예약한 경우에만
    그 예약으로 감당할 수 있는 DPU channel 수를 광고한다.

6번은 2026-09-04 회의에서 확정된 새 요구사항이다. 현재 구현은 node peer만 TLS로
인증하므로 이 불변식을 아직 만족하지 않는다.

## 1.3 용어

| 용어 | 뜻 |
|---|---|
| workload Pod | application container가 속한 Kubernetes Pod |
| application | native API, preload 또는 gRPC adapter를 사용하는 실제 workload process |
| channel | 한 process의 DPU 등록, shared memory, ring 전체 |
| QP | channel 위의 full-duplex application byte stream |
| EQ | application thread가 자기 QP event를 받는 event queue |
| broker | Pod 하나를 위해 DOCA device, Comch, mmap 등록을 소유하는 host process. 목표 구조에서는 `dpumeshd`가 만든 worker다 |
| current node agent | 현재 구현에서 Kubernetes 조회, kernel 증거, broker launch와 relay를 한꺼번에 맡는 DaemonSet |
| `dpumeshd` | 목표 구조의 node-resident host service. Device Plugin, kernel attestation, broker supervision과 DPU relay를 한 process에서 맡되 Kubernetes bearer token은 갖지 않는다 |
| Device Plugin | kubelet에 `dpumesh.io/channel` virtual devices를 광고하고 선택된 container에 그 allocation의 Unix socket 하나만 mount하는 `dpumeshd`의 Kubernetes interface |
| node reserve | `dpumeshd`와 최대 channel 수의 broker 비용을 node의 고정 system CPU/memory로 미리 빼 두는 운영 계약 |
| DPA EU | host forward ring을 drain하는 DPA execution unit |
| ARM worker | routing, DMA, reverse publication, Linkerd runtime을 담당하는 DPU CPU thread |
| K | Pod 하나가 사용하는 forward ring 수 |
| L | RX landing stripe 수. 현재 L은 ARM worker 수 A에서 유도된다 |
| N | DPA execution unit 수 |
| A | ARM data worker 수 |
| custody | 어느 component가 아직 bytes를 보존하고 재전송 또는 오류 책임을 지는지 |
| receipt | 명령, 환경, raw log, counter, 판정을 보존한 재검증 가능한 결과 |

---

# 2. 현재 구현 상태

## 2.1 기능별 상태

| 영역 | 상태 | 현재 근거 | 남은 일 |
|---|---|---|---|
| native C API | DONE | host contract tests와 hardware workload | API freeze 검토 |
| POSIX preload | DONE | socket/poll/epoll contract와 hardware campaign | Go/static binary에는 적용 불가 |
| gRPC C++ | DONE | release 4/4, ASAN+UBSAN 4/4, hardware correctness | C API freeze 뒤 유지 계약 정리 |
| per-Pod broker data path | VERIFY | 실제 gRPC deployment와 성능 receipt | 목표 host runtime에서 장애·격리 campaign |
| device-free workload path | DONE | workload가 `/dev/infiniband`를 갖지 않는 실제 deployment | 유지 |
| PSS Restricted workload | OPEN | 현재 webhook은 `hostPath` 두 개를 주입 | library가 포함된 workload image와 Device Plugin socket mount로 전환 |
| Kubernetes channel allocation | OPEN | 현재 label/affinity와 agent singleton만 사용 | `dpumesh.io/channel` Device Plugin 구현 |
| host runtime 분리 | OPEN | 현재 DaemonSet agent가 host systemd를 호출 | `dpumeshd`가 직접 worker를 감독하도록 이동 |
| Pod registration | DONE | signed assertion, two-phase READY, teardown tests | failure campaign |
| same-node DMA | DONE | native/preload/gRPC/HTTP hardware campaigns | worker progress 결함 |
| Linkerd L7 | DONE | policy/routing/balancing campaign | 성능 후순위 |
| controller/topology/feed | DONE | unit/contract와 single-node deployment | delta generation은 후순위 |
| peer channel upper layer | VERIFY | recording transport end-to-end test | 두 node 실증 |
| TCP peer carrier | DONE | host roundtrip/backpressure/close/reuse | crypto fault test 확대 |
| RDMA peer carrier | VERIFY | build와 carrier lifecycle tests | 실제 fabric 및 두 DPU |
| node-to-node TLS 1.3 | VERIFY | host TLS tests | 새 workload mTLS 요구로 재설계 |
| workload/pod-to-pod mTLS | OPEN | 없음 | P1/P2 핵심 |
| Go surface | DEFERRED | 없음 | API 동결 뒤 팀 공동 작업 |

## 2.2 현재 배포 기준

2026-09-04 조회 기준 단일 Kubernetes node rapids4만 Ready다. 현재 gRPC scope의
client, server, alternate server, controller, node agent, webhook이 Running이고
container restart는 0이었다. DPU startup banner는 다음 구성을 보고했다.

~~~text
N/K/A = 32/8/8
L7 layer = attached
L7 worker = all
L7 policy = fail-closed
load balancing = round-robin
~~~

gRPC control ping은 응답했고, DPU worker 합계는 opened=closed=73,
ACTIVE=PENDING=TASKS=ORPHANED=0이었다. 이것은 현재 배포가 깨끗하게 idle이라는
관측이지 broker failure와 장시간 worker progress를 증명하는 receipt는 아니다.

## 2.3 현재 로컬 검증 기준

- make test: 전체 통과
- gRPC release CTest: 4/4
- gRPC Clang ASAN+UBSAN CTest: 4/4
- embedded Linkerd adapter Rust tests: 38/38
- peer TCP wire: roundtrip, 최대 burst, backpressure, close, slot reuse 통과
- peer RDMA wire: 현재 host에 local RDMA address가 없어 skip

새 변경은 최소한 이 기준을 깨지 않아야 한다.

## 2.4 아직 주장하면 안 되는 것

- 실제 두 DPU 사이 application stream이 동작한다.
- cross-node traffic이 pod-to-pod mTLS다.
- broker가 어떤 실패에서도 자동으로 올바르게 복구한다.
- 90K 또는 closed-loop 최대 RPS가 장시간 재현 가능한 운영 한계다.
- 127 Pod가 현재 geometry에서 검증됐다.
- shared DPU를 서로 신뢰하지 않는 tenant가 안전하다.
- Go application이 source change 없이 지원된다.

---

# 3. Kubernetes 배치, 프로세스와 주소 공간

이 절은 이후 모든 control/data plane 판단의 출발점이다.

## 3.1 현재 실제 배치

~~~text
Kubernetes (API object + kubelet)
│
├─ namespace dpumesh-system                     PSS Restricted
│  └─ dpumesh-controller Deployment
│     ├─ Pod/Service/EndpointSlice get/list만 가진 유일한 cluster reader
│     ├─ topology(Ed25519)·membership·service-target(HMAC) feed 서명
│     ├─ WorkloadGrant 서명
│     └─ node-mTLS HTTPS: /workload-grant /topology.v1 /membership.v1
│        /service-targets.v1 /workload-scope /node
│
├─ application namespace                        PSS Restricted
│  └─ workload Pod P
│     └─ application container 하나
│        ├─ image에 libdpumesh.so.5 / preload / gRPC adapter 포함
│        ├─ automountServiceAccountToken: false
│        ├─ requests = limits: dpumesh.io/channel = 1
│        └─ /run/dpumesh/channel.sock
│           Device Plugin이 할당 slot socket 하나만 read-only mount
│
└─ kubelet ──Register / ListAndWatch / Allocate──▶ dpumeshd
────────────────────────────────────────────────────────────────────
host (Kubernetes 밖)
│
├─ dpumeshd.service                             systemd, node당 1개, root
│  ├─ Device Plugin server                      같은 process
│  ├─ slot listener N개                         /run/dpumesh/slots/*, mode 0666
│  ├─ SO_PEERCRED + /proc starttime + cgroup → Pod UID, container ID
│  ├─ node 인증서로 controller client            grant 요청, feed 수신, /node 보고
│  ├─ feed 배달 thread                          controller → DPU receiver, 2초 주기
│  ├─ scope tunnel                              DPU 평문 HTTP → node mTLS → controller
│  ├─ delegated cgroup                          dpumeshd.service/{manager, workers/pod<uid>.g<gen>}
│  └─ broker PID/starttime/cgroup의 유일한 owner
│
├─ dmesh_broker(P)                              dpumeshd의 직계 자식, Pod/container 아님
│  ├─ workers/pod<uid>.g<generation> cgroup
│  ├─ private PID/mount/network/cgroup namespace, 빈 tmpfs root
│  ├─ uid/gid 65532, capability 0, no_new_privs, execve/execveat seccomp 거부
│  ├─ DOCA device, Comch, exported mmap, registered memfd 소유
│  └─ payload가 아닌 control과 doorbell만 relay
│
└─ PCIe
   └─ BlueField Linux
      ├─ dpumesh-feed-receiver.service          unprivileged, 파일 설치만
      └─ dpumesh_dpu                            ARM control thread + data worker
                                                + embedded Linkerd + DPA
~~~

가장 자주 생기는 오해를 명시한다.

- broker는 application container와 같은 주소 공간이 아니다.
- broker는 sidecar container도, 별도 Kubernetes Pod도 아니다.
- broker는 systemd unit이 아니라 `dpumeshd`의 자식 process다. `dpumeshd`가 죽으면
  parent-death signal로 함께 죽고 재입양(re-adoption)은 없다.
- broker 회계는 workload Pod cgroup이 아니라 `dpumeshd.service` 아래 worker leaf다.
  Kubernetes는 `dpumesh.io/channel` 하나로 slot을 세고, host system reserve가 broker
  비용을 덮는다.
- BlueField의 ARM/DPA process는 별도 BlueField OS에서 돌며 Kubernetes agent가 없다.
- host에 Kubernetes 자격증명이 없다. `dpumeshd`는 node 인증서만, broker와 workload는
  아무것도 갖지 않는다.

이 선택은 containerd shim이나 virtiofsd처럼 Pod 밖의 infrastructure helper를 Pod
수명과 연결하는 모델이다. 장점은 workload Pod에 privileged sidecar가 없고 host에
Kubernetes bearer token이 없다는 것이다. 대가는 Kubernetes object만 조회해서 broker
상태를 볼 수 없고, `dpumeshd` 재시작이 그 node의 모든 channel을 끊는 fail-stop이라는
것이다. §3.17이 kubelet 재시작과 `dpumeshd` SIGKILL 복구를 확인했고, §7의 나머지
broker lifecycle scenario는 이 구조 위에서 아직 열려 있다.

## 3.2 현재 주소 공간과 공유 메모리

| 대상 | 자기 주소 공간에서 보는 것 | 소유하지 않는 것 |
|---|---|---|
| application | TX/RX memfd, K개 forward ring, L개 reverse ring, eventfd | DOCA device/context/Comch |
| broker | 같은 memfd의 별도 VA mapping, DOCA mmap/export, Comch, PE | application QP/EQ 상태와 payload 해석 |
| `dpumeshd` | slot listener, launch socket, worker cgroup dirfd, node 인증서 | TX/RX data mapping, DOCA object |
| DPU ARM | imported host mapping, DPU staging, proxy/session state | host virtual address |
| DPA | ring descriptor와 등록 handle로 접근한 bytes | Kubernetes identity와 L7 policy |

application과 broker의 virtual address는 같을 필요도 없고 실제로 같지 않다.
memfd가 같은 physical pages를 각 process 주소 공간에 매핑한다. DPU에는 host
pointer가 아니라 DOCA export/import 결과와 offset이 전달된다.

memfd에는 F_SEAL_SHRINK, F_SEAL_GROW, F_SEAL_SEAL을 걸어 크기와 shape를
고정한다. application과 DPU가 내용을 갱신해야 하므로 F_SEAL_WRITE는 사용하면
안 된다. application이 만든 fd를 broker가 등록하는 방향은 금지한다. 등록
memory와 fd는 broker가 만들고 application으로 단방향 전달한다.

## 3.3 현재 보안 경계

~~~text
untrusted                         trusted node infrastructure
────────────────────────────────────────────────────────────────
application container │ kernel │ dpumeshd   │ broker │ local DPU
                      │        │            │        │
자기 신원 주장 불가   │ 증거   │ 판정/서명  │ 장치   │ 정책/데이터
장치 접근 불가        │ 격리   │ launch     │ 등록   │ 처리
~~~

가정:

- workload는 hostile할 수 있다.
- host kernel, `dpumeshd`, broker, 그 node의 DPU는 하나의 node trust boundary다.
- 한 node infrastructure가 침해되면 그 node의 workload traffic과 identity는
  잃을 수 있다.
- 한 node가 다른 node에 배치된 workload까지 사칭하는 것은 막아야 한다.
- controller와 CA는 cluster authority다.

현재 설계가 지원하지 않는 더 강한 모델:

- 같은 node/DPU를 공유하는 tenant가 DPU 자체를 신뢰하지 않는다.
- DPU 하나가 침해돼도 같은 node의 다른 workload identity를 대리할 수 없어야 한다.
- PCIe/DMA local leg를 host/DPU 관리자에게도 암호화해야 한다.

이 모델에는 application end-to-end TLS, tenant별 dataplane 또는 TEE가 필요하다.

## 3.4 2026-09-05에 확정한 목표 배치

이 절은 이전 결정 기록이다. DPU의 배치는 §0.4가 대체한다.

현재 구현을 그대로 동결하지 않는다. per-Pod broker가 제공하는 fault/address-space
격리는 유지하되 Kubernetes에는 controller와 workload만 둔다. BlueField DPU와
node runtime은 Kubernetes 밖의 host/BlueField OS가 관리한다.

~~~text
Kubernetes side (API objects + kubelet)
│
├─ namespace: dpumesh-system                 PSS Restricted
│  └─ dpumesh-controller Deployment
│     ├─ Kubernetes object의 유일한 cluster reader
│     ├─ topology와 WorkloadGrant 생성/서명
│     └─ controller Service의 node-mTLS endpoint
│
├─ application namespace                    PSS Restricted
│  └─ workload Pod P
│     └─ application container 하나
│        ├─ app image에 libdpumesh/preload/gRPC adapter 포함
│        ├─ requests = limits: dpumesh.io/channel = 1
│        └─ /run/dpumesh/channel.sock
│           Device Plugin이 allocation socket 하나만 read-only mount
│
└─ kubelet
   └─ Device Plugin gRPC
      `/var/lib/kubelet/device-plugins/dpumesh.sock`
                         ▲
                         │ ListAndWatch / Allocate
─────────────────────────┼─────────────────────────────────────────────
Outside Kubernetes / worker host and BlueField trust domain
                         │
├─ dpumeshd.service                         host systemd service, one/node
│  ├─ Device Plugin server                  같은 process, 별도 daemon 아님
│  ├─ node certificate/private key
│  ├─ controller node-mTLS client
│  ├─ SO_PEERCRED + /proc/cgroup 검증
│  ├─ DPU control-plane L4 relay와 node-local feed
│  ├─ channel socket/slot registry의 유일한 owner
│  └─ broker PID/starttime/cgroup/lifetime의 유일한 owner
│
├─ delegated cgroup: dpumeshd.service/workers/P
│  └─ dmesh_broker worker P                 Pod도 container도 아님
│     ├─ exactly one Pod UID / channel slot
│     ├─ private PID, mount, network, cgroup namespace
│     ├─ private virtual address space
│     ├─ steady uid/gid 65532, capability 0
│     ├─ no_new_privs + exec-deny seccomp
│     └─ DOCA/Comch/memfd/ring의 lifetime owner
│
└─ PCIe
   └─ BlueField
      ├─ dpumesh_dpu ARM process            separate BlueField OS/VA
      ├─ embedded Linkerd runtimes
      └─ DPA execution contexts
~~~

영구 host component는 `dpumeshd` 하나다. 별도 node-agent DaemonSet, 별도
Kubernetes adapter process, node-wide broker와 per-Pod systemd transient unit을
두지 않는다. DPU에도 Kubernetes agent나 kubelet을 설치하지 않는다. `dpumeshd`
안의 Device Plugin server는 독립 authority가 아니라 kubelet adapter일 뿐이며 같은
slot registry를 사용한다.

이 결론은 다음 권한 분리를 만든다.

| 권한 | 유일한 owner | 함께 갖지 않는 것 |
|---|---|---|
| Kubernetes cluster object read | controller Pod | host root, DPU device |
| node key와 host kernel evidence | `dpumeshd` | ServiceAccount bearer token |
| channel device allocation | kubelet + `dpumeshd` Device Plugin interface | workload가 고르는 slot id |
| 한 Pod의 DOCA object와 registered memory | 그 Pod의 broker worker | 다른 Pod mapping, Kubernetes credential |
| routing, policy, DMA, peer transport | DPU | Kubernetes API credential |

배치와 실제 byte 경로는 같지 않다. 목표의 control plane과 steady data plane을
분리해서 읽으면 다음과 같다.

~~~text
CONTROL / setup and lifetime

API server ──Pod/Service/EndpointSlice/Node──▶ controller
                                                │
                                         signed feed / WorkloadGrant
                                                │ node-mTLS
kubelet ──Device Plugin RPC──▶ dpumeshd ◀───┘
                                  │
application ──Unix HELLO──────────▶│──direct exec──▶ broker P ──Comch register──▶ DPU

DATA / after READY

workload Pod P                                            workload Pod Q
application VA                                            application VA
TX memfd + forward ring                                   RX memfd + reverse ring
        │                                                         ▲
        │ descriptor / registered bytes                            │ SG-DMA + event
════════╪══════════════════════ PCIe ═══════════════════════════════╪════════════
        ▼                                                         │
      DPA EU ──▶ DPU staging ──▶ ARM worker + Linkerd ─────────────┘

broker P/Q: mapping과 Comch/doorbell lifetime owner, payload copy/route 아님
dpumeshd: setup, identity와 lifetime owner, READY 뒤 payload path에 없음
~~~

## 3.5 제거하는 구조와 제거 이유

구성 요소가 많아지면 각 component 사이에 새로운 인증, retry, registry와 upgrade
순서가 생긴다. 다음 항목은 목표 구조에 넣지 않는다.

| 넣지 않는 것 | 이유 |
|---|---|
| mutating webhook | 권한 경계가 아니라 PodSpec 자동 작성 편의 기능일 뿐이다. workload가 image와 resource를 명시하면 별도 admission component가 필요 없다 |
| DPUmesh 전용 RuntimeClass | Pod 밖의 broker를 Pod별 overhead로 이중 회계하는 대신 node가 최대 broker 비용을 한 번 고정 예약한다 |
| 별도 `kube-adapter` DaemonSet | controller가 이미 Kubernetes truth를 읽는다. adapter가 다시 Pod/Service를 watch하면 두 authoritative cache가 생긴다. Device Plugin만 필요하므로 `dpumeshd`가 kubelet socket을 직접 제공한다 |
| current node-agent DaemonSet | Kubernetes bearer token, hostPID/network, systemd, cgroup, iptables와 signing key를 한 container에 모은다 |
| DRA driver | 현재 검증 cluster는 Kubernetes v1.31.14다. 목표 기준에서 DRA를 병행하면 allocation owner와 test matrix만 둘이 된다 |
| CSI driver | Device Plugin `AllocateResponse.mounts`가 allocation별 Unix socket 전달을 이미 제공한다. storage abstraction이 필요한 데이터가 없다 |
| broker sidecar | workload Pod가 privileged/device trust domain으로 올라가고 application과 fate-share한다 |
| broker Pod | same-node FD/memfd handoff, kernel PID evidence와 host device bootstrap을 위해 다시 host bridge가 필요해져 hop만 하나 늘어난다 |
| node-wide shared broker | memory-safety failure, queue tail과 restart scope가 node 전체 Pod에 전파된다 |
| per-worker systemd transient unit | `dpumeshd`가 child PID/starttime과 delegated cgroup을 직접 소유하므로 중복 supervisor다 |
| broker re-adoption state file | 목표에서는 parent-death가 worker를 종료시키는 fail-stop을 택한다. 새 daemon이 출처를 모르는 device-owning process를 재채택하지 않는다 |
| DPU nodeAffinity injection | extended resource는 그것을 광고한 node에만 schedule된다. label과 resource라는 두 placement truth를 두지 않는다 |

DRA는 폐기되는 기술 선택이 아니라 현재 기준선에서 사용하지 않는 선택이다.
Kubernetes 기준선을 v1.35 이상으로 올리고 device sharing/claim preparation이 실제로
Device Plugin보다 lifecycle을 단순하게 만든다는 migration test가 있을 때만 별도
결정으로 교체한다. 한 deployment에서 두 allocation framework를 함께 사용하지 않는다.

## 3.6 주소 공간, namespace와 cgroup

목표 구조의 각 process는 다음처럼 분리된다.

| 대상 | network namespace와 주소 | virtual address space | cgroup/회계 |
|---|---|---|---|
| controller | controller Pod network, Pod IP | controller process 전용 | controller container cgroup |
| application | workload Pod network, Pod IP | application process 전용 | Kubernetes application container cgroup |
| `dpumeshd` | host network, node IP와 Unix sockets | host daemon 전용 | delegated `dpumeshd.service/manager` cgroup |
| broker P | 빈 private network namespace, IP 없음 | broker P 전용 | `dpumeshd.service/workers/<pod-uid>/<generation>` |
| DPU ARM | BlueField Linux network/address | DPU process 전용 | BlueField OS가 관리 |
| DPA | IP 없음 | DPA execution/mapping context | DPA runtime이 관리 |

application과 broker는 주소 공간을 공유하지 않는다. broker가 만든 같은 memfd page를
서로 다른 virtual address에 `MAP_SHARED`로 mapping할 뿐이다. descriptor의 authority는
virtual pointer가 아니라 `(registered mapping handle, checked offset, checked length,
generation)`이다.

broker는 더 이상 kubelet이 소유하는 Pod cgroup 안으로 이동하지 않는다. systemd가
`Delegate=yes`로 넘긴 `dpumeshd.service` subtree에 Pod UID별 worker cgroup을 만든다.
daemon은 시작 직후 자기 PID를 `manager/` leaf로 옮기고 service root의 필요한
controller를 enable한 뒤 `workers/<pod-uid>/<generation>/` leaf를 만든다. cgroup v2의
한 cgroup에 process를 두면서 그 아래에 domain controller를 enable하지 않는 규칙을
지키기 위한 순서다. systemd만 service root를 만들고 제거하며, `dpumeshd`만 그 아래를
관리한다. kubelet은 이 subtree를 수정하지 않는다.

node 전체의 회계는 §3.10의 고정 reserve가 맡고, 실제 broker CPU/memory 상한은
worker leaf의 `cpu.max`, `memory.high`, `memory.max`, `pids.max`가 맡는다. Pod UID는
metric과 audit correlation이지 kubelet cgroup에 대한 소유권 주장이 아니다.

worker는 최종 cgroup에 들어간 **뒤** TX/RX memfd와 ring을 만들고 모든 등록 page를
prefault/mlock한다. shared page의 최초 memory-cgroup charge와 NUMA placement를 broker
worker에 고정한 뒤 application에 fd를 보낸다. cgroup을 나중에 이동하거나
application의 first touch에 맡기지 않는다. `RLIMIT_MEMLOCK`, worker `memory.max`와
node-wide pinned-memory budget은 READY 전에 모두 확인한다. 같은 physical memfd page를
application이 다시 map해도 별도 physical allocation이나 두 번째 charge라고 세지
않는다.

## 3.7 Kubernetes namespace와 Pod Security 계약

namespace는 trust class마다 나눈다.

| namespace class | 들어가는 것 | Pod Security |
|---|---|---|
| `dpumesh-system` | controller | `enforce=restricted`, version은 cluster minor에 고정 |
| application namespaces | meshed/unmeshed workload | `enforce=restricted`, `audit=restricted`, `warn=restricted` |
| host | `dpumeshd`, broker workers | Pod Security 적용 대상이 아님; systemd/LSM/seccomp/cgroup 정책 적용 |
| BlueField OS | DPU ARM process와 DPA contexts | Kubernetes 적용 대상이 아님; DPU OS/package 정책 적용 |

목표 구조에는 privileged DPUmesh Pod가 없으므로 별도 privileged namespace도 만들지
않는다. controller는 non-root, `allowPrivilegeEscalation=false`, read-only root
filesystem, `RuntimeDefault` seccomp/AppArmor, capability drop ALL을 만족한다.

v1.31 cluster의 namespace label은 다음 값에서 시작한다. cluster minor upgrade 때
새 restricted rule을 먼저 warn/audit로 확인한 뒤 세 version을 함께 올린다.

~~~yaml
metadata:
  labels:
    pod-security.kubernetes.io/enforce: restricted
    pod-security.kubernetes.io/enforce-version: v1.31
    pod-security.kubernetes.io/audit: restricted
    pod-security.kubernetes.io/audit-version: v1.31
    pod-security.kubernetes.io/warn: restricted
    pod-security.kubernetes.io/warn-version: v1.31
~~~

controller만 전용 ServiceAccount와 Pod, Service, EndpointSlice, Node의 필요한 read
verb를 가지며 Secret list/watch나 workload namespace write 권한을 갖지 않는다.
signing key는 read-only Secret volume으로 mount한다. 특정 identity TokenRequest가
필요하면 resourceName/namespace가 고정된 별도 최소 권한으로 두고 일반
ServiceAccount token create 권한을 주지 않는다.

DPUmesh는 workload용 ServiceAccount/RBAC를 만들거나 token을 mount하지 않는다.
meshed workload는
[`automountServiceAccountToken: false`](https://kubernetes.io/docs/tasks/configure-pod-container/configure-service-account/)
를 명시하고 별도 projected `serviceAccountToken` volume도 두지 않는다. Kubernetes
API가 필요한 application은 이 기본 profile의 범위 밖이며 별도 권한 검토 없이는
meshed workload로 배포하지 않는다. `hostPath`, host namespace, device node와
privileged securityContext도 추가하지 않으며 제출 PodSpec의 일반 보안 검사는 Pod
Security Admission이 맡는다.

## 3.8 Device Plugin과 channel allocation 계약

현재 Kubernetes v1.31 기준으로 `dpumeshd`는 Device Plugin API `v1beta1` server를
host에서 실행한다. kubelet restart를 감지하면 socket을 다시 만들고 재등록한다.

~~~text
kubelet registration endpoint
  /var/lib/kubelet/device-plugins/kubelet.sock
        ▲ Register(version=v1beta1,
        │          endpoint=dpumesh.sock,
        │          resource=dpumesh.io/channel)
        │
dpumeshd plugin endpoint
  /var/lib/kubelet/device-plugins/dpumesh.sock
        └─ GetDevicePluginOptions / ListAndWatch / Allocate
~~~

두 socket은 node local이고 workload에 mount하지 않는다. `dpumeshd`는 kubelet
registration socket의 재생성을 감지해 bounded backoff로 다시 등록한다. plugin
endpoint file owner/mode와 peer credential을 검사하며 일반 host user의 gRPC 요청을
kubelet 요청으로 받아들이지 않는다.

광고 resource와 단위:

~~~text
resource name: dpumesh.io/channel
one virtual device id: one possible per-Pod channel
advertised count: min(
    configured Pod/channel ceiling,
    floor(MAX_DPA_RINGS * N / K),
    DPU가 현재 받아들일 수 있다고 보고한 slot 수,
    floor(node pinned-memory budget / worst-case locked bytes per channel)
)
~~~

현재 N=32, K=8, DPA EU당 ring ceiling=16이면 ring 공급 상한은 64 channel이다.
`MAX_PODS=127` wire ceiling 및 host pinned-memory 상한과 작은 값을 광고한다.
TX/RX mapping, K/L ring, provider metadata와 정렬 여유를 포함한 worst-case 값을
release마다 측정·고정하며 단순히 `64 MiB + 64 MiB`만 계산하지 않는다. DPU,
controller-mTLS 또는
`dpumeshd` broker runtime이 준비되지 않으면 모든 virtual device를 `Unhealthy`로
보고하여 새 Pod가 schedule되지 않게 한다.

모든 slot은 동등하므로 preferred-allocation RPC를 구현하지 않는다.
`GetDevicePluginOptions`는 `PreStartRequired=false`와
`GetPreferredAllocationAvailable=false`를 반환한다. generation은 container start
callback이 아니라 실제 Unix connection/authorization에서 만든다. `Allocate`는
container request당 device id가 정확히 하나가 아니면 거부하고 device node, env나
CDI device를 반환하지 않는다.

각 virtual device id에는 kubelet checkpoint와 container 재생성에서 변하지 않는
host 경로가 하나 있다. parent directory는 `root:root 0700`, socket inode는
`root:root 0666`이다. socket은 selected container에 exact file로만 mount되고
authorization은 파일 uid/gid가 아니라 peer cgroup/controller evidence로 한다.
일반 host user는 parent를 탐색하지 못한다. 예를 들면 다음과 같다.

~~~text
/run/dpumesh/alloc/channel-003.sock
~~~

`Allocate(device_id=channel-003)`은 이 정확한 socket 파일만 container의 고정
경로로 read-only bind mount한다.

~~~text
host allocation socket
    → /run/dpumesh/channel.sock              selected container에서 보이는 경로
~~~

application에는 host directory, 다른 allocation socket, device id, node key 또는
`/dev/infiniband`가 보이지 않는다. application이 slot 번호를 메시지로 주장하지도
않는다. 어느 listener에서 connection을 받았는지가 slot identity다.

여기서 **path, device id, socket inode와 generation은 서로 다른 것**이다.

| 값 | 재사용 여부 | 이유 |
|---|---|---|
| device id `channel-003` | node 수명 동안 안정적 | kubelet device checkpoint와 일치해야 함 |
| host/container path | 안정적 | kubelet이 저장한 `AllocateResponse`로 container를 다시 만들 수 있음 |
| socket inode | 한 daemon incarnation 동안 유지, daemon 재시작 때만 교체 | 같은 container의 channel 재연결은 허용하되 crash 전 mount와 새 daemon을 분리 |
| `daemon_incarnation` | `dpumeshd` 시작마다 random 128-bit 새 값 | crash 전 registry/grant와 crash 후 runtime을 분리 |
| slot `generation` | 한 daemon incarnation 안에서 단조 증가 | 같은 slot의 순차 사용과 async completion을 fence |

worker가 끝나도 listener는 유지하고 accepted connection만 닫으며 다음 성공한
connection에 slot generation을 올린다. daemon restart의 listener 교체만 close →
unlink → 새 socket 생성 → owner/mode 확인 → listen 순서다. 이미 실행 중인
container의 bind mount는 old inode를 계속 보므로 새 daemon에 우연히 붙지 않는다.
broker loss로 application process가 종료되고 kubelet이 container를 새로 만들 때에는
안정적인 host path를 다시 resolve하여 새 inode를 mount한다. `dpumeshd`는 이전 daemon
incarnation, 이전 accepted connection 또는 이전 generation을 새 worker에 붙이지
않는다.

listener가 열려 있어도 slot에 live worker 또는 authorization이 진행 중이면 두 번째
connection은 `busy`로 닫는다. 한 번 terminal 처리한 `(peer PID, starttime,
container id)`가 current process-termination contract 아래에서 즉시 다시 붙는 것도
거부하고 새 container incarnation을 기다린다. local API를 channel-fatal/reconnect로
바꾸기로 동결할 때만 이 마지막 규칙을 함께 바꾼다.

Device Plugin `Allocate` 요청에는 Pod UID가 들어오지 않는다. 따라서 Allocate는
workload identity authority가 아니다. identity는 application이 실제로 시작된 뒤
다음 증거로 확정한다.

~~~text
allocation별 listener
  + SO_PEERCRED(pid, uid, gid)
  + /proc/<pid>/stat starttime
  + /proc/<pid>/cgroup에서 얻은 Pod UID/container id
  + controller가 확인한 live Pod/node/container/resource/Service
  = controller-signed WorkloadGrant
~~~

이 구분을 어기고 `DPUMESH_SLOT_ID` 같은 application environment 값을 authority로
사용하지 않는다.

`dpumeshd`의 slot state는 다음 하나뿐이며 Device Plugin과 supervisor가 같은
instance를 본다.

~~~text
DPU/runtime unavailable
        │
        ▼
   UNHEALTHY
        │ readiness
        ▼
FREE_LISTENING ──accept──▶ AUTHORIZING ──grant──▶ STARTING ──▶ REGISTERING
       ▲                      │                                      │
       └──── local cleanup ◀──┘ deny/pre-register failure            │
       │                                                             ▼
       └── generation++ ◀── CLEANUP_WAIT ◀── DRAINING ◀────────── READY
                                  ▲                         registration + app IPC
                                  └── crash/timeout after registration
~~~

`READY`까지 성공한 뒤에만 Pod channel 하나가 존재한다고 센다. DPU registration을
시도하기 전 실패는 local fd/child/cgroup을 즉시 지운다. registration을 보냈거나
READY였던 generation은 broker Comch disconnect 또는 `POD_QUIESCED` 뒤 DPU가 old
mapping/ring 0을 확인할 때까지 `CLEANUP_WAIT`에 둔다. 그 확인 전에는 같은 slot에서
새 grant/worker를 열지 않는다. daemon restart도 DPU가 old daemon incarnation의
registration 0을 보고할 때까지 모든 device를 Unhealthy로 유지한다.
`ListAndWatch`는 `UNHEALTHY`와 `CLEANUP_WAIT` slot을 Unhealthy로, 나머지를 Healthy로
보고한다. READY slot이 Healthy여도 kubelet의 exclusive allocation 때문에 다른
container에 다시 배정되지 않는다.

## 3.9 명시적인 workload 계약과 scheduling 순서

webhook은 권한 경계가 아니며 목표 구조에서는 사용하지 않는다. application
container가 다음 세 가지를 자기 image와 PodSpec에 명시한다.

1. image 안에 호환되는 `libdpumesh`, preload 또는 gRPC adapter를 포함한다.
2. 같은 container의 request와 limit에 `dpumesh.io/channel: 1`을 선언한다.
3. workload에 `automountServiceAccountToken: false`를 선언하고 projected
   `serviceAccountToken` volume을 두지 않는다. DPUmesh는 workload용
   ServiceAccount/RBAC/token을 추가하지 않는다.

~~~yaml
spec:
  automountServiceAccountToken: false
  containers:
    - name: app
      image: registry.example/app-with-dpumesh@sha256:...
      resources:
        requests:
          dpumesh.io/channel: 1
        limits:
          dpumesh.io/channel: 1
~~~

resource를 요청한 regular container가 target이다. 한 Pod에서는 정확히 한 container가
정확히 한 channel을 요청한다. 별도 target annotation, init container, library volume,
env injection과 DPU nodeAffinity는 없다. 필요한 application 설정은 image 또는 일반
PodSpec이 소유한다. controller는 resource 요청 container가 없거나 둘 이상이면
WorkloadGrant를 발급하지 않는다.

~~~text
사용자가 완성된 PodSpec을 CREATE
  │
  ├─ Pod Security Admission: Restricted 검사
  ├─ scheduler: healthy dpumesh.io/channel이 있는 node 선택
  ├─ kubelet → dpumeshd Device Plugin Allocate(device id)
  │  └─ allocation socket 하나를 resource 요청 container에 mount
  └─ application: /run/dpumesh/channel.sock 연결
~~~

channel을 요청했지만 node에 available resource가 없으면 Pod는 정상 Kubernetes
의미대로 `Pending`이다. request를 빼면 DPUmesh workload가 아니므로 socket과 grant가
없고 application은 fail-closed한다. Device Plugin의 exclusive allocation과 connect
뒤의 kernel/controller 검증이 실제 권한을 정하며, PodSpec의 env, annotation과 HELLO
Service 값은 그 자체로 신뢰하지 않는다.

## 3.10 node 단위 고정 resource reserve

`dpumesh.io/channel`은 DPU ring/slot capacity와 동시에 시작 가능한 broker worker 수를
나타낸다. Pod 밖의 broker 비용을 Pod마다 RuntimeClass로 붙이지 않는다. 대신 node를
준비할 때 다음 최악 조건 비용을 한 번 고정 예약한다.

~~~text
dpumesh incremental reserve = dpumeshd base budget
                             + advertised_channel_count × per-broker budget
~~~

node의 [`systemReserved`](https://kubernetes.io/docs/tasks/administer-cluster/reserve-compute-resources/)
CPU/memory에는 기존 OS daemon budget에 이 incremental reserve를 더하고 Node
Allocatable에서 빠졌는지 확인한다. CPU Manager `static` node의
`reservedSystemCPUs`는 전체 kube/system CPU reserve를 포함하고,
`dpumeshd.service`의 `AllowedCPUs`는 그 reserved set 안에 둔다. 실제 설정값은 broker
lifecycle/soak receipt의 p99 CPU와 memory high-watermark에 안전 여유를 더해 정한다.
node reserve와 `dpumeshd` service limit이 광고하려는 channel 수를 감당하지 못하면
device를 광고하지 않는다.

실제 broker process는 `dpumeshd` subtree에 있으므로 `kubectl top pod`의 container
합계에 자동으로 나타난다고 주장하지 않는다. service 전체 cgroup limit과 worker별
`cpu.max`, `memory.high`, `memory.max`, `pids.max`를 함께 강제하고, `dpumeshd`가 다음
per-Pod metric을 Pod UID와 generation label로 내보낸다.

- broker CPU time, RSS와 cgroup throttle/OOM
- channel slot과 DPA ring 수
- PID/starttime, launch/exit reason
- registration/quiescence state와 마지막 progress 시각

사용하지 않는 channel의 reserve도 다른 Pod에 돌려주지 않는다. 이 고정 비용은
일부 자원을 덜 쓰는 대신 scheduler와 host process 사이에 두 번째 동적 회계 체계를
만들지 않는 선택이다. CPU Manager `static` node에서는 broker가 workload exclusive
core를 침범하지 않는 reserved cpuset receipt가 있어야 한다. `dpumeshd`는 kubelet의
CPU assignment나 checkpoint를 수정하지 않는다.

## 3.11 `dpumeshd`와 broker launch 계약

`dpumeshd`는 host package가 설치하는 systemd service다. broker binary와 project
DSO도 같은 versioned, root-owned package에서 가져온다. DaemonSet image에서 host로
binary를 복사하거나 content-addressed staging directory를 갱신하지 않는다.

manager가 root로 시작하는 이유는 kubelet plugin socket, peer `/proc` evidence,
namespace/cgroup 생성과 broker credential transition 때문이다. root라는 이름을
제한으로 오해하지 않고 systemd unit에 다음 경계를 둔다.

| 항목 | 계약 |
|---|---|
| filesystem | package와 `/etc/dpumesh`는 read-only; write는 `/run/dpumesh`, delegated cgroup subtree와 자기 Device Plugin endpoint만; LSM이 다른 plugin socket의 unlink/rename을 거부 |
| devices | 필요한 BlueField/DOCA device allowlist만 service descendants에 허용; block/storage device 없음 |
| capabilities | namespace/evidence/uid 전환에 실제 필요한 집합만 bounding; `NET_ADMIN`, `SYS_MODULE`, `BPF`, `PERFMON` 없음 |
| network | manager는 AF_UNIX와 controller/DPU relay용 AF_INET/AF_INET6만; broker는 새 network namespace에 IP 없음 |
| credentials | node key는 manager만 read; broker와 application에 전달하지 않음 |
| process control | manager가 만든 child PID/starttime만 추적·signal; arbitrary host PID kill/ptrace를 정상 경로로 사용하지 않음 |
| host firewall | operator/CNI가 설치; daemon이 iptables/nftables를 수정하지 않음 |

최종 capability와 syscall 목록은 선언과 실제 사용 trace의 교집합으로 생성하고
negative test로 불필요한 항목을 하나씩 거부한다. “`privileged=false`와 비슷하다”는
표현으로 대신하지 않는다.

application connection 하나의 순서는 다음과 같다.

~~~text
application       dpumeshd          controller         broker P          DPU
    │                 │                  │                  │               │
    │ HELLO(Service)  │                  │                  │               │
    ├────────────────▶│                  │                  │               │
    │                 │ peer PID/start   │                  │               │
    │                 │ cgroup→Pod UID   │                  │               │
    │                 │ slot generation │                  │               │
    │                 │ node-mTLS scope request             │               │
    │                 ├─────────────────▶│                  │               │
    │                 │                  │ live Pod/node/   │               │
    │                 │                  │ target container/│               │
    │                 │                  │ resource/Service │               │
    │                 │◀──── scoped authorization ──────────┤               │
    │                 │                  │                  │               │
    │                 │ direct exec + namespace bootstrap   │               │
    │                 ├────────────────────────────────────▶│               │
    │                 │ app socket + worker cgroup fd       │               │
    │                 ├────────────────────────────────────▶│               │
    │                 │                  │                  │ Comch connect │
    │                 │                  │                  ├──────────────▶│
    │                 │                  │                  │◀─ challenge ──┤
    │                 │ grant(nonce) request                │               │
    │                 │◀────────────────────────────────────┤               │
    │                 ├─────────────────▶│                  │               │
    │                 │◀─ signed WorkloadGrant ─────────────┤               │
    │                 ├────────────────────────────────────▶│               │
    │                 │                  │                  │ grant/register│
    │                 │                  │                  ├──────────────▶│
    │                 │                  │                  │ mmap/rings     │
    │ READY + SCM_RIGHTS                  │                  │               │
    │◀───────────────────────────────────────────────────────┤               │
~~~

`dpumeshd`는 HELLO를 `MSG_PEEK`으로 bounded validation하지만 소비하지 않는다.
broker는 넘겨받은 같은 accepted socket에서 HELLO를 다시 읽는다. manager는
slot/generation, Pod UID, container id, Service와 peer PID/starttime을 자기 registry에
보관하고 broker에는 accepted socket과 이미 연 worker cgroup fd만 보낸다. path나
cgroup string을 workload가 제공하지 않는다.

worker 생성은 외부 `/usr/bin/systemd-run`과 `/usr/bin/unshare`를 호출하지 않는다.
`dpumeshd`가 root-owned broker supervisor를 직접 exec하고, 그 작은 supervisor가
`unshare(2)`/`fork(2)`로 PID namespace를 만든다. 최종 worker는 전달받은 cgroup fd로
자기 전용 leaf에 들어간 뒤 mount/cgroup/network namespace를 분리한다. 이 단순한
두-process bootstrap에서 다음 결과는 변하지 않는다.

- manager가 wrapper와 broker PID/starttime의 유일한 owner다.
- worker는 한 Pod UID와 generation만 가진다.
- worker는 다른 worker의 proc, filesystem, network와 shared memory를 볼 수 없다.
- 두 parent edge의 `PR_SET_PDEATHSIG`와 service cgroup으로 `dpumeshd`가 사라지면
  worker도 남지 않는다.
- child는 exec/device open 전에 `PR_SET_PDEATHSIG`를 설정하고 expected parent PID와
  daemon incarnation을 다시 확인한다. fork와 prctl 사이에 parent가 죽은 race에서는
  즉시 `_exit`하며 device를 열지 않는다. systemd도 service cgroup 전체를 stop한다.
- 새 `dpumeshd`는 old worker를 re-adopt하지 않는다.
- worker는 device/provider/bootstrap을 끝낸 뒤 uid/gid 65532, capability 0,
  `no_new_privs`와 exec/execveat 거부 seccomp로 전환한다. DOCA provider의 ioctl,
  futex와 file access를 추측성 allowlist로 막는 것은 이 단순 배치의 계약이 아니다.
- broker에는 node private key, controller client credential, Kubernetes token,
  kubelet plugin socket과 다른 allocation listener를 전달하지 않는다.

user namespace는 DOCA provider와 이미 열린 device/context의 동작을 실제
BlueField에서 검증하기 전까지 필수 계약에 넣지 않는다. 현재 계약은 host uid/gid
drop, capability 0, `no_new_privs`, exec-deny seccomp와 별도 PID/mount/network/cgroup
namespace까지다. 더 넓은 syscall allowlist나 user namespace는 실제 DOCA
READY/teardown receipt가 생길 때만 별도 hardening으로 채택한다.

## 3.12 Controller 인증과 WorkloadGrant

source IP는 node identity가 아니다. 목표 controller의 node endpoint는 TLS 1.3
mutual authentication을 요구한다. node name은 request body가 아니라 client
certificate SAN에서 얻는다. `dpumeshd`는 node별 private key를 host root-only
storage에 보유하고, controller는 operator가 등록한 node CA/identity mapping을
검증한다.

이 host-node mTLS credential은 DPU가 inter-node peer TLS에 쓰는 DPU static key와
다른 keypair다. host credential은 `dpumeshd → controller` 요청만, DPU key는
`DPU ↔ DPU` channel만 인증한다. 어느 private key도 broker에 전달하지 않는다.
host certificate는 cluster id와 exact Kubernetes node name을 SAN에 묶고
clientAuth 용도로만 발급한다. rotation은 old/new 두 credential의 bounded overlap만
허용하며 revoked/expired credential은 새 grant와 feed 요청을 열지 못한다. 이미
발급한 WorkloadGrant는 자기 `not-after`보다 오래 살아남지 않는다.

controller가 발급하는 `WorkloadGrant`는 최소 다음을 서명한다.

| field | 의미 |
|---|---|
| grant id/version | replay와 format fence |
| cluster/node | 어느 node TCB가 요청했는지 |
| Pod UID | recreate와 구분되는 workload incarnation |
| namespace/ServiceAccount | policy principal |
| container id/name | resource를 받은 container |
| requested/authorized Service | HELLO가 요구한 권한의 결과 |
| channel slot/generation | 한 daemon incarnation 안의 Device Plugin allocation과 reuse fence |
| daemon incarnation | `dpumeshd` restart 전후의 registry, socket과 grant fence |
| DPU challenge nonce | 현재 Comch connection binding |
| issued/not-after | bounded freshness |
| controller key id/signature | DPU가 offline 검증할 cluster authority |

encoding은 versioned canonical binary이고 전체 크기는 4 KiB 이하로 제한한다.
signature는 controller rotation key의 Ed25519이며 DPU는 key id별 public key만 가진다.
unknown version, duplicate field, noncanonical length/padding은 거부한다. 기본 lifetime은
60초, protocol maximum은 300초, clock skew 허용은 30초다. DPU challenge nonce는
connection마다 256-bit random이고 성공/실패와 관계없이 한 번 판정한 nonce를 replay
cache에 300초 보존하여 두 번째 grant 사용을 거부한다. cache는 4096개로 bound하고
가득 차면 unexpired entry를 버리지 않고 새 registration을 rate-limit/refuse한다.
grant는 channel bootstrap용이지 steady-state lease/heartbeat가 아니다.

controller는 Pod가 live이고 certificate의 node에 배치됐으며 정확히 한 container가
`dpumesh.io/channel=1`을 요청하고 ServiceAccount token이 mount되지 않으며 Service가
허용될 때만 grant를 발급한다.
`dpumeshd`와 broker는 grant field를 덮어쓰지 못하고 DPU는 nonce, node, daemon
incarnation, slot, generation과 expiry를 모두 검사한다. workload는 Service를 요청할 수 있을 뿐 Pod
UID, ServiceAccount, slot이나 nonce를 선택하지 못한다.

topology/feed와 Linkerd control-plane relay도 host의 `dpumeshd`가 운반한다. payload는
해석하지 않는 L4 relay이고 Kubernetes Service/endpoint truth는 controller가 준다.
DPUmesh node component가 Kubernetes ServiceAccount token을 가지지 않으며 현재
node-agent의 broad `serviceaccounts/token` permission은 제거한다. identity issuance가
TokenRequest를 필요로 하면 기존 controller가 정확한 전용 ServiceAccount에 대해서만
수행한다. 별도 identity daemon을 추가하지 않는다.

목표 경계의 입출력과 실패 계약은 다음 표 하나로 고정한다.

| 경계 | 신뢰하는 입력 | 성공 시 ownership/return | 실패 시 결과 |
|---|---|---|---|
| kubelet → Device Plugin `ListAndWatch` | kubelet registration connection | stable device id와 current health; exclusive allocation은 kubelet 소유 | DPU/runtime unavailable이면 `Unhealthy`; existing data fd를 plugin이 닫지 않음 |
| kubelet → Device Plugin `Allocate` | kubelet이 고른 device id | 그 id의 stable host socket path를 target container path 하나에 read-only mount | unknown/unhealthy id면 container create 실패; Pod identity를 여기서 만들지 않음 |
| application → `dpumeshd` | allocation listener, kernel `SO_PEERCRED`/cgroup; HELLO의 Service는 요청일 뿐 | 검증 완료까지 accepted fd는 daemon 소유, launch 뒤 broker로 단 한 번 이전 | malformed/unauthorized/timeout이면 ERROR 가능한 범위에서 전송 후 close; worker 없음 |
| `dpumeshd` → controller | node-mTLS identity, kernel evidence, live DPU challenge | bounded-expiry signed WorkloadGrant | wrong node/Pod/container/resource/Service/replay면 stable reason으로 거부; broker registration 없음 |
| `dpumeshd` → broker | accepted app fd와 worker cgroup fd, 고정 package binary | daemon은 child PID/starttime/cgroup, broker는 app fd와 자기 DOCA object 수명을 소유 | exec/bootstrap/barrier 실패면 fd/cgroup/child 전부 회수하고 slot은 새 generation 전까지 미사용 |
| broker → DPU | DPU nonce에 묶인 exact WorkloadGrant와 registration geometry | DPU는 imported mapping/ring, broker는 host export를 `POD_QUIESCED`까지 보존 | grant/geometry/import 실패면 READY 없음; partial DPU state rollback |
| broker → application | exact READY packet과 ordered `SCM_RIGHTS` fd set | application은 받은 fd reference/mapping, broker는 원본 object lifetime 소유 | packet/fd/seal/geometry 하나라도 틀리면 application은 전부 close하고 channel 생성 실패 |
| controller → `dpumeshd` → DPU | signed canonical generation 또는 end-to-end protected control bytes | DPU가 signature/generation 검증 뒤 whole snapshot 교체 | unavailable/bad signature는 기존 valid generation 유지; withdrawal로 해석하지 않음 |

어느 경계도 timeout을 성공으로 취급하지 않는다. retry는 같은 request id와 같은
immutable input에만 허용하며, 성공 전 생성한 fd, child, cgroup, DPU mapping은 그
경계의 owner가 rollback한다.

## 3.13 목표 failure와 lifetime 계약

| event | 목표 동작 | failure scope |
|---|---|---|
| application 정상 destroy/socket EOF | broker unregister → DPU quiesce → memory/device destroy → worker exit → slot generation 회수 | Pod P |
| broker crash | app socket terminal, application process가 기존 계약대로 종료, Kubernetes가 container를 restart하고 fresh generation 연결 | Pod P |
| application SIGKILL | accepted connection EOF로 broker가 quiesce 후 종료; old connection/generation은 재사용하지 않음 | Pod P |
| kubelet restart | `dpumeshd`가 plugin endpoint를 다시 등록; existing app↔broker channel은 유지하고 kubelet checkpoint가 같은 device id를 소유 | allocation control만 일시 중단 |
| controller rollout | existing data channel은 유지; 새 WorkloadGrant만 bounded fail-closed | 새 channel |
| node package upgrade | node cordon → Pod drain으로 새 allocation/traffic 제거 → eviction마다 worker quiesce → live worker 0 → service/package 교체 → health 확인 → uncordon | 계획된 node maintenance |
| `dpumeshd` graceful stop | 모든 worker에 quiesce 요청, deadline 뒤 SIGKILL; Device Plugin 전부 Unhealthy | node |
| `dpumeshd` crash | parent-death contract가 worker를 종료; app은 terminal; restart한 daemon은 새 daemon incarnation/socket inode를 만들고 DPU cleanup 뒤만 slot을 재개 | node |
| DPU/Comch loss | 모든 local worker가 TRANSPORT_DOWN; slots Unhealthy; DPU ready/cleanup 뒤 fresh channels만 허용 | node |
| host reboot/power loss | kubelet checkpoint는 exclusive device assignment 복구에만 쓰고 identity로 신뢰하지 않으며, 새 daemon incarnation과 fresh Pod/container connection만 등록 | node |
| Pod deletion/placement change | controller grant revoke + local socket/cgroup evidence로 해당 worker quiesce; 다른 Pod 유지 | Pod P |
| old connection/grant replay | daemon incarnation, slot generation, Pod UID, nonce 또는 expiry mismatch로 거부 | 요청 하나 |

목표에서는 host runtime restart를 transparent하게 숨기지 않는다. `dpumeshd`는 node
TCB이고 그 실패는 node-scoped fail-stop이다. 반면 kubelet의 Device Plugin
re-registration이나 controller rollout은 existing broker data plane을 끊지 않는다.
이 둘을 같은 “agent restart”라는 말로 합치지 않는다.

## 3.14 Network와 우회 방지

repository는 다음 NetworkPolicy 기본을 제공한다.

- `dpumesh-system`: ingress/egress default deny 후 DNS, controller의 API-server
  egress와 controller node-mTLS port의 허용 node-address ingress만 연다.
- meshed application namespace: default deny가 기본이며 DNS와 명시적으로 선언한
  non-mesh destination만 허용한다.
- protected data port로의 일반 Pod-network ingress와 직접 kernel egress는 허용
  목록 없이는 열리지 않는다.

`dpumeshd`는 host process이므로 NetworkPolicy 대상이 아니다. node firewall은
controller node-mTLS destination, DPU management addresses와 필요한 Linkerd
control-plane relay만 허용한다. controller 요청의 최종 authentication은 firewall이나
source IP가 아니라 mTLS certificate다.

POSIX preload는 syscall security boundary가 아니다. hostile workload가 raw syscall로
kernel TCP를 열 수 있으므로, mesh 강제가 필요한 deployment는 PSS와 NetworkPolicy,
node ingress guard를 함께 적용한다. “DPUmesh에 들어온 뒤 fail-closed”와 “다른 길로
나갈 수 없음”을 별도 test로 증명한다.

## 3.15 Kubernetes 관점의 최종 판정

**판정은 조건부로 적절하다.** Kubernetes는 node의 모든 infrastructure process가
Pod/container여야 한다고 요구하지 않는다. 공식
[Device Plugin 문서](https://kubernetes.io/docs/concepts/extend-kubernetes/compute-storage-net/device-plugins/)도
plugin을 DaemonSet뿐 아니라 수동 또는 operating-system package로 배포할 수 있게
한다. kubelet, container runtime과 CNI daemon처럼 node 수명에 붙는 host service가
존재하는 것 자체는 Kubernetes model 위반이 아니다.

다만 host process는 Kubernetes가 자동으로 안전하게 만들어 주지 않는다.
[Pod Security Standards](https://kubernetes.io/docs/concepts/security/pod-security-standards/)와
NetworkPolicy는 Pod에 적용되며 `dpumeshd`나 broker를 sandbox하지 않는다. 따라서
다음 대체 owner가 모두 있어야 이 배치를 채택할 수 있다.

| Kubernetes Pod가 제공했을 성질 | 목표 구조에서 대신 책임지는 것 |
|---|---|
| image/version rollout | root-owned host package + systemd unit version |
| process parent와 restart | systemd가 `dpumeshd`, `dpumeshd`가 worker PID/starttime과 parent-death chain 소유 |
| namespace/securityContext | clone namespace + uid/gid drop + no-new-privs + seccomp/LSM |
| resources/limits | node의 고정 system reserve + service/worker cgroup limit |
| Pod identity | allocation listener + kernel cgroup evidence + controller live-object check |
| logs/metrics | journald와 Pod UID/generation label의 daemon metrics |
| deletion/recreation fence | socket inode + daemon incarnation + slot generation + DPU nonce |
| network isolation | broker의 empty network namespace + host firewall |

Device Plugin도 identity system으로 과대평가하지 않는다. `Allocate`는 device id를
고르고 mount를 반환하지만 Pod UID를 전달하지 않으며 `Deallocate` RPC도 없다.
kubelet이 exclusive device allocation을 소유하고, `dpumeshd`는 stable device id를
광고할 뿐이다. 실제 Pod binding은 connect 뒤 kernel/controller evidence로 하고,
socket EOF만 보고 device가 다른 Pod에 재할당됐다고 추측하지 않는다.

고정 node reserve는 broker 비용을 개별 Pod에 귀속하지 않는다. advertised channel을
모두 사용했을 때의 비용을 node allocatable에서 미리 제외하고, 실제 사용량과 hard
limit은 delegated service/worker cgroup에서 측정·강제한다. 이 보수적인 회계가
RuntimeClass와 host cgroup 사이의 이중 계약보다 단순하다.

이 구조가 현재 privileged DaemonSet보다 나은 이유는 “host daemon이라서”가 아니라
권한 조합을 끊기 때문이다. controller는 Kubernetes credential만, `dpumeshd`는 host
권한과 node key만, broker는 한 Pod의 이미 열린 device/mapping만 가진다. 반대로 host
kernel 또는 `dpumeshd`가 침해되면 그 node 전체를 잃는다는 threat model은 바뀌지
않는다. 이를 Pod 간 hostile-infrastructure isolation으로 표현해서는 안 된다.

## 3.16 이 결정의 구현 완료 조건

다음 조건을 모두 만족하기 전에는 목표 배치가 구현됐다고 쓰지 않는다.

- [x] BlueField DPU와 `dpumeshd`가 Kubernetes 밖에 있고, current node-agent
  DaemonSet과 mutating webhook 없이 controller와 workload가 시작된다.
- [x] v1.31 kubelet이 `dpumesh.io/channel` capacity를 보고하고 부족한 Pod를 Pending에
  둔다.
- [x] Allocate가 다른 allocation이나 host directory가 아닌 socket inode 하나만
  target container에 준다.
- [x] meshed workload image가 필요한 library를 포함하고, 명시적인 channel resource를
  요청한 최종 PodSpec이 PSS Restricted dry-run과 실제 admission을 통과한다.
- [x] DPUmesh가 workload용 ServiceAccount/RBAC/token, init container, hostPath와
  RuntimeClass를 추가하지 않는다는 receipt가 있다.
- [x] node 고정 reserve와 systemd aggregate limit이 advertised channel 전체의 최악
  조건 broker 비용을 감당한다는 receipt가 있다.
- [x] broker worker가 delegated host cgroup에서 별도 PID/mount/network/VA, uid 65532,
  capability 0, `no_new_privs`와 exec-deny seccomp로 실행된다.
- [x] controller node endpoint가 mTLS 없이는 거부되고 PodCIDR source IP만으로는
  인증되지 않는다.
- [x] WorkloadGrant가 Pod UID, container, daemon incarnation, slot/generation과 DPU
  nonce에 묶인다.
- [ ] B1~B7의 목표형 lifecycle/fault suite와 unrelated-Pod collateral check가
  통과한다.
- [x] `dpumeshd` crash 뒤 old worker, mapping, socket과 grant가 재채택되지 않는다.
- [x] native, preload와 C++ gRPC가 같은 allocation/grant path를 사용한다.
- [x] `make test`와 기존 hardware correctness 기준선이 회귀하지 않는다.

### 3.17 구현된 최소 배포 (2026-09-05)

지원 배포의 Kubernetes object는 controller와 application뿐이다. webhook은 할 일이
없으므로 배포하지 않으며, node-agent DaemonSet과 workload용 RBAC/token도 없다.
host에는 `dpumeshd.service` 하나, BlueField OS에는 `dpumesh_dpu` 하나만 상주한다.

실제 `rapids4`/BlueField 배포에서 다음을 확인했다.

- kubelet v1.31.14가 channel 2개를 capacity/allocatable로 광고하고 세 번째 Pod는
  `Insufficient dpumesh.io/channel`로 Pending이 된다.
- 두 workload의 CRI mount는 서로 다른 slot socket 하나뿐이고 ServiceAccount token
  mount가 없다.
- controller 보호 경로는 client certificate 없이는 403, 등록한 node certificate로는
  200이며 controller RBAC은 get/list만 가진다.
- broker 두 개는 별도 PID/mount/network namespace와 worker cgroup에서 uid 65532,
  capability 0, `NoNewPrivs=1`, seccomp mode 2로 실행된다.
- kubelet restart는 Device Plugin을 즉시 재등록하며 Pod/container/broker를 바꾸지
  않는다. `dpumeshd` SIGKILL은 old worker를 종료하고 새 incarnation/socket inode와
  fresh grant로 Pod container를 복구한다.
- 두 경우 뒤 실제 64-byte hardware request가 `fail=0`, `drops=0`으로 통과한다.

이 완료 표시는 single-node control-plane/Kubernetes 교체 범위다. §11 이후의
two-node RDMA/workload mTLS 성능 실증과 장시간 soak는 별도 단계이며 이 구현 완료에
포함하지 않는다.

`design/` 문서는 구현된 시스템의 whitepaper다. 따라서 이 절의 목표 구조를 미리
복사하지 않는다. 각 구현 slice가 code, contract test와 deployment receipt까지
완료된 뒤에만 해당 현재-state 문장을 `design/CONTROL.md`, `design/DATA.md`,
`design/API.md`와 `design/GRPC.md`에 옮긴다. 구현 전 architecture와 migration은
이 `PLAN.md`가 단독으로 소유한다.

---

# 4. 현재 구현의 Pod 생성부터 종료까지

이 장은 §3.4의 구조가 실제로 실행되는 순서를 코드 기준으로 기록한다. 배치의
근거와 권한 분리는 §3.4~§3.16에 있다.

## 4.1 Admission과 scheduling

1. workload PodSpec이 transport를 직접 선언한다. `automountServiceAccountToken:
   false`, 정확히 하나의 regular container가 `dpumesh.io/channel`을 requests =
   limits = 1로 요청, 서버면 `DPUMESH_SERVICE`, `DPUMESH_RINGS_PER_POD`는 node
   runtime과 같은 값. image가 adapter를 포함하고 POSIX workload는 `LD_PRELOAD`를
   image 또는 PodSpec에 적는다.
2. webhook, annotation, env injection, nodeAffinity는 없다. extended resource를
   광고한 node에만 schedule된다.
3. kubelet이 `dpumeshd`의 Device Plugin에 `Allocate`를 호출하면 그 slot의 host
   socket 하나가 `/run/dpumesh/channel.sock`에 read-only mount된다. Pod identity는
   여기서 만들지 않는다.
4. PodSpec 위반은 admission이 아니라 등록 시점에 controller가 거부한다. token
   mount, request≠limit, init container 소유, 다른 node, terminating Pod,
   selector 불일치가 그 예다. Pod는 뜨지만 channel 생성이 실패한다.
5. DPU 또는 controller feed가 unavailable이면 `ListAndWatch`가 모든 slot을
   `Unhealthy`로 광고하므로 새 workload는 schedule되지 않는다. 기존 channel의
   data fd는 닫지 않는다.

## 4.2 Channel 생성과 broker handoff

application이 dmesh_create_channel 또는 dpumesh_init을 호출하면 다음 순서가
실행된다.

~~~text
application       dpumeshd             controller        broker(P)           DPU
    │                 │                    │                 │                 │
    │ HELLO(Service)  │                    │                 │                 │
    ├────────────────▶│ MSG_PEEK           │                 │                 │
    │                 │ SO_PEERCRED PID    │                 │                 │
    │                 │ starttime 2회 읽기 │                 │                 │
    │                 │ cgroup → Pod UID,  │                 │                 │
    │                 │   container ID     │                 │                 │
    │                 │ slot generation++  │                 │                 │
    │                 │ worker cgroup 생성 │                 │                 │
    │                 │ fork (직계 자식)   │                 │                 │
    │                 ├─ launch sock: app fd + cgroup dirfd ─▶│                 │
    │                 │ uid 0, parent PID, │                 │ cgroup 이동     │
    │                 │  cgroup 이동 확인  │                 │ namespace 분리  │
    │                 ├─ GO ─────────────────────────────────▶│                 │
    │                 │ app fd 사본 close  │                 │ HELLO 소비      │
    │                 │                    │                 │ DOCA open       │
    │                 │                    │                 │ Comch connect   │
    │                 │                    │                 ├────────────────▶│
    │                 │                    │                 │◀ REG_CHALLENGE ─┤
    │                 │◀─ manager sock: grant 요청(nonce) ───┤                 │
    │                 │ PID/starttime/     │                 │                 │
    │                 │  Service 대조      │                 │                 │
    │                 ├─ node mTLS ───────▶│ K8s 7조건 검증  │                 │
    │                 │◀─ WorkloadGrant ───┤ Ed25519 서명    │                 │
    │                 ├─ grant bytes ────────────────────────▶│                 │
    │                 │                    │                 │ WORKLOAD_ASSERT │
    │                 │                    │                 │ POD_REGISTER    │
    │                 │                    │                 ├────────────────▶│
    │                 │                    │                 │◀ POD_ASSIGNED ──┤
    │                 │                    │                 │ memfd/mmap/ring │
    │                 │                    │                 │ MMAP_EXPORT × N │
    │                 │                    │                 ├────────────────▶│
    │                 │                    │                 │◀ INIT READY ────┤
    │                 │                    │                 │ privilege drop  │
    │◀── READY + SCM_RIGHTS (K+L+3 fd) ─────────────────────┤                 │
    │ 검증 후 mmap    │                    │                 │ uid 65532 progress
~~~

`dpumeshd`는 HELLO를 MSG_PEEK으로 검사하고 소비하지 않는다. accepted fd 자체를
launch socket으로 broker에 넘기고 자기 사본은 launch 직후 닫는다. 사본을 계속 들고
있으면 broker가 죽어도 application이 HUP을 받지 못한다.

broker가 grant를 요청하는 시점은 DPU nonce를 받은 뒤다. `dpumeshd`는 요청자를
manager socket의 `SO_PEERCRED` PID, uid, supervision table의 starttime으로 확인하고
Service가 launch 기록과 같아야 controller에 전달한다. containerStatus가 늦게 채워질
수 있으므로 같은 nonce로 0.5초 간격 최대 15회 재시도하며, 성공한 grant는 정확히
하나만 broker에 전달한다.

slot 상태는 하나의 lock 아래 다음 순서로만 움직인다.

~~~text
FREE_LISTENING ─HELLO─▶ AUTHORIZING ─evidence─▶ STARTING ─launch─▶ REGISTERING
      ▲                    │ 거부                  │ 실패
      │                    ▼                       ▼
      │            FREE_LISTENING 또는 UNHEALTHY(runtime not ready)
      │
      └─ broker exit ─ 등록 도달 전: 즉시
                     ─ 등록 도달 후: CLEANUP_WAIT 5초 뒤
~~~

generation은 AUTHORIZING 진입마다 증가하므로 늦게 도착한 grant나 completion이 다음
tenant로 들어가지 못한다.

## 4.3 등록 barrier

POD_ASSIGNED는 사용할 수 있다는 뜻이 아니다. 전체 barrier는 다음과 같다.

~~~text
POD_REGISTER
  → POD_ASSIGNED
  → host memory 등록/export
  → DPU mmap import
  → 모든 DPA RING_ADD_ACK
  → POD_INIT_RESULT(READY, L)
~~~

application은 READY와 fd set을 모두 검증한 뒤에만 QP를 만들 수 있다. READY
수신자는 다음을 mmap 전에 확인한다.

- IPC magic과 version 3
- READY packet 48 bytes의 정확한 크기와 zero padding
- fd_count = K + L + 3
- MSG_TRUNC와 MSG_CTRUNC 부재
- K, L, ring size, TX/RX bytes의 상한
- memfd shape seals: `F_SEAL_SHRINK|F_SEAL_GROW|F_SEAL_SEAL`, `F_SEAL_WRITE` 없음
- DPU가 준 pod_id, service_id와 geometry

등록은 phase별로 idempotent하게 retry된다. ready channel은 heartbeat 등록을
계속 보내지 않는다.

## 4.4 같은 node의 데이터 경로

~~~text
source application
  │ 1. dmesh_alloc: TX registered memory 예약
  │ 2. bytes 기록
  │ 3. dmesh_post_send: forward descriptor publish
  ▼
DPA execution unit
  │ 4. descriptor와 bytes를 DPU staging으로 DMA
  ▼
ARM worker + embedded Linkerd
  │ 5. policy, protocol, route, backend 선택
  │ 6. destination registered RX memory로 SG-DMA
  ▼
destination application
  │ 7. reverse ring event 수신
  │ 8. bytes 처리 후 RX buffer release
  ▼
source reverse completion
     9. delivered bytes와 TX capacity 반환
~~~

`dpumeshd`는 1~9에 참여하지 않는다. broker도 payload를 전달하지 않는다.
broker는 Comch PE와 DPU doorbell을 progress하고 Pod-global eventfd를 깨우는
역할만 한다. application의 drain thread가 reverse ring을 해석하고 EQ별 event로
분배한다.

## 4.5 Opaque와 protocol-aware 경로

- opaque Service는 한 QP/stream이 선택된 backend에 connection-pinned된다.
- HTTP/1, HTTP/2, gRPC Service는 source ARM worker의 Linkerd stack에서 request를
  해석하고 request별 route, retry, timeout, policy, backend selection을 수행한다.
- destination endpoint는 Linkerd가 선택한 정확한 Pod UID로 유지된다.
- local destination이면 live registration으로 검증한다.
- remote destination이면 signed topology와 remote admission을 사용한다.
- destination application 앞에는 두 번째 Linkerd byte-stream proxy가 없다.
  따라서 local DMA endpoint에 ordinary Linkerd endpoint TLS를 켜면 ciphertext가
  application에 도착한다. local endpoint TLS는 의도적으로 disabled다.

## 4.6 정상 종료

정상 channel destruction은 다음 순서를 지킨다.

~~~text
새 send 중단
  → QP FIN/reset과 outstanding custody 정리
  → POD_UNREGISTER 반복
  → DPA ring 제거
  → 진행 중인 DMA 완료 또는 fault 처리
  → imported mapping 제거
  → POD_QUIESCED
  → broker가 DOCA mmap과 memfd 해제
  → application mapping/fd 해제
  → broker 종료
  → supervisor가 빈 private root mountpoint 제거
  → dpumeshd가 worker cgroup leaf 제거, slot은 5초 cleanup barrier 뒤 FREE_LISTENING
~~~

POD_QUIESCED 전에 broker memory를 없애면 DPU가 해제된 host page를 DMA할 수 있다.
반대로 DPU가 quiesced인데 host fd를 영구 보존하면 Pod churn마다 pinned memory가
누적된다. 양쪽 barrier가 모두 필요하다.

## 4.7 비정상 종료의 현재 동작

| 사건 | 현재 동작 | 상태 |
|---|---|---|
| application socket close/process death | broker가 EOF 감지, POD_UNREGISTER→POD_QUIESCED 뒤 exit; Kubernetes가 container를 재시작해 새 HELLO, 새 slot generation | node-agent 구조 receipt(2026-09-04)만 있음, 이 구조에서 VERIFY |
| broker SIGKILL | application control thread가 EOF 감지, transport down publish 후 process SIGTERM; slot은 CLEANUP_WAIT 5초; Kubernetes restart로 새 broker/nonce/grant | 동일 |
| Comch/DPU down | broker가 TRANSPORT_DOWN 송신; application process 종료 후 fresh broker/channel로 재등록 | VERIFY |
| `dpumeshd` stop/crash (SIGKILL 포함) | parent-death signal로 모든 broker 종료; 새 incarnation, socket inode, grant로 Pod container 복구; feed/controller readiness 전까지 slot `Unhealthy` | HW PASS 2026-09-05 (§3.17) |
| kubelet restart | Device Plugin 즉시 재등록; Pod/container/broker 불변 | HW PASS 2026-09-05 (§3.17) |
| membership withdrawal | DPU가 routing을 제거하고 해당 registration의 ring/mapping quiescence 시작 | VERIFY |
| 30초 미등록 Comch peer | DPU가 disconnect를 요청하고 slot 회수 (`DMESH_REGISTRATION_TIMEOUT_NS`) | 코드 존재, HW 주입 미실행 (§7 B7) |

중요: 현재 library는 unexpected broker/transport loss 때 application process에
SIGTERM을 보내고 _exit(75)한다. 이것은 Kubernetes restart를 recovery primitive로
삼는 구현이다. API freeze에서 다음 둘 중 하나를 명시적으로 선택해야 한다.

1. process restart가 public contract다. 이 경우 library가 process를 종료할 수
   있음을 API와 운영 문서에 명시하고 non-Kubernetes 사용을 제한한다.
2. channel-level fatal event를 public API에 추가하고 application이 재생성 또는
   종료를 결정한다. 기존 QP와 mapping을 새 channel 아래 원자적으로 바꾸지 않고
   자동 복구를 약속해서는 안 된다.

현재 동작을 암묵적으로 유지하는 것은 허용하지 않는다.

---

# 5. 현재 구현의 Control plane

이 장은 현재 코드가 사용하는 authority와 feed다. §3.12의 분리, 즉 controller가
Kubernetes truth와 `WorkloadGrant`를, `dpumeshd`가 host evidence와 relay를 맡는
구조가 그대로 구현돼 있다.

## 5.1 권한 계층

| 범위 | component | 권한과 책임 |
|---|---|---|
| cluster | dpumesh-controller | Pod/Service/EndpointSlice get/list; topology(Ed25519)·membership·service-target(HMAC) feed 서명; WorkloadGrant 서명; `/workload-scope` 중재 조회; `/node`로 DPU 공개키 수집 |
| node/host | `dpumeshd` | kernel evidence 확인, Device Plugin, broker launch/supervision, feed와 scope tunnel relay(내용 미해석), node 인증서 보유 |
| Pod resource owner | broker | device, Comch, registered memory의 수명 |
| node/DPU | dpumesh_dpu | grant 검증(key_id별 public key), feed signature/generation 검증, policy, routing, DMA, peer transport |
| workload | application | Service 요청, byte ownership, QP/EQ 처리 |

어느 node component도 Kubernetes bearer token을 갖지 않는다. cluster 범위
statement는 controller key로만 서명되고 DPU는 public key만 가진다. `dpumeshd`는
grant key도 feed key도 없이 서명된 bytes만 운반한다. node-local feed는 그 node가
침해되면 local workload를 잃는 threat model 아래 node-scoped HMAC key를 사용한다.

## 5.2 Workload identity binding

등록 시 identity는 kernel evidence와 Kubernetes snapshot을 controller 서명으로
결합한다.

~~~text
SO_PEERCRED PID
  → /proc starttime 2회 읽기 (PID reuse fence)
  → cgroup v2 path에서 Pod UID + 64-hex container ID
  → dpumeshd → controller, node mTLS (node name은 certificate SAN)
  → controller가 Kubernetes snapshot으로 검증:
      Pod live, spec.nodeName = certificate node,
      token 미mount, 정확히 하나의 container가 channel=1,
      containerID 일치, IPv4 Pod IP,
      요청 Service의 selector가 Pod와 일치, ready EndpointSlice
  → Ed25519 WorkloadGrant (version 3, 1545 bytes)
      nonce, slot, generation, daemon incarnation, expiry, pod_ip, service_account
  → broker가 WORKLOAD_ASSERT로 DPU에 제시
  → DPU가 nonce, node, incarnation, slot, generation, expiry, replay 검사
~~~

workload가 보내는 service string은 요청일 뿐 권한이 아니다. controller가 실제
Kubernetes object를 보고 허용할 때만 grant에 들어간다. workload는 Pod UID,
ServiceAccount, slot, nonce를 선택하지 못한다.

nonce/grant는 broker 구조에서도 유지한다.

- 전용 device policy가 완벽하면 `dpumeshd`가 launch한 broker만 Comch를 만들 수
  있어 launch 자체가 1차 boundary다.
- 실제 RDMA node는 storage/ML workload와 HCA를 공유할 수 있다.
- device를 가진 다른 process가 Comch를 열더라도 signed grant가 없으면 arbitrary
  Service registration을 얻지 못하게 하는 2차 방벽이 필요하다.
- Pod lifetime당 한 번의 signature이며 steady data path 비용이 없다.

## 5.3 Controller와 feed

controller는 generation마다 다음을 canonical하게 묶는다.

- Service name과 ClusterIP/port
- ready endpoint Pod UID와 Pod IP
- namespace와 ServiceAccount
- Pod가 배치된 Kubernetes node
- node peer address
- node/DPU public identity material
- protection class와 policy lookup에 필요한 정보

feed는 세 종류다.

| feed | 서명 | 상한 | 내용 |
|---|---|---|---|
| `/topology.v1` | Ed25519, controller key | 16 MiB | cluster generation |
| `/membership.v1` | HMAC-SHA256, feed keyring | 256 KiB | 그 node의 grant-eligible (Pod UID, Service) |
| `/service-targets.v1` | HMAC-SHA256, feed keyring | 1 MiB | Service별 ClusterIP와 ready endpoint, endpoint Pod UID |

`dpumeshd`가 2초 주기로 세 feed를 받아 DPU feed receiver(port 4788)에 배달한다.
배달은 SHA-256 digest로 idempotent하고, receiver는 임시 파일에 쓰고 fsync 뒤
`/etc/dpumesh/feeds/*.v1`로 atomic rename한다. 같은 주기에 DPU node public key를
읽어 controller `/node`에 보고하므로 topology가 node peer address와 DPU key를
담는다. 한 주기가 성공해야 slot이 healthy이고, 실패하면 최대 30초 backoff로
재시도하며 그동안 새 workload는 schedule되지 않는다.

consumer는 signature, key id, grammar, bound, strictly increasing generation을
검사한다. 새 generation이 없거나 잘못됐다고 기존 정상 generation을 지우지
않는다. unavailable은 withdrawal이 아니다.

Linkerd control plane과의 연결은 두 갈래다.

- embedded proxy는 DPU 설정의 `LINKERD2_PROXY_*`로 linkerd-identity, destination,
  policy에 직접 mTLS를 맺는다. identity material은 delivered file이며 `dpumeshd`에
  Linkerd relay는 없다.
- DPU의 workload-scope 조회는 `dpumeshd`의 scope tunnel(기본 `192.168.100.1:28089`,
  동시 16)을 거쳐 node mTLS로 controller `/workload-scope`에 닿는다. tunnel은
  protocol-blind이고, controller는 certificate의 node에 그 Pod가 있을 때만 답한다.

application Pod가 DPU control port 또는 signing key에 직접 접근하지 않는다.

## 5.4 정책

- outbound policy watch의 source_workload는 등록에서 얻은 정확한 workload다.
- target은 synthetic backend가 아니라 실제 Kubernetes Service ClusterIP:port다.
- destination Pod의 inbound policy는 destination registration마다 watch한다.
- destination verdict의 source identity는 controller가 서명한 grant의
  ServiceAccount, namespace, Pod IP를 사용한다.
- protected L7 session이 policy를 얻지 못하면 fail-closed다.
- traffic 결과와 DPU verdict counter가 함께 일치해야 policy test가 PASS다.

---

# 6. 현재 구현의 Data plane과 ownership 계약

이 장의 application API, ring, DPA와 Linkerd 계약은 목표에서도 유지한다. 다만
agent↔broker launch와 initial IPC endpoint는 current 구현이며 §3.8과 §3.11로
교체될 대상이다.

## 6.1 Public native API

현재 기본 모델:

- process당 channel 하나
- polling thread당 EQ 하나
- connection당 full-duplex QP 하나
- QP는 생성한 EQ의 thread가 소유
- dmesh_alloc이 registered TX memory를 예약
- dmesh_post_send가 reservation ownership을 transport로 넘김
- EAGAIN이면 그 QP가 TX_READY를 one-shot으로 arm
- RX event의 buffer는 dmesh_release_rx_buffer 전까지 application 소유
- dmesh_eq_fd는 외부 epoll loop에 연결 가능
- dmesh_eq_next_deadline_ns는 buffered tail의 최대 지연을 보장

API freeze에서 다음을 문장과 test로 확정한다.

- 각 함수의 thread-safe 범위
- channel/EQ/QP destruction 순서
- callback 또는 event가 object destruction 뒤 발생할 수 있는지
- partial send와 flush의 의미
- FIN, reset, peer refusal, transport loss의 errno/event mapping
- outstanding reservation을 취소하는 방법
- application이 RX buffer를 오래 보유할 때의 bounded behavior
- fork/exec 지원 여부
- library가 process signal/exit를 호출할 수 있는지
- native, preload, gRPC surface를 한 process에서 섞지 않는 제한
- ABI major bump 조건

완료 조건:

- [ ] design/API.md에 위 항목이 모호하지 않게 적혀 있다.
- [ ] public header comment가 같은 계약을 표현한다.
- [ ] 각 오류/teardown path가 host contract test에 있다.
- [ ] API 변경이 ABI compatible인지 명시하고, incompatible이면 SONAME을 올린다.

## 6.2 Application↔broker IPC

전송은 AF_UNIX SOCK_SEQPACKET이다. 메시지는 고정 크기이며 version과 padding을
검사한다.

| 메시지 | 방향 | 의미 |
|---|---|---|
| HELLO | app→agent/broker | Service와 IPC version 요청 |
| READY | broker→app | pod/service id, geometry, mapping handle, fd_count |
| ERROR | broker→app | setup 또는 protocol failure |
| RESOLVE | app→broker | Service name/address lookup relay |
| RESOLVE_ACK | broker→app | DPU resolution result |
| TRANSPORT_DOWN | broker→app | Comch/DPU path가 더는 유효하지 않음 |

READY의 fd 순서는 계약이다.

~~~text
forward ring 0..K-1
reverse ring 0..L-1
TX memfd
RX memfd
doorbell eventfd
~~~

불변식:

- data payload는 Unix socket을 지나지 않는다.
- broker socket의 reader는 application control thread 하나뿐이다.
- RESOLVE reply와 asynchronous ERROR/TRANSPORT_DOWN이 서로 packet을 빼앗지 않는다.
- request_id가 ACK와 맞지 않으면 결과로 채택하지 않는다.
- malformed message를 받은 뒤 connection을 계속 사용하지 않는다.
- version은 application C constant와 agent Python constant가 함께 바뀐다.
- fd를 하나라도 검증하지 못하면 받은 fd 전체를 닫고 channel 생성에 실패한다.

## 6.3 현재 Agent↔broker launch 계약

launch는 다음 barrier를 가진다.

1. agent가 application peer를 인증한다.
2. Pod parent 아래 broker child cgroup을 안전하게 찾고 dirfd를 연다.
3. root-private launch socket과 random token을 만든다.
4. host systemd가 content-addressed broker binary를 새 PID namespace에서 시작한다.
5. agent가 accepted application socket과 cgroup dirfd를 SCM_RIGHTS로 보낸다.
6. broker가 자기 PID를 target cgroup으로 이동하고 확인을 돌려보낸다.
7. agent가 final PID/starttime/cgroup을 검증하고 registry/state file을 publish한다.
8. final-go barrier 뒤 broker가 untrusted HELLO를 읽는다.

경로 string을 workload로부터 받지 않는다. host cgroup root 아래 canonical Pod UID
경로를 agent가 재도출한다. symlink/traversal 또는 둘 이상의 후보가 있으면
fail-closed다.

목표 구조는 이 transient systemd unit, state file과 re-adoption barrier를 유지하지
않는다. `dpumeshd`가 stable Device Plugin path에서 peer를 받은 뒤 controller
authorization, delegated worker cgroup, direct child bootstrap과 fd handoff를 하나의
owner로 처리한다. 목표 launch의 전체 계약은 §3.11 하나가 소유한다.

## 6.4 Broker↔DPU registration 계약

~~~text
Comch connect
  → REG_CHALLENGE
  → WORKLOAD_ASSERT
  → POD_REGISTER
  → POD_ASSIGNED
  → mmap/ring import
  → POD_INIT_RESULT
  → steady state
  → POD_UNREGISTER
  → POD_QUIESCED
~~~

WORKLOAD_ASSERT 검사는 최소한 key id, canonical encoding, version, node, time,
nonce, signature, replay, Service binding을 포함한다. 미등록 connection은 30초
뒤 transport disconnect로 회수한다.

## 6.5 Host↔DPA ring 계약

- forward ring은 application/host가 producer, DPA EU가 consumer다.
- reverse ring은 지정된 ARM completion path가 단일 producer, host drain이 consumer다.
- pod id와 generation이 slot reuse를 fence한다.
- source port는 FIN/reset ACK가 retirement를 증명할 때까지 재사용하지 않는다.
- ring counter width와 wrap 비교는 ABI contract test로 보호한다.
- MAX_DPA_RINGS, structure size, alignment 변경은 host와 DPA를 같이 rebuild한다.

## 6.6 DPUmesh↔Linkerd adapter ABI

adapter는 다음 경계만 노출한다.

- exact flow identity와 target
- session open/close
- input segment custody
- output reservation/commit
- backend channel take/release
- policy/destination/identity feed
- metric과 refusal reason

Linkerd가 application host pointer, broker fd 또는 DOCA object를 알게 하지 않는다.
DPUmesh C 코드가 HTTP/gRPC header를 해석하지 않는다.

## 6.7 API freeze의 완료 정의

P1은 문서만 편집했다고 끝나지 않는다.

- [ ] component와 주소 공간 그림이 현재 manifest/process와 일치한다.
- [ ] admission→register→data→teardown sequence가 코드와 일치한다.
- [ ] public C API의 thread, ownership, error, teardown 계약이 test로 고정된다.
- [ ] app↔broker IPC version과 fd order가 양쪽 test로 고정된다.
- [ ] broker failure 시 process restart 여부를 결정한다.
- [ ] peer transport가 요구하는 identity, ordering, custody, failure 계약이 정해진다.
- [ ] 같은 node와 다른 node의 암호화 범위가 명시된다.
- [ ] design 문서끼리 같은 용어와 같은 상태를 사용한다.
- [ ] make test, gRPC release/sanitizer, Rust tests, doc link check가 통과한다.

---

# 7. P0-1/P1 gate: per-Pod broker 장애 경로를 실증한다

## 7.1 왜 첫 번째인가

broker는 security boundary이면서 registered memory owner다. 정상 traffic 성능이
좋아도 broker가 죽을 때 stale DMA, 중복 broker, cross-Pod 영향 또는 무한 restart가
생기면 per-Pod 구조를 완료했다고 할 수 없다. broker 배치는 §3.4에서 확정했다.
이 장은 그 선택을 다시 고르는 절이 아니라 목표 구조가 약속한 failure scope를
관측으로 증명하는 gate다.

현재 unit test는 IPC framing, agent authorization, webhook을 검증하지만 실제
process, cgroup, DPU disconnect, kubelet/`dpumeshd` restart를 한 시나리오로 묶지
않는다. migration 전에는 current node-agent/systemd 경로에서 B1, B2, B5, B7의
baseline을 한 번 보존한다. 목표 PASS 판정은 target `dpumeshd`/Device Plugin 경로를
대상으로 한다. current re-adoption을 더 강화하는 일은 migration gate가 아니다.

## 7.2 만들 시험

새 hardware suite의 작업 이름은 broker-lifecycle로 한다. 최종 파일명은 구현 시
정하되 current/target mode와 exact component version을 receipt에 적고 다음 성질을
가져야 한다.

- 현재 deployment를 식별하고 예상하지 않은 scope이면 시작하지 않는다.
- 시작 전 Pod restart, container id, Device Plugin device id, socket inode,
  daemon incarnation, slot generation, broker PID/starttime와 DPU
  registration/session counter를 snapshot한다.
- background load는 종료를 보장하고 PID를 기록한다.
- 각 fault 뒤 Ready만 보지 않고 실제 point fail=0을 요구한다.
- unrelated native/preload 또는 두 번째 gRPC pair를 collateral control로 둔다.
- cleanup은 fault가 나도 실행되며 원래 replica와 fixture를 복원한다.
- 모든 row는 PASS/FAIL을 가지며 nodata는 FAIL이다.
- raw log, commands, timestamps, before/after counters를 receipt directory에 남긴다.

## 7.3 시나리오

### B1. Broker SIGKILL

절차:

1. client/server와 각 broker PID, starttime, Pod UID mapping을 기록한다.
2. 60초 background traffic을 시작한다.
3. 대상 broker 하나에 SIGKILL을 보낸다.
4. application이 transport loss를 감지하고 container가 유한 시간 안에 재시작하는지
   본다.
5. old broker PID/starttime/cgroup/socket generation이 제거되고 새 broker 하나만 생기는지
   확인한다.
6. Pod Ready와 broker READY 뒤 후속 point를 실행한다.
7. collateral pair의 restart, failure, DPU session을 확인한다.

PASS:

- fault 대상의 오류와 restart 횟수는 유한하다.
- 새 broker의 PID/starttime은 old와 다르다.
- 같은 Pod UID와 allocation에 live broker가 최대 하나다.
- old DPU registration과 mapping은 quiesce된다.
- old accepted connection 또는 generation으로 새 registration을 열 수 없다.
- 후속 point fail=0이다.
- collateral pair는 restart=0이고 traffic을 계속 처리한다.

### B2. Application process/container SIGKILL

PASS:

- application socket HUP 뒤 broker가 종료한다.
- DPU가 POD_QUIESCED에 도달한다.
- old broker PID와 worker cgroup이 남지 않는다.
- Kubernetes restart 뒤 fresh broker와 fresh generation으로 등록한다.
- unrelated Pod는 영향이 없다.

### B3. kubelet restart와 Device Plugin 재등록

PASS:

- kubelet 중단 동안 existing broker PID/starttime, channel generation과 traffic이
  유지된다.
- `dpumeshd`가 kubelet registration socket 복구 뒤 plugin endpoint를 다시 등록한다.
- kubelet checkpoint가 이미 할당된 device id를 다른 Pod에 중복 할당하지 않는다.
- `ListAndWatch` recovery 뒤 capacity와 health가 실제 DPU/slot 상태와 같다.
- kubelet 복구 뒤 새 workload allocation과 container mount가 성공한다.

### B4. `dpumeshd` graceful stop과 crash

graceful stop과 `SIGKILL`을 별도 arm으로 실행한다.

PASS:

- graceful stop은 worker마다 bounded quiesce를 시도하고 deadline 뒤 남은 worker를
  종료한다.
- crash arm에서는 parent-death contract로 모든 old worker가 유한 시간 안에
  사라진다.
- 모든 application은 terminal socket 결과를 받아 현재 public contract대로
  container restart에 도달한다.
- 재시작한 daemon의 128-bit incarnation은 old와 다르고 listener inode도 새것이다.
- old PID, cgroup, mapping, grant와 socket inode를 re-adopt하지 않는다.
- DPU가 old registration을 cleanup하기 전 device health를 올리지 않는다.
- fresh container mount와 fresh grant로만 후속 point가 성공한다.

이 사건의 failure scope는 node다. unrelated Pod가 살아남아야 한다는 B1/B2의
조건을 적용하지 않는다. 대신 node 밖의 control plane과 다른 DPU node가 영향을
받지 않아야 한다.

### B5. DPU/Comch restart

현재 contract는 process restart 기반이다.

PASS:

- 모든 broker/application이 transport loss를 감지한다.
- Kubernetes가 workload를 복구한다.
- old mapping과 registration이 남지 않는다.
- Pod UID당 live broker가 하나다.
- DPU ready 뒤 모든 pair의 후속 point가 fail=0이다.
- 재시도 폭주나 무한 5초 backoff가 없다.

API freeze에서 channel-level recovery를 선택하면 이 시나리오의 기대 결과를
그 계약에 맞게 바꾸고 다시 실행한다.

### B6. 격리, Pod Security와 회계

sacrificial Pod에서 확인한다.

- broker /proc/1은 broker 자신이다.
- host의 다른 PID가 broker procfs에 보이지 않는다.
- network namespace가 `dpumeshd`와 다르고 IP address가 없다.
- mountinfo에 writable host cgroup mount가 남지 않는다.
- broker uid/gid가 전용 값이다.
- CapEff는 0이다.
- `no_new_privs=1`이고 allowlist 밖의 exec/network/filesystem-open 계열 syscall이
  seccomp로 거부된다.
- workload container에서 /dev/infiniband open이 실패한다.
- arbitrary non-root uid의 selected container는 read-only exact-file mount를 통해
  socket에 connect할 수 있고, sibling/unallocated Pod에는 그 path가 보이지 않는다.
- workload PodSpec과 controller PodSpec이 PSS Restricted를 통과하며
  `hostPath`, host namespace와 privileged field가 없다.
- CPU quota를 줄이면 해당 broker worker cgroup만 throttle된다.
- memory pressure/OOM이 다른 Pod broker를 죽이지 않는다.
- broker CPU/memory는 `dpumeshd.service/workers/<pod-uid>/<generation>`에서 정확히
  한 번 집계되고 Pod UID metric으로 노출된다.
- node의 고정 system CPU/memory reserve가 다른 system daemon 비용과 advertised
  channel 전체의 broker budget을 포함하고, `dpumeshd` service aggregate limit이
  그중 DPUmesh budget을 강제한다.
- kubelet의 workload Pod cgroup과 `dpumeshd` delegated subtree를 어느 한쪽도
  교차 수정하지 않는다.

### B7. 미인증 Comch timeout

PASS:

- challenge만 받고 등록하지 않은 connection이 30초 뒤 disconnect된다.
- timeout/retry/disconnect counter가 증가한다.
- disconnect callback이 slot을 실제로 회수한다.
- 같은 slot에 정상 broker가 다시 등록할 수 있다.

## 7.4 완료 gate

- [ ] B1~B7 executable suite가 있다.
- [ ] 각 scenario가 실제 BlueField deployment에서 PASS했다.
- [ ] raw receipt가 bench/report/data 아래 보존됐다.
- [ ] 발견된 bug가 수정되고 같은 scenario가 red→green으로 바뀌었다.
- [ ] make test와 기존 gRPC correctness가 회귀 없이 통과했다.
- [ ] target suite가 통과하고 배포 receipt가 생긴 뒤에만 design whitepaper의
  broker lifecycle 문장을 target current-state로 바꾼다.

---

# 8. P0-2: worker progress 변동을 해결한다

## 8.1 관측된 문제

64-byte gRPC open-loop에서 오래된 deployment의 80K point가 achieved ratio
0.9878, p99 904ms까지 무너진 적이 있다. fresh deploy에서도 worker 5가 progress를
잃어 92K에서 73,587 schedule drop과 나쁜 98/99K 관측을 만든 적이 있다. full
redeploy 뒤 같은 80K의 세 번 중앙값은 ratio 1.0000, p99 4.973ms로 회복됐다.

근거는
[grpc-professor-20260902/ANALYSIS.md](bench/report/data/grpc-professor-20260902/ANALYSIS.md)에
있다.

따라서 age만 원인이라고 할 수 없고, 한 번의 clean maximum도 재현 가능한 capacity로
볼 수 없다.

## 8.2 먼저 구분할 failure class

| class | 관측 signature | 가능한 계층 |
|---|---|---|
| worker hard wedge | worker admin /live도 timeout, CPU 100%, listener Recv-Q 증가 | Linkerd runtime poll loop |
| data worker starvation | main/control 또는 특정 worker affinity 충돌 | CPU pinning/scheduler |
| queued overload residue | active/task/backend channel이 point 사이 0으로 안 돌아옴 | harness/teardown |
| allocator exhaustion | arena/unit free가 회복하지 않음 | DPU proxy allocator |
| DMA progress stall | inflight/retry/stall queue가 정체 | DOCA DMA engine |
| notification loss | ring에 work가 있지만 wake/arm generation이 안 움직임 | doorbell/arm protocol |
| session generation leak | opened-closed 또는 pending이 누적 | lifecycle |
| instrument failure | client가 control request에 답하지 않음, nodata | benchmark client |

하나를 고르기 전에 stall 순간의 snapshot이 모든 class를 구분할 수 있어야 한다.

## 8.3 관측을 추가할 위치

### ARM worker

- loop pass total
- last successful progress monotonic timestamp
- per-pass Dmesh/Linkerd progress result
- idle/progressed/unpublishable result 수
- doca_pe_progress call과 productive call 수
- maintenance deadline wake 수
- worker별 CPU와 context switch

### Session과 L7 runtime

- opened, closed, active, pending, tasks
- stack build와 teardown generation
- backend channel live/retired
- input segment와 output publication
- h2 connection poll 수
- session walk에서 방문한 entry 수

### Proxy/DMA

- DMA tasks inflight
- retry batch와 retry probe
- emit/stall/ack queue head-tail
- allocator free/high-watermark
- arena stall
- peer/local poison과 refusal
- reverse publication pending

### Host/application/broker

- broker wake와 doorbell count
- drain pass, arm_epoch, watchdog/self-wake
- EQ budget exhaustion
- client pending/drop/worker failure
- Pod recursive cgroup CPU와 throttle
- process restart와 broker PID/starttime

metric은 loaded path에서 per-request 비용을 눈에 띄게 바꾸지 않아야 한다. 자주
변하는 값은 counter로 두고, text log는 state transition과 failure snapshot에만
쓴다.

## 8.4 재현 matrix

### Fresh deploy 반복

동일 commit, 동일 N/K/A=32/8/8, 2.5GHz, 동일 pin에서 다음을 반복한다.

1. full deploy
2. correctness smoke
3. 64B 80K, 90K, 92K open-loop 각 3회
4. point 사이 quiescence gate
5. 모든 worker snapshot
6. deploy를 최소 10회 새로 만들며 반복

98/99K는 알려진 경계 위 stress로 별도 기록하되 clean capacity 표본과 섞지 않는다.

### Soak

- 6시간
- 12시간
- 24시간

soak는 계속 최대 load를 거는 시험과 idle aging을 분리한다.

- idle-aged deployment 뒤 표준 probe
- 낮은 일정 load를 유지한 deployment 뒤 probe
- 주기적으로 connection churn을 준 deployment 뒤 probe

각 probe가 deployment state를 바꾸므로 timestamp, 이전 point, session count를
함께 기록한다.

## 8.5 Point 사이 quiescence gate

grpc_closed_sweep.sh를 포함한 high-concurrency runner는 새 point 전에 다음을
확인해야 한다.

- client/server restart 0
- opened == closed
- active == 0
- pending == 0
- live task == 0
- backend channel residue 0
- DMA inflight/stall queue 0

시간 안에 0이 되지 않으면 다음 point를 실행하지 않고 현재 point를 dirty/rejected로
기록한다. overload cancellation의 residue 위에서 다음 성능치를 측정하지 않는다.

## 8.6 수정 원칙

- 재현 signature를 얻기 전에 poll budget이나 sleep을 추측으로 바꾸지 않는다.
- correctness와 performance 변경을 같은 commit에 섞지 않는다.
- worker 하나의 stall을 전체 평균 CPU로 가리지 않는다.
- DPU process 전체 restart로 증상을 없앤 뒤 fix라고 부르지 않는다.
- fix는 failure를 재현한 exact point에서 red→green을 보여야 한다.
- 새로운 busy poll로 progress를 얻었다면 idle CPU와 control starvation을 함께
  판정한다.

## 8.7 완료 gate

- [ ] failure snapshot이 위 class 중 하나로 원인을 좁힌다.
- [ ] 최소 하나의 deterministic 또는 높은 확률의 reproducer가 있다.
- [ ] root cause를 설명하는 code-level invariant가 문서화된다.
- [ ] 수정 전 reproducer가 실패하고 수정 후 통과한다.
- [ ] fresh deploy 10회 matrix에서 unexplained worker stall이 없다.
- [ ] 6/12/24시간 결과가 모두 clean하다.
- [ ] 기존 policy, broker lifecycle, gRPC sanitizer가 통과한다.
- [ ] accepted open-loop capacity를 새 build에서 다시 산정한다.

---

# 9. 이번 주 할 일

이번 주는 2026-09-05 architecture 검토 직후 시작하는 hardware-independent 및 현재 rapids4
작업 주기를 뜻한다. 날짜보다 gate를 우선한다. 새 node B가 없어도 할 수 있는
일을 먼저 끝낸다.

## W0. 계획 정리

- [x] 임시 control-plane 보고서의 유효한 설계 판단을 이 문서에 흡수한다.
- [x] 과거 구현 예정과 현재 실제 코드를 구분한다.
- [x] 현재 K8s/broker 주소 공간과 lifecycle 그림을 만든다.
- [x] node-to-node TLS와 workload mTLS의 차이를 open requirement로 기록한다.
- [x] 미뤄온 성능·규모 항목을 우선순위와 함께 한 번 더 모은다.
- [x] per-Pod broker worker, 단일 host `dpumeshd`, Device Plugin과 Restricted
  workload 구조를 확정한다.
- [x] 별도 privileged node-agent Pod, DRA/CSI, broker Pod/sidecar, per-worker systemd
  unit과 re-adoption을 목표 구조에서 제거한다.

## W1. Broker lifecycle suite

- [ ] B1~B7을 current baseline과 target acceptance로 구분해 실행하는 suite와
  receipt schema를 만든다.
- [ ] destructive target을 Pod UID와 broker PID/starttime으로 두 번 확인한다.
- [ ] collateral control pair를 준비한다.
- [ ] 정상 상태 dry-run에서 suite 자체가 false failure를 내지 않는지 확인한다.
- [ ] migration 전에 rapids4 current 경로에서 B1, B2, B5, B7 baseline을 기록한다.
- [ ] target 구현 뒤 B1~B5 lifecycle을 실행한다.
- [ ] B6 confinement/PSS/accounting을 실행한다.
- [ ] B7 unauthenticated timeout을 실행한다.
- [ ] 발견된 bug를 작은 단위로 고치고 scenario를 재실행한다.

이번 주 exit:

- target B1~B5가 raw receipt와 함께 PASS
- B6/B7이 장비 제약으로 못 끝나면 정확한 blocker와 이미 통과한 절반을 기록
- Pod-scoped fault에는 다른 Pod가 영향을 받지 않았고 node-scoped fault에는 다른
  node/control plane이 영향을 받지 않았다는 counter가 있음

## W2. Worker progress 관측

- [ ] 기존 metric 중 위 snapshot에 이미 있는 것과 없는 것을 표로 만든다.
- [ ] hot path overhead가 작은 counter만 추가한다.
- [ ] one-command snapshot helper를 만든다.
- [ ] point 사이 quiescence gate를 runner에 추가한다.
- [ ] fresh deploy 3회로 instrumentation과 receipt schema를 검증한다.
- [ ] 긴 soak 전에 short reproducer를 시도한다.

이번 주 exit:

- stall이 다시 발생하면 원인 class를 구분할 수 있는 snapshot이 자동 보존됨
- point residue를 다음 결과에 섞지 않음
- 6/12/24시간 soak를 unattended가 아니라 결과 보존 가능한 형태로 시작할 준비가 됨

## W3. API/설계 freeze 준비

P0 코드 수정을 막지 않는 범위에서 다음 decision 목록만 확정한다.

- [ ] broker/DPU local transport loss 때 process restart를 public contract로 둘지 결정
- [x] peer transport loss는 QP fatal event로 노출하고 process restart나 transparent
  replay를 하지 않는다. §11.12가 public mapping을 정한다.
- [x] workload mTLS identity는 ServiceAccount principal과 unique Pod incarnation을
  함께 인증한다. §11.4가 certificate binding을 정한다.
- [x] node TLS는 association bootstrap/control lane에만 쓰고 workload TLS는
  cross-node application data lane에 쓴다. application payload를 중첩 TLS로 이중
  암호화하지 않는다. §11.3이 정확한 암·복호화 경계를 정한다.
- [x] 외부 transport API의 thread, ownership, backpressure, custody, callback 및
  teardown 계약을 작성한다. §11.7~§11.12가 기준이다.

이번 주에는 Go wrapper와 cross-node RDMA 성능 최적화를 시작하지 않는다.

---

# 10. P1: 확정한 Kubernetes 구조를 구현하고 API를 동결한다

이 단계는 architecture를 다시 선택하는 단계가 아니다. §3.4~§3.16의 배치를 작은
교체 단위로 구현하고, current data-plane contract가 새 lifecycle에서도 유지되는지
증명하는 단계다. 구현 전 목표 설명은 PLAN에만 있고, `design/`은 배포 receipt가
생긴 slice까지만 현재형으로 갱신한다.

## 10.1 Authority와 ownership pass

구현 전에 각 상태를 쓰는 component를 하나로 제한한다.

| 상태/행위 | 유일한 writer | reader |
|---|---|---|
| Kubernetes Pod/Service/topology 해석 | controller | `dpumeshd`, DPU |
| workload PodSpec | workload owner | API server/kubelet/controller |
| extended-resource allocation | kubelet Device Manager | `dpumeshd` plugin interface |
| slot/socket/daemon generation | `dpumeshd` | broker, controller, metrics |
| broker PID/cgroup/lifetime | `dpumeshd` | operator/metrics |
| 한 channel의 device/memory mapping | broker worker | application, DPU |
| routing/policy/DMA/peer state | DPU | controller feed, application events |

controller와 `dpumeshd`가 각각 Pod registry를 authoritative하게 유지하거나,
kubelet과 `dpumeshd`가 각각 device allocation을 결정하면 안 된다. controller는 live
Kubernetes object를 판정하고, `dpumeshd`는 연결에서 얻은 kernel evidence를 제시하며,
kubelet은 어떤 container가 virtual device를 받는지 결정한다.

## 10.2 확정한 결정과 하나의 남은 API 결정

### Broker와 host placement — 확정

- active meshed Pod마다 broker worker 하나를 둔다.
- broker는 Pod/container가 아닌 host process이며 private PID/mount/network/cgroup
  namespace와 별도 virtual address space를 갖는다.
- node-wide shared broker, broker Pod와 privileged sidecar는 사용하지 않는다.
- host의 영구 process는 `dpumeshd` 하나다. Device Plugin, host evidence,
  worker supervisor와 DPU relay를 같은 process의 명확한 module로 둔다.
- per-worker systemd unit, 외부 `systemd-run`/`unshare`, state-file re-adoption과
  별도 privileged node-agent DaemonSet은 제거한다.

### Kubernetes integration — 확정

- DPU ARM OS를 같은 cluster의 node로 등록하고 runtime을 DaemonSet Pod로 운영한다.
  OS/BFB/driver 준비는 별도이며 host dpumeshd/broker 배치는 유지한다 (§0.4).
- v1.31 기준 `dpumesh.io/channel` Device Plugin 하나만 allocation authority로 쓴다.
- DRA와 CSI를 병행하지 않는다.
- workload image가 library를 포함하고, target container가 channel resource를
  명시한다. Device Plugin은 allocation socket 하나만 주며 `hostPath`, device와 host
  namespace를 주지 않는다.
- mutating webhook, init container와 DPUmesh 전용 RuntimeClass를 사용하지 않는다.
- node 고정 reserve는 최악 조건 broker 비용, delegated worker cgroup은 실제 limit,
  per-Pod metric은 관측을 담당한다.
- controller와 application namespace는 PSS Restricted를 만족한다.
- DPUmesh는 workload용 ServiceAccount/RBAC/token을 추가하지 않는다.

### Local transport loss — 아직 public API 결정 필요

peer/node간 transport loss는 §11.12처럼 explicit QP fatal event다. 같은 QP의
transparent reconstruction, outstanding byte replay, workload process restart는 하지
않는다. broker↔DPU local loss는 current process-termination contract를 유지할지
explicit channel-fatal event로 바꿀지 B1/B5 receipt 뒤 결정한다. 어느 쪽이든 old
mapping/QP를 새 generation 아래 자동 replay하지 않는다.

### Workload identity — 확정

policy identity인 namespace/ServiceAccount와 workload incarnation인 cluster/node/Pod
UID를 구분한다. `grant`는 `daemon_incarnation`, slot/generation과 DPU nonce까지
controller-signed `WorkloadGrant`에 묶고, `direct`는 trusted dpumeshd의 authenticated
REGISTER로 같은 대응을 session/generation으로 유지한다 (§0.4). cross-node certificate는
ServiceAccount principal과 exact Pod incarnation을 함께 인증하며 §11.4~§11.5가
issuance와 rotation을 정한다.

## 10.3 구현 순서와 rollback 경계

각 단계는 독립 test가 통과하기 전 다음 단계로 넘어가지 않는다.

1. current B1/B2/B5/B7, API, webhook mutation과 capacity 값을 baseline receipt로
   남긴다. 이것은 obsolete current runtime을 강화하는 작업이 아니라 비교 기준이다.
2. root-owned package와 `dpumeshd.service` skeleton을 만든다. Device Plugin
   registration/ListAndWatch, stable device id/path, rotating inode와 daemon incarnation을
   hardware 없이 fake-DPU test로 닫는다. 이때 current agent는 아직 traffic owner다.
3. controller node-mTLS endpoint와 `WorkloadGrant` codec/verifier를 추가한다. mTLS
   부재, wrong-node, stale Pod, wrong container/resource, nonce/generation replay를 모두
   negative test로 닫는다.
4. delegated cgroup/namespace/credential/seccomp bootstrap과 per-Pod worker supervisor를
   구현한다. fake broker로 PID/starttime, parent death, cgroup limit과 cleanup을 먼저 검사한 뒤
   실제 DOCA broker를 연결한다.
5. workload image에 library를 포함하고 target container가 channel resource를
   명시하도록 manifest를 전환한다. Restricted dry-run과 resource 누락/중복 container
   negative test를 통과시킨 뒤 mutating webhook을 제거한다.
6. 새 경로의 B1~B7과 native/preload/C++ gRPC conformance를 staging에서 실행한다.
   실제 cluster manifest 적용이나 DPU 배포가 필요해지는 시점에는 먼저 사용자에게
   exact 대상, 명령, rollback을 제시하고 승인을 받는다.
7. target receipt가 모두 PASS한 뒤 current node-agent DaemonSet/RBAC/hostPath,
   webhook/RBAC와 transient-unit 코드를 제거한다. 같은 release에서 channel capacity,
   node reserve와 rollback package를 고정한다.
8. code, test와 배포 상태가 target이 된 뒤 `design/CONTROL.md`, `design/DATA.md`,
   `design/API.md`, `design/GRPC.md`를 현재형 whitepaper로 갱신한다.

rollback은 단계 6까지 current와 target 중 **한 경로 전체**를 선택하는 방식이다.
한 workload가 current agent socket과 Device Plugin socket을 동시에 받거나, 한 node가
두 supervisor로 같은 broker를 관리하는 혼합 모드는 허용하지 않는다.

## 10.4 현재 코드에서 목표 코드로 옮기는 지도

처음 보는 구현자도 current component를 단순 삭제해서 기능을 잃지 않도록 다음
순서로 책임을 옮긴다.

| current 위치 | 현재 책임 | 목표 위치/변경 | 삭제 가능한 시점 |
|---|---|---|---|
| `bench/workload_attest_agent.py` | `SO_PEERCRED`, `/proc`/cgroup evidence | `dpumeshd`의 allocation acceptor | fake peer와 real Pod UID negative test 통과 뒤 |
| 같은 agent | Pod/Service 조회와 authorization | controller `WorkloadGrant` endpoint | node-mTLS와 stale/wrong-node/container test 통과 뒤 |
| 같은 agent | `systemd-run`/`unshare`, cgroup 이동 | `dpumeshd` direct-child/delegated cgroup supervisor | B1/B2/B4/B6 통과 뒤 |
| 같은 agent | state file와 broker re-adoption | 제거; daemon incarnation fail-stop | B4에서 old worker/grant/socket 0 확인 뒤 |
| 같은 agent | topology/feed/Linkerd relay | `dpumeshd` L4 relay; content authority는 controller | existing channel/controller rollout test 통과 뒤 |
| `controller/dpumesh_controller.py` source-IP reporter | node/CIDR로 `/node`, `/workload-scope` caller 추정 | client certificate SAN 기반 node-mTLS endpoint와 signed grant | unauthenticated/source-IP-only request가 거부된 뒤 |
| `controller/dpumesh_webhook.py` | 모든 regular container에 env/mount, 두 `hostPath`, DPU affinity | 제거; workload image와 manifest가 library/resource/config를 명시 | target workload smoke와 resource 누락/중복 negative test 통과 뒤 |
| `src/core` attestation client | `DPUMESH_ATTEST_SOCKET`의 shared agent socket 연결 | allocation이 mount한 `/run/dpumesh/channel.sock` 연결 | native/preload/gRPC IPC conformance 뒤 |
| `src/broker/dmesh_broker.c` bootstrap | launch token, self cgroup migration, agent assertion request | root-only launch socket에서 accepted fd/cgroup fd 수신, parent/cgroup 검증, grant 수신 | fake/real DOCA READY와 teardown 뒤 |
| `bench/k8s/workload-agent.yaml`과 RBAC | privileged node Pod와 API token | 제거 | target deployment B1~B7 PASS 뒤 |
| current workload manifests | library/agent `hostPath`, node affinity | library 포함 image, extended resource, 필요 시 명시적 app config | 같은 workload image의 target smoke 뒤 |
| controller/webhook manifests | shared permissive namespace, root/hostPath | controller만 `dpumesh-system` Restricted로 이동하고 webhook/RBAC 삭제 | PSA enforce dry-run과 controller rollout test 뒤 |
| `bench/system`/deployment scripts | agent가 host runtime을 간접 설치·호출 | versioned `dpumeshd`/broker package와 systemd unit 설치/상태 확인 | rollback package와 uninstall test 뒤 |

이 표의 “삭제 가능한 시점” 전에는 old code를 지우지 않지만 두 runtime을 같은 Pod에
동시에 주입하지 않는다. test fixture는 current 또는 target을 명시적으로 선택한다.

## 10.5 Freeze 산출물

- [x] 목표 component/deployment/address-space diagram (§3.4, §3.6)
- [x] 목표 scheduling/register/teardown sequence (§3.9, §3.11, §3.13)
- [x] authority와 allocation contract (§3.8, §3.12, §10.1)
- [ ] local/remote encryption diagram
- [ ] public C API contract table와 local transport-loss 결정
- [ ] IPC 및 wire ABI table
- [ ] executable failure matrix B1~B7
- [ ] threat model/non-goal negative tests
- [ ] target 배포 receipt 뒤 모든 design 문서의 현재형 정합성
- [ ] contract test mapping

---

# 11. P2: workload mTLS를 포함한 cross-node 전송

상태: **설계 동결 대상**. 아래의 `현재(v1)`은 코드가 이미 제공하는 계약이고,
`목표(v2)`는 구현 전에 contract test로 고정할 계약이다. 배포와 실제 RDMA 장비
검토는 이 단계에서 제외한다. TCP/in-memory carrier로 증명할 수 있는 계약을 먼저
닫는다.

이 절과 기존 설계 문서가 충돌할 때 현재 구현의 설명에는 기존 설계 문서를, v2의
구현 목표에는 이 절을 적용한다. 특히 per-workload certificate를 범위 밖으로 둔
`design/CONTROL.md` 문장은 v1 설명으로만 유효하다. v2 구현 전에는 이 PLAN과
contract test fixture만 동결한다. v2 code, test와 deployment receipt가 생긴 뒤에만
`design/API.md`, `design/CONTROL.md`, `design/DATA.md`를 현재 구현 설명으로 갱신한다.

## 11.1 코드 검토 결과: 현재(v1)에 실제로 있는 것

현재 호출 경로는 다음과 같다.

~~~text
px_resolve_backend / Linkerd-selected Pod UID
  → px_peer_stream_ready
    → dmesh_peer_open                         node별 lazy open
      → dmesh_peer_transport                  TLS 1.3 session
        → peer_wire_tcp | peer_wire_rdma      message carrier
    → dmesh_peer_stream_request               STREAM_OPEN
  → dmesh_peer_stream_data_send               DATA + source custody
  → destination px_peer_{deliver,source_deliver}
    → SG-DMA → destination REV_DONE publish
  → dmesh_peer_{delivered,source_delivered}
    → STREAM_ACK
  → px_peer_release                           source custody/credit 반환
~~~

구현과 유지되는 host test로 확인된 내용:

| 항목 | 현재 계약과 근거 |
|---|---|
| carrier | `peer_wire_ops`: whole-message all-or-nothing send/recv, TCP 구현과 RDMA 구현 |
| node session | `peer_tls`: mutual TLS 1.3, self-signed Ed25519 certificate, peer raw key 추출 |
| node binding | TLS peer key를 signed topology의 node name→DPU key와 비교 |
| sharding | ARM worker마다 peer table과 carrier 하나; worker `w`는 peer base port+`w`에 연결 |
| channel | worker와 remote node당 최대 하나, lazy open, 60초 idle eviction, incarnation fence |
| stream | full-duplex handle 하나, 양쪽 allocator namespace를 owner bit로 분리 |
| routing | exact remote Pod UID와 node를 topology에서 고르고 destination이 placement를 재검증 |
| authorization | destination이 topology의 namespace/ServiceAccount/IP로 inbound verdict 계산 |
| custody | destination host RX mapping에 `REV_DONE`을 publish한 뒤 `STREAM_ACK`; 그 뒤 source credit 반환 |
| bounds | peer별 stream/open-rate/RX staging/TX inflight/extent-slot bound |
| failure | malformed/refused data frame 또는 transport fault는 node channel 전체를 synchronous reset |
| tests | peer channel, TLS tamper/key, TCP roundtrip/backpressure/loss/reuse, full-stack loopback |

현재 보안 모델은 다음과 같다.

~~~text
Pod A ─DMA─ DPU A ═ TLS(node A, node B) ═ DPU B ─DMA─ Pod B
                         │
                         └─ STREAM_OPEN의 Pod UID를 signed topology로 검증
~~~

TLS key와 certificate principal은 node/DPU다. workload identity는 TLS certificate가
아니라 node-authenticated channel 안의 claim과 signed topology lookup으로 성립한다.
따라서 v1은 **node-pair encrypted + topology-authenticated workload claim**이지,
workload/pod-to-pod mTLS가 아니다.

## 11.2 코드 검토에서 확인한 v1의 빈틈

이 목록은 “코드가 없다”와 “코드는 있으나 목표 계약과 다르다”를 구분한다.

| 빈틈 | 현재 동작 | v2에서 필요한 결정 |
|---|---|---|
| workload credential | 없음 | 양쪽 exact Pod incarnation에 묶인 certificate와 private key |
| certificate 선택 | node별 certificate 하나 | destination Pod를 handshake 전에 안전하게 선택하는 bootstrap |
| secure association | node channel 자체 | Pod pair별 workload TLS state와 cache/lifetime |
| wire ABI | C struct를 native byte order/layout으로 `memcpy` | endian·field width가 명시된 encoder/decoder와 golden vector |
| version negotiation | header version mismatch면 channel fault | signed capability와 no-downgrade rollout gate |
| `src_generation` | STREAM_OPEN에 실리지만 consumer가 읽지 않음 | v2 wire에서 제거; topology version은 검증 context로만 사용 |
| refusal detail | `destination_opened()`의 여러 실패가 `no-pod`로 합쳐짐 | identity/authz/resource/protocol reason과 failure scope 분리 |
| reset | STREAM_RESET 메시지가 없음 | stream/association/node reset을 서로 다른 범위로 정의 |
| transport ordering | 실제 TCP/TLS/RC channel은 모든 stream을 직렬화 | 보장 계약은 per-stream order, 현재 cross-stream HOL은 구현 한계로 명시 |
| send 결과 | would-block이어도 frame을 보관하고 success, 다음 call만 inflight | `TAKEN`과 `RETRY`가 custody 소유자를 정확히 표현 |
| reconnect | channel loss가 모든 stream을 poison하고 custody 해제 | 새 association/stream만 허용, 미확인 bytes 자동 replay 금지 |
| public 오류 | peer open/loss가 주로 poison/EOF로 수렴 | graceful EOF와 remote fatal error를 별도 event로 노출 |
| endpoint fan-out | 한 `px_conn`에 remote Pod pin 하나 | destination Pod별 association/stream binding |
| secret refresh | connection마다 새 traffic key지만 장수 channel 갱신 없음 | time/byte KeyUpdate 또는 bounded reconnect 계약 |
| Pod withdrawal | `px_peer_pod_gone()`이 busy writer의 `INFLIGHT`를 무시하고 retry queue가 없음; 남은 handle은 idle eviction도 막음 | association을 Pod UID에 묶고 withdrawal을 pending control queue에서 terminal 전달/reset |
| ACK 검증 | ACK range 안에서 하나 이상 찾으면 성공하여 hole/overlap/exact count를 검증하지 않음 | range 전체가 현재 outstanding set과 정확히 일치해야 retire |
| refusal 계수 | TX bound 초과를 `dmesh_peer_tx_charge()`와 caller가 같은 channel에 두 번 더할 수 있음 | refusal을 한 계층에서 peer/node/metric에 각각 정확히 한 번 기록 |

`make test`는 이 v1 계약에서 통과한다. RDMA wire test는 local RDMA address가 없어
skip된다. 이는 v1의 host contract가 보존됐다는 뜻이지 v2가 구현됐다는 뜻이 아니다.
특히 busy-writer 중 `POD_GONE`, hole/overlap ACK, refusal exact-count arm은 현재 test에
없다. 이 세 항목은 hardware 없이 재현 가능하므로 v2 작업 전에 regression test로
고정하고, v1 path를 유지하는 동안에도 방치하지 않는다.

## 11.3 동결 결정: DPU edge-terminated workload mTLS

2026-09-05 재검토 메모: 이 절은 기존 workload mTLS 설계 기준이다. **RoCEv2 IPsec
packet offload 대안은 §11.19에 별도 기록했으며, 2026-09-07 월요일 미팅에서 채택 여부를
결정한다.** 아직 이 절의 요구사항을 변경하거나 IPsec 구현을 승인한 것은 아니다.

### 왜 workload mTLS를 선택하는가

node TLS는 “어느 DPU와 연결됐는가”를 증명한다. 원격 inbound policy가 판정하는 주체는
node가 아니라 namespace/ServiceAccount와 exact Pod incarnation이다. 현재 v1도 signed
topology와 destination validation으로 일반 Pod UID claim을 제한하므로, node DPU 전체를
신뢰하는 현재 threat model에서 node TLS가 곧 insecure라는 뜻은 아니다. controller가
서명한 short-lived workload capability를 node TLS channel에 강하게 binding하는 방식도
같은 DPU-compromise boundary 안에서는 가능한 대안이다.

따라서 workload mTLS는 link confidentiality의 논리적 필수조건이나 compromised DPU에
대한 방어가 아니다. 이 계획에서 그것을 선택하는 이유는 원격 policy principal, exact
Pod credential, proof of possession, 독립 lifecycle과 traffic key를 **하나의 표준 TLS
association 결과**로 만들겠다는 요구 때문이다. 이 요구가 사라진다면 더 단순한
node-TLS+signed-capability 설계를 다시 비교해야 하며, workload TLS를 관성적으로 유지하지
않는다.

workload mTLS가 추가로 주는 보장은 다음 네 가지다.

1. controller가 승인한 `(cluster, node, Pod UID, namespace, ServiceAccount)`와 private-key
   possession을 association의 traffic key에 묶는다.
2. source와 destination 모두 exact Pod certificate를 검증한 뒤에만 policy와 DATA를
   처리하므로 confused-deputy와 cross-Pod misbinding을 fail-closed로 만든다.
3. Pod recreate, node 이동, credential rotation/withdrawal을 node의 다른 Pod session과
   분리해 끝낼 수 있다.
4. wire/log/metric의 association identity와 원격 policy principal이 같은 인증 결과에서
   나오므로 단순한 node-authenticated metadata claim보다 감사 가능성이 높다.

요구사항이 node 사이 link encryption뿐이라면 현재 node TLS로 충분하다. workload mTLS가
필요한 이유는 encryption 강도를 한 겹 더 올리기 위해서가 아니라 **per-workload 원격
authorization identity를 cryptographic association에 바인딩하기 위해서**다. 이 구조도
한 DPU가 그 node의 workload private key와 plaintext를 모두 다루므로 compromised local
DPU로부터 workload를 보호하지는 않는다. 그 위협은 명시적 non-goal이며, 이를 이유로
TLS나 key ownership을 application에 옮기지 않는다.

### 암호화 경계

application과 broker는 TLS key, TLS record 또는 ciphertext를 만들거나 해석하지 않는다.
암호화 여부는 routing이 destination node를 확정한 뒤 DPU에서만 결정한다.

~~~text
same node
Pod A TX plaintext ─DMA─ DPU ─DMA─ Pod B RX plaintext
                         (crypto 없음)

different nodes
Pod A TX plaintext ─DMA─ source DPU
                         ├─ remote route/policy 및 workload association READY 확인
                         └─ workload TLS encrypt (정확히 1회)
                                  ↓ ciphertext only
                         TCP/RDMA peer carrier
                                  ↓ ciphertext only
                     destination DPU
                         ├─ workload TLS authenticate/decrypt
                         └─ DATA/sequence 검증 ─DMA─ Pod B RX plaintext
~~~

binding 규칙:

1. host TX mapping, forward ring과 source DPU staging까지는 plaintext다. source DPU가
   remote carrier에 넘기기 직전에 workload TLS record를 만들며, `peer_wire_ops`에는
   application plaintext를 절대 넘기지 않는다.
2. destination은 carrier에서 받은 authenticated ciphertext의 tag와 sequence를 검증한
   뒤에만 plaintext를 destination staging/DMA에 공개한다. 인증 실패 데이터는 host RX
   mapping에 한 byte도 쓰지 않는다.
3. same-node path에는 workload TLS를 만들지 않는다. DPUmesh API는 application에
   plaintext byte stream을 주고받게 하며, application-side encryption을 이 보안 요구의
   구현이나 acceptance 증거로 인정하지 않는다.
4. node TLS 1.3은 static node key와 signed topology binding을 사용한 저용량
   **control lane**이다. `NODE_CAPS`, `ASSOC_OPEN`, certificate 선택, workload TLS
   handshake relay, drain/reset만 운반하며 application payload는 운반하지 않는다.
5. workload TLS 1.3은 exact Pod pair의 **data lane**이다. handshake가 `READY`가 된 뒤
   STREAM_OPEN/DATA/ACK/FIN/RESET을 운반한다. handshake record는 Pod metadata를 숨기기
   위해 node control TLS 안에서 relay하지만, READY 뒤 application-data record는 node
   TLS를 다시 통과하지 않고 carrier로 직접 나간다. 따라서 payload 암호화는 한 번이다.
6. carrier 앞의 clear envelope에는 protocol version, node-channel incarnation,
   association id, ciphertext length만 보인다. Pod UID, ServiceAccount, Service key와
   payload는 보이지 않는다. length/timing traffic analysis와 padding은 non-goal이다.
7. v1 plaintext/data frame으로 downgrade하지 않는다. 필요한 capability가 없으면
   `protocol-unsupported`로 거부한다. software crypto fallback은 TLS를 유지하는 것이며
   plaintext fallback이 아니다.
8. TCP와 RDMA는 같은 envelope, control lane, workload TLS 및 stream contract를 쓴다.
   carrier 구현에 identity, policy, certificate 또는 crypto policy를 복제하지 않는다.

정확한 claim은 **DPU edge-terminated, workload-identity mutual TLS**다. “Pod-to-Pod”는
인증 principal과 association scope를 뜻하고 TLS 실행 위치를 뜻하지 않는다.

## 11.4 Workload identity와 certificate 계약

하나의 workload credential은 policy principal과 Pod instance를 함께 인증한다.

| 이름 | 값 | 용도 |
|---|---|---|
| `policy_identity` | `<service-account>.<namespace>.serviceaccount.identity.<trust-domain>` | Linkerd policy principal과 일치 |
| `pod_uid` | Kubernetes immutable Pod UID | recreate를 구분하는 workload incarnation |
| `node_name` | canonical Kubernetes node name | 이 credential을 사용할 수 있는 DPU 범위 |
| `cluster_id` | controller가 서명한 DNS-label cluster identifier | 다른 cluster의 같은 이름 재사용 방지 |
| `credential_serial` | issuer의 nonzero unique serial | rotation과 관측 correlation |

certificate 형식은 다음으로 동결한다.

- leaf key는 DPU 안에서 생성한 ECDSA P-256 key다. private key는 DPU 밖으로 나가지
  않고 workload Pod, broker, `dpumeshd`에도 전달하지 않는다.
- SAN `dNSName`은 `policy_identity` 하나다.
- SAN `uniformResourceIdentifier`는 다음 canonical workload-instance URI 하나다.
  `dpumesh://<cluster-id>/node/<node-name>/pod/<pod-uid>/ns/<namespace>/sa/<service-account>`
  각 component는 Kubernetes/controller가 이미 검증한 canonical DNS label/subdomain 또는
  UUID text이며 percent-encoded alternative를 허용하지 않는다.
- leaf는 위 두 SAN을 정확히 하나씩 가져야 한다. wildcard, 추가 DPUmesh URI, CN fallback은
  허용하지 않는다. 별도 private X.509 OID를 만들지 않는다.
- issuer serial은 positive 63-bit integer로 제한하고 wire에서는 `u64`로 encode한다.
  zero, 음수 또는 63 bit를 넘는 serial의 leaf는 거부한다.
- issuer chain은 cluster workload trust anchor로 검증한다. self-signed workload
  certificate와 node certificate를 workload credential로 받아들이지 않는다.
- EKU는 clientAuth와 serverAuth를 모두 포함한다. source association은 client,
  destination은 server 역할을 하며 reply는 같은 full-duplex association을 쓴다.
- workload data session은 TLS 1.3만, ALPN `dpumesh-workload/2`, 양방향 certificate
  required로 고정한다. TLS 1.2, session ticket/resumption, 0-RTT,
  anonymous/PSK-only mode는 끈다. 초기 cipher suite는 software/hardware backend가
  같은 wire를 만들 수 있는 `TLS_AES_128_GCM_SHA256` 하나로 고정하고 negotiated suite와
  crypto backend를 metric에 남긴다. 다른 TLS 1.3 suite 추가는 capability, 상호운용 및
  known-answer test를 함께 추가하는 protocol 변경이다.
- certificate의 principal/instance URI가 `ASSOC_OPEN`, signed topology 또는 local live
  registration 중 하나와 다르면 association 전체를 거부한다.
- local `dma_generation`은 certificate field가 아니다. DPU 내부의 Pod slot reuse
  fence이며 association이 잡은 slot/generation과 모든 async completion에서 비교한다.

ServiceAccount가 같은 replica도 서로 다른 private key, serial, Pod instance URI를
가진다. 정책상 같은 principal일 수 있지만 cryptographic Pod instance는 같지 않다.

## 11.5 Credential issuance와 rotation 계약

`dpumesh-controller`는 registration authority(RA)다. signing key는 별도 workload
issuer가 보유하며 어떤 DPU나 `dpumeshd`도 CA private key를 갖지 않는다. RA endpoint는
inter-DPU node-control TLS와 분리된 control-plane HTTPS node-mTLS로 인증한다.

~~~text
local Pod READY
  → DPU: P-256 key + CSR 생성
  → WORKLOAD_CSR_V1 {
        request_id, node_name, pod_uid, namespace, service_account,
        cluster_id, nonce, requested_lifetime, CSR DER
    }
  → node credential로 request 서명
  → dpumeshd L4 gateway는 encrypted bytes만 relay
  → controller가 caller node key, live Pod UID/node/SA, topology를 검증
  → issuer가 leaf 발급
  → DPU가 chain/SAN/instance URI/key match를 다시 검증하고 cache publish
~~~

RA protocol은 `POST /v1/workload-certificates`다. HTTPS server identity는 configured
controller name과 별도 control-plane trust anchor로 검증하고, client는 topology에
publish된 node static key의 certificate를 제시한다. `dpumeshd` gateway는 TCP
pass-through만 한다.

request body는 최대 16 KiB의
`application/vnd.dpumesh.workload-csr-v1` binary다. 모든 integer와 string 규칙은
§11.10과 같다.

~~~text
u32 magic='DWC1', u16 version=1, u16 flags=0
bytes16 request_id, u64 issued_at_unix_s, u32 requested_lifetime_s
string cluster_id, string node_name, string pod_uid
string namespace, string service_account
bytes32 nonce
u32 csr_der_len, bytes csr_der
bytes64 node_ed25519_signature       // 앞의 모든 byte에 대한 서명
~~~

success response는 `request_id`, `credential_serial`, `not_before`, `not_after`와
각각 `u32 length + DER`인 leaf/intermediate chain을 돌려준다. refusal response는 같은
`request_id`와 stable issuer reason만 돌려주며 CSR/Pod metadata를 echo하지 않는다.
HTTP/network timeout은 terminal issuance refusal이 아니라 같은 request id로 재시도한다.
body나 chain이 64 KiB를 넘으면 protocol failure다.

권한과 실패 계약:

- RA는 caller node에 현재 배치된 live Pod만 승인한다. 다른 node의 Pod UID 요청은
  `issuer-wrong-node`다.
- issuer reason은 `issuer-wrong-node`, `issuer-pod-not-live`, `issuer-binding`,
  `issuer-replay`, `issuer-bad-csr`, `issuer-rate`, `issuer-unavailable`로 제한한다.
  앞의 다섯 개는 request terminal, `issuer-rate`는 `Retry-After` 뒤 같은 request id로,
  `issuer-unavailable`은 bounded exponential backoff로 재시도한다.
- workload의 projected bearer token을 `dpumeshd`, broker나 DPU에 전달하지 않고,
  `dpumeshd`에는 Kubernetes API bearer credential 자체를 주지 않는다.
- 동일 `request_id` 재전송은 같은 CSR에 같은 terminal result를 돌려주는 idempotent
  operation이다. 다른 CSR에 같은 id를 쓰면 protocol error다.
- CSR private-key proof, node request signature, nonce replay cache, 요청 시각/clock
  skew, 최대 lifetime을 모두 검사한다.
- 기본 leaf lifetime은 1시간, refresh 시작은 lifetime의 70%, 최소 refresh 여유는
  5분, clock skew 허용은 30초다. 값은 config가 아니라 protocol maximum 아래의
  운영 default다.
- 새 association은 가장 최근 valid credential만 사용한다. 기존 association은
  양쪽 credential 중 먼저 만료되는 시각 30초 전까지만 유지할 수 있다.
- refresh 성공 시 구 credential과 새 credential은 최대 5분 겹친다. 그 뒤 새 stream
  open을 막고 기존 association을 bounded drain한 후 reset한다.
- Pod unregister/delete, node 이동, namespace/ServiceAccount 변경은 즉시 credential을
  withdraw하고 관련 association/stream을 reset한 뒤 private key를 cleanse한다.
- issuer가 일시 중단되어도 valid credential의 기존 association은 expiry 전까지
  계속된다. 새 association은 credential 없이 열리지 않으며 plaintext/v1 fallback이 없다.
- CSR/HTTP/issuer I/O는 control thread가 담당한다. data worker는 blocking issuance를
  호출하지 않고 immutable, refcounted credential snapshot을 acquire-load한다. rotation은
  새 snapshot을 publish하고 구 snapshot은 그것을 pin한 association이 끝난 뒤 cleanse한다.

## 11.6 Association과 stream의 단위 및 소유자

association lookup key는 정확히 다음 tuple이다. destination credential serial은
source가 handshake 전에 알 필요가 없으므로 lookup input이 아니다. responder가 선택한
serial은 workload TLS 검증 뒤 association의 authenticated state에 pin한다.

~~~text
(source_pod_uid,
 destination_pod_uid,
 source_credential_serial,
 remote_node_name,
 arm_worker_id)
~~~

계약:

- association은 방향이 있다. source가 handshake initiator이며 새 application stream도
  source만 연다. destination reply는 같은 stream과 같은 full-duplex TLS를 사용한다.
  반대 방향의 새 connection은 반대 방향 association을 쓴다.
- 한 association은 exact Pod pair만 운반한다. ServiceAccount principal만 같다고 다른
  Pod UID의 stream을 합치지 않는다.
- source QP를 소유한 ARM worker가 association과 stream을 소유한다. 현재와 같이
  worker `w`의 node carrier/control channel은 peer worker `w`에 연결한다. object를 worker 사이에서
  직접 호출하지 않는다.
- v2 node pair는 같은 nonzero ARM worker count `A`를 advertise해야 한다. 다르면
  `protocol-unsupported`이고 일부 worker만 연결하지 않는다. 한 node의 carrier/credential
  초기화도 A개 전부 성공한 뒤 한 번에 publish하며 partial coverage는 rollback한다.
- association id는 node-channel incarnation 안에서 initiator가 할당한 nonzero
  64-bit 값이다. 양 node가 동시에 association을 열 수 있으므로 최상위 owner bit를
  canonical node-name order로 나누고, 나머지 63 bit는 해당 owner가 재사용하지 않는다.
  node-channel incarnation이 바뀌면 모두 stale다.
- stream id는 association 안에서 initiator가 할당한 nonzero 64-bit 값이며 재사용하지
  않는다. destination은 `STREAM_OPEN_ACK`에서 같은 id를 echo한다.
- association cache reference를 호출자가 놓아도 live stream, pending callback 또는
  custody가 있으면 파괴하지 않는다. 모두 0이고 idle timeout이 지난 뒤에만 evict한다.
- 같은 source connection이 request별로 다른 remote Pod를 고르면 각 destination Pod의
  association에서 별도 stream을 연다. `px_conn`의 단일 `peer_pod_uid` pin은
  destination-keyed pin map으로 바꾼다.
- 이 fan-out은 protocol-aware L7 connection에만 적용한다. opaque/L4 byte stream은
  첫 backend Pod 하나에 계속 pin되며 중간에 destination을 바꾸지 않는다.

## 11.7 내부 C API와 return/ownership 계약

carrier seam은 v1의 `struct peer_wire_ops`를 유지한다.

| call | contract |
|---|---|
| `connect(ctx, ip, port, &conn)` | 0이면 `conn` ownership을 caller에게 넘김; connection은 아직 pending일 수 있음 |
| `progress(ctx, accepted, max, &n)` | 실제 이동이 있으면 nonzero; pending이라는 이유만으로 nonzero가 아님; accepted connection ownership도 caller로 이동 |
| `send_msg(conn, p, n)` | 1이면 whole message를 carrier가 복사/소유, 0이면 byte 0개를 받았으므로 동일 call 재시도 가능, -1이면 terminal |
| `recv_msg(conn, p, cap)` | 양수면 whole message 하나, 0이면 empty, 음수면 terminal; partial message를 반환하지 않음 |
| `established/faulted` | read-only state; 둘 다 false인 상태는 connect pending |
| `close(conn)` | ownership을 가진 계층이 정확히 한 번 호출하는 terminal close; 뒤에 conn pointer 사용 금지 |
| `epfd(ctx)` | 전체 carrier context용 level-triggered progress hint 하나; -1이면 polling |
| `ctx_free(ctx)` | listener와 남은 connection을 닫고 context 파괴 |

`dmesh_peer_transport_new`가 성공하면 `wire_ctx` ownership을 가져가며 실패하면 caller가
계속 소유한다. transport/channel 어느 계층에서도 `send_msg(1)`을 remote delivery로
해석하지 않는다. 이 seam 아래에는 Kubernetes, Pod, certificate 또는 policy type을
추가하지 않는다.

그 위 table은 현재 v1의 `struct dmesh_peer_transport` 계약이다.

| call | contract |
|---|---|
| `connect(ctx, ip, port, prologue, n, &conn)` | 0이면 runtime이 prologue를 복사하고 conn ownership을 caller에게 넘김; negative errno면 아무 pointer/byte도 보관하지 않음 |
| `peer_key(conn, key)` | 0이면 node TLS와 prologue가 끝나 raw peer key를 반환; 음수는 아직 ready 아님이며 실제 fault는 runtime progress가 channel reset으로 보고 |
| `send(conn, p, n)` | `n`이면 plaintext 전부를 TLS가 복사해 받음, 0이면 아무 byte도 받지 않은 would-block, 음수면 terminal; partial positive는 금지 |
| `recv(conn, p, cap)` | TLS plaintext byte stream의 `1..cap` bytes, 0 empty, negative terminal; v1 frame codec가 reassemble |
| `close(conn)` | channel owner가 정확히 한 번 호출; 모든 TLS/carrier buffer를 cleanse하고 뒤의 pointer 사용 금지 |

v2에서는 이 object가 모든 payload를 감싸는 outer transport가 아니다. carrier-envelope
dispatcher, node-control TLS lane, workload TLS data lane으로 분리한다. 기존
`connect/peer_key`와 node TLS `send/recv` 의미는 control lane에 재사용할 수 있지만
workload STREAM/DATA payload가 이 `send`를 호출하는 것은 금지한다. carrier connection
ownership은 dispatcher 하나가 갖고 두 lane은 ciphertext message만 제출한다.

`peer_key<0`만 반복해서 handshake fault를 pending으로 오해하지 않도록
`dmesh_peer_transport_progress`가 deadline/carrier/TLS fault를 반드시 owner callback으로
올린다. node-control send success도 association이나 stream delivery가 아니다.

아래 이름은 목표 interface다. public API가 아니라 `dpu_proxy`와 peer subsystem 사이의
worker-local contract다. 구현 header와 contract test가 이 의미를 그대로 가져야 한다.

~~~c
enum dmesh_xfer_result {
    DMESH_XFER_TAKEN = 0,       /* 요청과 넘긴 ownership을 subsystem이 보유 */
    DMESH_XFER_PENDING = 1,     /* async setup 중; 같은 요청을 다시 제출하지 않음 */
    DMESH_XFER_RETRY = 2,       /* 아무 ownership도 받지 않음; progress 후 재시도 */
    DMESH_XFER_REFUSED = 3,     /* object-local terminal refusal */
    DMESH_XFER_FAILED = 4,      /* parent transport/association terminal fault */
};

struct dmesh_workload_id {
    char cluster_id[64];
    char pod_uid[64];
    char node_name[256];
    char namespace_name[64];
    char service_account[256];
    /* Remote destination lookup에서는 0일 수 있다. READY association은 양쪽
     * authenticated serial을 별도 state에 반드시 pin한다. */
    uint64_t credential_serial;
};

struct dmesh_local_workload_ref {
    struct dmesh_workload_id id;
    uint32_t pod_slot;
    uint32_t dma_generation;
};

struct dmesh_assoc_key {
    struct dmesh_local_workload_ref source;
    struct dmesh_workload_id destination;
    uint32_t worker_id;
};

struct dmesh_stream_spec {
    uint16_t destination_port;
    char source_service_key[128];
    char destination_service_key[128];
};

enum dmesh_xfer_result
dmesh_assoc_get(struct dmesh_peer_table *, const struct dmesh_assoc_key *,
                uint64_t request_token, struct dmesh_assoc **out,
                enum dmesh_xfer_reason *reason);
void dmesh_assoc_put(struct dmesh_assoc *);

enum dmesh_xfer_result
dmesh_stream_open(struct dmesh_assoc *, uint64_t stream_id,
                  const struct dmesh_stream_spec *, void *stream_cookie,
                  enum dmesh_xfer_reason *reason);

enum dmesh_xfer_result
dmesh_stream_send(struct dmesh_stream *, uint64_t seq,
                  const struct iovec *, size_t iovcnt,
                  uint32_t bytes, void *custody_token,
                  enum dmesh_xfer_reason *reason);

enum dmesh_xfer_result dmesh_stream_finish(struct dmesh_stream *, uint64_t final_seq,
                                           enum dmesh_xfer_reason *reason);
void dmesh_stream_reset(struct dmesh_stream *, enum dmesh_xfer_reason);
void dmesh_stream_rx_delivered(struct dmesh_stream *, uint64_t delivery_token);
void dmesh_stream_rx_failed(struct dmesh_stream *, uint64_t delivery_token,
                            enum dmesh_xfer_reason);
int dmesh_peer_progress(struct dmesh_peer_table *, int budget);
~~~

`enum dmesh_xfer_reason`의 수치도 계약이다. §11.11의 `0x0000..0x03ff` wire status는
같은 수치의 enum 값으로 사용한다. wire로 보내지 않는 local-only 값은
`wrong-worker=0x8001`, `bad-state=0x8002`, `duplicate-token=0x8003`,
`too-large=0x8004`, `bad-argument=0x8005`로 고정한다. 구현 내부 오류를 새 wire
reason으로 암묵 변환하지 않고 `FAILED/bad-state`와 local log로 처리한다.

API 규칙:

1. 모든 call과 callback은 association owner ARM worker에서만 실행한다. API 자체는
   thread-safe가 아니며 외부 lock으로 다른 worker에서 호출하는 것도 금지한다.
2. 모든 call은 synchronous `REFUSED/FAILED`일 때 `reason`을 채운다. 그 밖의 결과는
   `reason=OK`다. `assoc_get(PENDING)`과 `stream_open(PENDING)`의 terminal reason은
   각각 async callback으로만 온다.
3. `assoc_get(TAKEN)`은 READY cache hit이며 `*out`이 non-NULL이다. 새 setup을 받은
   경우 `PENDING`이고 `*out=NULL`이다. `stream_open`은 새 open을 받으면 항상
   `PENDING`이며 같은 `stream_cookie`에 opened/refused callback 중 하나만 온다.
   READY 전에 끝난 association request는 `assoc_refused`만, READY 뒤 끝난 object는
   `assoc_down`만 받는다.
4. `TAKEN`은 would-block 때문에 subsystem이 한 frame을 보관한 경우까지 포함한다.
   caller는 같은 request/data를 재제출하지 않는다.
5. `PENDING`은 `assoc_get`/`stream_open`에만 사용한다. 완료는 callback 하나로 온다.
6. `RETRY`는 subsystem이 pointer, iovec, token 어느 것도 보관하지 않았다는 뜻이다.
7. `stream_send(TAKEN)` 뒤 `custody_released(token, disposition)` callback은 정확히
   한 번 온다. `DELIVERED`는 destination REV_DONE 뒤 ACK를 받은 경우이고 `FAILED`는
   reset/transport loss로 더는 전달을 증명할 수 없는 경우다.
8. `stream_finish`는 앞선 DATA 뒤에 ordered FIN을 소유한다. `FIN_ACK` 전에는 source
   port retirement를 완료하지 않는다.
9. `stream_reset`은 idempotent다. 이후 send/finish는 `REFUSED`이고, outstanding
   custody는 각각 `FAILED`로 반환한다.
10. callback은 오직 `dmesh_peer_progress` 안에서 직렬로 호출한다. 위 API call에
   re-entrant callback을 하지 않는다. callback에서 같은 object를 reset 요청할 수
   있지만 실제 free는 현재 callback batch 뒤로 미룬다.
11. `assoc_put` 뒤 raw pointer 사용은 금지한다. live stream이 association lifetime을
   pin하고, stream close callback 뒤 stream pointer는 무효다.
12. request token, association id, stream id, custody token은 각 namespace에서
    중복될 수 없다. wrap 시 새 object 생성을 거부하고 parent를 재생성한다.
13. `dmesh_peer_progress`는 처리한 event/frame 수, idle이면 0을 반환한다. 개별
    transport fault는 callback으로 보고하고 음수로 전체 worker loop를 끝내지 않는다.
    음수는 invalid table/budget 같은 programmer error에만 사용한다.

callback contract:

~~~c
assoc_ready(request_token, assoc)
assoc_refused(request_token, reason)
assoc_down(assoc, reason)
stream_opened(stream_cookie, stream)
stream_refused(stream_cookie, reason)
stream_rx(stream, seq, bytes, len, delivery_token)
custody_released(custody_token, DELIVERED | FAILED)
peer_fin(stream, final_seq)
local_fin_acked(stream, final_seq)
stream_closed(stream)
stream_down(stream, reason)
~~~

`stream_rx`의 bytes는 callback 동안만 유효하다. callback이 `RX_HELD`를 반환하면
subsystem staging과 `delivery_token`이 유지되고 caller가
`dmesh_stream_rx_delivered(token)` 또는 `dmesh_stream_rx_failed(token)`를 정확히 한 번
호출한다. `RX_DELIVERED`면 callback 복귀 시 ACK 대상이 된다. `RX_RETRY`는 없다.
수신 후 자원이 부족하면 frame을 decrypt하기 전에 association input을 backpressure하고,
이미 받은 bytes를 무제한 별도 queue에 복사하지 않는다.

## 11.8 State machine과 teardown 계약

### Node channel

~~~text
CLOSED → CONNECTING → NODE_TLS → PROLOGUE/CAPS → OPEN
   ▲          └────────────── fault/timeout ───────┘
   └──── idle(no association/custody), rekey, topology withdrawal, fault
~~~

node channel fault는 그 위의 모든 association을 `node-channel-down`으로 끝낸다.
새 connection은 새 incarnation을 갖고 기존 object나 bytes를 이어받지 않는다.

### Workload association

~~~text
NEW → OPEN_SENT → WORKLOAD_TLS → VERIFYING → READY → DRAINING → CLOSED
 │       │            │           │         │
 └────── refusal/timeout/cert/topology/reset ┴──────────────▶ CLOSED
~~~

- `ASSOC_OPEN_ACK`는 handshake를 시작해도 된다는 admission일 뿐 authentication
  성공이 아니다.
- 양쪽 certificate chain, policy identity, Pod extension, topology placement, local
  registration generation이 모두 맞은 뒤에만 `READY`다.
- credential expiry/rotation drain에서는 새 stream을 거부하고 live stream의 bounded
  close만 기다린다. hard expiry, Pod withdrawal, node-channel loss는 즉시 reset한다.

### Application stream

~~~text
OPENING → OPEN ↔ LOCAL_FIN | PEER_FIN → BOTH_FIN → CLOSED
    └──────── protocol/authz/lifecycle fault ─────→ RESET → CLOSED
~~~

- DATA는 방향별 `seq`가 1부터 연속 증가한다. duplicate, gap, wrap은 stream reset이다.
- FIN은 `final_seq`를 포함한다. receiver가 1..final_seq를 모두 destination path에
  넘긴 뒤 EOF를 publish하고 `FIN_ACK`한다.
- 양쪽 FIN과 FIN_ACK, outstanding custody 0이 모두 성립해야 graceful close다.
- RESET은 정상 EOF가 아니다. 아직 전달 증명되지 않은 source custody는 `FAILED`다.
- stream 하나의 unknown id, bad seq, policy withdrawal은 가능하면 그 stream만 reset한다.
  frame boundary나 association TLS integrity를 믿을 수 없으면 association 전체를 reset한다.
- graceful path는 `local_fin_acked`/`peer_fin`을 필요한 방향에 각각 한 번 보낸 뒤
  `stream_closed`로 끝난다. failure path는 `stream_down` 하나로 끝나며
  `stream_closed`나 `peer_fin`을 추가로 보내지 않는다.
- parent fault callback 순서는 (1) 새 call 차단, (2) outstanding custody의 `FAILED`,
  (3) 각 `stream_down`, (4) 마지막 `assoc_down`이다. terminal callback 뒤 그 object에
  callback을 더 보내지 않는다.

## 11.9 Custody, ordering과 backpressure

source custody 전이:

~~~text
CALLER_OWNS
  ├─ stream_send(RETRY/REFUSED/FAILED) ──────────────── caller가 계속 소유
  └─ stream_send(TAKEN) → XFER_OWNS
          ├─ destination SG-DMA + REV_DONE + DELIVERY_ACK → DELIVERED callback
          └─ stream/association/node loss                 → FAILED callback
~~~

반드시 유지할 불변식:

- carrier/TLS write 성공은 delivery가 아니다.
- association/stream이 READY가 되기 전에 application이 post한 bytes는 source의 기존
  QP/staging custody에 그대로 둔다. association subsystem으로 복사하거나 workload record/carrier envelope를
  만들지 않는다. setup timeout이면 QP error를 내고 기존 proxy poison path가 그
  arrival의 custody를 한 번만 해제한다.
- destination은 host RX mapping에 payload와 REV_DONE을 publish하기 전 ACK하지 않는다.
- source는 ACK 전에 L4 arrival custody나 L7 arena chunk를 재사용하지 않는다.
- ACK는 `(association incarnation, stream_id, direction, seq range)`를 모두 확인한다.
  range 일부만 존재하는 ACK, 겹친 ACK, 이미 retire된 ACK는 protocol error다.
- reconnect는 outstanding DATA, OPEN, FIN을 자동 replay하지 않는다. application-level
  protocol이 retry할 수는 있지만 DPUmesh는 exactly-once를 주장하지 않는다.
- stream order만 public guarantee다. 한 workload association의 TLS record queue와
  현재 단일 TCP/RC carrier ordering 때문에 cross-stream head-of-line blocking이 있을
  수 있으며 이는 기능 오류가 아니라 추후 carrier/sharding 최적화 대상이다.

bound는 association, remote peer/worker, local worker global 세 단계에서 동시에 적용한다.

| bound | association | peer/worker | worker global | 초과 동작 |
|---|---:|---:|---:|---|
| live association | — | 256 | 4096 | 새 association refusal |
| concurrent handshake | — | 16 | 64 | retry-after/backpressure |
| association open rate | — | 100/s | 1000/s | rate refusal |
| live stream | 1024 | 4096 | 16384 | stream refusal |
| RX staging bytes | 4 MiB | 16 MiB | 64 MiB | workload data-lane input backpressure, timeout 시 assoc reset |
| TX un-ACKed bytes | 4 MiB | 16 MiB | 64 MiB | `stream_send(RETRY)` |
| TX/RX extent slots | 2048 | 8192 | 32768 | `stream_send(RETRY)` 또는 input backpressure |
| one DATA extent | 64 KiB | 64 KiB | — | malformed/size refusal |
| handshake bytes | 64 KiB | 1 MiB aggregate | 4 MiB aggregate | association reset |
| handshake time | 5 s | — | — | association timeout |
| RX input pause | 5 s | — | — | `staging-limit` association reset |
| oldest un-ACKed DATA | 30 s | — | — | `delivery-timeout` stream reset |
| FIN/FIN_ACK | 5 s | — | — | `fin-timeout` stream reset |
| idle association | 60 s | — | — | stream/custody 0일 때 eviction |

위 값은 public ABI가 아니지만 security/resource contract다. 변경은 양쪽 protocol
capability와 bound test를 함께 바꿔야 하며 “동적으로 무제한”인 설정은 제공하지 않는다.
별도로 한 local source Pod와 한 remote source Pod UID가 node 전체에서 가질 수 있는
합계는 association 256, live stream 4096, un-ACKed bytes 16 MiB다. accounting key는
signed/certificate-bound Pod UID이며 association을 여러 worker/peer에 나눠도 합산한다.
Pod bound 초과는 그 Pod의 새 admission만 거부한다.

## 11.10 Wire v2 계약

v1 struct wire format을 확장하지 않는다. v2는 explicit codec를 쓰고 C struct padding을
전송하지 않는다.

공통 규칙:

- 모든 integer는 unsigned network byte order다. signed enum이나 pointer를 보내지 않는다.
- string은 NUL 없는 `u16 length + canonical ASCII bytes`다. 각 field별 최대 길이를
  decode 전에 검사한다.
- unknown version/type, nonzero reserved/flag, length overflow, trailing bytes는 해당
  framing 계층의 protocol fault다.
- decoder는 header를 전부 받은 뒤 payload length를 검사하고 allocation 전에 maximum을
  적용한다. partial carrier message를 valid frame으로 처리하지 않는다.
- wire reason code는 명시적 숫자 table이며 내부 C enum 값을 그대로 serialize하지 않는다.
- golden byte vector를 x86_64와 aarch64에서 같은 값으로 검사한다.

carrier가 가장 먼저 읽는 clear envelope는 32 bytes다. 이것은 routing용이며 application
plaintext가 아니다.

~~~text
u32 magic = 0x444d5332 ('DMS2')
u16 version = 2
u16 type                         // NODE_TLS_RECORD | WORKLOAD_TLS_RECORD
u32 flags = 0
u32 payload_len
u64 node_channel_incarnation
u64 association_id              // NODE는 0, WORKLOAD는 nonzero
~~~

| code | envelope type | association id | payload |
|---:|---|---|---|
| `0x0001` | `NODE_TLS_RECORD` | `0` | node-control TLS의 complete TLSCiphertext record 하나 |
| `0x0002` | `WORKLOAD_TLS_RECORD` | nonzero | READY workload association의 complete TLSCiphertext record 하나 |

두 payload 모두 최대 16,640 bytes이며 TLS record boundary와 정확히 일치해야 한다.
envelope는 암호화되지 않으므로 Pod UID나 Service key를 넣지 않는다. receiver는
`node_channel_incarnation`과 association lookup을 먼저 확인하고, TLS authentication이
성공한 뒤에만 안쪽 frame을 신뢰한다. association id 변조는 잘못 선택된 traffic key의
tag 검증 실패가 되며 plaintext를 공개하지 않는다. WORKLOAD record를 READY 전 보내거나
NODE record에 nonzero association id를 쓰는 것은 protocol fault다.

node-control TLS plaintext는 다음 32-byte frame header를 쓴다. node TLS가 이 header와
payload를 인증하며, 한 control frame은 여러 node TLS record에 걸칠 수 있다.

~~~text
u32 magic = 0x444d4332 ('DMC2')
u16 version = 2
u16 type
u32 flags = 0
u32 payload_len
u64 node_channel_incarnation
u64 association_id              // NODE_CAPS는 0
~~~

| code | control type | 방향 | payload 의미 |
|---:|---|---|---|
| `0x0001` | `NODE_CAPS` | both | protocol min/max, mandatory feature bits, hard maximums |
| `0x0101` | `ASSOC_OPEN` | source→destination | source/destination Pod tuple, source credential serial, topology version |
| `0x0102` | `ASSOC_OPEN_ACK` | destination→source | accepted 또는 stable reason; certificate 인증 성공을 뜻하지 않음 |
| `0x0103` | `ASSOC_HANDSHAKE_RECORD` | both | workload TLS handshake record 하나; application data 금지 |
| `0x0104` | `ASSOC_DRAIN` | either | new stream 금지, deadline과 reason |
| `0x0105` | `ASSOC_RESET` | either | association terminal reason |

control payload field order:

~~~text
NODE_CAPS       u16 min_version, u16 max_version, u64 feature_bits,
                u32 max_control_payload, u32 max_associations,
                u32 max_handshakes, u16 arm_worker_count, u16 reserved
ASSOC_OPEN      workload_id source, workload_id destination,
                u64 source_credential_serial, u64 topology_version,
                u64 open_nonce
ASSOC_OPEN_ACK  u16 status, u16 reserved, u32 max_workload_plaintext,
                u64 destination_credential_serial, u64 open_nonce
ASSOC_HANDSHAKE_RECORD
                raw complete workload-TLS handshake record
ASSOC_DRAIN     u16 reason, u16 reserved, u32 drain_timeout_ms
ASSOC_RESET     u16 reason, u16 reserved, u32 reserved
~~~

`workload_id`는 §11.4의 `cluster_id`, `pod_uid`, `node_name`, namespace,
ServiceAccount를 그 순서의 length-prefixed string으로 encode한다. `ASSOC_OPEN`에는
destination credential serial을 넣지 않는다. responder가 ACK에 선택 serial을 쓰고
실제 workload TLS leaf가 같은 serial/Pod binding인지 initiator가 검증한다. control
plaintext frame maximum은 64 KiB다. handshake record는 최대 16,640 bytes이고 하나의
`ASSOC_HANDSHAKE_RECORD`에 정확히 하나만 넣는다. workload TLS가 READY가 된 뒤 나오는
application-data record는 control frame으로 감싸지 않고 `WORKLOAD_TLS_RECORD` envelope로
직접 보낸다.

workload-TLS plaintext frame header는 24 bytes다.

~~~text
u16 version = 2
u16 type
u32 flags = 0
u32 payload_len
u32 reserved = 0
u64 stream_id                  // batch/control은 0
~~~

| code | workload type | 핵심 field | 처리 시점 |
|---:|---|---|---|
| `0x0001` | `STREAM_OPEN` | destination port, source/destination Service key | policy와 local registration 확인 |
| `0x0002` | `STREAM_OPEN_ACK` | stream id, status | success 뒤에만 DATA 가능 |
| `0x0003` | `STREAM_DATA` | direction-local `u64 seq`, bytes | ordered, staging bound 안에서만 accept |
| `0x0004` | `DELIVERY_ACK` | `(stream_id, seq_first, seq_count)` batch | REV_DONE 뒤 source custody 반환 |
| `0x0005` | `STREAM_FIN` | `final_seq` | 앞선 DATA 뒤 EOF publish |
| `0x0006` | `STREAM_FIN_ACK` | `final_seq` | FIN custody와 source port retirement 허용 |
| `0x0007` | `STREAM_RESET` | stable reason | stream-local terminal failure |

workload plaintext payload field order:

~~~text
STREAM_OPEN      string source_service_key, string destination_service_key,
                 u16 destination_port, u16 reserved, u64 topology_version
STREAM_OPEN_ACK  u16 status, u16 reserved, u32 reserved
STREAM_DATA      u64 seq, raw bytes[1..65536]
DELIVERY_ACK     u16 entry_count, u16 reserved, u32 reserved,
                 repeated { u64 stream_id, u64 seq_first,
                            u32 seq_count, u32 reserved }
STREAM_FIN       u64 final_seq
STREAM_FIN_ACK   u64 final_seq
STREAM_RESET     u16 reason, u16 reserved, u32 reserved
~~~

`DELIVERY_ACK` header의 `stream_id`는 0이고 최대 entry count는 64다. 나머지 stream
frame은 header의 nonzero `stream_id`를 사용한다. `STREAM_OPEN`의 topology version도
peer hint일 뿐이며 destination은 자기 held generation으로 두 Service와 Pod binding을
검증한다.

node TLS handshake가 끝난 뒤 첫 authenticated control frame은 양쪽 모두 `NODE_CAPS`다.
CAPS 교환 전 association control 또는 `WORKLOAD_TLS_RECORD`를 보내지 않는다. topology의
signed node record에도 peer protocol min/max를 추가한다. mandatory feature 교집합이
없으면 node control lane은 열 수 있어도 workload association은 열지 않는다.
workload-mTLS-required Service에는 v1 fallback이 없다.

## 11.11 Refusal reason과 failure scope

reason은 wire, metric, log에서 같은 stable kebab-case 이름을 쓴다. 민감한 certificate
내용이나 Pod metadata는 wire reason에 넣지 않는다.

| scope | 대표 reason | 끝내는 범위 |
|---|---|---|
| node | `node-unbound`, `node-key`, `protocol-unsupported`, `node-tls`, `carrier-fault` | node channel과 모든 association |
| association | `credential-missing`, `cert-expired`, `cert-chain`, `identity-mismatch`, `pod-binding`, `topology-stale`, `assoc-rate`, `assoc-limit`, `handshake-timeout`, `assoc-protocol`, `staging-limit`, `crypto-unavailable` | association과 그 stream |
| stream open | `no-pod`, `port-mismatch`, `service-mismatch`, `policy-denied`, `no-policy`, `stream-limit` | 요청 stream만 refusal |
| stream live | `bad-sequence`, `unknown-stream`, `delivery-failed`, `peer-reset`, `fin-timeout` | 해당 stream reset |
| local API | `wrong-worker`, `bad-state`, `duplicate-token`, `too-large` | call refusal; wire에 내보내지 않음 |

wire status code는 다음 값으로 고정한다.

| code | name | scope |
|---:|---|---|
| `0x0000` | `ok` | none |
| `0x0001` | `node-unbound` | node |
| `0x0002` | `node-key` | node |
| `0x0003` | `protocol-unsupported` | node |
| `0x0004` | `node-tls` | node |
| `0x0005` | `carrier-fault` | node |
| `0x0006` | `node-protocol` | node |
| `0x0101` | `credential-missing` | association |
| `0x0102` | `cert-expired` | association |
| `0x0103` | `cert-chain` | association |
| `0x0104` | `identity-mismatch` | association |
| `0x0105` | `pod-binding` | association |
| `0x0106` | `topology-stale` | association |
| `0x0107` | `assoc-rate` | association |
| `0x0108` | `assoc-limit` | association |
| `0x0109` | `handshake-timeout` | association |
| `0x010a` | `assoc-protocol` | association |
| `0x010b` | `staging-limit` | association |
| `0x010c` | `crypto-unavailable` | association |
| `0x0201` | `no-pod` | stream open |
| `0x0202` | `port-mismatch` | stream open |
| `0x0203` | `service-mismatch` | stream open |
| `0x0204` | `policy-denied` | stream open |
| `0x0205` | `no-policy` | stream open |
| `0x0206` | `stream-limit` | stream open |
| `0x0301` | `bad-sequence` | stream live |
| `0x0302` | `unknown-stream` | stream live |
| `0x0303` | `delivery-failed` | stream live |
| `0x0304` | `peer-reset` | stream live |
| `0x0305` | `fin-timeout` | stream live |
| `0x0306` | `delivery-timeout` | stream live |

unknown nonzero status는 그것을 받은 framing 계층의 protocol fault다. local-only
programmer error는 이 table에 넣지 않으며 `enum dmesh_xfer_reason`의 별도 범위로 둔다.

scope 결정 규칙:

- carrier envelope 또는 node-control TLS integrity를 잃으면 node scope다.
- workload TLS integrity 또는 association identity를 잃으면 association scope다.
- 인증된 inner frame에서 하나의 stream만 특정할 수 있는 오류는 stream scope다.
- resource exhaustion은 다른 peer/association을 reset하지 않고 새 admission이나 input을
  backpressure한다.
- topology skew로 인한 한 stream refusal은 node를 악성으로 간주하지 않는다. 반복은
  rate limit과 metric으로 다루고 node eviction은 controller만 결정한다.

## 11.12 Public native API mapping

local/remote 선택은 application에 투명하지만 fatal과 EOF는 같지 않다. v2 구현 시
기존 event layout을 바꾸지 않고 enum과 query symbol을 additive로 추가한다.

~~~c
DMESH_EVENT_QP_ERROR = 6

typedef struct dmesh_qp_error {
    uint32_t version;
    uint32_t size;
    int32_t  sys_errno;
    uint16_t scope;       /* LOCAL, NODE, ASSOCIATION, STREAM */
    uint16_t reason;      /* stable public class; wire detail과 1:1일 필요 없음 */
    uint8_t  retryable;   /* 새 QP를 만들면 재시도 가능한가; 자동 retry라는 뜻 아님 */
    uint8_t  reserved[7];
} dmesh_qp_error_t;

int dmesh_qp_last_error(dmesh_qp_t *, dmesh_qp_error_t *);
~~~

caller는 query 전에 `version=1`, `size=sizeof(dmesh_qp_error_t)`를 설정한다. sticky
error가 있으면 함수가 나머지 field를 채우고 `0`을 반환한다. 아직 error가 없으면
`ENODATA`, NULL 인자나 지원하지 않는 version/작은 size면 `EINVAL`을 직접 반환한다.
다른 public API와 마찬가지로 `-1/errno` 방식이 아니며, destroy된 QP pointer에는
호출할 수 없다. 더 큰 `size`는 version 1 prefix만 채우고 성공해 향후 확장을 허용한다.

관측 계약:

- remote association/stream open은 QP creation 뒤 비동기로 실패할 수 있다. 이미 성공한
  `dmesh_post_send`가 ownership을 넘긴 뒤 실패하면 그 QP에 sticky
  `DMESH_EVENT_QP_ERROR`가 정확히 한 번 발생한다.
- graceful peer FIN만 `DMESH_EVENT_RECV_FIN`이다. RESET, TLS, transport, policy/open
  failure를 FIN으로 위장하지 않는다.
- `QP_ERROR` 뒤 RX buffer release와 object destruction은 여전히 가능하고, 새 TX는
  실패한다. 같은 QP를 자동 reconnect하지 않는다.
- `dmesh_qp_last_error`는 event를 받기 전후 모두 sticky snapshot을 돌려주며 QP destroy
  뒤에는 호출할 수 없다.
- public errno mapping은 `policy-denied→EACCES`, credential/key/cert mismatch→
  `EKEYREJECTED`, topology/no destination→`EHOSTUNREACH`, protocol→`EPROTO`, timeout→
  `ETIMEDOUT`, carrier/peer reset→`ECONNRESET`, `crypto-unavailable→EOPNOTSUPP`,
  local resource terminal failure→`ENOBUFS`다.
  내부 backpressure는 terminal error가 아니며 기존 `EAGAIN/TX_READY`를 쓴다.
- 기존 `DMESH_EVENT_TX_ERROR`는 local descriptor/tail submission failure 의미를 유지한다.
  remote connection fatal은 `QP_ERROR`다.
- local deferred TX fault와 remote fatal이 경합하면 먼저 latch된 terminal 원인 하나만
  이기며 `TX_ERROR` 또는 `QP_ERROR` 중 event 하나만 발생한다. 뒤 원인은 counter/log에는
  남길 수 있지만 sticky public error와 errno를 덮어쓰지 않는다.
- preload는 `QP_ERROR`를 sticky `SO_ERROR`와 blocked read/write의 errno로 변환한다.
  gRPC adapter는 policy denial을 `PERMISSION_DENIED`, 그 밖의 remote transport/
  topology/credential failure를 `UNAVAILABLE`로 끝낸다. 어느 adapter도 같은 stream을
  replay하지 않는다.

이 추가는 struct layout을 바꾸지 않는 ABI-compatible extension 후보지만, unknown event
type을 무시하는 기존 consumer의 동작을 contract test로 확인한 뒤 minor API version을
올린다. 기존 event의 의미를 바꾸는 방식으로 구현하지 않는다.

## 11.13 Topology, policy와 lifecycle 변화

| 변화 | 새 admission | 기존 association/stream |
|---|---|---|
| source/destination Pod delete 또는 node 이동 | 즉시 거부 | association 즉시 reset |
| Pod의 namespace/ServiceAccount 불일치 | credential 발급 거부 | identity mismatch로 reset |
| endpoint set에서 destination withdrawal | 새 stream 거부 | 이미 열린 stream은 기본 drain; Pod delete면 reset |
| inbound policy 변경 | 새 stream에 새 verdict | 이미 열린 stream 유지라는 현재 정책을 보존 |
| node key rotation/withdrawal | node channel 거부 | node channel reset |
| workload cert refresh | 최신 serial만 새 association | overlap 동안 유지, deadline 뒤 drain/reset |
| node-control/workload-data traffic secret boundary | 새 key 사용 | §11.14의 KeyUpdate boundary에서 갱신 또는 reset |
| carrier loss | 새 node channel 필요 | 모든 association/stream `FAILED`, replay 없음 |

association이 잡은 topology version은 identity 결정의 근거를 관측하기 위한 값이지
peer가 보낸 version을 신뢰하는 hint가 아니다. 각 DPU는 자기가 검증·adopt한 generation만
사용한다.

## 11.14 Traffic-secret refresh

node-control TLS와 workload-data TLS 모두 다음 initial policy를 쓴다. application
payload를 운반하는 것은 workload-data TLS뿐이다.

- 한 방향 ciphertext 1 GiB 또는 30분 중 먼저 도달한 때 TLS 1.3 KeyUpdate를 요청한다.
- KeyUpdate가 5초 안에 완료되지 않으면 새 stream admission을 멈추고 association을
  reset한다. node-control update 실패는 node-channel scope다.
- certificate rotation은 KeyUpdate가 아니다. certificate principal/serial이 바뀌면
  새 association을 만들고 구 association을 drain한다.
- key update 전후의 DATA/ACK sequence와 custody는 이어진다. TLS connection 자체가
  끊긴 경우에는 이어지지 않는다.
- counter는 `peer_tls_key_updates_total{layer,result}`와
  `peer_tls_ciphertext_bytes_total{layer,direction}`로 노출하고 `layer`는
  `node_control` 또는 `workload_data`다.

OpenSSL이 자동으로 갱신할 것이라고 가정하지 않는다. byte/time boundary와 failure를
fake clock 및 recording carrier test로 직접 구동한다.

## 11.15 Crypto backend와 DPU hardware acceleration

### 현재 코드와 사용 가능한 seam

현재 `peer_tls.c`는 OpenSSL `SSL`과 memory BIO로 handshake와 record crypto를 모두
software에서 수행한다. build도 `libssl/libcrypto`만 연결하며 `doca-aes-gcm`을 사용하지
않는다. 설치된 DOCA 3.1 SDK에는 async AES-GCM encrypt/decrypt task, AES-128/256 key,
IV/tag/buffer/list/task-count capability query가 있다. 이것은 **사용 가능한 후보 API**라는
뜻일 뿐 실제 BlueField device에서 task가 지원되거나 TLS와 연동됐다는 receipt는 아니다.
현재 SDK header에서 이 API는 `DOCA_EXPERIMENTAL`이므로 version compatibility도 build와
startup capability gate에서 확인한다.

가속 목표는 application이나 broker에 crypto를 옮기는 것이 아니다. X.509 parsing,
certificate verify, ECDHE/HKDF, transcript와 KeyUpdate는 검증된 TLS 1.3 library가 계속
소유하고, 대역폭을 지배하는 workload-data record의 AES-128-GCM seal/open을 DPU crypto
engine으로 보낼 수 있게 한다. node-control TLS는 저용량이므로 initial implementation에서
software여도 된다.

TLS library가 traffic secret, nonce, record sequence와 AAD를 올바르게 소유한 채 DOCA
AES-GCM을 호출할 수 있는 supported integration이 먼저 증명돼야 한다. OpenSSL provider,
library record-offload hook 또는 동등하게 review 가능한 integration만 허용한다. 가속을
위해 key-log callback으로 secret을 빼거나 TLS key schedule/record protocol을 자체 구현하지
않는다. supported integration이 없으면 software backend가 correctness 기준이고 hardware
backend 상태는 OPEN으로 남긴다.

### 내부 backend 계약

association/stream은 구체적인 OpenSSL/DOCA type을 보지 않고 다음 worker-local seam만
사용한다.

~~~c
enum dmesh_crypto_submit {
    DMESH_CRYPTO_TAKEN = 0,    /* input/cookie ownership을 backend가 가져감 */
    DMESH_CRYPTO_RETRY = 1,    /* 아무 ownership도 가져가지 않음 */
    DMESH_CRYPTO_FAILED = 2,   /* connection/backend terminal */
};

struct dmesh_workload_tls_ops {
    int  (*conn_new)(void *ctx, const struct dmesh_tls_credential *, int role,
                     void *conn_cookie, struct dmesh_workload_tls **out);
    enum dmesh_crypto_submit (*handshake_record_in)(struct dmesh_workload_tls *,
                                                    const void *, uint32_t);
    enum dmesh_crypto_submit (*seal)(struct dmesh_workload_tls *,
                                     const struct iovec *, size_t, void *record_cookie);
    enum dmesh_crypto_submit (*open)(struct dmesh_workload_tls *,
                                     const void *, uint32_t, void *record_cookie);
    int  (*key_update)(struct dmesh_workload_tls *);
    int  (*progress)(void *ctx, int budget);
    void (*close)(struct dmesh_workload_tls *);
};
~~~

handshake output, `sealed(record_cookie, tls_record)`,
`opened(record_cookie, authenticated_plaintext)`, `crypto_failed(cookie, reason)`은 owner
worker의 `progress` 안에서만 callback되고 API call에 reentrant하지 않는다. output TLS
record는 §11.10의 complete-record bound를 만족한다. initiator `conn_new`는 첫 handshake
output을 schedule하고 responder는 첫 `handshake_record_in` 전에는 output하지 않는다.
`seal` input 합계는 최대 16,384 bytes이고 성공 callback 하나가 complete TLS record 하나를
돌려준다. 더 큰 workload frame은 codec이 같은 custody를 refcount한 ordered plaintext
chunk로 나누고 receiver codec이 인증된 record plaintext를 다시 조립한다.

ownership과 security 규칙:

1. `seal(TAKEN)` 뒤 source plaintext, iovec와 custody는 `sealed` 또는 `crypto_failed`까지
   유효하다. `sealed`가 준 ciphertext를 carrier가 소유한 뒤에도 application custody는
   destination `DELIVERY_ACK`까지 유지된다.
2. `open(TAKEN)` 뒤 ciphertext buffer는 terminal callback까지 backend 소유다.
   `opened`는 AEAD tag, TLS record sequence와 content type이 모두 검증된 plaintext만 준다.
   tag 실패 시 `opened`는 절대 오지 않고 association이 `assoc-protocol`로 reset된다.
3. `RETRY`는 input/cookie를 보관하지 않는다. crypto task slot이 돌아오면 worker progress
   hint를 발생시키며 caller가 같은 operation을 재시도한다.
4. backend는 connection별 record order와 KeyUpdate barrier를 보존한다. 같은 connection의
   record `N+1`을 `N`보다 먼저 carrier/DMA에 publish하지 않는다.
5. key object는 association과 KeyUpdate epoch에 속한다. 새 epoch가 READY가 되기 전 구
   key를 파괴하지 않고, barrier 뒤 raw staging과 구 key를 cleanse/destroy한다.
6. same-node stream은 이 interface를 호출하지 않는다. remote stream도 association이
   READY가 되기 전 application plaintext로 `seal`을 호출하지 않는다.

crypto inflight bound도 custody bound에 포함한다.

| bound | association | peer/worker | worker global | 초과 동작 |
|---|---:|---:|---:|---|
| encrypt+decrypt tasks | 32 | 128 | 256 | TX `RETRY`, RX input pause |
| plaintext waiting for seal | 기존 TX un-ACKed bound에 포함 | 기존 bound에 포함 | 기존 bound에 포함 | 별도 unbounded queue 금지 |
| ciphertext waiting for open/carrier | 기존 RX/TX staging bound에 포함 | 기존 bound에 포함 | 기존 bound에 포함 | pause 또는 timeout/reset |

DOCA backend는 worker마다 독립 AES-GCM context/progress ownership을 갖고 시작할 때
encrypt/decrypt, AES-128 key, 최소 12-byte IV, 16-byte tag, max buffer/list와 max task를
query한다. 실제 task slot은 `min(device maximum, 위 worker bound)`다. DPU-local plaintext
arena와 ciphertext arena를 `doca_buf`로 등록해 사용하며, list capability가 부족한 경우에도
bounded coalesce만 허용한다. host/application memory를 crypto device에 직접 등록하거나
application thread를 completion poller로 만들지 않는다.

local DPU config key `workload_crypto_backend`은 `software`, `auto`,
`required-hardware` 세 값이며 default는 `auto`다. wire/topology가 backend를 지정하지
않고 local operator와 startup capability probe만 선택한다.

- `software`: OpenSSL correctness backend만 사용한다.
- `auto`: startup capability/integration 검증이 실패하면 software TLS를 사용하고
  `crypto_backend_fallback_total{reason}`을 올린다. TLS와 workload identity는 그대로다.
- `required-hardware`: remote association admission을 `crypto-unavailable`로 거부한다.
  same-node plaintext DMA는 영향받지 않는다.
- 한 record/task의 terminal fault면 그 association을 reset한다. device/context fault면
  해당 worker에서 그 hardware context를 쓰는 association 전부를 reset한다. 같은
  association에서 software로 바꿔 record sequence/key ownership을 이어가지 않는다.
  `auto`는 backend를 degraded로 표시한 뒤 **새 association만** software로 열 수 있다.
- 어느 mode도 plaintext 또는 v1 data frame으로 fallback하지 않는다.

software↔hardware peer는 동일한 TLS 1.3 wire이므로 서로 연결돼야 하며 backend 종류를
wire capability로 협상하지 않는다. metric은
`workload_crypto_records_total{backend,direction,result}`,
`workload_crypto_bytes_total{backend,direction}`,
`workload_crypto_inflight{worker,backend}`와 fallback counter를 제공한다. DOCA task 제출,
completion, tag failure, task exhaustion과 runtime fault의 실제 hardware 판정은 §11.18로
미루되 이 seam과 mock async backend test는 hardware 없이 먼저 닫는다.

## 11.16 Hardware 없이 먼저 닫을 contract test

새 test는 carrier→node session→association→stream→proxy adapter 순서로 쌓는다.

- [ ] v1 현재 test를 변경 없이 유지하고 v2 codec golden vector를 추가한다.
- [ ] v1 busy-writer `POD_GONE` retry, exact ACK range, refusal counter 1회 계수 회귀 시험.
- [ ] x86_64/aarch64 동일 byte order, max/min length, reserved, overflow, trailing byte.
- [ ] node-control key/name/capability mismatch와 v1 downgrade refusal.
- [ ] workload certificate 양방향 handshake와 exact Pod instance URI match.
- [ ] wrong source/destination Pod, wrong node, wrong SA, wrong cluster, expired/not-yet-valid cert.
- [ ] issuance idempotency, nonce replay, wrong-node CSR, rotation overlap과 withdrawal.
- [ ] association/stream state transition, duplicate id/token, callback non-reentrancy.
- [ ] `TAKEN/RETRY`별 pointer/token ownership과 custody callback exactly once.
- [ ] DATA order, duplicate/gap/wrap, ACK partial/overlap/unknown range, FIN/FIN_ACK ordering.
- [ ] destination REV_DONE 전에는 ACK와 source credit이 발생하지 않음.
- [ ] per-association/per-peer byte·slot·rate bound와 한 stalled peer의 격리.
- [ ] bit flip, truncation, replay를 node-control/workload-data TLS 각각에 주입하고 plaintext delivery 0 확인.
- [ ] node/association/stream 각 scope fault가 정해진 object만 끝내는지 확인.
- [ ] carrier loss 후 새 incarnation/association만 열리며 outstanding byte replay가 0인지 확인.
- [ ] destination별 pin map으로 한 HTTP/2/gRPC connection이 두 remote Pod를 안전하게 선택.
- [ ] `QP_ERROR`, preload `SO_ERROR`, gRPC status mapping contract.
- [ ] 16 KiB 이하 한 extent marker에 대해 same-node는 TLS backend call 0회, remote는
  workload `seal` 1회이고 node-control plaintext 및 carrier payload에는 marker가 한 번도
  나타나지 않음.
- [ ] workload handshake READY 전 `seal` 0회, decrypt tag verify 전 destination DMA 0 byte.
- [ ] software↔software와 software↔mock-hardware interop의 frame/custody 결과가 동일함.
- [ ] async crypto `TAKEN/RETRY`, out-of-order completion barrier, task bound와 runtime fault.
- [ ] time/byte KeyUpdate와 timeout boundary.

host-only 완료 gate:

- [ ] 모든 wire/API field, owner, return, callback, state, timeout, bound가 header와 test에 있다.
- [ ] current v1 test와 전체 `make test`가 통과한다.
- [ ] workload identity/crypto negative arm 각각 destination application delivery가 0이다.
- [ ] fault 뒤 custody token이 누락·중복 없이 전부 terminal callback을 받는다.
- [ ] plaintext와 v1 fallback이 없다는 recording-carrier receipt가 있다.

## 11.17 구현 순서

1. v1 busy `POD_GONE`, ACK exactness, refusal count regression을 추가하고 작은 fix로 닫는다.
2. v2 reason table, codec와 golden vector만 구현한다. proxy path는 바꾸지 않는다.
3. 현재 `peer_transport`를 carrier dispatcher와 node-control TLS lane으로 분리하고 v1 regression을 유지한다.
4. workload credential cache와 RA/issuer mock contract를 구현한다.
5. `dmesh_workload_tls_ops`와 software reference backend를 구현한다.
6. association state machine과 workload TLS를 in-memory carrier에서 구현한다.
7. stream v2, FIN/RESET, exact ACK/custody를 recording proxy callback에 연결한다.
8. `dpu_proxy`의 단일 pin을 destination-keyed map으로 바꾸고 L4/L7 remote path를 연결한다.
9. public `QP_ERROR`, preload, gRPC mapping을 추가한다.
10. TCP full-stack fault/rotation/churn test를 닫는다.
11. mock async backend로 task/custody contract를 닫은 뒤 supported DOCA AES-GCM
    integration을 구현한다.
12. 같은 upper contract에 RDMA carrier를 연결한다. 실제 배포와 hardware receipt는 별도다.

각 단계는 앞 단계 test를 유지한다. v1 구조를 한 번에 지우지 않고, v2 TCP fixture가
동일 payload/custody/lifecycle test를 통과한 뒤에만 proxy default를 v2로 바꾼다.

## 11.18 배포·hardware gate — 현재 작업에서는 pass

이 절은 software contract가 닫힌 뒤의 순서만 보존한다. 이번 코드/문서 검토에서는
실행하거나 성공으로 표시하지 않는다. 상세 장비 절차는 별도 로컬 운영 문서가
소유한다.

1. rapids4 관리 주소를 persistent configuration과 일치시킨다.
2. node B의 SSH/sudo/BlueField ownership을 확보한다.
3. BFB/DOCA/firmware, cable, private address, RoCE GID, ping, `rping`, `ib_send_bw`를
   양방향 확인한다.
4. Kubernetes join, host package, Device Plugin registration, node 고정 reserve,
   명시적인 workload resource, Restricted Pod admission과 node-local smoke를 닫는다.
5. canonical node map, 서로 다른 nonzero node key, protocol capability를 publish한다.
6. 두 DPU 사이 TCP로 node auth, workload mTLS, stream, refusal, loss/rotation을 먼저
   receipt한다.
7. 각 DPU에서 DOCA AES-GCM encrypt/decrypt/AES-128/12-byte-IV/16-byte-tag/task/list
   capability를 기록하고 `auto`가 실제 `doca` backend를 선택했는지 확인한다.
8. software↔DOCA와 DOCA↔DOCA workload TLS interop, tag fault, task exhaustion,
   KeyUpdate, context fault와 key destruction을 실행한다.
9. 동일 upper test를 RDMA carrier로 실행한다.
10. remote native/gRPC, bidirectional traffic, endpoint fan-out, Pod churn, peer restart,
   remote inbound policy, no-plaintext/no-fallback과 성능을 receipt한다.

최종 완료:

- [ ] 두 DPU가 expected node뿐 아니라 exact source/destination Pod incarnation과 policy
  principal을 상호 인증한다.
- [ ] remote application stream이 실제 bytes를 운반한다.
- [ ] destination REV_DONE 뒤 ACK를 받은 경우에만 source custody가 `DELIVERED`다.
- [ ] lifecycle/policy/crypto fault가 정해진 scope에서 fail-closed다.
- [ ] reconnect가 old stream/bytes를 replay하지 않는다.
- [ ] 지원되는 BlueField에서는 workload-data AES-GCM record가 DOCA backend counter로
  관측되고 application/broker crypto call은 0이며 software backend와 wire interop한다.
- [ ] TCP host receipt와 raw two-node RDMA receipt가 모두 보존된다.

## 11.19 미결정 대안: inter-node RoCEv2 + IPsec inline hardware offload

상태: **DISCUSSION ONLY / 구현·배포 미착수**. 2026-09-05 논의 내용을 보존한다.
2026-09-07 월요일 미팅에서 요구사항과 feasibility 확인 순서를 결정한다. 이 절은
§11.3~§11.18을 대체하는 확정 설계가 아니며, 현재 TLS를 끄라는 작업 지시도 아니다.
이번 작업에서는 문서만 기록하고 종료한다.

### 11.19.1 원하는 경로와 세 가지 방식의 차이

목표는 같은 node 내부 전송에는 추가 암호화를 넣지 않고, **node 밖으로 나가는 peer
traffic만 DPU/NIC의 전용 hardware에서 암호화해 RDMA로 전송**하는 것이다. 여기서
검토하는 IPsec 대상은 Ethernet/IP 기반 **RoCEv2**다. native InfiniBand나 RoCEv1에
같은 설정이 적용된다고 가정하지 않는다.

| 방식 | 인증·암호화 단위 | 암호화 위치 | 현재 상태 / 한계 |
|---|---|---|---|
| 현재 v1 node TLS + RDMA | node TLS session | DPU ARM OpenSSL, ciphertext를 verbs로 전송 | 구현됨. 전용 inline crypto offload가 아님 |
| 기존 v2 workload mTLS + DOCA AES-GCM 후보 | Pod credential별 TLS association | TLS record buffer를 async crypto에 제출한 뒤 RDMA 송신 | §11.15의 미구현 integration. buffer 기반 lookaside이지 packet inline이 아님 |
| 이번 IPsec 후보 | DPU/node 사이 IPsec SA와 packet | NIC packet pipeline에서 ESP 암·복호화 | RoCEv2 full offload 지원 근거는 있음. 우리 ARM-origin QP 경로는 미검증 |

IPsec 후보의 개념 경로:

```text
Pod A → 기존 node-local DMA/처리 → DPU A의 등록된 송신 buffer
     → NIC DMA read → NIC ESP 암호화 → 암호화된 RoCEv2 traffic
     → NIC ESP 검증·복호화 → DPU B receive buffer → 기존 DMA/처리 → Pod B
```

이는 암호화를 위해 CPU가 별도 ciphertext buffer를 만들 필요가 없는 경로의 후보다.
DMA 자체가 없어지는 것은 아니며, 현재 구현에 이미 있는 frame 조립/SEND/RECV 복사도
자동으로 사라지지 않는다. TCP용 TLS offload 설정을 verbs QP에 적용하는 방식도 아니다.

IPsec은 TLS가 아니다. IKEv2 certificate 인증으로 양쪽 node를 인증하더라도 정확한
명칭은 **node-authenticated IPsec**이지 node-to-node mTLS가 아니다. control lane에
mTLS를 유지할 수 있지만, 그것만으로 IPsec data lane이 pod-to-pod mTLS가 되지는 않는다.
문자 그대로 per-Pod TLS session이 요구사항이면 IPsec 단독 대안은 충족하지 못한다.

### 11.19.2 근거와 아직 증명되지 않은 부분

- [NVIDIA IPsec Full Offload — RDMA traffic](https://docs.nvidia.com/networking/display/mlnxofedv23102131201lts/ipsec-full-offload.pdf):
  RoCEv2와 SR-IOV VF의 full offload를 명시하며, `ip xfrm`의 `offload packet` 또는
  동등한 packet-offload 설정을 안내한다. kernel network stack을 우회하는 RDMA에는
  crypto-only offload와 full packet offload를 구분해야 한다. **우리 DPU ARM의 실제
  PF/VF/SF·QP·netdev 조합에서 된다는 실측 근거는 아직 아니다.**
- [NVIDIA DOCA East-West Overlay Encryption](https://docs.nvidia.com/doca/sdk/DOCA-East-West-Overlay-Encryption-Application/index.html):
  DPU의 strongSwan 제어와 hardware packet 암호화의 구현 참고다. host에서 OVS/VXLAN을
  통과하는 예제이므로 설정을 그대로 ARM-origin RoCE에 복사하지 않는다. 특히 예제의
  VXLAN selector와 software stack 조건은 해당 경로의 조건이지 모든 RoCE의 조건이 아니다.
- [ReDMArk, USENIX Security 2021, §7.3](https://www.usenix.org/system/files/sec21-rothenberger.pdf):
  RoCE의 IPsec 보호를 논의하는 논문 근거다. IPsec endpoint 보호와 RDMA QP의 source
  binding을 구분한다. 다른 인증된 endpoint의 QP 사칭까지 자동으로 해결한다고 읽으면
  안 된다. 이 논문은 현재 BlueField 구성의 inline 성능이나 zero-copy 완료 증거가 아니다.

따라서 “RDMA는 IPsec inline 암호화가 원천적으로 불가능하다”도, “BlueField니까 현재
코드 그대로 반드시 된다”도 결론으로 쓰지 않는다. 아래 절차는 이 근거에서 도출한
**DPUmesh용 설계 제안**이며, vendor가 DPUmesh integration을 보증한 내용이 아니다.

### 11.19.3 node 단위 보호가 충분하다고 판단할 수 있는 조건

§3.3처럼 host kernel, 등록 경로, broker, DPU와 controller/CA를 신뢰하고 일반 workload와
외부 network를 신뢰하지 않는 모델이라면, 다음을 모두 만족하는 **IPsec + workload
authorization** 조합은 검토할 수 있다. 단순히 “node끼리 암호화했으니 충분”은 근거가 아니다.

1. 외부 network의 도청·변조·replay는 인증된 peer 사이 ESP와 anti-replay가 막는다.
   unprotected ingress/egress는 차단하며 같은 node 내부 plaintext는 신뢰 경계 안에 둔다.
2. source Pod는 자기 UID/namespace/ServiceAccount를 임의로 claim하지 못한다. 신뢰된
   local registration에서 얻은 identity만 사용하고, remote에서는 controller가 승인한
   node 소유권과 exact Pod incarnation, destination policy를 검증한다.
3. workload claim은 **실제로 인증된 node와 현재 data channel**에 결합한다. 필요한 경우
   controller-signed short-lived capability를 사용하며 대상 node/service, Pod incarnation,
   expiry와 replay 범위를 명시한다. IPsec source IP만으로 Pod identity를 만들지 않는다.
4. Pod 삭제·재생성·이동, node credential 회수, policy 변경 때 기존 권한과 stream을
   철회한다. 다른 node 또는 이전 incarnation의 claim은 application delivery 전에 거부한다.
5. 한 node/DPU가 침해되면 그 node의 workload를 사칭할 수 있다는 한계를 수용한다.
   기존 DPU-terminated workload TLS도 DPU가 해당 Pod private key를 보유하므로 이 위협을
   독립적으로 해결하지는 못한다. 다른 node의 workload까지 사칭할 수 있어서는 안 된다.

위 조건은 network confidentiality/integrity와 workload authorization을 분리해 만족시키는
논거다. **per-Pod TLS proof of possession, 독립 traffic key, TLS association 단위 audit와
동일한 기능을 제공한다는 뜻은 아니다.** 이 차이를 수용하지 못하면 기존 workload mTLS
설계를 유지한다. 수용한다면 §1.2의 workload 상호 인증 invariant와 §11의 TLS-specific
계약을 명시적으로 개정해야 하며, 이 메모만으로 기존 요구사항을 완화하지 않는다.

### 11.19.4 구현 전에 닫아야 하는 hardware feasibility gate

아래는 미팅 후 승인받아 수행할 순서다. 지금 장비 설정, service restart, firmware 변경은
하지 않는다. 실제 2-node 준비 절차는 별도 로컬 운영 문서를 따른다.

1. 양쪽 장비의 crypto-enabled SKU, firmware, BFB/DOCA, kernel, mlx5 driver와 iproute2,
   IKE daemon 버전을 기록한다. 기존 장비 메모를 현재 capability 측정치로 간주하지 않는다.
2. `peer_wire_rdma.c`가 사용하는 RDMA device/port/GID와 실제 netdev·PF/VF/SF·physical
   egress를 대응시킨다. **DPU ARM process가 만든 QP의 traffic**이 packet offload 경로에
   들어가는지 확인한다. host VF의 성공이나 host-transit OVS 예제로 대신하지 않는다.
3. 그 조합의 vendor-supported full-offload recipe를 고른다. DMFS/switchdev/function
   capability, kernel/backport 조건은 해당 버전 문서로 확인한다. 서로 다른 문서의
   kernel 6.6 조건과 BlueField BFB kernel 5.15 예제를 한 가지 설치 절차로 섞지 않는다.
4. 승인된 격리 fabric에서 synthetic payload로 양방향 RoCEv2 baseline을 확보한 뒤,
   동일 ARM-origin QP 경로에 IPsec packet offload를 설정한다. test용 static SA는 packet
   처리 확인용일 뿐이며 production 인증·key lifecycle의 완료로 인정하지 않는다.
5. 양방향 SA(Security Association), ingress/egress policy, selector, replay window,
   sequence exhaustion/ESN, rekey 지원과 ESP overhead를 포함한 MTU를 확인한다. NIC의
   RDMA header/ICRC 처리까지 지원되는 경로를 사용하며 소프트웨어에서 임의 패킷 변환하지 않는다.
6. 외부 link capture와 양쪽 NIC IPsec/RDMA counter를 함께 남긴다. DPU 내부 capture는
   암호화 전/복호화 후 지점일 수 있으므로 plaintext가 보였다는 이유만으로 wire 유출로
   단정하지 않는다. 반대로 연결 성공이나 ARM CPU 감소만으로 hardware offload를 입증하지 않는다.

selector 주의: 현재 `bench/bench.sh`의 `DPUMESH_PEER_PORT` 기본값 `47900`은 peer
service/RDMA CM 설정이고 RoCEv2 wire UDP destination port `4791`과 다르다. 실제
생성되는 packet을 기준으로 RoCEv2 flow 또는 전용 DPU IP pair를 보호한다. overlay 예제의
UDP `4789`를 무조건 사용하지 않는다. CM/control packet 보호 범위도 별도로 확인한다.
여러 Pod stream이 하나의 peer/QP에 multiplex되므로 node IP pair SA가 자동으로
per-Pod SA가 되지 않는다. per-Pod SA를 원하면 별도의 flow 식별·steering 설계가 필요하다.

### 11.19.5 IPsec 제어와 fail-closed 구현 후보

첫 후보는 DPU의 privileged node service가 IKEv2와 Linux XFRM packet-offload policy를
관리하고, mesh process는 그 service의 검증된 상태를 받아 admission하는 분리 구조다.
DOCA packet/flow API 기반 별도 구현은 대안이며 동일 flow를 두 관리자가 동시에 소유하지
않는다. 구체적인 service/API 이름은 아직 정하지 않았다.

- production은 양쪽 node certificate와 명시적 peer identity 검증을 설계한다. IKE credential과
  현재 TLS node key의 관계는 controller가 승인한 mapping으로 정한다. CA private key를
  DPU에 배포하거나 TLS session secret을 임의로 IPsec key로 전용하지 않는다.
- 선택한 stack에서 `offload packet` / `hw_offload = packet` 등 **packet offload 필수**
  설정을 사용한다. 정확한 문법과 지원은 해당 stack에서 검증한다. software/crypto-only로
  내려갈 수 있는 `auto`는 이번 hardware 요구의 성공 조건으로 쓰지 않는다.
- policy가 설치되어 unprotected traffic을 차단하고, 양방향 인증·SA·실제 hardware 설치가
  확인된 뒤에만 peer DATA를 허용한다. XFRM state가 보인다는 사실만으로 끝내지 않는다.
- SA 부재/expiry, rekey 실패, daemon/NIC restart에서도 hardware/driver 경로 자체가
  plaintext를 차단해야 한다. user-space 상태 polling은 이 차단을 대신할 수 없다.
  장애 때 QP/admission을 닫고, policy 제거 순서 때문에 plaintext가 나갈 틈도 없게 한다.
- rekey 중 old/new SA overlap, anti-replay, certificate 회수, node 재시작과 connection
  incarnation의 관계를 명시한다. SPI 변경마다 application stream을 반드시 끊을 필요는
  없지만, 동일한 인증 peer와 보호 상태의 연속성을 입증하지 못하면 fail-closed한다.
- application Pod에 XFRM, SA 또는 DPU flow 변경 권한을 주지 않는다. 제어 service의 API와
  socket도 local caller를 인증한다. key material을 log나 benchmark receipt에 남기지 않는다.

IPsec 미지원·설치 실패 시 plaintext RDMA로 fallback하지 않는다. 기존 TLS를 별도 운영
모드로 유지하는 선택은 가능하지만, 그것을 hardware inline 목표 달성으로 보고하지 않는다.

### 11.19.6 실제 코드 기준 integration 변경 지점

IPsec 설정만 설치하면 현재 코드는 **TLS ciphertext를 다시 IPsec으로 암호화**한다.
아래 분리가 필요하며, `peer_tls_*` 호출만 삭제하면 인증까지 사라지므로 그렇게 수정하지 않는다.

| 현재 파일 / symbol | 현재 역할 | IPsec안을 채택했을 때 필요한 변경 |
|---|---|---|
| [doca/peer_transport.c](doca/peer_transport.c), `conn_feed`, `conn_drain`, `transport_peer_key` | 모든 carrier 위에 TLS와 node-key 인증을 제공 | carrier와 security mode를 분리. IPsec data 경로는 검증된 보호·identity binding 이후에만 plaintext frame을 carrier에 넘김 |
| [doca/peer_transport.h](doca/peer_transport.h), `peer_transport_config` | TLS seed와 wire 설정 | 인증된 node, IPsec 보호 상태, channel binding과 fault notification 계약 추가. 기존 TLS config/default 보존 |
| [doca/peer_channel.c](doca/peer_channel.c), `dmesh_peer_authenticated`, `bound_key` | TLS에서 얻은 key를 topology의 expected key와 비교 | IKE identity↔mesh node identity mapping과 실제 data channel 증명을 검증하는 별도 admission 설계 |
| [doca/peer_wire_rdma.c](doca/peer_wire_rdma.c), `rdma_send_msg`, `rdma_recv_msg` | RC SEND/RECV와 등록된 slot pool | verbs carrier는 유지. 보호 대상 device/flow를 제어 계층과 연결하고 IPsec fault 때 connection을 중단 |
| [doca/dpu_proxy.c](doca/dpu_proxy.c), remote delivery/policy 경로 | source/destination와 host DMA/custody 관리 | workload claim 검증과 incarnation 철회를 보존. IPsec이라는 이유로 destination policy나 ACK gate를 생략하지 않음 |
| [bench/bench.sh](bench/bench.sh), peer 설정 전달 | transport/bind/port 설정 | 향후 명시적인 security mode와 capability receipt를 연결. 기존 환경변수만으로 IPsec이 활성화됐다고 취급하지 않음 |

인증 seam의 핵심 미결정 사항:

1. 현재 `peer_key()`는 TLS handshake가 입증한 key를 반환한다. IPsec 경로에서 topology의
   expected key를 그냥 복사해 반환하면 **expected identity를 인증 결과로 위조**하는 셈이다.
   인증 evidence의 종류를 구분하는 API와 negative test부터 설계한다.
2. node mTLS control lane을 유지한다면 그 lane에서 승인한 양쪽 node/incarnation과
   fresh challenge를 실제 IPsec-protected data channel에 결합하는 bootstrap이 필요하다.
   data channel의 주소/QP 정보, local policy 소유권과 검증된 IKE identity를 함께 확인한다.
   metadata에 IP나 QPN을 적는 것만으로 결합이 입증되는 것은 아니다.
3. 다른 정상 인증 node의 SA/QP로 claim을 재사용하거나 old connection의 bootstrap을
   replay하지 못하게 해야 한다. bootstrap field/증명 방식/timeout/rekey 연속성은 미팅 후
   별도 protocol review와 fixture로 동결한다. 이 메모는 완성된 channel-binding protocol이 아니다.
4. DATA뿐 아니라 OPEN/ACK/FIN/RESET 등 data channel 제어 frame도 보호한다. 별도 control
   mTLS의 소량 이중 암호화는 허용 여부를 정하되, bulk payload에서 TLS를 제거할지는 요구사항
   결정 뒤에만 바꾼다. remote direct WRITE로 host memory를 노출하는 변경은 포함하지 않는다.

### 11.19.7 memcpy와 성능 검증은 별도 항목

현재 `dmesh_peer_stream_data_send()`는 payload를 `channel->tx_frame`으로 복사하고,
`rdma_send_msg()`는 다시 registered SEND slot으로 `memcpy()`한다. `rdma_recv_msg()`도
receive slot에서 caller buffer로 복사한다. TLS 경로에는 memory BIO와 `c->in/out` staging이
추가된다. 따라서 **IPsec inline 도입 = 현재 RDMA 경로 전체 zero-copy**가 아니다.

IPsec data mode가 채택되면 TLS-specific staging 제거를 검토할 수 있다. frame/SEND 복사
제거는 registered buffer 소유권과 scatter/gather, RX 복사 제거는 receive buffer lease와
repost 시점까지 바꾸는 독립 작업이다. 현재 QP의 `max_send_sge = 1`도 고려해야 한다.
local SEND CQ 완료 전에 buffer를 재사용하지 않으며, source custody의 `DELIVERED`는 여전히
destination `REV_DONE` 뒤 `STREAM_ACK` 수신으로만 확정한다. SEND CQ는 전달 ACK가 아니다.

공통 RDMA buffer API와 현재 TLS 연결의 구체적인 복사 제거 절차는 §11.20에 기록한다.
§13.4의 Linkerd TX 중간 복사 제거 계획은 그대로 유지하며, 이 IPsec 검토와 별도로 평가한다.
IPsec 또는 복사 제거만으로 성능 향상률을 사전에 약속하지 않는다.

향후 검증 순서:

1. host-only fixture: 보호 미완료 admission, 다른 node/Pod claim, bootstrap replay,
   policy 철회, SA fault 때 delivery 0과 custody terminal 처리를 검증한다. mock의 성공은
   hardware pass가 아니다.
2. 2-DPU synthetic traffic: 실제 ARM-origin QP에서 plaintext baseline 대비 IPsec packet
   offload의 wire/counter receipt를 확보한다. plaintext baseline은 격리된 test에 한정한다.
3. production 인증 후보: certificate 기반 IKE, rekey/expiry/회수, daemon/NIC restart,
   잘못된 SA와 plaintext injection 때 유출·application delivery가 없는지 확인한다.
4. mesh 연결: node binding, Pod churn과 destination policy를 유지하면서 양방향 L4/L7
   traffic을 검증한다. 현재 TLS 경로와 동일 payload·concurrency·MTU 조건에서 throughput,
   p50/p99 latency, ARM CPU, memory bandwidth, packet/drop/replay counter를 기록한다.
5. 복사 최적화는 별도 전후 결과로 분리한다. workload TLS안과 node IPsec안은 인증 단위가
   다르므로 성능 수치만으로 “동일한 보안에 더 빠름”이라고 결론 내리지 않는다.

### 11.19.8 2026-09-07 월요일 미팅 결정 항목

- [ ] 요구사항은 실제 per-Pod TLS association인가, 아니면 workload authorization을
  보존한 node 경계 암호화인가? IPsec을 mTLS라고 부르지 않는 용어에도 합의한다.
- [ ] §11.19.3의 신뢰 경계와 보안상 차이를 수용하는가? capability/channel binding의
  검증 책임과 credential 발급·철회 주체는 누구인가?
- [ ] ARM-origin RoCEv2 full-offload feasibility 실험을 진행할 것인가? 양쪽 장비 접근,
  설정 변경 권한, 복구 방법과 담당자를 정한다.
- [ ] node pair SA로 충분한가? per-Pod key isolation이 필요하면 기존 workload TLS 또는
  별도 per-Pod flow 설계를 유지해야 한다.
- [ ] full-offload 미지원이면 기존 TLS를 유지할 것인가, 장비/경로를 바꿀 것인가?
  software fallback을 hardware 목표 달성으로 인정하지 않는다.
- [ ] 채택 후에만 §1.2·§11.3~§11.18의 요구사항/구현 순서를 일관되게 개정한다.
  미채택이면 본 절은 검토 기록으로 남기고 기존 workload mTLS 계획을 유지한다.

---

## 11.20 RDMA 중간 복사 제거 — 공통 buffer lease와 TLS 전용 연결

상태: **A 공통 TX/RX lease 구현·software 회귀 완료 / B TLS 연결 미착수**.
A는 [direct-tx-rdma-20260905](bench/report/data/direct-tx-rdma-20260905/SUMMARY.md)에서
배포·측정했다. 실제 두 DPU fabric gate는 계속 별도다. 2026-09-05 실제 코드를
기준으로 작성했다. §11.19의 IPsec 채택 결정과 독립적으로 구현할 수 있다. 현재 TLS 인증과
wire protocol을 유지한다. A의 API/type은 구현됐으며, B의 TLS 직접 연결은 아래 설계로 남는다.

### 11.20.1 무엇을 없애고 무엇은 유지하는가

BIO는 OpenSSL의 입출력 인터페이스이며, 현재 `peer_tls.c`는 memory BIO를 사용한다.
송신 암호문을 출력 BIO에서 꺼내고 수신 암호문을 입력 BIO에 넣는 것은 mesh 코드의 역할이다.
RDMA는 여기서 RC QP의 SEND/RECV를 사용하며, remote WRITE로 host memory에 직접 쓰는
구현이 아니다. 다음 두 복사는 이 carrier와 TLS 사이에 있는 **중간 복사**다.

```text
현재 TX: TLS 출력 BIO → c->out → memcpy → registered SEND slot → NIC
개선 TX: TLS 출력 BIO ─────────────────→ registered SEND slot → NIC

현재 RX: NIC → registered RECV slot → memcpy → c->in → TLS 입력 BIO
개선 RX: NIC → registered RECV slot ────────────────→ TLS 입력 BIO
```

작업을 두 층으로 나눈다. 두 층을 모두 연결했을 때 위 복사가 없어지는 것이며, 각각의
효과를 더해서 “TX 두 번, RX 두 번 제거”로 세지 않는다.

| 작업 | 하는 일 | TLS/IPsec 관계 |
|---|---|---|
| A. 공통 기반 | TX slot을 빌려 채우고 제출하는 reserve/commit, RX slot을 빌려 읽고 반환하는 acquire/release | 암호화 방식과 무관. carrier는 bytes와 lifetime만 관리 |
| B. TLS 전용 연결 | `peer_tls_out/in()`이 A의 slot을 직접 사용하도록 transport pump 변경 | 현재 TLS용 adapter. data TLS를 IPsec으로 대체하면 이 부분만 다른 producer/consumer로 교체 |

제외 범위: `channel->tx_frame`의 payload 조립 복사, OpenSSL 내부 record/BIO 복사,
proxy delivery arena 복사, host↔DPU DMA, §13.4의 Linkerd TX 작업, IPsec 설정 및
새 custom BIO/crypto backend. RC SEND/RECV와 현재 `max_send_sge = 1`을 유지한다.
SGE는 NIC에 전달하는 buffer 주소·길이·등록 key이고, 이번에는 기존 contiguous slot 하나면
충분하다. 전체 경로 zero-copy나 encryption cost 제거로 표현하지 않는다.

### 11.20.2 먼저 읽을 코드와 변경 위치

아래 순서로 읽은 뒤 수정한다. line number보다 symbol을 기준으로 찾는다.

| 파일 / symbol | 현재 동작 | 필요한 변경 |
|---|---|---|
| [doca/peer_wire.h](doca/peer_wire.h), `peer_wire_ops` | copy-in `send_msg`, copy-out `recv_msg`만 제공 | optional lease callback과 소유권 계약 추가 |
| [doca/peer_wire_rdma.c](doca/peer_wire_rdma.c), `rdma_conn`, `rdma_arm_conn` | 연결별 MR, SEND 16개/RECV 32개 slot, SEND busy bit | slot state와 lease generation 추가. 기존 MR/pool을 재사용 |
| 같은 파일, `rdma_send_msg`, `rdma_recv_msg` | SEND slot으로 복사 / RECV slot에서 복사 후 즉시 repost | lease 기반 primitive 구현, 기존 API는 copy compatibility wrapper로 유지 |
| 같은 파일, `rdma_poll_cq`, `rdma_release` | CQ에서 slot 반환, QP 파괴 후 MR 해제 | POSTED/LEASED 구분, stale token/completion와 shutdown 수명 검증 |
| [doca/peer_transport.c](doca/peer_transport.c), `conn_feed` | RECV→`c->in`→`peer_tls_in` | RX lease를 TLS에 직접 전달한 뒤 반환 |
| 같은 파일, `conn_drain`, `conn_flush`, `transport_send` | BIO→`c->out`→wire, 막히면 `out_len` 유지 | TX slot에 직접 drain, BIO pending까지 포함한 backpressure |
| 같은 파일, `conn_step`, `transport_recv`, `dmesh_peer_transport_pending` | handshake/prologue/수신/progress | 모든 TLS 출력 발생 지점에 공통 output pump와 정확한 pending 조건 적용 |
| 같은 파일, `conn_alloc`, `conn_free` | `calloc(2, PEER_WIRE_MSG_MAX)`, `free(c->out)` | borrowed slot을 소유 allocation과 분리; cleanup 순서 수정 |
| [doca/peer_tls.c](doca/peer_tls.c), `peer_tls_out`, `peer_tls_in` | 각각 `BIO_read`, `BIO_write` | 초기 구현은 함수와 crypto를 그대로 유지. caller의 buffer만 교체 |
| [doca/peer_wire_tcp.c](doca/peer_wire_tcp.c), TCP ops | 기존 copy API | 초기에는 lease callback NULL, 기존 경로 유지 |

`peer_transport.c`와 `peer_wire_rdma.c`만 바꾸고 끝내지 않는다. interface 소비자인
in-memory test carrier와 close/pending 경로를 함께 바꿔야 한다. public native API나
peer frame format을 바꾸는 작업은 아니다.

### 11.20.3 A — 공통 lease API 계약

lease는 “buffer를 잠시 사용할 권리와 그 반환 token”이다. raw pointer만 반환하면
slot 재사용 후 이전 caller의 release를 구별할 수 없으므로 token이 필요하다.
다음 type과 callback을 `peer_wire.h`에 추가하는 것을 시작안으로 한다.

```c
/* 제안: connection epoch, slot index, 해당 slot의 lease generation을 검증한다. */
struct peer_wire_token {
    uint64_t conn_epoch;
    uint64_t lease_generation;
    uint32_t slot;
    uint32_t kind;                 /* TX / RX: 교차 사용 금지 */
};
struct peer_wire_tx_lease {
    uint8_t *data;
    size_t cap;
    struct peer_wire_token token;
};
struct peer_wire_rx_lease {
    const uint8_t *data;
    size_t len;
    struct peer_wire_token token;
};
/* peer_wire_ops 끝에 추가할 optional callback */
int (*tx_reserve)(void *wc, struct peer_wire_tx_lease *out);
int (*tx_commit)(void *wc, struct peer_wire_tx_lease *lease, size_t len);
int (*tx_cancel)(void *wc, struct peer_wire_tx_lease *lease);
int (*rx_acquire)(void *wc, struct peer_wire_rx_lease *out);
int (*rx_release)(void *wc, struct peer_wire_rx_lease *lease);
```

- TX 3개/RX 2개는 각각 한 묶음이다. transport 생성 시 묶음 안에 일부 callback만 있으면
  config error로 거부한다. TX만 또는 RX만 먼저 지원하는 단계는 허용한다.
- `reserve/acquire`: `1`은 lease 획득, `0`은 지금 자원/완료 없음, `-1`은 fault다.
  `0/-1`이면 out을 비우고 소유권을 주지 않는다. TX cap은 현재 `PEER_WIRE_MSG_MAX`
  전체를 수용한다. initial scope는 connection당 각 방향 outstanding lease 최대 1개다.
- `commit`: 유효한 lease의 `0 < len <= cap`을 한 번에 post하고 `1` 반환한다.
  성공한 reserve는 SEND WR 자리까지 확보하므로 **commit에는 would-block `0`이 없다**.
  post 실패는 terminal `-1`이며 TLS byte를 재암호화해서 복구하지 않는다.
- 유효 token으로 commit하면 성공/실패 모두 caller의 lease는 소비된다. invalid length면
  post 없이 connection fault 처리하고 예약을 회수한다. `cancel/release`는 성공 `0`,
  fault `-1`이며 유효 lease는 실패 때도 소비된다. 반환 후 struct를 비워 재사용을 막는다.
- invalid/stale/다른 방향 token은 `-1`로 거부하되 현재 다른 owner의 slot에는 손대지 않는다.
  pointer 산술만 믿지 말고 epoch/slot/generation/state/길이를 검사한다. generation wrap 시
  이전 token과 충돌시키지 말고 connection을 retire하는 정책으로 닫는다.
- 새 연결 generation은 현재 RDMA WR epoch 검증과 일관되게 부여하되 lifetime이 다른 lease
  generation을 별도로 둔다. 기존 WR packing의 epoch bit 수를 무심코 바꾸지 않는다.
- 모든 호출은 현재처럼 동일 worker가 직렬 실행한다. 임의 thread가 callback을 호출하는
  기능은 추가하지 않는다. lease를 타 worker/async DMA에 넘기는 확장은 별도 계약이 필요하다.
- callback 존재 여부에 따른 **copy compatibility 경로**는 허용한다. 이는 TLS/RDMA를
  plaintext/TCP로 바꾸는 security/transport fallback과 다르다. RDMA lease 모드 실행 중
  자원 부족을 숨기려고 임시 malloc+copy로 우회하지 않는다.

### 11.20.4 A — RDMA slot 상태와 기존 API 호환

기존 pool 크기와 MR 등록은 유지한다. packet마다 `ibv_reg_mr()`를 호출하거나 caller의
임의 heap buffer를 즉석 등록하지 않는다. slot state는 다음처럼 구분한다.

```text
TX: FREE → RESERVED → POSTED → FREE (성공 SEND CQ)
                   ↘ FREE          (post 전 cancel)
RX: POSTED → READY → LEASED → POSTED (release 후 repost 성공)
각 상태에서 terminal fault → 새 대여 중단 → owner 정리 → QP 파괴 → MR/pool 해제
```

1. `send_busy[]`의 역할을 FREE/RESERVED/POSTED state로 확장한다. reserve 시 FREE만
   선택하고 바로 RESERVED로 바꾼다. 아직 post하지 않은 예약을 legacy `send_msg()`나
   다음 reserve가 다시 사용하지 못하게 한다.
2. commit은 같은 slot 주소/`c->mr->lkey`를 SGE에 넣어 기존 `IBV_WR_SEND`와
   `IBV_SEND_SIGNALED`로 post한다. NIC가 읽는 동안 pointer를 caller에게 다시 주지 않는다.
   CQ는 epoch가 일치하는 POSTED slot만 FREE로 만든다. completion error면 connection fault다.
3. RX CQ는 slot을 READY로 만들고 기존 `rq_slot/rq_len` queue에 넣는다. acquire는
   가장 오래된 READY를 dequeue해서 LEASED로 바꾸지만 **repost하지 않는다**. release에서
   `rdma_post_recv()`하고 성공 때만 POSTED로 바꾼다. repost 실패는 terminal fault다.
4. queue count만 보고 free slot 수를 계산하지 않는다. LEASED slot은 queue 밖에 있어도
   소비 중이다. fault 전 정상 상태에서는 TX/RX 각 pool의 상태 개수 합이 16/32여야 한다.
5. legacy `rdma_send_msg()`는 reserve→memcpy→commit으로 구현한다. 반환 `1` 뒤 caller가
   원본을 재사용할 수 있다는 기존 계약을 유지한다. legacy `rdma_recv_msg()`는
   acquire→cap 검사→memcpy→release로 구현하고 cap 초과의 기존 terminal error를 보존한다.
6. lease와 legacy API를 섞어도 pending RX lease를 건너뛰어 다음 message를 먼저 전달하지
   않는다. 겹친 acquire/reserve는 `0`으로 거부한다. token 중복 반환은 위의 `-1` 규칙이다.
7. close 시 새 대여를 막고 caller의 lease 사용을 먼저 끝낸다. transport가 들고 있는
   미제출 TX는 cancel, RX는 release/terminal 정리 후 wire close를 호출한다. QP 파괴로
   DMA가 더는 접근하지 않는 상태를 만든 뒤 MR과 pool을 해제한다. **close 후 pointer를
   사용하거나 해제된 wc로 release를 호출하는 동작은 금지**한다.

현재 TLS adapter는 lease를 동기적인 pump 호출 안에서만 사용하므로 비동기 사용자 종료를
기다리는 별도 큐는 필요 없다. 미래 IPsec consumer가 slot에서 비동기 DMA를 시작한다면
완료/취소와 close barrier를 추가한 뒤 사용해야 하며, 이 초기 구현이 그 수명까지 해결했다고
주장하지 않는다. local SEND CQ는 slot 재사용 조건일 뿐, source custody의 `DELIVERED`는
계속 destination `REV_DONE` 뒤 `STREAM_ACK`로만 결정한다.
일반 SEND buffer 재사용 조건은 [rdma-core ibv_post_send 문서](https://raw.githubusercontent.com/linux-rdma/rdma-core/master/libibverbs/man/ibv_post_send.3)를 따른다.

### 11.20.5 B — TLS RX 연결부터 구현

`conn_feed()`에서 RX callbacks가 있으면 아래 순서로 바꾸고, 없으면 기존 `c->in` 경로를
사용한다. 다음은 성공/실패 시 반드시 lease를 반환하기 위한 의사코드다.

```c
struct peer_wire_rx_lease rx = {0};
int r = wire->rx_acquire(c->wc, &rx);
if (r <= 0)
    return r;                        /* 0: 아직 없음, -1: fault */
int fed = peer_tls_in(c->tls, rx.data, rx.len);
int released = wire->rx_release(c->wc, &rx); /* fed 실패 때도 호출 */
return (fed == 0 && released == 0) ? 1 : -1;
```

현재 `peer_tls_in()`은 memory BIO에 `BIO_write()`하고, 전체 입력이 복사됐을 때만
성공한다. 그러므로 성공 반환 즉시 RX slot을 반환해도 된다. `SSL_read()`나 application
delivery까지 기다리지 않는다. 실패 시에는 부분 입력이 있었을 수 있으므로 같은 message를
다시 feed하지 않고 connection을 fault 처리한다. release의 repost 실패도 성공으로 숨기지 않는다.

입력 BIO로의 복사는 남는다. 이번에 없어지는 것은 `RECV slot → c->in` 한 번이다.
향후 custom BIO가 외부 pointer를 보관하도록 변경되면 이 반환 시점도 반드시 재설계한다.
[OpenSSL memory BIO 문서](https://docs.openssl.org/3.0/man3/BIO_s_mem/)와 현재
`peer_tls_in()` 구현을 함께 기준으로 삼는다.

### 11.20.6 B — TLS TX 연결과 재암호화 방지

TLS는 `SSL_write()`로 받은 평문을 이미 session sequence에 반영한다. 그 뒤 slot이 없다는
이유로 상위에 `0`을 반환하면 상위가 같은 평문을 다시 보내므로 중복 전송이 된다.
기존 `transport_send()`의 “이전 출력이 밀려 있으면 새 평문을 받지 않음”을 그대로 보존한다.

새 lease 경로는 `c->out` 대신 **출력 BIO 자체에 아직 제출하지 못한 암호문을 유지**한다.
추가 heap queue를 만들지 않는다. 공통 output-pending predicate를 추가한다.

```text
pending_output = legacy out_len != 0 || peer_tls_out_pending(tls) != 0
```

TX pump의 순서:

1. `peer_tls_out_pending()`이 0이면 아무 slot도 빌리지 않고 끝낸다. pending bytes가
   `PEER_WIRE_MSG_MAX`를 넘으면 기존 bounded-output 계약 위반으로 fault 처리한다.
2. slot reserve가 `0`이면 BIO를 읽지 않는다. bytes를 BIO에 남기고 CQ에 의한 자원 반환을
   기다린다. `-1`이면 connection fault다. 성공 시 slot cap도 검사한다.
3. 기존 `peer_tls_out()`을 호출하되 출력 주소를 `tx.data + written`으로 바꿔 채운다.
   전체 pending을 cap 내에서 drain한다. 남은 output이 cap을 넘거나 예상과 다르게 읽기가
   멈추면 무한 반복하지 않고 cancel/fault한다. empty output은 cancel하며 zero-length SEND는 없다.
4. 채운 길이로 commit한다. commit 성공 뒤 slot은 NIC owner이고 transport는 pointer를
   버린다. commit fault는 이미 BIO에서 소비한 bytes를 재생성하지 말고 connection을 닫는다.
5. 반환은 실제 bytes를 제출했으면 progressed, slot 부족이면 no-progress다. “대기 중”을
   “진행함”으로 보고 worker를 busy-loop시키지 않는다.

`transport_send()`도 함께 변경한다.

```text
이전 output pump → 여전히 pending이면 0 반환 (새 평문은 아직 TLS에 넣지 않음)
→ peer_tls_write(이번 평문) 정확히 한 번
→ 새 output pump
→ fault면 -1, 아니면 평문 len 반환 (BIO에 output이 남아도 이미 accepted임)
```

`conn_drain/conn_flush`는 이 공통 pump를 호출하는 구조로 정리하거나 하나로 합친다.
lease 경로와 legacy 경로가 동일 BIO를 각각 읽지 않게 한다. RX만 lease인 단계에서는
TX legacy 경로를 그대로 두어 incremental test가 가능하게 한다.

**DATA send만 변경하면 불완전하다.** 다음 call site를 모두 확인한다.

- `conn_step()`의 TLS handshake: 이전 output을 우선 pump하고, 막혀 있으면 새 handshake
  step으로 output을 계속 누적하지 않는다. handshake 완료와 마지막 output 제출 상태를
  따로 추적한다. RX feed/progress를 무조건 멈추지는 않아 상호 backpressure를 풀 수 있게 한다.
- initiator prologue: `out_len == 0` 대신 pending predicate를 사용하고 prologue를 TLS가
  받아들였다는 상태를 명시한다. 제출 대기 때문에 prologue를 다시 `SSL_write()`하지 않는다.
- `transport_recv()`와 responder의 `conn_read_prologue()`: `peer_tls_read()`도 TLS control
  output을 만들 수 있으므로 호출 후 output pump를 실행한다. pending output이 있는 동안
  새 SSL 처리로 계속 쌓지 않는다. 읽은 plaintext는 출력 대기만으로 버리거나 다시 읽지 않는다.
- pending일 때 inbound를 BIO에 feed할 필요가 있다면 unread input 총량에도 명시적인 bound를
  둔다. 무제한 RX drain으로 registered pool의 제한을 unbounded BIO queue로 옮기지 않는다.
  초기 제안은 unread input `PEER_WIRE_MSG_MAX` 이내이며 다음 message가 이를 넘으면 lease를
  acquire하기 전에 backpressure한다. peek/길이 확인이 없다면 full-size 1개가 들어갈 공간을
  보수적으로 확보한 뒤 acquire한다. input pending 조회 helper가 필요하면 `peer_tls.h/c`에 추가한다.
- `dmesh_peer_transport_pending()`과 ESTABLISHED progress: `out_len`뿐 아니라 BIO pending도
  포함하고, 다음 application write가 없어도 output을 재시도한다. CQ event를 읽고 slot이
  돌아오는 기존 epoll/progress 경로를 유지한다. 같은 blocked 상태만으로 progress=1을 반환하지 않는다.

이 gating은 아직 구현되지 않았다. 특히 양방향 SEND ring 포화 + TLS control output test가
통과하기 전에는 “memory BIO에 남기면 끝”으로 구현 완료 처리하지 않는다. 암호화 backend를
바꾸거나 custom TLS record 생성으로 이 문제를 우회하지 않는다.

### 11.20.7 allocation, 실패 처리와 호환 경로

현재 `conn_alloc()`은 하나의 2×73728-byte allocation을 `out/in`으로 나누며
`conn_free()`는 두 영역을 cleanse하고 `free(c->out)`한다. 여기에 borrowed SEND pointer를
그냥 대입하면 registered pool을 잘못 free/cleanse하므로 금지한다.

1. 초기에는 기존 allocation을 유지해도 된다. 소유 buffer와 일시 lease를 별도 field/local
   variable로 두고 correctness부터 닫는다. 이 단계도 두 중간 memcpy는 제거할 수 있다.
2. 이후 방향별 owned scratch allocation으로 분리한다. TX lease 지원 시 out scratch,
   RX lease 지원 시 in scratch를 생략한다. 어느 방향이 NULL이어도 cleanup이 안전해야 한다.
3. cleanse/free는 자신이 소유한 allocation에만 적용한다. borrowed slot을 직접 free하거나
   NIC가 사용하는 POSTED buffer를 cleanse하지 않는다. 기존 secret cleanup 보장은 유지한다.
4. lease callbacks 부재 시 TCP/in-memory carrier는 종전 copy 경로를 사용한다. security
   identity, frame bytes, auth timeout과 channel/custody 반환 값은 두 경로에서 같아야 한다.
5. production 기본값 전환 전에 copy 경로와 lease 경로를 각각 강제하는 test fixture를 둔다.
   이를 위해 평문 모드나 새 환경변수를 임의로 추가하지 않는다.

### 11.20.8 구현 순서와 따라 실행할 검증

작업 순서는 **A의 계약/테스트 → A의 RX + B의 RX → A의 TX + B의 TX → scratch 정리**다.
각 단계는 기존 test를 유지하는 작은 변경으로 나눈다.

1. `peer_wire.h`의 계약과 callback validation부터 추가한다. 기존 TCP와 모든 ops initializer가
   계속 build되는지 확인한다. callbacks NULL 경로는 그대로 동작해야 한다.
2. [tests/peer_transport_test.c](tests/peer_transport_test.c)의 기존 `STUB_OPS`는 copy regression으로
   보존하고, bounded leased carrier fixture를 별도로 추가한다. 현재 stub의 malloc queue를
   단순히 pointer 반환하도록 바꾸는 것으로 실제 lifetime test를 대신하지 않는다.
3. lease fixture에는 reserve 부족, delayed SEND completion, held RX, commit/repost failure와
   stale generation을 주입한다. 공통 slot 상태 처리를 hardware 없이 테스트하려면 해당
   helper를 production RDMA 코드와 공유해 mock에서만 올바른 별도 구현을 만드는 것을 피한다.
4. `rdma_conn` RX state/acquire/release 및 legacy wrapper를 구현하고 `conn_feed()`에 연결한다.
   실제 `peer_tls_in()`을 사용해 release 직후 원래 RX buffer를 poison/재사용해도 이후
   복호화 결과가 변하지 않는지 확인한다. 이 test는 BIO가 입력을 소유했다는 증거다.
5. TX state/reserve/commit/cancel과 legacy wrapper를 구현한다. 이어 output pump, send,
   handshake/prologue/read/pending을 함께 바꾼다. `test_send_backpressure()`를 copy/lease 양쪽에
   적용하고 TLS가 받은 frame 수와 destination 수신 seq가 정확히 한 번인지 검사한다.
6. [tests/peer_wire_test.c](tests/peer_wire_test.c)에 실제 RDMA lease test를 추가한다. 장비가
   없으면 기존처럼 RDMA arm은 SKIP이다. TCP roundtrip 성공을 RDMA lease 검증으로 세지 않는다.
7. [tests/peer_tls_test.c](tests/peer_tls_test.c)의 암호화/auth regression과
   [tests/peer_channel_test.c](tests/peer_channel_test.c)의 custody/lifecycle regression을 유지한다.
8. 불필요 scratch allocation을 제거한 뒤 sanitizer와 종료 후 slot/lease 잔량을 다시 검증한다.

필수 negative/lifetime test:

- TX 16 slot을 completion 없이 채운 뒤 다음 reserve=0, 기존 POSTED bytes 불변.
  CQ 반환 뒤에만 재사용. reservation cancel은 SEND를 생성하지 않음.
- RX release 전 같은 slot에 새 receive를 post하지 않음. 32-slot 소유권 합계 보존,
  release/repost로 진행 재개. FIFO, cap 초과, stale/중복/다른 방향 token 거부.
- TLS 이전 output blocked 상태에서 새 frame의 SSL write가 0회; 이미 accepted된 frame은
  재시도에도 SSL write가 1회. 해제 후 output 순서·bytes·destination delivery가 정확함.
- slot이 막힌 handshake/prologue와 양방향 traffic, idle 후 남은 BIO output, read가 생성하는
  control output, input bound에서의 backpressure. lost wake나 무한 progress 없음.
- commit 실패, BIO feed 실패, repost 실패, active lease/POSTED 상태의 close, reconnect 뒤
  old CQ/token: use-after-free, double return, lease leak 없이 기존 terminal custody 처리.
- SEND CQ만 완료하고 destination ACK를 지연하면 source `DELIVERED`는 아직 발생하지 않음.
  key mismatch/unbound node, FIN/POD_GONE와 transport loss의 기존 거부 범위 보존.

기존 [Makefile](Makefile)의 target으로 구현 후 실행할 명령은 다음과 같다. 이번 문서 작업에서
실행한 결과가 아니며, 필요한 OpenSSL/rdma-core 개발 dependency가 있는 환경에서 수행한다.

```sh
make build/test/peer_tls_test build/test/peer_transport_test build/test/peer_channel_test build/test/peer_wire_test
./build/test/peer_tls_test
./build/test/peer_transport_test
./build/test/peer_channel_test
./build/test/peer_wire_test
make test-hostfree
make test
```

현재 `test-hostfree`에는 `peer_transport_test`/`peer_wire_test`가 포함되지 않는다. 따라서
그 target만 통과했다고 lease regression 완료로 보고하지 않는다. 새 독립 state test 파일을
만들 경우 Makefile target과 CI 호출도 함께 추가한다. ASan/UBSan은 별도 build directory에서
해당 test를 다시 compile하고 실행하며 정상 build artifact와 섞지 않는다.

### 11.20.9 성능·완료 기준과 IPsec 이후 재사용

실제 감소량은 정상 처리한 **wire ciphertext bytes** 기준으로 TX/RX 각 한 번의 중간 복사다.
application payload 길이는 TLS overhead와 다르므로 혼용하지 않는다. 제안 counter는
TX/RX lease bytes, compatibility-copy bytes, reserve blocked, live/high-watermark lease,
commit/repost fault다. bounded worker aggregate로 집계하며 Pod별 metric label을 추가하지 않는다.
BIO copy와 frame assembly까지 이 counter 하나로 측정했다고 표현하지 않는다.

hardware A/B는 기존 copy, RX-only, TX-only, TX+RX를 같은 TLS/backend·compiler·ring depth·
payload·concurrency에서 비교한다. 작은 message와 1/8/64 KiB payload, 단방향/양방향을
구분하고 throughput, CPU/byte, p50/p99, memory 사용과 RNR/queue stall을 남긴다.
이번에는 `max_send_sge`, ring depth, IPsec, Linkerd batching을 동시에 바꾸지 않는다.

- [x] 공통 API가 TLS/BIO type을 노출하지 않으며 owner/state/token 계약 test가 있다.
- [ ] TLS+RDMA lease 경로에서 TX `c->out→slot`, RX `slot→c->in` copy bytes가 각각 0이다.
- [ ] TLS wire/auth 결과, frame 순서와 ACK/custody contract가 copy 경로와 동일하다.
- [ ] backpressure/종료/재연결 뒤 dangling pointer, stale release, slot leak가 없다.
- [ ] hardware RDMA 검증과 성능 A/B를 별도로 기록했다. CPU/p99/throughput 회귀가 있다면
  lease hold 시간·batching·progress 변경을 설명하고 수정하기 전에는 완료로 표시하지 않는다.

나중에 IPsec data mode를 선택하면 A의 API/pool/state machine을 재사용하고 B의 TLS
producer/consumer만 교체한다. RX plaintext를 parser나 host DMA가 참조하면 그 사용이 끝난
뒤 release해야 한다. TLS 때의 “BIO_write 직후 release”를 그대로 적용하면 안 된다.
frame 조립까지 송신 slot에 직접 하는 추가 최적화는 이 API를 활용할 수 있지만, 현재
`channel->tx_frame`의 retry/소유권 계약 변경은 별도 후속 계획이다.

---

# 12. P3: Go와 추가 통합

Go 작업은 P1 API freeze와 P0 안정성 뒤 팀원과 진행한다.

## 12.1 왜 preload로 해결할 수 없는가

preload는 dlsym(RTLD_NEXT)로 libc socket call을 interpose한다. static binary와
libc를 거치지 않고 syscall을 내는 Go runtime에는 적용되지 않는다. Go 지원을
source change 없이 된다고 약속하지 않는다.

## 12.2 권장 surface

- C native API를 감싼 작은 cgo transport
- net.Conn 구현
- net.Listener 구현
- context cancellation/deadline
- SetDeadline
- half-close 또는 지원하지 않는 semantics의 명시
- Go runtime poller와 연결할 fd/eventfd
- gRPC custom dialer와 listener

Go wrapper는 별도의 registration, identity, retry 정책을 만들지 않는다. 같은
channel과 signed grant를 사용한다.

## 12.3 시작 조건

- [ ] public C API thread/error/teardown freeze
- [ ] transport loss contract 결정
- [ ] broker lifecycle PASS
- [ ] 최소 native conformance fixture
- [ ] 팀원과 ownership 및 review 범위 합의

## 12.4 완료 조건

- [ ] net.Conn behavior test
- [ ] net.Listener accept test
- [ ] cancellation/deadline/backpressure
- [ ] gRPC Go unary/streaming
- [ ] process/channel teardown
- [ ] actual DPU hardware smoke
- [ ] C++ gRPC와 같은 admission/security path 사용

---

# 13. 계속 미뤄온 항목

이 절은 잊지 않기 위한 목록이지만 P0~P3보다 먼저 하지 않는다.

## 13.1 High-concurrency quiescence gate

상태: OPEN, P0 worker progress에 포함.

grpc_closed_sweep.sh가 8KiB total concurrency 4096 이상 point 사이 session,
task, backend channel, DMA residue를 검사하지 않는다. gate를 추가한 뒤 rejected
stress sequence를 다시 실행한다.

완료:

- point마다 clean pre/post state
- residue가 있으면 다음 point 중단
- overload 자료와 capacity 자료 분리

## 13.2 ARM worker per-event fixed cost

상태: 진행 중. driver 루프의 고정비는 아래까지 줄였고, 남은 병목은 스택 task와
DOCA 내부에 있다.

현재 driver 루프는 다음과 같이 돈다. Rust drain이 C engine drain보다 먼저 실행되어
같은 pass 안에서 stack 출력이 DMA로 넘어간다. endpoint는 stack이 건드렸을 때만
dirty 비트를 세우고 worker signal 하나를 올리므로 driver는 idle endpoint를 lock 없이
건너뛴다. maintenance 마감은 timer entry 하나를 주기마다 reset해서 잡고, wait가
끝나면 실제로 울린 notification source만 clear하며, wake eventfd는 tick이 게시된
경우에만 한 번 읽는다. vendored h2 patch로 client connection poll은 요청당 약 2회다.

[park-arm-20260906](bench/report/data/park-arm-20260906/SUMMARY.md) 기준 gRPC 64 B의
요청당 worker syscall은 epoll_pwait 8.3→7.0, write 1.2→0.68, read 2.4→2.7이고,
CPU/RPC는 capacity 64 B/1 KiB/8 KiB/64 KiB에서 −2.2/−2.4/−1.3/−3.0%, matched에서
−1.7/−1.7/−2.6/−3.3%, opaque TCP에서 −9.7~−12%다. callchain으로 본 syscall의
주인은 다음과 같다.

- epoll_pwait의 절반 이상은 tokio `park_yield`(epoll 0)다. h2/hyper task가 coop
  예산(128연산)을 소진해 `defer`되면 runtime이 I/O driver를 한 번 폴링한다. driver
  task 자신의 yield는 이 폴링에 무임승차하므로 driver 쪽에서는 못 없앤다.
  나머지는 DOCA PE의 `clear_notification`과 `request_notification` 내부 epoll이다.
- read의 87%는 `mlx5dv_devx_get_event`(PE event channel)다. 울린 PE만 clear하면
  clear의 read는 줄지만 건너뛴 event를 다음 arm에서 DOCA가 읽으므로 총수는 비슷하다.
- write는 DMA 완료 콜백의 `dpu_request_host_doorbell → dpu_wake_main`과 DOCA arm의
  eventfd write가 남는다.
- arm 1.8회/RPC 가운데 실제 스레드 park는 0.4~0.7회다. select!가 Pending을 낸 직후
  stack task가 signal을 올려 깨우는 pass는 arm의 DOCA syscall만 소비한다.

남은 레버와 위험:

- h2 연결 task를 `tokio::task::unconstrained`로 spawn하면 coop `defer`가 사라진다.
  spawn 지점은 vendored linkerd의 `linkerd/proxy/http/src/h2.rs`와
  `linkerd/app/src/dmesh.rs` 두 곳이다. task 공정성을 끄는 변경이므로 입력이 drain
  예산으로, 출력이 chunk로 유계임을 근거로 두되 별도 A/B 없이는 적용하지 않는다.
- arm 전에 stack task에 한 번 더 양보하고 다시 drain하면 헛 arm이 준다.
- host doorbell을 main 스레드가 소유하는 구조는 wake write를 남긴다. 소유권 설계 항목이다.
- 요청당 runtime pass 약 21회와 Linkerd service stack 깊이는 그대로다.

목표는 100 RPS에서 200 ARM µs/RPC 이하, closed-loop single request p50 0.7 ms 미만,
knee regression 없음이다.

## 13.3 남은 session cost attribution

상태: DEFERRED.

per-workload stack sharing으로 session당 ARM time은 약 3.9ms에서 0.4~0.5ms로
줄었다. 동기 stack build 중 약 148.4µs는 계측됐고, 남은 것은 policy/destination
discovery, reconnect layer, endpoint/balancer construction 등 asynchronous
boundary다.

SessionMetrics 하나를 확장해 원인을 분해하며 별도 metric surface를 만들지 않는다.

## 13.4 Linkerd TX 경로: arena batching과 고정비

상태: 구현과 hardware A/B 완료. 전송 계약은 [design/DATA.md §4.3](design/DATA.md)과
§18.1이 정의하고, 남은 항목은 이 절 끝의 목록이다.

Linkerd 출력은 endpoint별 미게시 64 KiB arena chunk에 한 번 복사되고 driver pass가
chunk 단위로 DMA 또는 peer custody로 넘긴다. 이 경로의 비용은 네 번의 같은 rig A/B로
확정했다. 각 단계는 직전 단계 binary와 같은 날 같은 geometry에서 비교한 값이며, 단계
사이 곱은 다른 날 캠페인을 잇는 것이라 rig 편차를 포함한다.

| 단계 | receipt | gRPC capacity CPU/RPC 변화 64 B / 1 KiB / 8 KiB / 64 KiB | 무엇이 바뀌었나 |
|---|---|---:|---|
| Vec → direct | [direct-tx-rdma-20260905](bench/report/data/direct-tx-rdma-20260905/SUMMARY.md), [direct-tx-diagnosis-20260906](bench/report/data/direct-tx-diagnosis-20260906/SUMMARY.md) | −0.2 / +3.4 / +2.5 / +5.2% | 중간 Vec 제거, write마다 즉시 publication |
| direct → batch | [arena-tx-batching-perf-20260906](bench/report/data/arena-tx-batching-perf-20260906/SUMMARY.md) | −1.0 / −3.6 / −2.7 / −3.2% | endpoint별 chunk에 여러 write를 모아 pass마다 publication |
| batch → lean | [tx-fixed-overhead-20260906](bench/report/data/tx-fixed-overhead-20260906/SUMMARY.md) | −4.4 / −3.4 / −2.9 / −3.8% | writer quota·grant 삭제, endpoint lock 통합, waker/thread 캐시, dirty 비트와 worker signal |
| lean → lean3 | [park-arm-20260906](bench/report/data/park-arm-20260906/SUMMARY.md) | −2.2 / −2.4 / −1.3 / −3.0% | maintenance timer 고정, 울린 PE만 clear, wake eventfd 1회 읽기 |

측정이 확정한 사실:

- Linkerd 경로의 payload 복사는 staging→h2 buffer와 h2 buffer→arena 두 번이다. 제거된
  Vec 복사는 캐시에 있는 버퍼 사이의 복사였고, DMA가 읽고 간 cold arena로의 복사가 남았다.
  64 KiB에서 memcpy 비중은 6.41→5.60%(−0.8pp)만 줄었다. 복사 횟수가 아니라 cold 복사
  수가 비용을 정한다. 복사에서 더 얻으려면 TX가 staging 참조 piece를 publish하거나
  RX 복사를 없애야 한다.
- write당·pass당 고정비는 64 B에서는 보이지 않고 64 KiB에서 보인다. publication은
  호스트 도착 단위(약 9.5 KB)에 묶이므로 64 KiB 요청 하나가 write와 pass를 약 7회 만든다.
- 64 KiB PMU는 instructions/RPC와 cycles/RPC가 단계마다 같이 줄었다. batch까지는
  instruction이 늘고 cycle이 줄어 stall 성격이었고, lean 이후로는 instruction 자체가 준다.
- opaque TCP는 요청당 CPU가 작아 syscall 절감이 크게 드러난다(lean→lean3 −9.7~−12%).
  gRPC의 capacity RPC/s 변화는 작고 64 B/8 KiB는 3회 범위가 겹친다. 판정은 CPU/RPC 범위
  분리로 한다.
- 측정 중 host(rapids4)에서 빌드나 테스트를 돌리면 64 KiB capacity가 8% 떨어진다.
  host가 부하 생성기이므로 캠페인 동안 host 작업은 금지한다. direct 64 KiB 재측정
  3회 산포가 13%였으므로 3회 중앙값 비교는 범위 분리를 함께 본다.
- 09-05 이전부터 있던 opaque 8 KiB/64 KiB capacity drain 실패와 고동시성 heap
  corruption은 이 작업이 해결하지 않았다.

열린 결함: DPU 런타임이 로그·dmesg·journal·core 없이 사라진 사례가 두 번 있다
(batch binary는 controller feed가 새 Pod를 싣기 전 첫 dial이 거절돼 세션 8개가
poison된 직후, lean2 binary는 run 전환 직후). sudo journal에 kill 기록이 없고 main
루프는 `while (true)`라 정상 return이 불가능하다. `dpu_main.c`가 fatal signal
backtrace와 `atexit` 흔적을 stderr에 남기므로 다음 발생 때 경로가 남는다. 배포
harness는 rollout 뒤 5초를 기다리고 warmup을 최대 4회 재시도한다.

남은 일:

- [ ] TX가 staging 범위 안의 payload `Bytes`를 복사 대신 staging 참조 piece로 publish
      하도록 vendored h2 `FramedRead`가 staging에서 직접 `Bytes`를 만들고 RX custody를
      TX 완료까지 연장하는 설계.
- [ ] §13.2의 tokio coop `defer`와 헛 arm 레버 A/B.
- [ ] DPU 런타임 조용한 종료의 원인.
- [ ] opaque 8 KiB/64 KiB capacity drain 실패.
- [ ] 정리된 트리(reservation API 제거, driver yield 복원)의 HTTP/policy hardware
      correctness와 성능 재확인. 정리 뒤 재측정은 하지 않았다.

## 13.5 ARM worker 16개와 control CPU

상태: OPEN.

현재 array는 MAX_ARM_WORKERS=16을 허용하지만 main/control thread가 CPU index A에
pin된다. 허용 CPU 16개에서 A=16이면 worker 0과 충돌할 수 있다. A=12까지는
측정됐고 130K clean RPC/s에서 worker balance가 양호했다.

A=16 전에 main/control 전용 CPU를 예약하고 control progress가 worker load에
굶지 않는지 검증한다.

## 13.6 Matched-core Linkerd 비교

상태: DEFERRED.

기존 4.3×/4.6×/2.2× 최대 RPS 비교는 DPUmesh ARM worker 8개와
LINKERD2_PROXY_CORES=1 sidecar를 비교했다. absolute system result로는 유효하지만
per-configured-core 비교는 아니다.

향후:

- 양 Pod Linkerd proxy CPU limit 4
- direct TCP 10K control
- absolute와 per-core 결과 함께 보고

## 13.7 Node density

상태: DEFERRED.

- MAX_PODS wire ceiling: 127
- MAX_DPA_RINGS: EU당 16
- 현재 BlueField N: 32
- default K: 2
- benchmark throughput geometry K: 8

검증된 범위는 K=8에서 32 Pods, K=2에서 한 Service 48 Pods다. 127은 이론적
wire ceiling이지 검증 claim이 아니다.

127 근처에서는 다음이 먼저 문제가 될 수 있다.

- DPA poll당 ring control block scan
- Pod당 약 64MiB DPU staging의 high-watermark, 총 약 8GiB
- registration churn

127 초과는 pod/service id wire field 확대와 ABI bump가 필요하다. 실제 deployment
요구가 생기기 전에는 하지 않는다.

## 13.8 Incremental topology generation

상태: DEFERRED.

현재 Pod 하나의 churn도 전체 generation을 다시 sign/publish/fetch/verify/parse한다.
single-digit node rig에서는 dominant cost가 아니다.

향후 delta는:

- signed base version
- add/remove record
- strictly increasing version
- base가 없거나 실패하면 full generation fetch
- periodic full anchor
- refused delta가 held generation을 지우지 않음

fleet size와 churn rate에서 full republish가 dominant가 될 때 bytes/event와 DPU
parse time으로 정당화한다.

## 13.9 ARM/x86 equivalent study

상태: DEFERRED.

논문을 위한 비교이며 deployment blocker가 아니다. 같은 Linkerd revision,
control plane, compiler, flags, request size, concurrency로 x86 full-stack을 만들고
cycles, instructions, cache miss, migration, context switch, latency, throughput을
비교한다.

## 13.10 eBPF redirection

상태: DEFERRED.

eBPF는 static/Go binary의 투명한 syscall redirection 후보지만 resource ownership,
shared-memory transport, identity 문제를 해결하지 않는다. Go explicit API가
실용적이지 않다는 증거가 생길 때만 조사한다.

## 13.11 Cross-node secondary destination

상태: OPEN. 설계는 §11에 고정됐고 구현이 남아 있으며 P2에 포함한다.

현재 한 stream의 두 번째 remote destination은 peer-pin-conflict로 거부된다.
protocol-aware connection에서 request별 endpoint가 여러 node로 갈 수 있도록
§11.6의 destination-keyed pin map과 exact Pod-pair association을 구현한다.

## 13.12 Traffic-secret refresh

상태: OPEN. 설계는 §11에 고정됐고 구현이 남아 있으며 P2에 포함한다.

§11.14의 1 GiB/30분 TLS KeyUpdate, 5초 failure boundary, certificate rotation 시 새
association+bounded drain 계약을 구현하고 시험한다.

---

# 14. 이미 확인한 결과와 다시 하지 않을 추측

## 14.1 Broker와 host transport

[broker-design-20260828/SUMMARY.md](bench/report/data/broker-design-20260828/SUMMARY.md)
및
[doorbell-relay-20260901/SUMMARY.md](bench/report/data/doorbell-relay-20260901/SUMMARY.md)의
결과:

- memfd MAP_SHARED 전환은 반복 ABA에서 성능 중립이었다.
- shared broker는 simultaneous wake를 직렬화해 tail을 키웠다.
- per-Pod broker는 wake delay가 Pod 수에 따라 늘지 않았다.
- 최종 broker는 dispatch하지 않고 doorbell만 relay한다.
- workload drain의 10~100µs exponential backoff가 busy period의 broker wake를
  줄였다.
- broker build는 legacy in-process PE보다 host CPU와 conc32 성능이 나아졌다.

따라서 shared broker로 합치거나 broker에 reverse ring parsing을 다시 넣는 것은
새로운 명확한 증거 없이는 하지 않는다.

## 14.2 gRPC와 Linkerd

[grpc-professor-20260902/FINAL.md](bench/report/data/grpc-professor-20260902/FINAL.md)의
단일 node receipt:

| frame | DPUmesh closed-loop median | Linkerd sidecar | 비율 |
|---|---:|---:|---:|
| 64B | 106.8K RPS | 25.0K | 4.3× |
| 1KiB | 89.1K RPS | 19.3K | 4.6× |
| 8KiB | 34.3K RPS | 15.2K | 2.2× |

이 수치는 해당 build와 closed-loop 조건의 결과다. D4가 닫히기 전 운영 capacity나
모든 배포의 upper bound로 일반화하지 않는다.

확인된 원인:

- gRPC adapter의 per-write flush가 transport coalescing을 깨뜨렸고 제거됐다.
- embedded Rust staticlib는 자동으로 Linkerd의 allocator를 상속하지 않았으며
  jemalloc과 LTO가 유효했다.
- 작은 frame의 dominant cost는 Host core가 아니라 ARM worker의 fixed per-request
  work다.
- backend registry lock은 측정상 contention이 없었다.
- per-request endpoint selection은 matched-rate에서 유의미한 data-path 비용 차이가
  없었다.
- session stack sharing은 churn cost를 크게 줄였고 steady request cost는 바꾸지
  않았다.

## 14.3 Worker spin에 대한 기존 교훈

internal poll이 event가 아니라 지속 state로 Ready를 반환하면 runtime driver가
notification fd를 progress할 기회를 잃는다. 이때 worker CPU는 99.9%인데 admin
listener까지 bound-and-unanswered가 된다. FIN steady state를 outstanding work로
세지 않고, unpublishable state는 maintenance period로 bound하는 guard가 이미
있다.

D4는 이 과거 bug와 signature가 같은지부터 검사하되 같다고 가정하지 않는다.

## 14.4 Harness 규칙

- reply에 rcnt가 없으면 nodata이고 FAIL이다.
- unreadable counter는 0이 아니라 NA다.
- policy/routing은 client 결과와 DPU verdict가 모두 필요하다.
- Pod를 재생성한 뒤 measurement 전에 다시 pin한다.
- Running이 아니라 Ready를 기다린다.
- geometry는 환경 기본값이 아니라 running DPU banner에서 읽는다.
- open-loop clean capacity와 closed-loop plateau를 섞지 않는다.
- evidence capture 실패가 cleanup/deploy를 막지 않게 하되 결과에는 실패를 남긴다.

---

# 15. 시험과 receipt 규율

## 15.1 기본 gate

~~~text
make test

ctest --test-dir build/grpc-release --output-on-failure

ASAN_OPTIONS=detect_leaks=0 \
UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
ctest --test-dir build/grpc-asan-clang --output-on-failure

cd linkerd/rust
cargo test --locked
~~~

hardware correctness는 배포를 변경하므로 명시적으로 해당 campaign을 실행할 때만:

~~~text
bash bench/suite/grpc_correctness.sh hardware
~~~

## 15.2 Receipt directory

새 campaign은 최소한 다음을 보존한다.

~~~text
bench/report/data/<campaign>-<date>/
├─ SUMMARY.md
├─ commands.txt
├─ environment.txt
├─ stages.csv
├─ counters-before.txt
├─ counters-after.txt
├─ dpu-banner.txt
├─ pod-state.txt
└─ raw/
~~~

environment에는 commit, dirty state, submodule revision, N/K/A, CPU pin, clock,
DOCA/BFB/firmware, Kubernetes node와 Pod image를 포함한다.

## 15.3 판정 규칙

1. stage마다 기대한 application 결과와 DPU counter를 함께 정의한다.
2. 아무 응답이 없는 것은 expected refusal이 아니다.
3. restart, drop, pending, residue를 숨기고 throughput만 PASS할 수 없다.
4. 같은 날, 같은 topology, 같은 clock, 반복 표본으로 비교한다.
5. 변경 직후 첫 warm run은 성능 판정에서 제외한다.
6. raw data는 수정하지 않고 derivation script를 보존한다.
7. 실패한 실험도 삭제하지 않고 rejected 이유를 붙인다.
8. hardware blocker와 software failure를 구분한다.

---

# 16. 최종 실행 순서

## Phase A — 현재 안정성

- [ ] current B1/B2/B5/B7 baseline receipt
- [ ] worker snapshot과 quiescence gate
- [ ] fresh deploy reproducer
- [ ] worker root cause fix
- [ ] 6/12/24시간 soak
- [ ] current capacity re-baseline

## Phase B — Kubernetes 구조 구현과 동결

- [x] per-Pod broker와 단일 host `dpumeshd` placement 확정
- [x] Kubernetes 밖의 DPU/`dpumeshd`, Device Plugin, 명시적 workload와 Restricted
  namespace 구조 확정
- [x] `dpumeshd` package, Device Plugin과 delegated worker supervisor 구현
- [x] controller node-mTLS/WorkloadGrant, workload image/resource 전환과 webhook 제거
- [ ] target broker lifecycle B1~B7
- [ ] current node-agent/RBAC/hostPath/transient-unit 제거
- [ ] process restart/channel fatal contract 확정
- [ ] control/data lifecycle diagram과 코드 일치
- [ ] public C API freeze
- [ ] IPC/L7/peer internal contract freeze
- [ ] workload identity와 certificate granularity 확정

## Phase C — cross-node

- [ ] TCP workload mTLS와 fault injection
- [ ] node B와 RDMA fabric
- [ ] two-node control/identity
- [ ] remote stream과 custody
- [ ] lifecycle/policy/rotation
- [ ] performance receipt

## Phase D — integrations

- [ ] Go native transport
- [ ] net.Conn/net.Listener
- [ ] Go gRPC
- [ ] 다른 언어는 같은 conformance contract로 추가

## Phase E — 최적화와 규모

- [ ] ARM per-event cost
- [ ] session cost attribution
- [ ] Linkerd TX 중간 복사 제거 — direct와 arena batching 구현·배포·A/B 완료. 8 KiB gRPC capacity -2.17%로 성능 acceptance OPEN (§13.4)
- [x] RDMA 공통 TX/RX buffer lease — API·상태 시험·배포·BlueField CPU 비교 완료; 실제 fabric gate 별도 (§11.20.3~§11.20.4)
- [ ] TLS BIO↔RDMA direct 연결 — RX부터 적용, TX pending/재시도 보존 (§11.20.5~§11.20.9)
- [ ] A=16 control CPU
- [ ] matched-core sidecar comparison
- [ ] density beyond measured range
- [ ] incremental topology
- [ ] ARM/x86 study

Phase를 건너뛰려면 선행 phase가 필요 없다는 증거와 새 acceptance gate를 이 문서에
먼저 기록한다. 코드부터 바꾸고 이유를 나중에 맞추지 않는다.

---

# 17. 프로젝트 완료의 의미

연구 prototype 전체를 완료라고 부르기 위한 최소 조건:

- workload Pod는 PSS Restricted 아래 `hostPath`, privilege와 device 없이 admission부터
  teardown까지 동작한다.
- per-Pod broker failure는 다른 Pod에 전파되지 않고, kubelet/controller restart는
  existing data channel을 유지하며, `dpumeshd`/DPU restart는 node 범위에서 bounded
  fail-stop한다.
- worker progress가 fresh/aged deployment에서 재현 가능하다.
- public API와 내부 ABI가 문서와 contract test로 고정된다.
- same-node는 DMA isolation, cross-node는 workload mTLS라는 경계가 실제 packet과
  fault test로 증명된다.
- 두 실제 DPU node에서 data, policy, churn, recovery, rotation이 동작한다.
- native, preload, C++ gRPC가 같은 registration/security path를 사용한다.
- Go integration도 그 path를 재사용한다.
- 성능 수치는 topology, instrument, 반복과 failure 조건을 함께 공개한다.

그 전까지 가장 정확한 표현은 다음이다.

> DPUmesh는 single-node DPU service-mesh data path와 control plane이 구현·실증된
> research prototype이다. per-Pod broker의 완전한 failure receipt, stable worker
> progress, workload-identity cross-node mTLS와 real two-node RDMA receipt는 진행
> 중이다.

---

# 18. Working tree 전송 계약

이 절은 working tree의 data-path 구현과 검증 상태를 정의한다. 세부 ownership과
state machine은 [design/DATA.md](design/DATA.md)를 따른다.

## 18.1 Linkerd arena TX batching

각 `DmeshIo` endpoint는 최대 하나의 미게시 batch를 소유한다. batch는 C data plane이
빌려준 64 KiB 등록 arena chunk이며 process-unique token으로 이름 붙는다. scalar와
vectored write는 caller slice의 ordered prefix를 그 chunk에 한 번 복사하며, 소유
worker thread에서 endpoint lock을 쥔 채 `dmesh_l7_tx_batch_write`를 부른다. Rust는
token, 길이, exact route와 sealed 상태만 보관하고 caller pointer나 arena pointer는
보관하지 않는다. 첫 accepted write가 route를 고정하고, 같은 request connection을
쓰는 origin, local backend, remote backend endpoint도 각자 batch를 가진다.

batch가 찼거나 chunk가 없으면 write는 아무것도 받지 않고 writer를 park한다.
accepted write는 endpoint를 dirty로 표시하고 worker signal을 올린다. `poll_flush`는
latch된 오류를 보고할 뿐 즉시 반환한다. `poll_shutdown`은 admission을 닫고 driver가
buffered DATA를 게시한 뒤 FIN을 게시한다.

driver pass는 C engine을 drain하고, dirty이거나 재시도를 빚진 endpoint마다
`dmesh_l7_tx_batch_flush`로 batch를 게시하고, buffered batch 없이 park된 writer를
깨우고, 소비된 staging custody를 돌려주고, write half를 닫고 buffered batch가 없는
endpoint의 FIN을 순서대로 게시한다. 거절된 publication은 batch와 bytes를 유지하고
다시 복사하지 않는다. pass는 session 64개, 게시 256 KiB까지 방문한 뒤 stack에 양보한다.
일이 없는 pass는 completion과 DMA notification을 arm하고 한 번 더 drain한 뒤 그
descriptor, wake eventfd, worker signal, timer entry 하나로 잡은 1 ms maintenance
마감을 기다린다. wait 뒤에는 울린 source만 clear한다. abort와 connection close는
미게시 batch만 반환하고, 게시된 chunk는 DMA unit 또는 peer stream이 완료 경로에서
돌려준다.

| Metric | 의미 |
|---|---|
| `tx_arena_copy_bytes` | arena batch에 복사된 bytes |
| `tx_accepted_bytes` | DMA 또는 peer custody로 이전된 bytes |
| `tx_publications` | custody가 이전된 batch 수 |
| `tx_retries` | 가득 찬 batch·빈 arena로 거절된 write와 거절된 publication |
| `tx_errors` | terminal TX error |

## 18.2 RDMA carrier lease

`peer_wire_ops`는 whole-message API와 complete optional lease group을 제공한다.
TX group은 reserve/commit/cancel, RX group은 acquire/release로 구성되며 일부 callback만
등록한 carrier는 거부된다. RDMA connection은 등록 buffer의 16개 SEND slot과 32개
RECV slot을 소유한다.

```text
TX: FREE → RESERVED → POSTED → FREE
RX: POSTED → READY → LEASED → POSTED
```

각 방향에는 outstanding lease가 하나만 존재한다. token은 process-unique connection
epoch, slot, per-slot generation과 방향을 검증하며 pointer와 capacity 또는 length도
일치해야 한다. invalid token은 slot 상태를 바꾸지 않는다. SEND completion은 local
slot 재사용만 허용한다. application delivery custody는 destination `REV_DONE` 뒤의
`STREAM_ACK`에서 반환된다.

## 18.3 검증 상태

- `make test` 전체 통과
- Linkerd adapter Rust 41/41 통과
- `dmesh-doca` 30/30 통과
- gRPC release 및 ASAN+UBSAN 각각 4/4 통과
- C proxy-lane ASAN+UBSAN 통과
- 93개 hardware 표본에서 application failure, drop, overflow, worker failure와
  reorder 0
- 모든 hardware arm 종료에서 arena 1,024/1,024 free, live chunk 0

64 KiB gRPC capacity는 12,905 RPC/s, CPU 599.50 us/RPC, p99 7,610 us다.
8 KiB capacity는 21,949 RPC/s이며 performance acceptance는 OPEN이다. 측정 방법,
반복 범위, publication 분포와 PMU 결과는
[arena TX batching hardware receipt](bench/report/data/arena-tx-batching-perf-20260906/SUMMARY.md)에
보존한다.
