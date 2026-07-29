#!/bin/sh
set -eu

for binary in "$@"; do
  for frame in 64 1024 8192; do
    body=$((frame - 16))
    output=$("$binary" --selftest "$body" 2 0.10 1000 const)
    case "$output" in
        "OK selftest=1 "*) ;;
        *)
            echo "generator_selftest_test: unexpected output from $binary: $output" >&2
            exit 1
            ;;
    esac
    reported_frame=$(printf '%s\n' "$output" |
        awk '{for(i=1;i<=NF;i++)if($i~/^frame=/){sub(/^frame=/,"",$i);print $i}}')
    [ "$reported_frame" -eq "$frame" ] || {
        echo "generator_selftest_test: frame mismatch from $binary: $output" >&2
        exit 1
    }
    scheduled=$(printf '%s\n' "$output" |
        awk '{for(i=1;i<=NF;i++)if($i~/^scheduled=/){sub(/^scheduled=/,"",$i);print $i}}')
    drops=$(printf '%s\n' "$output" |
        awk '{for(i=1;i<=NF;i++)if($i~/^drops=/){sub(/^drops=/,"",$i);print $i}}')
    [ "${scheduled:-0}" -ge 90 ] || {
        echo "generator_selftest_test: too few arrivals from $binary: $output" >&2
        exit 1
    }
    [ "${drops:-1}" -eq 0 ] || {
        echo "generator_selftest_test: unexpected scheduler drops from $binary: $output" >&2
        exit 1
    }
  done
done

echo "generator_selftest_test: PASS"
