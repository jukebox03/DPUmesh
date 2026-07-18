FROM ubuntu:22.04

RUN apt-get update && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y ca-certificates && \
    rm -rf /var/lib/apt/lists/*

# The matched pure-C greeter server (bench.h wire frame). Replaces the former Go
# server. Reads ECHO_PORT (listen port); the echo-tcp pod sets it to 9092 so
# sidecar2 forwards ${TCP_PORT} -> 127.0.0.1:9092 exactly as before.
COPY build/bin/echo_sock /usr/local/bin/echo_sock

EXPOSE 9092

ENTRYPOINT ["/usr/local/bin/echo_sock"]
