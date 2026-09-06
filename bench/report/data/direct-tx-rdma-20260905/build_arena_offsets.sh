set -euo pipefail
set -a
source .env
set +a
receipt=bench/report/data/direct-tx-rdma-20260905
ssh "$DPU_HOST" "mkdir -p /home/jukebox/DPUmesh/$receipt"
scp -q "$receipt/arena_offsets.c" "$DPU_HOST:/home/jukebox/DPUmesh/$receipt/arena_offsets.c"
ssh "$DPU_HOST" 'cd /home/jukebox/DPUmesh && cc -O2 -D_GNU_SOURCE -DDOCA_ALLOW_EXPERIMENTAL_API -DDMESH_L7_RUNTIME_OWNER=1 -Iinclude -I. -Ilinkerd/include $(pkg-config --cflags doca-common doca-comch doca-dpa doca-dma) -ffunction-sections -fdata-sections -Wl,--gc-sections bench/report/data/direct-tx-rdma-20260905/arena_offsets.c -o /tmp/direct-arena-offsets $(pkg-config --libs doca-common) && /tmp/direct-arena-offsets > /tmp/arena-offsets.json && cat /tmp/arena-offsets.json' > "$receipt/raw/arena-offsets.json" 2> "$receipt/raw/arena-offsets-build.log"
