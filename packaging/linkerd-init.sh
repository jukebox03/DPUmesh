#!/bin/sh
set -eu
umask 077
openssl genpkey -algorithm EC -pkeyopt ec_paramgen_curve:P-256 -out /identity/key.pem
openssl pkcs8 -topk8 -nocrypt -in /identity/key.pem -outform DER -out /identity/key.p8
openssl req -new -key /identity/key.pem -subj "/CN=$LINKERD2_PROXY_IDENTITY_LOCAL_NAME" \
  -addext "subjectAltName=DNS:$LINKERD2_PROXY_IDENTITY_LOCAL_NAME" -outform DER -out /identity/csr.der
rm /identity/key.pem
