FROM ubuntu:22.04

RUN apt-get update && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y ca-certificates && \
    rm -rf /var/lib/apt/lists/*

# Matched pure-C TCP-baseline client using the bench.h wire frame.
# Reads BENCH_TARGET and CTRL_PORT.
COPY build/bin/bench_sock /usr/local/bin/bench_sock

EXPOSE 9092

ENTRYPOINT ["/usr/local/bin/bench_sock"]
