FROM ubuntu:22.04

RUN apt-get update && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y \
      ca-certificates libibverbs1 libnl-3-200 libnl-route-3-200 \
      libyaml-0-2 libsasl2-2 openssl && \
    rm -rf /var/lib/apt/lists/*

COPY build/bin/bench_sock /usr/local/bin/bench_sock
COPY build/bin/echo_sock /usr/local/bin/echo_sock
COPY build/lib/libdmesh_preload.so /usr/local/lib/libdmesh_preload.so
COPY doca-libs/ /usr/local/lib/
COPY build/lib/libdpumesh.so.4 /usr/local/lib/
COPY bench/k8s/registry /etc/dpumesh/registry

RUN ldconfig
ENV LD_LIBRARY_PATH=/usr/local/lib
EXPOSE 9092 9100
