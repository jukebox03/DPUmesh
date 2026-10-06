#!/bin/sh
# Reserve the fixed host budget that backs dpumeshd's advertised channels.
set -eu

if [ "$(id -u)" -ne 0 ]; then
    echo "configure-kubelet-reserve.sh must run as root" >&2
    exit 1
fi

config=${1:-/var/lib/kubelet/config.yaml}
cpus=${2:-0-2}
memory=${3:-3Gi}
case "$cpus" in
    *[!0-9,-]*|'') echo "invalid reserved CPU set: $cpus" >&2; exit 2 ;;
esac
case "$memory" in
    *[!0-9A-Za-z]*|'') echo "invalid reserved memory: $memory" >&2; exit 2 ;;
esac
[ -f "$config" ] || { echo "kubelet config not found: $config" >&2; exit 1; }

temporary=$(mktemp)
trap 'rm -f "$temporary"' EXIT
awk -v cpus="$cpus" -v memory="$memory" '
BEGIN { have_cpu=0; in_system=0; have_memory=0 }
/^reservedSystemCPUs:/ {
    print "reservedSystemCPUs: \"" cpus "\""
    have_cpu=1
    next
}
/^systemReserved:/ {
    print
    in_system=1
    next
}
in_system && /^  memory:/ {
    print "  memory: \"" memory "\""
    have_memory=1
    next
}
in_system && /^[^[:space:]#]/ {
    if (!have_memory) print "  memory: \"" memory "\""
    in_system=0
    have_memory=1
}
{ print }
END {
    if (in_system && !have_memory) print "  memory: \"" memory "\""
    if (!have_cpu) print "reservedSystemCPUs: \"" cpus "\""
    if (!have_memory) {
        print "systemReserved:"
        print "  memory: \"" memory "\""
    }
}
' "$config" > "$temporary"

if cmp -s "$config" "$temporary"; then
    echo "kubelet reserve already configured: CPUs=$cpus memory=$memory"
    exit 0
fi

# A Static memory manager pins reservedMemory to the system and kube
# reservations; changing systemReserved alone makes kubelet refuse to start.
if grep -Eq '^memoryManagerPolicy:[[:space:]]*"?Static"?[[:space:]]*$' "$config"; then
    echo "memoryManagerPolicy is Static: update reservedMemory together with systemReserved by hand" >&2
    exit 1
fi

# A static CPU manager checkpoints its CPU sets in cpu_manager_state, and
# kubelet refuses to start once the reserved set no longer matches it. The
# checkpoint is retired while kubelet is stopped. Exclusive assignments
# ("entries") would be lost with it, so drain those Pods first.
state="$(dirname "$config")/cpu_manager_state"
static_cpu=0
if grep -Eq '^cpuManagerPolicy:[[:space:]]*"?static"?[[:space:]]*$' "$config"; then
    static_cpu=1
    if [ -s "$state" ] && grep -q '"entries"' "$state"; then
        echo "Pods hold exclusive CPUs ($state lists entries); drain them before changing the reserve" >&2
        exit 1
    fi
fi

backup="$config.dpumesh-pre-reserve"
if [ ! -e "$backup" ]; then
    cp -p "$config" "$backup"
fi
mode=$(stat -c '%a' "$config")
owner=$(stat -c '%u' "$config")
group=$(stat -c '%g' "$config")
if [ "$static_cpu" = 1 ]; then
    systemctl stop kubelet
    install -o "$owner" -g "$group" -m "$mode" "$temporary" "$config"
    if [ -e "$state" ]; then
        [ -e "$state.dpumesh-pre-reserve" ] || cp -p "$state" "$state.dpumesh-pre-reserve"
        rm -f "$state"
    fi
    systemctl start kubelet
else
    install -o "$owner" -g "$group" -m "$mode" "$temporary" "$config"
    systemctl restart kubelet
fi
echo "kubelet reserve configured: CPUs=$cpus memory=$memory; backup=$backup"
