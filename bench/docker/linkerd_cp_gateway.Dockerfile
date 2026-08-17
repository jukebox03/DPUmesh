FROM python:3.12.5-slim-bookworm

COPY bench/linkerd_cp_relay.py /usr/local/bin/dpumesh-linkerd-cp-gateway
RUN chmod 0555 /usr/local/bin/dpumesh-linkerd-cp-gateway

USER 0:0
ENTRYPOINT ["/usr/local/bin/dpumesh-linkerd-cp-gateway"]
