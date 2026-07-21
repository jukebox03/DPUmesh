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

COPY build/bin/preload_runner /usr/local/bin/preload_runner
COPY build/bin/tcp_echo /usr/local/bin/tcp_echo
COPY build/bin/tcp_client /usr/local/bin/tcp_client
COPY build/lib/libdmesh_preload.so /usr/local/lib/libdmesh_preload.so
COPY doca-libs/ /usr/local/lib/
COPY build/lib/libdpumesh.so.3 /usr/local/lib/

RUN ldconfig

ENV LD_LIBRARY_PATH=/usr/local/lib
EXPOSE 9092

ENTRYPOINT ["/usr/local/bin/preload_runner"]
