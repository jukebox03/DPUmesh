#!/bin/sh
# configure-kubelet-reserve.sh and check-cpu-reserve.py without a node: root and
# systemctl are stubbed, kubelet's files live in a scratch directory.
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir "$work/bin"
cat > "$work/bin/id" <<'EOF'
#!/bin/sh
[ "${1:-}" = -u ] && { echo 0; exit 0; }
exec /usr/bin/id "$@"
EOF
cat > "$work/bin/systemctl" <<EOF
#!/bin/sh
echo "\$*" >> "$work/systemctl.log"
EOF
chmod +x "$work/bin/id" "$work/bin/systemctl"
reserve() { PATH="$work/bin:$PATH" sh "$root/packaging/configure-kubelet-reserve.sh" "$@"; }
fail() { echo "kubelet_reserve_test: FAIL: $*" >&2; exit 1; }

config="$work/config.yaml"
state="$work/cpu_manager_state"

# Static CPU manager: kubelet is stopped, the checkpoint retired (and kept as a
# backup), the reserve written, and kubelet started.
printf 'kind: KubeletConfiguration\ncpuManagerPolicy: static\nreservedSystemCPUs: "12-15"\n' > "$config"
printf '{"policyName":"static","defaultCpuSet":"0-11","checksum":1}' > "$state"
reserve "$config" 0-2 3Gi > /dev/null
grep -q '^reservedSystemCPUs: "0-2"$' "$config" || fail "reserve not written"
grep -q '^  memory: "3Gi"$' "$config" || fail "memory not written"
[ ! -e "$state" ] || fail "static checkpoint kept"
[ -s "$state.dpumesh-pre-reserve" ] || fail "checkpoint backup missing"
[ "$(cat "$work/systemctl.log")" = "stop kubelet
start kubelet" ] || fail "static path did not stop and start kubelet"

# Unchanged: nothing restarts.
rm -f "$work/systemctl.log"
reserve "$config" 0-2 3Gi | grep -q 'already configured' || fail "no-op not detected"
[ ! -e "$work/systemctl.log" ] || fail "no-op restarted kubelet"

# Exclusive CPU assignments would be lost: refuse.
printf '{"policyName":"static","defaultCpuSet":"2-11","entries":{"pod":{"app":"0-1"}},"checksum":1}' > "$state"
if reserve "$config" 0-3 3Gi > /dev/null 2>&1; then fail "exclusive assignments not refused"; fi
grep -q '^reservedSystemCPUs: "0-2"$' "$config" || fail "refused change was written"

# A Static memory manager needs reservedMemory changed by hand: refuse.
printf 'cpuManagerPolicy: none\nmemoryManagerPolicy: Static\n' > "$config"
if reserve "$config" 0-2 3Gi > /dev/null 2>&1; then fail "Static memory manager not refused"; fi

# No static CPU manager: a plain restart, no checkpoint handling.
rm -f "$work/systemctl.log" "$state"
printf 'kind: KubeletConfiguration\n' > "$config"
reserve "$config" 0-2 3Gi > /dev/null
[ "$(cat "$work/systemctl.log")" = "restart kubelet" ] || fail "plain path did not restart kubelet"

# dpumeshd's AllowedCPUs must lie inside reservedSystemCPUs.
printf '[Service]\nAllowedCPUs=0-2\n' > "$work/dpumeshd.service"
check() { python3 "$root/packaging/check-cpu-reserve.py" "$work/dpumeshd.service" "$config" > /dev/null 2>&1; }
printf 'reservedSystemCPUs: "0-3,8"\n' > "$config"
check || fail "AllowedCPUs inside the reserve refused"
printf 'reservedSystemCPUs: "12-15"\n' > "$config"
if check; then fail "AllowedCPUs outside the reserve accepted"; fi
printf 'kind: KubeletConfiguration\n' > "$config"
if check; then fail "unset reserve accepted"; fi

echo "kubelet_reserve_test: PASS"
