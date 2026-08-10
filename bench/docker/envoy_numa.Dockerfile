ARG ENVOY_BASE=envoyproxy/envoy:v1.30-latest
FROM ${ENVOY_BASE}

USER root
RUN apt-get update && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      numactl && \
    rm -rf /var/lib/apt/lists/*

COPY bench/docker/numa-entrypoint.sh /usr/local/bin/numa-entrypoint.sh

ENTRYPOINT ["/usr/local/bin/numa-entrypoint.sh", "/docker-entrypoint.sh"]
