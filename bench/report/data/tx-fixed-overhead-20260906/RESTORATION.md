# Restoration

- 실행 중 DPU PID 3456922의 binary SHA256은 campaign 전과 같은 direct
  `2200fc9207e01b7e07e3aa0d45f26d55fef6f9aee07fb4a61cdfd3169389422e`다
  (`raw/initial-binary.txt`, `raw/restored-binary.txt`).
- `/home/jukebox/DPUmesh/doca/build/dpumesh_dpu`도 direct로 되돌렸다. lean 빌드 결과는
  `/tmp/tx-fixed-overhead-20260906/lean`(SHA256 `1577c3cc…`)에만 남아 있다.
- DPU는 `DPUMESH_L7_SVC=''`, `DPUMESH_L7_OPAQUE_SVC=''`, N/K/A=32/8/8인 L4 profile이다.
- campaign 이전 native Deployment 두 개의 Pod template·replica와 `echo-dpumesh-native`
  Service spec을 그대로 재적용했고 두 Pod가 Ready, restart 0이다(`raw/restored-workloads.json`).
- native 64 B smoke: 169,523 RPC/s, fail/drop/overflow/worker_fail/reorder 0.
- 이 campaign이 시작한 28086/28087/28088 control relay는 종료했고 port는 비어 있다.
  기존 28089 controller relay는 유지했다.
- trace instance `txfo_20260906`와 uprobe는 각 trace window 뒤 제거했다.
- 모든 L7 arm 종료 전에 arena 1,024/1,024 free, live chunk 0을 7회 확인했다
  (`raw/quiescence-*-arena.txt`).

DPU scratch 정리(사용자 승인 뒤, park-arm campaign과 함께): `/tmp/tx-fixed-overhead-20260906/`을
삭제했다. lean binary는 DPU에 남아 있지 않다. DPU `/home/jukebox/DPUmesh/bench/report/data/
tx-fixed-overhead-20260906/arena_offsets.c`는 남아 있다.
