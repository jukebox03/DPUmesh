#!/bin/bash
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
# shellcheck source=../bench/suite/policy_route_judge.sh
. "$ROOT/bench/suite/policy_route_judge.sh"

expect_pass() {
    if ! policy_route_judge_lb "$@"; then
        echo "expected PASS: $LB_JUDGE_REASON" >&2
        exit 1
    fi
}

expect_fail() {
    if policy_route_judge_lb "$@"; then
        echo "expected FAIL: $LB_JUDGE_OBSERVED" >&2
        exit 1
    fi
    [ -n "$LB_JUDGE_REASON" ] || {
        echo "failed judgement carried no reason" >&2
        exit 1
    }
}

expect_pass 'OK rcnt=100 fail=0 dist=1:100' 3 3 1 1 3
expect_pass 'OK rcnt=100 fail=0 dist=1:50;2:50' 2 2 2 1 2

# Missing client data, errors, and zero traffic are never successes.
expect_fail ''                                      1 1 1 1 1
expect_fail 'ERR context deadline exceeded'         1 1 1 1 1
expect_fail 'OK fail=0 dist=1:100'                  1 1 1 1 1
expect_fail 'OK rcnt=0 fail=0 dist=NA'              1 1 0 1 1
expect_fail 'OK rcnt=100 fail=1 dist=1:100'         1 1 1 1 1

# Client success cannot hide a missing or contradictory DPU reading.
expect_fail 'OK rcnt=100 fail=0 dist=1:100'        NA 1 1 1 1
expect_fail 'OK rcnt=100 fail=0 dist=1:100' mixed-1-2 2 1 1 1
expect_fail 'OK rcnt=100 fail=0 dist=1:100'         2 1 1 1 1
expect_fail 'OK rcnt=100 fail=0 dist=NA'            1 1 0 1 1
expect_fail 'OK rcnt=100 fail=0 dist=1:50;2:50'     2 2 2 1 1

csv=$(mktemp)
trap 'rm -f "$csv"' EXIT
header='stage,fixture,arm,expected,observed,verdict,rcnt,fail,admitted,denied,route_hits,target_mismatch,note'
valid_row='L1,opaque,dpumesh,serve,serve,PASS,100,0,,,,,dist=0:50;1:50'
printf '%s\n' \
    "$header" \
    "$valid_row" >"$csv"
policy_route_csv_valid "$csv"

: >"$csv"
if policy_route_csv_valid "$csv" 2>/dev/null; then
    echo "empty CSV passed the schema gate" >&2
    exit 1
fi

printf '%s\n' \
    'stage,fixture,arm,expected,observed,result,rcnt,fail,admitted,denied,route_hits,target_mismatch,note' \
    "$valid_row" >"$csv"
if policy_route_csv_valid "$csv" 2>/dev/null; then
    echo "CSV with the wrong header passed the schema gate" >&2
    exit 1
fi

printf '%s\n' \
    "$header" \
    'L2,opaque,dpumesh,serve,serve,,100,0,,,,,dist=0:50;1:50' >"$csv"
if policy_route_csv_valid "$csv" 2>/dev/null; then
    echo "CSV row without a verdict passed the schema gate" >&2
    exit 1
fi

printf '%s\n' \
    "$header" \
    "$valid_row" \
    'L2,opaque,dpumesh,serve,serve,PASS,100,0,,,,,dist=0:50,1:50' >"$csv"
if policy_route_csv_valid "$csv" 2>/dev/null; then
    echo "malformed CSV row passed the schema gate" >&2
    exit 1
fi

echo "policy-route judgement contract: pass"
