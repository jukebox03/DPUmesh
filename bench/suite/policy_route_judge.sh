#!/bin/bash
# Pure judgement helpers for policy_route.sh. Keep these free of cluster I/O so
# a stopped client or an unreadable DPU gauge can be exercised in hosted CI.

policy_route_result_field() {  # policy_route_result_field <reply> <name>
    sed -n "s/.*[[:space:]]$2=\([^ ]*\).*/\1/p" <<<"$1"
}

policy_route_judge_lb() {  # <reply> <ready> <expected-ready> <backends> <min> <max>
    local result="$1" ready="$2" expected_ready="$3"
    local backends="$4" min_backends="$5" max_backends="$6"

    LB_JUDGE_RCNT=$(policy_route_result_field "$result" rcnt)
    LB_JUDGE_FAIL=$(policy_route_result_field "$result" fail)
    : "${LB_JUDGE_RCNT:=NA}" "${LB_JUDGE_FAIL:=NA}"
    LB_JUDGE_OBSERVED="rcnt=$LB_JUDGE_RCNT+fail=$LB_JUDGE_FAIL+ready=$ready+backends=$backends"
    LB_JUDGE_REASON=""

    case "$LB_JUDGE_RCNT" in
        ''|NA|*[!0-9]*) LB_JUDGE_REASON="the client returned no readable request count" ;;
    esac
    if [ -z "$LB_JUDGE_REASON" ] && [ "$LB_JUDGE_RCNT" -le 0 ]; then
        LB_JUDGE_REASON="the client completed no requests"
    fi
    case "$LB_JUDGE_FAIL" in
        ''|NA|*[!0-9]*)
            [ -n "$LB_JUDGE_REASON" ] || LB_JUDGE_REASON="the client returned no readable failure count" ;;
    esac
    if [ -z "$LB_JUDGE_REASON" ] && [ "$LB_JUDGE_FAIL" -ne 0 ]; then
        LB_JUDGE_REASON="the client reported failures"
    fi
    case "$ready" in
        ''|NA|*[!0-9]*)
            [ -n "$LB_JUDGE_REASON" ] || LB_JUDGE_REASON="the DPU ready-endpoint gauge was unreadable" ;;
    esac
    if [ -z "$LB_JUDGE_REASON" ] && [ "$ready" -ne "$expected_ready" ]; then
        LB_JUDGE_REASON="the DPU held $ready ready endpoints; expected $expected_ready"
    fi
    case "$backends" in
        ''|*[!0-9]*)
            [ -n "$LB_JUDGE_REASON" ] || LB_JUDGE_REASON="the backend attribution was unreadable" ;;
    esac
    if [ -z "$LB_JUDGE_REASON" ] &&
       { [ "$backends" -lt "$min_backends" ] || [ "$backends" -gt "$max_backends" ]; }; then
        LB_JUDGE_REASON="the reading reached $backends backends; expected $min_backends..$max_backends"
    fi

    [ -z "$LB_JUDGE_REASON" ]
}

policy_route_csv_valid() {  # policy_route_csv_valid <stages.csv>
    awk -F, '
        BEGIN {
            header="stage,fixture,arm,expected,observed,verdict,rcnt,fail,admitted,denied,route_hits,target_mismatch,note"
        }
        NR == 1 {
            if ($0 != header) {
                printf "line 1 is not the 13-column stages header\n" > "/dev/stderr";
                bad=1
            }
            next
        }
        NF != 13 {
            printf "line %d has %d columns; expected 13\n", NR, NF > "/dev/stderr";
            bad=1;
            next
        }
        $6 != "PASS" && $6 != "FAIL" {
            printf "line %d has no PASS/FAIL verdict\n", NR > "/dev/stderr";
            bad=1
        }
        END {
            if (NR == 0) {
                print "CSV is empty; expected the stages header" > "/dev/stderr";
                bad=1
            }
            exit bad
        }
    ' "$1"
}
