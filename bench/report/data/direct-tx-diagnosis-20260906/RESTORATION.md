**복원과 최종 검증 완료**

Production TX 코드와 기존 binary는 변경하지 않았다. 추가 파일은 이 진단 디렉터리에만 있다.

- 총 114개 표본: clean 72 / profile 12 / trace 24 / PMU 6.
  보존된 모든 reply가 OK이며 fail/drops/overflow/worker_fail=0이다.
- 24개 histogram의 dropped=0, 12개 profile의 lost samples=0.
- after의 모든 표본에서 publication retries=0, accepted bytes와 arena copy bytes가 일치했다.
- 최종 L7 측정 종료 시 8 worker의 session/task/registration/DMA/queue/stall/ACK/FIN
  잔량이 모두 0이었다. arena peak나 free-list 전체 스캔을 대신하는 주장은 하지 않는다.
- `dtx_20260906` trace instance와 uprobe event group이 제거됐음을 확인했다.
- native 두 Deployment의 Pod template과 Service spec이 작업 시작 전 snapshot과 정확히 일치한다.
- 복원 후 DPU PID 3331009의 executable SHA256은
  `2200fc9207e01b7e07e3aa0d45f26d55fef6f9aee07fb4a61cdfd3169389422e`다.
  `DPUMESH_L7_SVC`와 `DPUMESH_L7_OPAQUE_SVC`는 모두 빈 값으로 L4 상태다.
- 두 native workload가 Ready이며 native smoke의 fail/drops/overflow/worker_fail=0이다.
  native reply의 `pending=64`는 측정 cutoff 값이며 잔여 세션 gauge로 해석하지 않는다.
- 이 작업에서 시작한 control relay(28086/28087/28088)를 종료했다.
  작업 전부터 있던 controller relay 28089는 유지했다.
- 진단 Python 파일의 문법과 shell 스크립트 구문을 확인했다. Production 코드 변경이 없어
  전체 unit/build suite를 재실행하지 않았다. 실제 계측·배포·복원 및 데이터 불변조건을 검증했다.

근거: [validation.json](validation.json),
[복원 smoke](raw/restored-native-smoke.txt),
[복원 Pod 상태](raw/restored-native-pods.txt),
[최종 hash/L4/probe 검사](raw/restored-final-check.txt),
[원래 workload](raw/initial-workloads.json),
[복원 workload](raw/restored-workloads.json),
[relay 종료 기록](raw/temporary-relay-cleanup.json).

perf 원본은 `.data.gz`로 lossless 압축했다. 큰 metrics/profile/symbol text도 `.txt.gz`로
보관하며 집계기는 이를 직접 읽는다. 압축 전후 SHA256으로 동일성을 확인했다.
[perf artifact manifest](raw/perf-artifacts.json)와 [text artifact manifest](raw/text-artifacts.json)에
크기와 원본 hash가 있다. CPU profile과 trace를 성능 acceptance 표본으로 섞지 않았다.
