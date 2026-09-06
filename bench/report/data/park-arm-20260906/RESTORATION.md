# Restoration

- 실행 중 DPU PID 3533612의 binary SHA256은 campaign 전과 같은 direct
  `2200fc9207e01b7e07e3aa0d45f26d55fef6f9aee07fb4a61cdfd3169389422e`다.
- `/home/jukebox/DPUmesh/doca/build/dpumesh_dpu`도 direct로 되돌렸다. lean2/lean3 빌드는
  아래 scratch 정리로 DPU에 남아 있지 않다.
- DPU는 `DPUMESH_L7_SVC=''`, `DPUMESH_L7_OPAQUE_SVC=''`, N/K/A=32/8/8인 L4 profile이다.
- native Deployment 두 개와 `echo-dpumesh-native` Service를 campaign 전 spec으로 재적용했고
  두 Pod가 Ready, restart 0이다. native 64 B smoke 163,605 RPC/s, fail 0.
- 이 campaign의 28086/28087/28088 control relay는 종료했고 port는 비어 있다. 28089
  controller relay는 유지했다.
- 모든 L7 arm 종료 전 arena 1,024/1,024 free, live chunk 0을 확인했다.

DPU scratch 정리(사용자 승인 뒤): `/tmp/park-arm-20260906/`(1.9 GB)와
`/tmp/tx-fixed-overhead-20260906/`(93 MB)를 삭제했다. lean/lean2/lean3 binary는 DPU에
남아 있지 않다. 이전 campaign의 `/tmp/arena-tx-batching-perf-20260906/`(direct/batch
binary)와 `/tmp/direct-tx-diagnosis-20260906/`는 그대로다. DPU root filesystem은
92% 사용(9.9 GB 남음)이며 DPU kubelet의 ephemeral-storage eviction 로그는 이 campaign과
무관하게 계속된다.
