# Build context created on the DPU by build-dpu-image.sh. Includes the ARM
# executable and its actual shared-library dependencies, never host keys/config.
# BASE is the DPU's own Ubuntu release, so the copied libraries match its glibc.
ARG BASE=ubuntu:22.04
FROM ${BASE}
RUN apt-get update && apt-get install -y --no-install-recommends python3 openssl && rm -rf /var/lib/apt/lists/*
COPY rootfs/ /
ENV LD_LIBRARY_PATH=/usr/local/lib/dpumesh
ENTRYPOINT ["/usr/local/bin/dpumesh-dpu-entrypoint"]
