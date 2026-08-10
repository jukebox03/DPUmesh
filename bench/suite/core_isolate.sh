#!/bin/bash
# Keep processes that are not part of the benchmark off the benchmark cores.
#
# Narrows every user-space process that still holds every online CPU to the
# complement of the benchmark range. A process already restricted to a subset
# is left alone.
#
#   on    narrow unpinned processes to the complement
#   off   return them to every online CPU
#   show  count processes in each set
set -euo pipefail

RED='\033[0;31m'; GREEN='\033[0;32m'; NC='\033[0m'
info() { echo -e "${GREEN}[iso]${NC} $*"; }
die()  { echo -e "${RED}[iso]${NC} $*" >&2; exit 1; }

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# shellcheck disable=SC1091
[ -f "$ROOT/.env" ] && { set -a; . "$ROOT/.env"; set +a; }
: "${HOST_PASS:?.env must define HOST_PASS}"

ACTION="${1:-show}"
RANGE="${2:-18-35}"
LOW=${RANGE%-*}; HIGH=${RANGE#*-}
ONLINE=$(cat /sys/devices/system/cpu/online)
LAST=${ONLINE##*-}
COMPLEMENT=""
[ "$LOW" -gt 0 ] && COMPLEMENT="0-$((LOW - 1))"
[ "$HIGH" -lt "$LAST" ] &&
  COMPLEMENT="${COMPLEMENT:+$COMPLEMENT,}$((HIGH + 1))-$LAST"
[ -n "$COMPLEMENT" ] || die "benchmark range $RANGE leaves no complement of $ONLINE"
FULL="$ONLINE"
SUDO() { echo "$HOST_PASS" | sudo -S "$@"; }

# User-space processes whose affinity is the given set; a kernel thread has no
# address space.
with_affinity() {
  local want="$1" p list
  for p in /proc/[0-9]*; do
    [ -r "$p/status" ] || continue
    list=$(awk '/^VmSize:/{v=1} /^Cpus_allowed_list:/{c=$2}
                END{if (v) print c}' "$p/status" 2>/dev/null) || continue
    [ "$list" = "$want" ] && echo "${p##*/}"
  done
}

case "$ACTION" in
  on|off)
    target=$COMPLEMENT; [ "$ACTION" = off ] && target=$FULL
    if [ "$ACTION" = off ]; then
      mapfile -t pids < <(with_affinity "$COMPLEMENT")
    else
      mapfile -t pids < <(with_affinity "$FULL")
    fi
    n=0
    for pid in "${pids[@]}"; do
      SUDO taskset -apc "$target" "$pid" >/dev/null 2>&1 && n=$((n + 1)) || true
    done
    info "$ACTION: $n processes -> $target"
    ;;
  show)
    info "$(with_affinity "$FULL" | wc -l) processes may run on $RANGE; \
$(with_affinity "$COMPLEMENT" | wc -l) narrowed to $COMPLEMENT"
    ;;
  *) die "usage: $0 on|off|show [LOW-HIGH]" ;;
esac
