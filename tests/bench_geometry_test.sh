#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "$ROOT/bench/suite/deployed_geometry.sh"
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT
empty_env="$scratch/empty.env"
: >"$empty_env"

check_geometry() {
    local workers="$1" expected="$2" got
    got=$(DPUMESH_ENV_FILE="$empty_env" \
          DPUMESH_THROUGHPUT_WORKERS="$workers" \
          DPUMESH_DPA_THREADS=7 DPUMESH_RINGS_PER_POD=3 \
          DPUMESH_ARM_WORKERS=2 DPUMESH_L7_LINKERD_WORKER=0 \
          bash "$ROOT/bench/bench.sh" geometry)
    [ "$got" = "$expected" ] || {
        echo "geometry mismatch for W=$workers: $got" >&2
        return 1
    }
}

# The canonical deployment knob overrides every independently supplied N/K/A
# value, so Host K and DPU N/K/A/L7 cannot silently disagree.
check_geometry 4  "throughput_workers=4 N=32 K=4 A=4 l7_workers=all"
check_geometry 6  "throughput_workers=6 N=30 K=6 A=6 l7_workers=all"
check_geometry 8  "throughput_workers=8 N=32 K=8 A=8 l7_workers=all"
check_geometry 12 "throughput_workers=12 N=24 K=12 A=12 l7_workers=all"

# The repository default is optional. Exercise a copy with no adjacent .env so
# a developer's local file cannot hide stdout/stderr noise from a clean CI run.
clean_root="$scratch/clean"
mkdir -p "$clean_root/bench"
cp "$ROOT/bench/bench.sh" "$clean_root/bench/bench.sh"
env -u DPUMESH_ENV_FILE \
    DPUMESH_THROUGHPUT_WORKERS=4 \
    DPUMESH_DPA_THREADS=7 DPUMESH_RINGS_PER_POD=3 \
    DPUMESH_ARM_WORKERS=2 DPUMESH_L7_LINKERD_WORKER=0 \
    bash "$clean_root/bench/bench.sh" geometry \
    >"$scratch/clean.out" 2>"$scratch/clean.err"
got=$(<"$scratch/clean.out")
[ "$got" = "throughput_workers=4 N=32 K=4 A=4 l7_workers=all" ] &&
    [ ! -s "$scratch/clean.err" ] || {
        echo "optional default environment file polluted geometry output" >&2
        exit 1
    }

set +e
DPUMESH_ENV_FILE="$empty_env" DPUMESH_THROUGHPUT_WORKERS=16 \
    bash "$ROOT/bench/bench.sh" geometry \
    >/dev/null 2>&1
rc=$?
set -e
[ "$rc" -eq 2 ] || {
    echo "A=16 must fail before deployment (got rc=$rc)" >&2
    exit 1
}

set +e
DPUMESH_ENV_FILE="$empty_env" DPUMESH_THROUGHPUT_WORKERS=5 \
    bash "$ROOT/bench/bench.sh" geometry \
    >/dev/null 2>&1
rc=$?
set -e
[ "$rc" -eq 2 ] || {
    echo "unmeasured throughput preset A=5 must fail (got rc=$rc)" >&2
    exit 1
}

# A caller-selected configuration file is an assertion, unlike the optional
# repository default. Refuse a misspelt path instead of silently using defaults.
set +e
DPUMESH_ENV_FILE="$scratch/missing.env" \
    bash "$ROOT/bench/bench.sh" geometry \
    >"$scratch/missing.out" 2>"$scratch/missing.err"
rc=$?
set -e
[ "$rc" -eq 1 ] && [ ! -s "$scratch/missing.out" ] &&
    grep -q "DPUMESH_ENV_FILE does not exist" "$scratch/missing.err" || {
        echo "missing explicit environment file was not rejected cleanly" >&2
        exit 1
    }

set +e
DPUMESH_ENV_FILE= bash "$ROOT/bench/bench.sh" geometry \
    >"$scratch/empty.out" 2>"$scratch/empty.err"
rc=$?
set -e
[ "$rc" -eq 1 ] && [ ! -s "$scratch/empty.out" ] &&
    grep -q "DPUMESH_ENV_FILE must not be empty" "$scratch/empty.err" || {
        echo "empty explicit environment file was not rejected cleanly" >&2
        exit 1
    }

# Validation fixtures consume the same canonical deployment value. A stale
# explicit K/A must not make them inspect only part of a 12-worker deployment.
got=$(DPUMESH_THROUGHPUT_WORKERS=12 DPUMESH_RINGS_PER_POD=3 \
      DPUMESH_ARM_WORKERS=2 resolve_deployed_geometry "$ROOT")
[ "$got" = "12 12" ] || {
    echo "fixture geometry did not follow canonical W=12: $got" >&2
    exit 1
}

# Expert density geometry remains independent and uses effective normalized A.
got=$(DPUMESH_THROUGHPUT_WORKERS= DPUMESH_RINGS_PER_POD=12 \
      DPUMESH_ARM_WORKERS=10 resolve_deployed_geometry "$ROOT")
[ "$got" = "12 6" ] || {
    echo "expert geometry normalization mismatch: $got" >&2
    exit 1
}

# The DPU banner path, against a stub bench.sh: a half geometry is announced
# and ignored in favour of the effective K/A the DPU printed, and a DPU that
# cannot be read fails at once instead of being retried with longer tails.
fake="$scratch/fake"
mkdir -p "$fake/bench"
cat >"$fake/bench/bench.sh" <<'EOS'
#!/usr/bin/env bash
[ "$1" = dpubanner ] || { echo "unexpected: $*" >&2; exit 9; }
case "${FAKE_DPU:-up}" in
    up)    echo "[12:00:00] DPU PROXY MODE ON N/K/A=24/12/12 L7=all" ;;
    empty) exit 0 ;;
    down)  exit 255 ;;
esac
EOS
chmod +x "$fake/bench/bench.sh"

got=$(DPUMESH_THROUGHPUT_WORKERS= DPUMESH_RINGS_PER_POD=8 DPUMESH_ARM_WORKERS= \
      resolve_deployed_geometry "$fake" 2>"$fake/err")
[ "$got" = "12 12" ] && grep -q "only one of DPUMESH_RINGS_PER_POD=8" "$fake/err" || {
    echo "half geometry must be announced and the banner used: got=$got err=$(cat "$fake/err")" >&2
    exit 1
}

got=$(DPUMESH_THROUGHPUT_WORKERS= DPUMESH_RINGS_PER_POD= DPUMESH_ARM_WORKERS= \
      resolve_deployed_geometry "$fake" 2>"$fake/err")
[ "$got" = "12 12" ] && [ ! -s "$fake/err" ] || {
    echo "no geometry must read the banner silently: got=$got err=$(cat "$fake/err")" >&2
    exit 1
}

for mode in down empty; do
    rc=0
    got=$(FAKE_DPU=$mode DPUMESH_THROUGHPUT_WORKERS= DPUMESH_RINGS_PER_POD= \
          DPUMESH_ARM_WORKERS= resolve_deployed_geometry "$fake" 2>"$fake/err") || rc=$?
    [ "$rc" -eq 1 ] && [ -z "$got" ] && grep -q "DPU" "$fake/err" || {
        echo "DPU $mode must fail with rc=1 and a DPU message: rc=$rc got=$got err=$(cat "$fake/err")" >&2
        exit 1
    }
done

echo "bench_geometry_test: PASS"
