#!/bin/bash
# Provision the identity inputs consumed by the embedded Linkerd proxy.
# This is an operator/benchmark tool; a production controller runs the same
# TokenRequest and atomic-install lifecycle continuously.
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
REFRESH_INTERVAL="${LINKERD_IDENTITY_REFRESH_INTERVAL:-1800}"
TRUST_NAMESPACE="${LINKERD_CONTROL_NAMESPACE:-linkerd}"
TRUST_CONFIG_MAP="${LINKERD_TRUST_CONFIG_MAP:-linkerd-identity-trust-roots}"
OUTPUT_DIR="${LINKERD_PROVISION_DIR:-$PROJECT_ROOT/build/linkerd-identity}"
DPU_IDENTITY_DIR="${DPU_LINKERD_IDENTITY_DIR:-/etc/dpumesh/linkerd-identity}"
LOCAL_NAME="${LINKERD_LOCAL_NAME:-$SERVICE_ACCOUNT.$NAMESPACE.serviceaccount.identity.linkerd.$TRUST_DOMAIN}"
UNIT="dpumesh-linkerd-identity-agent.service"
HEALTH_FILE="$OUTPUT_DIR/renewal-health"

usage() {
    cat <<EOF
Usage: $0 provision|refresh-token|refresh-dpu|install-dpu|start-agent|stop-agent|status|show

Environment:
  LINKERD_IDENTITY_NAMESPACE         workload namespace (default: $NAMESPACE)
  LINKERD_IDENTITY_SERVICE_ACCOUNT   service account (default: $SERVICE_ACCOUNT)
  LINKERD_PROVISION_DIR              local output (default: $OUTPUT_DIR)
  DPU_LINKERD_IDENTITY_DIR           remote install path (default: $DPU_IDENTITY_DIR)
  FORCE=1                            replace an existing key and CSR

After install-dpu, deploy with:
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

install_dpu() {
    need rsync
    : "${DPU_HOST:?DPU_HOST must be set}"
    : "${DPU_PASS:?DPU_PASS must be set}"
    for file in key.p8 csr.der token.txt trust-anchors.pem; do
        [ -s "$OUTPUT_DIR/$file" ] || {
            echo "missing $OUTPUT_DIR/$file; run provision first" >&2
            exit 1
        }
    done

    local remote_stage="/tmp/dpumesh-linkerd-identity-stage"
    ssh "$DPU_HOST" "mkdir -p '$remote_stage' && chmod 700 '$remote_stage'"
    rsync -az --chmod=F600 "$OUTPUT_DIR/key.p8" "$OUTPUT_DIR/csr.der" \
        "$OUTPUT_DIR/token.txt" "$OUTPUT_DIR/trust-anchors.pem" \
        "$DPU_HOST:$remote_stage/"
    ssh "$DPU_HOST" "echo '$DPU_PASS' | sudo -S install -d -m 0700 '$DPU_IDENTITY_DIR' && \
        echo '$DPU_PASS' | sudo -S install -m 0600 '$remote_stage/key.p8' '$DPU_IDENTITY_DIR/.key.p8.new' && \
        echo '$DPU_PASS' | sudo -S install -m 0644 '$remote_stage/csr.der' '$DPU_IDENTITY_DIR/.csr.der.new' && \
        echo '$DPU_PASS' | sudo -S install -m 0600 '$remote_stage/token.txt' '$DPU_IDENTITY_DIR/.token.txt.new' && \
        echo '$DPU_PASS' | sudo -S install -m 0644 '$remote_stage/trust-anchors.pem' '$DPU_IDENTITY_DIR/.trust-anchors.pem.new' && \
        echo '$DPU_PASS' | sudo -S mv '$DPU_IDENTITY_DIR/.key.p8.new' '$DPU_IDENTITY_DIR/key.p8' && \
        echo '$DPU_PASS' | sudo -S mv '$DPU_IDENTITY_DIR/.csr.der.new' '$DPU_IDENTITY_DIR/csr.der' && \
        echo '$DPU_PASS' | sudo -S mv '$DPU_IDENTITY_DIR/.token.txt.new' '$DPU_IDENTITY_DIR/token.txt' && \
        echo '$DPU_PASS' | sudo -S mv '$DPU_IDENTITY_DIR/.trust-anchors.pem.new' '$DPU_IDENTITY_DIR/trust-anchors.pem' && \
        rm -f '$remote_stage/key.p8' '$remote_stage/csr.der' '$remote_stage/token.txt' '$remote_stage/trust-anchors.pem'"
    echo "installed Linkerd identity for $LOCAL_NAME at $DPU_HOST:$DPU_IDENTITY_DIR"
}

refresh_dpu() {
    need rsync
    : "${DPU_HOST:?DPU_HOST must be set}"
    : "${DPU_PASS:?DPU_PASS must be set}"
    refresh_material

    local remote_stage="/tmp/dpumesh-linkerd-token-stage"
    ssh "$DPU_HOST" "mkdir -p '$remote_stage' && chmod 700 '$remote_stage'"
    rsync -az --chmod=F600 "$OUTPUT_DIR/token.txt" \
        "$OUTPUT_DIR/trust-anchors.pem" "$DPU_HOST:$remote_stage/"
    ssh "$DPU_HOST" "echo '$DPU_PASS' | sudo -S install -d -m 0700 '$DPU_IDENTITY_DIR' && \
        echo '$DPU_PASS' | sudo -S install -m 0600 '$remote_stage/token.txt' '$DPU_IDENTITY_DIR/.token.txt.new' && \
        echo '$DPU_PASS' | sudo -S install -m 0644 '$remote_stage/trust-anchors.pem' '$DPU_IDENTITY_DIR/.trust-anchors.pem.new' && \
        echo '$DPU_PASS' | sudo -S mv '$DPU_IDENTITY_DIR/.token.txt.new' '$DPU_IDENTITY_DIR/token.txt' && \
        echo '$DPU_PASS' | sudo -S mv '$DPU_IDENTITY_DIR/.trust-anchors.pem.new' '$DPU_IDENTITY_DIR/trust-anchors.pem' && \
        rm -f '$remote_stage/token.txt' '$remote_stage/trust-anchors.pem'"
    echo "refreshed Linkerd token and trust roots atomically at $DPU_HOST:$DPU_IDENTITY_DIR"
}

run_agent() {
    local failures=0 temporary
    while true; do
        if ! refresh_dpu; then
            failures=$((failures + 1))
            mkdir -p "$OUTPUT_DIR"
            temporary=$(mktemp "$OUTPUT_DIR/.renewal-health.XXXXXX")
            printf 'last_result=error\nlast_attempt_unix=%s\nconsecutive_errors=%s\n' \
                "$(date +%s)" "$failures" > "$temporary"
            mv "$temporary" "$HEALTH_FILE"
            echo "Linkerd identity refresh failed; retrying in 30 seconds" >&2
            sleep 30
            continue
        fi
        failures=0
        temporary=$(mktemp "$OUTPUT_DIR/.renewal-health.XXXXXX")
        printf 'last_result=ok\nlast_attempt_unix=%s\nconsecutive_errors=0\n' \
            "$(date +%s)" > "$temporary"
        mv "$temporary" "$HEALTH_FILE"
        sleep "$REFRESH_INTERVAL"
    done
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
    systemctl --user status "$UNIT" --no-pager
    token_status
    if [ -s "$HEALTH_FILE" ]; then
        cat "$HEALTH_FILE"
    else
        echo "renewal_health=not-yet-recorded"
    fi
}

start_agent() {
    systemctl --user stop "$UNIT" >/dev/null 2>&1 || true
    systemctl --user reset-failed "$UNIT" >/dev/null 2>&1 || true
    systemd-run --user --collect --unit="${UNIT%.service}" \
        --property=Restart=always --property=RestartSec=5s \
        "$0" run-agent
    systemctl --user is-active --quiet "$UNIT"
    echo "Linkerd identity renewal agent started (interval=${REFRESH_INTERVAL}s)"
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
    refresh-dpu) refresh_dpu ;;
    install-dpu) install_dpu ;;
    run-agent) run_agent ;;
    start-agent) start_agent ;;
    stop-agent) systemctl --user stop "$UNIT" ;;
    status) status ;;
    show) show ;;
    *) usage; exit 2 ;;
esac
