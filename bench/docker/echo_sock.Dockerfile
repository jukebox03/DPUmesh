FROM ubuntu:22.04

RUN apt-get update && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y ca-certificates && \
    rm -rf /var/lib/apt/lists/*

# Matched pure-C greeter server using the bench.h wire frame.
# Reads ECHO_PORT; the echo-tcp pod sets it to 9092.
COPY build/bin/echo_sock /usr/local/bin/echo_sock

EXPOSE 9092

ENTRYPOINT ["/usr/local/bin/echo_sock"]
