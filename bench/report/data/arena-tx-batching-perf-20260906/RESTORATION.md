# Restoration

- Campaign 이전 native Deployment 두 개의 Pod template와 replica 수를 정확히 복원했다.
- `echo-dpumesh-native` Service spec과 port 9092를 정확히 복원했다.
- 두 native Pod가 Ready이며 restart는 0이다.
- 실행 중 DPU PID 3383809의 binary SHA256은 campaign 전과 같은
  `2200fc9207e01b7e07e3aa0d45f26d55fef6f9aee07fb4a61cdfd3169389422e`다.
- DPU는 `DPUMESH_L7_SVC=''`, `DPUMESH_L7_OPAQUE_SVC=''`, N/K/A=32/8/8인 L4 profile이다.
- native 64 B smoke: 180,780 RPC/s, fail/drop/overflow/worker_fail/reorder 0.
- 이 campaign이 시작한 28086/28087/28088 relay는 종료했다. 기존 28089 controller relay는 유지했다.
- trace instance `atxb_20260906`와 그 event는 제거됐다.
- 각 L7 arm 종료 전에 arena 1,024/1,024 free와 live chunk 0을 확인했다.

근거는 `raw/restored-*`, `raw/quiescence-*`, `raw/relay-process.json`에 있다.
