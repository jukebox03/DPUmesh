FROM python:3.12.5-slim-bookworm

RUN pip install --no-cache-dir cryptography

COPY controller/dpumesh_controller.py /usr/local/bin/dpumesh-controller
# The admission webhook ships in the same image; its Deployment selects it by
# overriding the entrypoint.
COPY controller/dpumesh_webhook.py /usr/local/bin/dpumesh-webhook
RUN chmod 0555 /usr/local/bin/dpumesh-controller /usr/local/bin/dpumesh-webhook

USER 0:0
ENTRYPOINT ["/usr/local/bin/dpumesh-controller"]
