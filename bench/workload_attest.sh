#!/bin/bash
# Provision the root-only registration keyring, install the DPU end of the
# delivery hop, and deploy the node agent.
#
# Everything here is bootstrap: key material and the one-time installation of
# the receiver unit. The control path itself — every feed the DPU consumes —
# runs through the agent's delivery loop and holds no login shell, password or
# sudo. Nothing in this script runs while the mesh is running.
set -euo pipefail

BENCH_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJ_ROOT="$(cd "$BENCH_DIR/.." && pwd)"
if [ -f "$PROJ_ROOT/.env" ]; then
    set -a
    source "$PROJ_ROOT/.env"
    set +a
fi

HOST_KEY_DIR="${DPUMESH_REGISTRATION_KEY_DIR_HOST:-/etc/dpumesh/registration.keys}"
DPU_KEY_DIR="${DPUMESH_REGISTRATION_KEY_DIR_DPU:-/etc/dpumesh/registration.keys}"
DEFAULT_KEY_ID="${DPUMESH_REGISTRATION_KEY_ID:-node-ed25519-v1}"
# Feed signing is a separate role from assertion signing: a feed publisher must
# hold no key that can mint identity, so the feed keyring is a disjoint
# directory with disjoint key files.
HOST_FEED_KEY_DIR="${DPUMESH_FEED_KEY_DIR_HOST:-/etc/dpumesh/feed.keys}"
DPU_FEED_KEY_DIR="${DPUMESH_FEED_KEY_DIR_DPU:-/etc/dpumesh/feed.keys}"
DEFAULT_FEED_KEY_ID="${DPUMESH_FEED_KEY_ID:-feed-hmac-v1}"
NS="${NS:-test-bench}"
# The DPU end of the delivery hop: an unprivileged account, a system unit, and
# a root directory it may write the four feeds into and nothing else.
FEED_ROOT="${DPUMESH_FEED_ROOT_DPU:-/etc/dpumesh}"
FEED_USER="${DPUMESH_FEED_USER:-dpumesh-feed}"
FEED_BIND="${DPUMESH_DPU_FEED_HOST:-192.168.100.2}"
FEED_PORT="${DPUMESH_DPU_FEED_PORT:-4788}"
FEED_UNIT="dpumesh-feed-receiver.service"

valid_key_id() {
    [[ "$1" =~ ^[A-Za-z0-9][A-Za-z0-9_.:-]{0,30}[A-Za-z0-9]$ ]]
}

need_host() { : "${HOST_PASS:?HOST_PASS is required}"; }
need_dpu() {
    need_host
    : "${DPU_HOST:?DPU_HOST is required}"
    : "${DPU_PASS:?DPU_PASS is required}"
}

install_host_key() {
    local host_dir="$1" key_id="$2" temporary
    valid_key_id "$key_id" || { echo "invalid key id: $key_id" >&2; exit 2; }
    echo "$HOST_PASS" | sudo -S mkdir -p "$host_dir"
    echo "$HOST_PASS" | sudo -S chown root:root "$host_dir"
    echo "$HOST_PASS" | sudo -S chmod 0700 "$host_dir"
    if echo "$HOST_PASS" | sudo -S test -f "$host_dir/$key_id.key" 2>/dev/null; then
        echo "$HOST_PASS" | sudo -S chown root:root "$host_dir/$key_id.key"
        echo "$HOST_PASS" | sudo -S chmod 0400 "$host_dir/$key_id.key"
        return
    fi
    temporary=$(mktemp)
    openssl rand 32 > "$temporary"
    chmod 0600 "$temporary"
    echo "$HOST_PASS" | sudo -S install -o root -g root -m 0400 \
        "$temporary" "$host_dir/$key_id.key"
    rm -f "$temporary"
}

set_active() {
    local host_dir="$1" key_id="$2" temporary
    valid_key_id "$key_id" || { echo "invalid key id: $key_id" >&2; exit 2; }
    echo "$HOST_PASS" | sudo -S test -f "$host_dir/$key_id.key"
    temporary=$(mktemp)
    printf '%s\n' "$key_id" > "$temporary"
    chmod 0600 "$temporary"
    echo "$HOST_PASS" | sudo -S install -o root -g root -m 0400 \
        "$temporary" "$host_dir/active"
    rm -f "$temporary"
}

