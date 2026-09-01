FROM python:3.12.5-slim-bookworm

# iptables carries the ingress guard: the agent rejects kernel-TCP SYNs to
# mesh-served Pod ports from the host's FORWARD hook.
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
       iptables libibverbs1 libnl-3-200 libnl-route-3-200 libstdc++6 systemd \
    && rm -rf /var/lib/apt/lists/*

RUN pip install --no-cache-dir cryptography

# The agent is the DPU's only control peer: the delivery loop and the absorbed
# control-plane relay travel with it, and the receiver module is copied for the
# one table both ends of the hop read their bounds from.
COPY bench/workload_attest_agent.py /usr/local/bin/dpumesh-workload-agent
COPY bench/feed_delivery.py /usr/local/bin/feed_delivery.py
COPY bench/linkerd_cp_relay.py /usr/local/bin/linkerd_cp_relay.py
COPY bench/dpumesh_feed_receiver.py /usr/local/bin/dpumesh_feed_receiver.py
COPY build/bin/dmesh_broker /usr/local/bin/dmesh_broker
COPY build/lib/libdpumesh.so.5 /usr/local/lib/
COPY doca-libs/ /usr/local/lib/
RUN chmod 0555 /usr/local/bin/dpumesh-workload-agent \
                /usr/local/bin/feed_delivery.py \
                /usr/local/bin/linkerd_cp_relay.py \
                /usr/local/bin/dpumesh_feed_receiver.py \
                /usr/local/bin/dmesh_broker \
    && ldconfig \
    && ! ldd /usr/local/bin/dmesh_broker | grep -q 'not found'

USER 0:0
ENTRYPOINT ["/usr/local/bin/dpumesh-workload-agent"]
