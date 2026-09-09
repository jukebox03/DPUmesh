# Build context created on the DPU by build-dpu-image.sh. Includes the ARM
# executable and its actual shared-library dependencies, never host keys/config.
FROM ubuntu:22.04
COPY rootfs/ /
ENV LD_LIBRARY_PATH=/usr/local/lib/dpumesh
ENTRYPOINT ["/usr/local/bin/dpumesh-dpu-entrypoint"]