install_dpu_key() {
    local host_dir="$1" dpu_dir="$2" key_id="$3"
    valid_key_id "$key_id" || { echo "invalid key id: $key_id" >&2; exit 2; }
    echo "$HOST_PASS" | sudo -S cat "$host_dir/$key_id.key" 2>/dev/null |
        ssh -o ConnectTimeout=8 "$DPU_HOST" \
            "umask 077; cat > /tmp/dpumesh-keyring.key.in; \
             echo '$DPU_PASS' | sudo -S mkdir -p '$dpu_dir'; \
             echo '$DPU_PASS' | sudo -S chown root:root '$dpu_dir'; \
             echo '$DPU_PASS' | sudo -S chmod 0700 '$dpu_dir'; \
             echo '$DPU_PASS' | sudo -S install -o root -g root -m 0400 \
                 /tmp/dpumesh-keyring.key.in '$dpu_dir/$key_id.key'; \
             rm -f /tmp/dpumesh-keyring.key.in"
}

# The registration keyring is asymmetric: the host file is the node's raw
# Ed25519 private seed and the DPU receives only the derived public key, so
# the DPU holds no key that can sign an assertion.
install_dpu_pubkey() {
    local host_dir="$1" dpu_dir="$2" key_id="$3" pub_hex
    valid_key_id "$key_id" || { echo "invalid key id: $key_id" >&2; exit 2; }
    pub_hex=$(echo "$HOST_PASS" | sudo -S cat "$host_dir/$key_id.key" 2>/dev/null |
        { printf '302e020100300506032b657004220420' | xxd -r -p; cat; } |
        openssl pkey -inform DER -pubout -outform DER 2>/dev/null |
        tail -c 32 | xxd -p -c 64)
    [ ${#pub_hex} -eq 64 ] || {
        echo "cannot derive the public key of $key_id (expected a 32-byte raw seed)" >&2
        exit 1
    }
    printf '%s\n' "$pub_hex" |
        ssh -o ConnectTimeout=8 "$DPU_HOST" \
            "umask 077; cat > /tmp/dpumesh-keyring.key.in; \
             echo '$DPU_PASS' | sudo -S mkdir -p '$dpu_dir'; \
             echo '$DPU_PASS' | sudo -S chown root:root '$dpu_dir'; \
             echo '$DPU_PASS' | sudo -S chmod 0700 '$dpu_dir'; \
             echo '$DPU_PASS' | sudo -S install -o root -g root -m 0400 \
                 /tmp/dpumesh-keyring.key.in '$dpu_dir/$key_id.key'; \
             rm -f /tmp/dpumesh-keyring.key.in"
}

sync_dpu_keyring() {
    local host_dir="$1" dpu_dir="$2" mode="$3" filename key_id
    while IFS= read -r filename; do
        key_id=${filename%.key}
        if [ "$mode" = pubkey ]; then
            install_dpu_pubkey "$host_dir" "$dpu_dir" "$key_id"
        else
            install_dpu_key "$host_dir" "$dpu_dir" "$key_id"
        fi
    done < <(echo "$HOST_PASS" | sudo -S find "$host_dir" -maxdepth 1 \
        -type f -name '*.key' -printf '%f\n' 2>/dev/null | LC_ALL=C sort)
}

prepare_keyring() {
    local host_dir="$1" dpu_dir="$2" default_key_id="$3" role="$4" mode="$5" active
    echo "$HOST_PASS" | sudo -S mkdir -p "$host_dir"
    echo "$HOST_PASS" | sudo -S chown root:root "$host_dir"
    echo "$HOST_PASS" | sudo -S chmod 0700 "$host_dir"
    if echo "$HOST_PASS" | sudo -S test -f "$host_dir/active" 2>/dev/null; then
        active=$(echo "$HOST_PASS" | sudo -S cat "$host_dir/active" 2>/dev/null)
        valid_key_id "$active" || {
            echo "invalid active $role key id: $active" >&2
            exit 1
        }
        # Reuse the active keyring. In particular, do not resurrect the
        # bootstrap key after it has been pruned at the end of a rotation.
        echo "$HOST_PASS" | sudo -S test -f "$host_dir/$active.key"
        echo "$HOST_PASS" | sudo -S chown root:root "$host_dir/$active.key"
        echo "$HOST_PASS" | sudo -S chmod 0400 "$host_dir/$active.key"
    else
        install_host_key "$host_dir" "$default_key_id"
        set_active "$host_dir" "$default_key_id"
    fi
    sync_dpu_keyring "$host_dir" "$dpu_dir" "$mode"
    echo "trusted $role keyring ready: host=$host_dir dpu=$dpu_dir"
}

# A key file present in both keyrings is a signing-capability leak: the feed
# publishers would hold a key that can mint identity. Key selection is
# filename-driven, and identical bytes under different names leak the same way.
assert_disjoint_keyrings() {
    [ "$HOST_KEY_DIR" != "$HOST_FEED_KEY_DIR" ] || {
        echo "registration and feed keyrings must be disjoint directories" >&2
        exit 1
    }
    [ "$DPU_KEY_DIR" != "$DPU_FEED_KEY_DIR" ] || {
        echo "registration and feed DPU keyrings must be disjoint directories" >&2
        exit 1
    }
    local overlap
    overlap=$(comm -12 \
        <(echo "$HOST_PASS" | sudo -S find "$HOST_KEY_DIR" -maxdepth 1 -type f \
            -name '*.key' -printf '%f\n' 2>/dev/null | LC_ALL=C sort) \
        <(echo "$HOST_PASS" | sudo -S find "$HOST_FEED_KEY_DIR" -maxdepth 1 -type f \
            -name '*.key' -printf '%f\n' 2>/dev/null | LC_ALL=C sort))
    [ -z "$overlap" ] || {
        echo "key id present in both keyrings: $overlap" >&2
        exit 1
    }
    overlap=$(comm -12 \
        <(echo "$HOST_PASS" | sudo -S find "$HOST_KEY_DIR" -maxdepth 1 -type f \
            -name '*.key' -exec sha256sum {} + 2>/dev/null | cut -d' ' -f1 | LC_ALL=C sort) \
        <(echo "$HOST_PASS" | sudo -S find "$HOST_FEED_KEY_DIR" -maxdepth 1 -type f \
            -name '*.key' -exec sha256sum {} + 2>/dev/null | cut -d' ' -f1 | LC_ALL=C sort))
    [ -z "$overlap" ] || {
        echo "identical key bytes present in both keyrings" >&2
        exit 1
    }
}

prepare() {
    need_dpu
    prepare_keyring "$HOST_KEY_DIR" "$DPU_KEY_DIR" "$DEFAULT_KEY_ID" registration pubkey
    prepare_keyring "$HOST_FEED_KEY_DIR" "$DPU_FEED_KEY_DIR" "$DEFAULT_FEED_KEY_ID" feed copy
    assert_disjoint_keyrings
}

deploy_agent() {
    need_host
    : "${IMG_WORKLOAD_AGENT:?IMG_WORKLOAD_AGENT is required}"
    command -v envsubst >/dev/null 2>&1 || {
        echo "envsubst not found (apt install gettext-base)" >&2
        exit 1
    }
    export NS IMG_WORKLOAD_AGENT
    envsubst < "$BENCH_DIR/k8s/workload-agent.yaml" | kubectl apply -f -
    # The image is rebuilt under one tag, so an unchanged Pod spec would keep
    # the previous agent binary running behind a successful apply.
    kubectl rollout restart daemonset/dpumesh-node-agent -n "$NS"
    kubectl rollout status daemonset/dpumesh-node-agent -n "$NS" --timeout=120s
    kubectl auth can-i list pods -n "$NS" \
        --as="system:serviceaccount:$NS:dpumesh-node-agent" | grep -qx yes
    [ "$(kubectl auth can-i list pods --all-namespaces \
        --as="system:serviceaccount:$NS:dpumesh-node-agent" 2>/dev/null || true)" = no ]
    echo "trusted workload node agent ready in namespace $NS"
}

# Install the DPU end of the delivery hop. One-time bootstrap: an unprivileged
# account, a system unit so a node reboot restores it with no operator session,
# and a root directory the receiver may write the four feeds into. After this
# the agent is the DPU's only control peer and nothing here runs again.
# The user units the delivery hop replaces. A unit started by an earlier deploy
# keeps running the copy of the script it read at start, so it survives the
# script losing the code — and it pushes feeds as root, taking the feed
# directory back from the hop's account every cycle. Retiring them is part of
# installing the hop, not a separate cleanup.
LEGACY_UNITS="dpumesh-membership.service dpumesh-topology.service \
dpumesh-linkerd-service-registry.service dpumesh-linkerd-identity-agent.service"

retire_legacy_units() {
    local unit
    for unit in $LEGACY_UNITS; do
        if systemctl --user is-active --quiet "$unit" 2>/dev/null; then
            systemctl --user stop "$unit" >/dev/null 2>&1 || true
            echo "retired the pre-hop user unit $unit"
        fi
        systemctl --user reset-failed "$unit" >/dev/null 2>&1 || true
    done
}

install_hop() {
    need_dpu
    retire_legacy_units
    local stage="/tmp/dpumesh-feed-receiver.py"
    scp -o ConnectTimeout=8 -q "$BENCH_DIR/dpumesh_feed_receiver.py" "$DPU_HOST:$stage"
    ssh -o ConnectTimeout=8 "$DPU_HOST" "
        set -e
        echo '$DPU_PASS' | sudo -S id -u '$FEED_USER' >/dev/null 2>&1 ||
            echo '$DPU_PASS' | sudo -S useradd --system --no-create-home \
                --shell /usr/sbin/nologin '$FEED_USER'
        echo '$DPU_PASS' | sudo -S install -d -o '$FEED_USER' -g '$FEED_USER' -m 0755 '$FEED_ROOT'
        # The identity bundle installs into a directory, and a directory the
        # hop's account does not own (the pre-hop root-installed one) refuses
        # every install silently on the agent side. install -d re-owns it.
        echo '$DPU_PASS' | sudo -S install -d -o '$FEED_USER' -g '$FEED_USER' -m 0700 \
            '$FEED_ROOT/linkerd-identity'
        echo '$DPU_PASS' | sudo -S install -o root -g root -m 0555 \
            '$stage' /usr/local/bin/dpumesh-feed-receiver
        rm -f '$stage'
        printf '%s\n' \
            '[Unit]' \
            'Description=DPUmesh feed receiver' \
            'Wants=network-online.target' \
            'After=network-online.target' \
            '' \
            '[Service]' \
            'User=$FEED_USER' \
            'ExecStart=/usr/bin/python3 /usr/local/bin/dpumesh-feed-receiver --bind $FEED_BIND --port $FEED_PORT --root $FEED_ROOT' \
            'Restart=always' \
            'RestartSec=2' \
            'NoNewPrivileges=yes' \
            'ProtectSystem=strict' \
            'ProtectHome=yes' \
            'PrivateTmp=yes' \
            'ReadWritePaths=$FEED_ROOT' \
            '' \
            '[Install]' \
            'WantedBy=multi-user.target' > /tmp/dpumesh-feed-receiver.service
        echo '$DPU_PASS' | sudo -S install -o root -g root -m 0644 \
            /tmp/dpumesh-feed-receiver.service /etc/systemd/system/$FEED_UNIT
        rm -f /tmp/dpumesh-feed-receiver.service
        echo '$DPU_PASS' | sudo -S systemctl daemon-reload
        echo '$DPU_PASS' | sudo -S systemctl enable '$FEED_UNIT'
        # Restart rather than start: the unit may already be running an older
        # copy of the receiver, and enabling it would leave that one there.
        echo '$DPU_PASS' | sudo -S systemctl restart '$FEED_UNIT'
        echo '$DPU_PASS' | sudo -S systemctl is-active --quiet '$FEED_UNIT'
    "
    # The hop can only install a feed if its account owns the directory it
    # installs into. Provisioning that runs later and creates a keyring under
    # the same root can take that back, so it is asserted rather than assumed.
    local owner
    owner=$(ssh -o ConnectTimeout=8 "$DPU_HOST" "stat -c %U '$FEED_ROOT'")
    [ "$owner" = "$FEED_USER" ] || {
        echo "$FEED_ROOT is owned by $owner, not $FEED_USER: the hop could not install a feed" >&2
        exit 1
    }
    echo "delivery hop ready at $FEED_BIND:$FEED_PORT (user=$FEED_USER root=$FEED_ROOT)"
}

case "${1:-status}" in
    prepare)
        prepare
        ;;
    deploy)
        deploy_agent
        ;;
    rotate-stage)
        need_dpu
        key_id="${2:?usage: $0 rotate-stage KEY_ID}"
        install_host_key "$HOST_KEY_DIR" "$key_id"
        install_dpu_pubkey "$HOST_KEY_DIR" "$DPU_KEY_DIR" "$key_id"
        assert_disjoint_keyrings
        echo "key $key_id staged on Host and DPU; restart DPU before activating it"
        ;;
    activate)
        need_host
        key_id="${2:?usage: $0 activate KEY_ID}"
        set_active "$HOST_KEY_DIR" "$key_id"
        echo "node agent now signs with $key_id"
        ;;
    prune)
        need_dpu
        key_id="${2:?usage: $0 prune KEY_ID}"
        valid_key_id "$key_id" || { echo "invalid registration key id: $key_id" >&2; exit 2; }
        active=$(echo "$HOST_PASS" | sudo -S cat "$HOST_KEY_DIR/active" 2>/dev/null)
        [ "$active" != "$key_id" ] || {
            echo "refusing to prune active registration key $key_id" >&2
            exit 1
        }
        echo "$HOST_PASS" | sudo -S rm -f "$HOST_KEY_DIR/$key_id.key"
        ssh "$DPU_HOST" "echo '$DPU_PASS' | sudo -S rm -f '$DPU_KEY_DIR/$key_id.key'"
        echo "pruned inactive registration key $key_id from Host and DPU"
        ;;
    feed-rotate-stage)
        need_dpu
        key_id="${2:?usage: $0 feed-rotate-stage KEY_ID}"
        install_host_key "$HOST_FEED_KEY_DIR" "$key_id"
        install_dpu_key "$HOST_FEED_KEY_DIR" "$DPU_FEED_KEY_DIR" "$key_id"
        assert_disjoint_keyrings
        echo "feed key $key_id staged on Host and DPU"
        ;;
    feed-activate)
        need_host
        key_id="${2:?usage: $0 feed-activate KEY_ID}"
        set_active "$HOST_FEED_KEY_DIR" "$key_id"
        echo "feed publishers now sign with $key_id"
        ;;
    feed-prune)
        need_dpu
        key_id="${2:?usage: $0 feed-prune KEY_ID}"
        valid_key_id "$key_id" || { echo "invalid feed key id: $key_id" >&2; exit 2; }
        active=$(echo "$HOST_PASS" | sudo -S cat "$HOST_FEED_KEY_DIR/active" 2>/dev/null)
        [ "$active" != "$key_id" ] || {
            echo "refusing to prune active feed key $key_id" >&2
            exit 1
        }
        echo "$HOST_PASS" | sudo -S rm -f "$HOST_FEED_KEY_DIR/$key_id.key"
        ssh "$DPU_HOST" "echo '$DPU_PASS' | sudo -S rm -f '$DPU_FEED_KEY_DIR/$key_id.key'"
        echo "pruned inactive feed key $key_id from Host and DPU"
        ;;
    install-hop)
        install_hop
        ;;
    retire-legacy-units)
        retire_legacy_units
        ;;
    membership-show)
        : "${DPU_HOST:?DPU_HOST is required}"
        ssh "$DPU_HOST" "cat '$FEED_ROOT/membership.v1'"
        ;;
    hop-status)
        : "${DPU_HOST:?DPU_HOST is required}"
        ssh "$DPU_HOST" "systemctl status '$FEED_UNIT' --no-pager" || true
        ;;
    stop)
        retire_legacy_units
        kubectl delete daemonset/dpumesh-node-agent -n "$NS" --ignore-not-found=true
        ;;
    status)
        kubectl get daemonset,pod -n "$NS" -l app=dpumesh-node-agent -o wide
        ;;
    *)
        echo "usage: $0 prepare|install-hop|retire-legacy-units|deploy|" >&2
        echo "       rotate-stage KEY_ID|activate KEY_ID|" >&2
        echo "       prune KEY_ID|feed-rotate-stage KEY_ID|feed-activate KEY_ID|" >&2
        echo "       feed-prune KEY_ID|membership-show|hop-status|stop|status" >&2
        exit 2
        ;;
esac
