FROM ubuntu:22.04

RUN apt-get update && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y \
      ca-certificates \
      libibverbs1 \
      libnl-3-200 \
      libnl-route-3-200 \
      libyaml-0-2 \
      libsasl2-2 \
      numactl \
      openssl \
      libasan6 \
      libubsan1 && \
    rm -rf /var/lib/apt/lists/*

# gRPC is linked statically, so only libdpumesh and DOCA are needed at runtime.
# The deployment fixes BENCH_TRANSPORT=dmesh and resolves BENCH_DST_SERVICE
# through the signed DPU topology.
# GRPC_BUILD_DIR selects the app build: build/grpc-release measures,
# build/grpc-asan reproduces a fault under ASan/UBSan.
ARG GRPC_BUILD_DIR=build/grpc-release
COPY ${GRPC_BUILD_DIR}/bench_grpc /usr/local/bin/bench_grpc
COPY doca-libs/ /usr/local/lib/
COPY build/lib/libdpumesh.so.4 /usr/local/lib/
COPY bench/docker/numa-entrypoint.sh /usr/local/bin/numa-entrypoint.sh

RUN ldconfig

ENV LD_LIBRARY_PATH=/usr/local/lib
EXPOSE 9092

ENTRYPOINT ["/usr/local/bin/numa-entrypoint.sh", "/usr/local/bin/bench_grpc"]
