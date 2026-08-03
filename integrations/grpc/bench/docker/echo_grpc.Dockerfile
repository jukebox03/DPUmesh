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
      openssl && \
    rm -rf /var/lib/apt/lists/*

# Serves grpc.testing.BenchmarkService. BENCH_TRANSPORT selects the listener:
# dmesh accepts through the DPUmesh passive listener, tcp binds ECHO_PORT.
COPY build/grpc-release/echo_grpc /usr/local/bin/echo_grpc
COPY doca-libs/ /usr/local/lib/
COPY build/lib/libdpumesh.so.4 /usr/local/lib/
COPY bench/k8s/registry /etc/dpumesh/registry
COPY bench/docker/numa-entrypoint.sh /usr/local/bin/numa-entrypoint.sh

RUN ldconfig

ENV LD_LIBRARY_PATH=/usr/local/lib
EXPOSE 9091

ENTRYPOINT ["/usr/local/bin/numa-entrypoint.sh", "/usr/local/bin/echo_grpc"]
