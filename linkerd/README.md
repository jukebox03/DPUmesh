# linkerd on DPUmesh

DPU ARM에서 도는 linkerd2-proxy를 DPUmesh 데이터패스 위에 올리는 작업 디렉터리.
`integrations/` 아래가 아니라 최상위에 있는 이유는 **코어 구현**이기 때문이다 —
DPUmesh의 mock L7(`doca/dpu_l7.c`)을 실제 L7 프록시로 대체한다.

| 문서 | 내용 |
|---|---|
| [`PLAN.md`](PLAN.md) | 통합 설계: seam 판정, 스레드 모델 before/after, 삽입 지점, 마일스톤 |
| [`CONTRACT.md`](CONTRACT.md) | 포팅 담당자와의 인터페이스 계약 — wire ABI 정본 + 8함수 |

## 레이아웃

```
linkerd/
  port/          서브모듈 youngmin-kaist/DPUMesh (main)
    DPUMesh/       포팅 쪽 C 데이터패스 — ABI 정합의 기준점, 최종적으로 대체됨
    linkerd2-proxy/  중첩 서브모듈 youngmin-kaist/linkerd2-proxy (dpumesh)
                     └ linkerd/doca/  ← 실제 접합면 (shim.c / driver.rs / io.rs)
  bench/         6열 캠페인 (Envoy×2 + linkerd×2 + TCP + DPUmesh). 호스트 사이드카
                 baseline이며 DPU 위 linkerd가 아니다. REPORT_LINKERD.md 참조
```

## 협업자 코드 가져오기

포팅은 원격에서 다른 사람이 작업한다. 중첩 서브모듈까지 한 번에 갱신한다.

```sh
git submodule update --remote --recursive linkerd/port
git -C linkerd/port submodule update --remote linkerd2-proxy   # 개별 갱신
git add linkerd/port && git commit -m "Bump linkerd port"       # 핀 고정
```

처음 클론할 때:

```sh
git submodule update --init --recursive linkerd/port
```

중첩 서브모듈의 URL이 SSH(`git@github.com:`)라 접근 권한이 없으면 실패한다.
로컬에서 https로 재작성해 두면 된다(저장소 로컬 설정, `.gitmodules`는 건드리지 않음):

```sh
git -C linkerd/port config url."https://github.com/".insteadOf "git@github.com:"
git -C linkerd/port config submodule.linkerd2-proxy.url \
    https://github.com/youngmin-kaist/linkerd2-proxy.git
```

## 현재 상태

- 포팅 쪽 HEAD: `DPUMesh@785bc1a` / `linkerd2-proxy@4f926826` (둘 다 2026-07-22, 이후 진전 없음)
- Rust 절반(`DmeshIo`, `Driver`, dmesh 억셉터, `DmeshOrTcp`)은 완성. 인바운드·흐름제어·
  다중 워커는 미완 — `PLAN.md` §9
- DPUmesh 쪽은 아직 코드 없음. `CONTRACT.md` §⑥의 D/E 항목이 이쪽 몫
