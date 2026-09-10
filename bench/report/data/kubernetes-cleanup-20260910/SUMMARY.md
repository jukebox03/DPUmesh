# Kubernetes 배포 및 코드 정리 완료 기록 — 2026-09-10

Host `rapids4`와 DPU `rapids4-dpu`를 같은 Kubernetes v1.34.11 클러스터에
배치했다. Controller는 유지했고, 등록은 host가 검증한 identity를 paired-DPU
TLS 세션으로 전달하는 단일 경로로 정리했다. Admin과 broker의 기존 수명 관계를
유지했으므로 **admin 재시작 시 broker와 기존 연결도 종료된다.**

## 최종 배치와 권한

| Component | 배치 | 권한 |
|---|---|---|
| Application | `test-bench`의 host Pod 2개 | UID 65532, capabilities 없음, Kubernetes token 없음 |
| DPU runtime | `dpumesh-system`의 DPU DaemonSet Pod | privileged, hostNetwork, 장치 및 설정 hostPath |
| Feed receiver | DPU runtime Pod의 sidecar | UID 65532, token 없음, feeds 디렉터리만 쓰기 |
| DPUmesh controller | 지정 노드 `rapids4`의 Deployment Pod | 비특권, 읽기 전용 Kubernetes RBAC |
| Node admin | host의 `dpumeshd.service` | root, systemd capability/device/CPU/memory 제한 |
| Per-pod broker | admin이 생성한 host process | root supervisor wrapper + UID 65532 데이터 worker |

Wrapper는 admin의 manager cgroup에서 자식 종료를 기다린다. 데이터 worker는
별도 자원 제한 cgroup과 private namespaces에서 실행되며 `CapEff=0`,
`NoNewPrivs=1`, seccomp가 적용돼 있다. 별도 supervisor 서비스나 Pod는 없다.
Host admin의 전용 API 인증서는 Pods 읽기가 가능하고 Pods 생성과 Secrets 읽기는
불가능함을 실제 RBAC 조회로 확인했다.

최종 상태는 controller/app Deployment 각각 1/1, runtime DaemonSet 1/1,
runtime Pod 2/2이며 모두 Ready다. DaemonSet의 `UP-TO-DATE`도 1이다.
기존 `jet1`은 Ready 상태를 유지한다.

- [실제 배포 상태와 RBAC](deployment-state.log)
- [Pod 배치·이미지·securityContext](placement-and-privileges.json)
- [실제 broker PID·UID·capabilities·cgroup](broker-privileges.log)

## 삭제 및 유지한 코드

Controller grant 발급·서명·검증, grant/manager 등록 분기, grant replay cache,
membership feed와 소비 코드, 관련 설정·빌드 항목·테스트를 삭제했다.
`workload_grant` 모듈은 현재 사용하는 canonical identity 및 feed 검증만 남겨
`workload_identity`로 정리했다. Host Pod 배포 설계와 독립 feed-receiver systemd
unit도 제거했다.

Controller의 topology/Service-target 발행, node 등록, workload-scope 중계는
현재 경로에서 사용되므로 유지했다. Admission drain, 세대 fencing, broker
quiesce/cleanup은 실제 사용하는 수명 관리 기능이므로 유지했다.

내부 등록 protocol은 version 5, identity 1433 bytes, request 1497 bytes,
response 36 bytes다. 구 grant 프로토콜과 호환되지 않으므로 host/admin/broker와
DPU는 함께 교체해야 한다. Application IPC version 3과 `libdpumesh.so.5`는
유지했다.

## Linkerd 실제 동작 검증

기존 Linkerd `edge-26.8.4` control plane을 사용했다. DPU의 embedded Linkerd
worker 8개 모두 `/ready` HTTP 200, identity 인증서 발급 성공 1회/실패 0회다.
발급 identity는 `dpu.dpumesh-system.serviceaccount.identity.linkerd.cluster.local`이다.

최종 runtime Pod에서 다음 순서로 검증했다.

1. Native echo Service를 Linkerd opaque 처리 대상으로 설정하고 실제 요청 성공 확인.
2. Linkerd `Server`의 `accessPolicy`를 `deny`로 변경.
   새 요청은 `rcnt=0`, `fail=1`로 차단됐고, DPU의
   `dmesh_control_events_total{kind="inbound",reason="denied"}`가 0에서 1로 증가.
3. `all-unauthenticated`로 복원 후 실제 통신 재검증:
   `rcnt=16120`, `fail=0`, `drops=0`, `worker_fail=0`.
   최종 inbound admitted counter는 4다.

8개 admin endpoint의 control event counter는 공용 값을 노출하므로 합산하지
않는다. Worker별 `dmesh_tx_accepted_bytes_total`에서도 실제 전송을 확인했다.
이는 기능 검증용 짧은 smoke이며 성능 비교 결과가 아니다.

- [최종 통신 결과](final-smoke.log)
- [실제 정책 차단 결과](policy-deny.log)
- [차단 전 metric](policy-before.log), [차단 후 metric](policy-after-deny.log)
- [전체 worker 준비·인증·정책 확인](linkerd-verification.log)
- [최종 worker 0 전체 metric](linkerd-final-4191.log)

Native Pod에는 `linkerd.io/control-plane-ns` label과
`config.linkerd.io/skip-inbound-ports` annotation이 필요했다. 전자는 policy
관찰을 활성화하며, 후자는 stock destination이 존재하지 않는 in-Pod proxy/TLS
listener를 광고하지 않도록 한다. DPU-side policy는 위 deny 실험으로 별도 확인했다.
Linkerd CSR의 CN/DNS SAN도 실제 identity와 일치하도록 수정했다.

`diagnostic-*` 파일은 설정을 수정하기 전의 진단 기록이다. Label 누락 상태에서는
정책이 적용되지 않았고, annotation 누락 상태에서는 endpoint 조회가 실패했다.
이때의 실패 응답은 정책 차단 증거로 사용하지 않았다.

이번 실제 배포 검증 범위는 같은 host의 native opaque 통신과 동적 inbound
정책이다. HTTP/gRPC workload, 두 host 사이의 peer 통신 및 workload mTLS를
이번 배포에서 검증했다고 주장하지 않는다.

## 빌드·검사·문서

다음 검사를 통과했다.

- `make -j8 bench test-hostfree test test-local-registration`:
  [전체 결과](tests.log). 격리된 Python 환경의 cryptography/grpc 의존성을 사용했다.
- Python F401/F821/F841 lint: [결과](python-lint.log).
- 변경 shell script syntax, Helm lint, Kubernetes server dry-run,
  `git diff --check`, 문서 상대 링크 검사: [결과](final-checks.log).

README, CONTROL/API/DATA/GRPC, PLAN, 배포·bench·CI·예제 문서 및 control 그림을
현재 코드와 맞췄다. 연구 제안은 구현 상태를 구분하고 삭제된 grant 관련 현재형
설명을 수정했다. 과거 측정 문서는 측정값을 보존하고 적용 revision을 명시했다.

배포 중 DPU kubelet을 현재 클러스터 버전으로 맞춰 가입시켰고, 기존 단독 runtime
및 feed-receiver 실행을 정리했다. DiskPressure는 재생성 가능한 build/image/apt
캐시를 정리해 해결했다. 실패한 이전 bench Pod와 가입용 bootstrap token 및
임시 파일은 제거했다. 설정 백업은 DPU의 `/var/backups/dpumesh-*`에 보관했다.

현재 배포 가이드: [packaging README](../../../../packaging/README-dpu-kubernetes.md).
최종 설계: [CONTROL](../../../../design/CONTROL.md).
