FROM ubuntu:22.04

RUN apt-get update && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y ca-certificates numactl && \
    rm -rf /var/lib/apt/lists/*

# Matched pure-C TCP-baseline client using the bench.h wire frame.
# Reads BENCH_TARGET and CTRL_PORT.
COPY build/bin/bench_sock /usr/local/bin/bench_sock
COPY bench/docker/numa-entrypoint.sh /usr/local/bin/numa-entrypoint.sh

EXPOSE 9092

ENTRYPOINT ["/usr/local/bin/numa-entrypoint.sh", "/usr/local/bin/bench_sock"]
