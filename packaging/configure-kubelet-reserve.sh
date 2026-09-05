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

backup="$config.dpumesh-pre-reserve"
if [ ! -e "$backup" ]; then
    cp -p "$config" "$backup"
fi
mode=$(stat -c '%a' "$config")
owner=$(stat -c '%u' "$config")
group=$(stat -c '%g' "$config")
install -o "$owner" -g "$group" -m "$mode" "$temporary" "$config"
systemctl restart kubelet
echo "kubelet reserve configured: CPUs=$cpus memory=$memory; backup=$backup"
