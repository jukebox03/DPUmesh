FROM python:3.12.5-slim-bookworm

COPY bench/workload_attest_agent.py /usr/local/bin/dpumesh-workload-agent
RUN chmod 0555 /usr/local/bin/dpumesh-workload-agent

USER 0:0
ENTRYPOINT ["/usr/local/bin/dpumesh-workload-agent"]
