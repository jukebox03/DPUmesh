FROM python:3.12.5-slim-bookworm

RUN pip install --no-cache-dir cryptography && \
    addgroup --system --gid 65532 dpumesh && \
    adduser --system --uid 65532 --ingroup dpumesh --no-create-home dpumesh

COPY controller/dpumesh_controller.py /usr/local/bin/dpumesh-controller
COPY controller/workload_grant.py /usr/local/bin/workload_grant.py
RUN chmod 0555 /usr/local/bin/dpumesh-controller /usr/local/bin/workload_grant.py

USER 65532:65532
ENTRYPOINT ["/usr/local/bin/dpumesh-controller"]
