FROM bench/dpumesh-workload-agent:latest

COPY build/bin/dmesh_broker_probe /usr/local/bin/dmesh_broker_probe
ENTRYPOINT ["/usr/local/bin/dmesh_broker_probe"]
