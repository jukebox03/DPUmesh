#!/bin/bash
# Provision the identity inputs consumed by the embedded Linkerd proxy.
#
# What is provisioned here is the long-lived half: the key that never leaves
# this node and the certificate request built from it. The short-lived half —
# the bound ServiceAccount token and the trust anchors — is minted by the node
# agent on the cadence the token's lifetime asks for, and the agent delivers
# all four to the DPU as one bundle. There is no push from here.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
if [ -f "$PROJECT_ROOT/.env" ]; then
    set -a
    source "$PROJECT_ROOT/.env"
    set +a
fi

NAMESPACE="${LINKERD_IDENTITY_NAMESPACE:-test-bench}"
SERVICE_ACCOUNT="${LINKERD_IDENTITY_SERVICE_ACCOUNT:-dpumesh-dpu}"
TRUST_DOMAIN="${LINKERD_TRUST_DOMAIN:-cluster.local}"
TOKEN_AUDIENCE="${LINKERD_TOKEN_AUDIENCE:-identity.l5d.io}"
TOKEN_DURATION="${LINKERD_TOKEN_DURATION:-1h}"
TRUST_NAMESPACE="${LINKERD_CONTROL_NAMESPACE:-linkerd}"
TRUST_CONFIG_MAP="${LINKERD_TRUST_CONFIG_MAP:-linkerd-identity-trust-roots}"
OUTPUT_DIR="${LINKERD_PROVISION_DIR:-$PROJECT_ROOT/build/linkerd-identity}"
DPU_IDENTITY_DIR="${DPU_LINKERD_IDENTITY_DIR:-/etc/dpumesh/linkerd-identity}"
LOCAL_NAME="${LINKERD_LOCAL_NAME:-$SERVICE_ACCOUNT.$NAMESPACE.serviceaccount.identity.linkerd.$TRUST_DOMAIN}"

usage() {
    cat <<EOF
Usage: $0 provision|refresh-token|status|show

Environment:
  LINKERD_IDENTITY_NAMESPACE         workload namespace (default: $NAMESPACE)
  LINKERD_IDENTITY_SERVICE_ACCOUNT   service account (default: $SERVICE_ACCOUNT)
  LINKERD_PROVISION_DIR              staging directory the node agent reads
                                     the key and CSR from (default: $OUTPUT_DIR)
  DPU_LINKERD_IDENTITY_DIR           where the agent's delivery lands on the DPU
                                     (default: $DPU_IDENTITY_DIR)
  FORCE=1                            replace an existing key and CSR

After provision, deploy with:
  LINKERD_IDENTITY_DIR=$DPU_IDENTITY_DIR
  LINKERD_TRUST_ANCHORS=$DPU_IDENTITY_DIR/trust-anchors.pem
  LINKERD_LOCAL_NAME=$LOCAL_NAME
EOF
}

ensure_service_account() {
    kubectl create serviceaccount "$SERVICE_ACCOUNT" -n "$NAMESPACE" \
        --dry-run=client -o yaml | kubectl apply -f - >/dev/null
}

need() {
    command -v "$1" >/dev/null 2>&1 || {
        echo "missing command: $1" >&2
        exit 1
    }
}

refresh_material() {
    need kubectl
    ensure_service_account
    mkdir -p "$OUTPUT_DIR"
    local stage
    stage=$(mktemp -d "$OUTPUT_DIR/.refresh.XXXXXX")
    trap 'rm -rf "$stage"' RETURN

    kubectl get serviceaccount "$SERVICE_ACCOUNT" -n "$NAMESPACE" >/dev/null
    kubectl create token "$SERVICE_ACCOUNT" -n "$NAMESPACE" \
        --audience="$TOKEN_AUDIENCE" --duration="$TOKEN_DURATION" > "$stage/token.txt"
    test -s "$stage/token.txt"
    kubectl get configmap "$TRUST_CONFIG_MAP" -n "$TRUST_NAMESPACE" \
        -o jsonpath='{.data.ca-bundle\.crt}' > "$stage/trust-anchors.pem"
    test -s "$stage/trust-anchors.pem"

    install -m 0600 "$stage/token.txt" "$OUTPUT_DIR/.token.txt.new"
    install -m 0644 "$stage/trust-anchors.pem" "$OUTPUT_DIR/.trust-anchors.pem.new"
    mv "$OUTPUT_DIR/.token.txt.new" "$OUTPUT_DIR/token.txt"
    mv "$OUTPUT_DIR/.trust-anchors.pem.new" "$OUTPUT_DIR/trust-anchors.pem"
    trap - RETURN
    rm -rf "$stage"
}

