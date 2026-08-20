#!/usr/bin/env python3
"""Turn one raw bench.sh reply line into a JSON object on stdout.

    bench/bench.sh point dpumesh 64 64 32 10 1000 2 | ci/bench-to-json.py sha=abc123

The reply's first token is the run status from bench_result_status(): "OK" only
when the run completed requests with no failed request and no failed worker.
Anything else is not a measurement, so it is refused here instead of being
recorded as a data point. Remaining tokens are key=value; numbers become JSON
numbers and everything else stays a string.

Extra KEY=VALUE arguments are merged in as metadata (commit, timestamp, label).
"""
import json
import sys


def typed(value):
    for cast in (int, float):
        try:
            return cast(value)
        except ValueError:
            pass
    return value


def main():
    out = {}
    for arg in sys.argv[1:]:
        key, sep, value = arg.partition("=")
        if not sep:
            sys.exit(f"bench-to-json: metadata must be KEY=VALUE, got {arg!r}")
        out[key] = typed(value)

    # bench.sh prints its own notices (a missing .env, for one) on stdout ahead
    # of the reply, so the point is the last line that starts with a status.
    text = sys.stdin.read()
    line = ""
    for candidate in text.splitlines():
        head = candidate.split()[:1]
        if head and head[0] in ("OK", "ERR"):
            line = candidate.strip()
    if not line:
        sys.exit("bench-to-json: no reply line in:\n" + (text.strip() or "<nothing>"))

    status, *fields = line.split()
    if status != "OK":
        sys.exit(f"bench-to-json: refusing a point that is not OK: {line[:200]}")

    out["status"] = status
    for field in fields:
        key, sep, value = field.partition("=")
        if sep:
            out[key] = typed(value)

    json.dump(out, sys.stdout, sort_keys=True)
    print()


if __name__ == "__main__":
    main()
