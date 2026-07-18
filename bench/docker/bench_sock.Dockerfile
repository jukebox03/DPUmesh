FROM ubuntu:22.04

RUN apt-get update && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y ca-certificates && \
    rm -rf /var/lib/apt/lists/*

# The matched pure-C TCP-baseline client (bench.h wire frame). Replaces the former
# Go client so the TCP baseline and the DPUmesh native client are the SAME language,
# isolating the transport rather than the runtime. Reads BENCH_TARGET (host:port of
# the sidecar) and CTRL_PORT (control listener, default 9092).
COPY build/bin/bench_sock /usr/local/bin/bench_sock

EXPOSE 9092

ENTRYPOINT ["/usr/local/bin/bench_sock"]