provision() {
    need openssl
    if { [ -e "$OUTPUT_DIR/key.p8" ] || [ -e "$OUTPUT_DIR/csr.der" ]; } &&
       [ "${FORCE:-0}" != 1 ]; then
        echo "identity key or CSR already exists in $OUTPUT_DIR (set FORCE=1 to replace)" >&2
        exit 1
    fi

    mkdir -p "$OUTPUT_DIR"
    local stage
    stage=$(mktemp -d "$OUTPUT_DIR/.provision.XXXXXX")
    trap 'rm -rf "$stage"' RETURN
    openssl ecparam -name prime256v1 -genkey -noout -out "$stage/key.pem"
    openssl pkcs8 -topk8 -nocrypt -in "$stage/key.pem" -outform DER -out "$stage/key.p8"
    openssl req -new -key "$stage/key.pem" -subj / \
        -addext "subjectAltName=DNS:$LOCAL_NAME" -outform DER -out "$stage/csr.der"
    install -m 0600 "$stage/key.p8" "$OUTPUT_DIR/key.p8"
    install -m 0644 "$stage/csr.der" "$OUTPUT_DIR/csr.der"
    trap - RETURN
    rm -rf "$stage"
    refresh_material
    echo "provisioned Linkerd identity for $LOCAL_NAME in $OUTPUT_DIR"
}

token_status() {
    python3 - "$OUTPUT_DIR/token.txt" <<'PY'
import base64
import json
import pathlib
import sys
import time

path = pathlib.Path(sys.argv[1])
if not path.is_file():
    print("token_status=missing")
    raise SystemExit
try:
    payload = path.read_text(encoding="ascii").strip().split(".")[1]
    payload += "=" * (-len(payload) % 4)
    claims = json.loads(base64.urlsafe_b64decode(payload))
    issued = int(claims["iat"])
    expires = int(claims["exp"])
except (IndexError, KeyError, ValueError, OSError, UnicodeError, json.JSONDecodeError):
    print("token_status=invalid")
    raise SystemExit
now = int(time.time())
print(f"token_issued_unix={issued}")
print(f"token_expires_unix={expires}")
print(f"token_seconds_remaining={expires - now}")
PY
}

status() {
    # The node agent renews the short-lived half, so its Pod log is the health surface.
    kubectl get pod -n "$NAMESPACE" -l app=dpumesh-node-agent -o wide 2>/dev/null || true
    token_status
}

show() {
    echo "LINKERD_LOCAL_NAME=$LOCAL_NAME"
    echo "LINKERD_IDENTITY_DIR=$DPU_IDENTITY_DIR"
    echo "LINKERD_TRUST_ANCHORS=$DPU_IDENTITY_DIR/trust-anchors.pem"
    if [ -s "$OUTPUT_DIR/csr.der" ]; then
        openssl req -inform DER -in "$OUTPUT_DIR/csr.der" -noout -text |
            sed -n '/Requested Extensions:/,/Signature Algorithm:/p'
    fi
    if [ -s "$OUTPUT_DIR/token.txt" ]; then
        echo "token=present"
        token_status
    else
        echo "token=missing"
    fi
}

case "${1:-}" in
    provision) provision ;;
    refresh-token) refresh_material ;;
    status) status ;;
    show) show ;;
    *) usage; exit 2 ;;
esac
