FROM ubuntu:22.04

RUN apt-get update && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y \
      ca-certificates \
      libibverbs1 \
      libnl-3-200 \
      libnl-route-3-200 \
      libyaml-0-2 \
      libsasl2-2 \
      openssl && \
    rm -rf /var/lib/apt/lists/*

COPY build/bin/loopback_dpumesh /usr/local/bin/loopback_dpumesh
COPY doca-libs/ /usr/local/lib/
COPY build/lib/libdpumesh.so.3 /usr/local/lib/
COPY bench/k8s/registry /etc/dpumesh/registry

RUN ldconfig

ENV LD_LIBRARY_PATH=/usr/local/lib

ENTRYPOINT ["/usr/local/bin/loopback_dpumesh"]
