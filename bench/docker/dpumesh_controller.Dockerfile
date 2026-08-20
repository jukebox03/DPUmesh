FROM python:3.12.5-slim-bookworm

RUN pip install --no-cache-dir cryptography

COPY controller/dpumesh_controller.py /usr/local/bin/dpumesh-controller
RUN chmod 0555 /usr/local/bin/dpumesh-controller

USER 0:0
ENTRYPOINT ["/usr/local/bin/dpumesh-controller"]
