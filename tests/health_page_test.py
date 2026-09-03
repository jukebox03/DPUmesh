#!/usr/bin/env python3
"""What the health page must not get wrong.

The page is the only place the nightly records are read from, so a rendering
mistake is indistinguishable from the node never having been in that state. The
two claims it makes are checked here: which runs count as faults, and where the
DPU's topology changed.
"""

import contextlib
import importlib.util
import io
import json
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MODULE_PATH = ROOT / "ci" / "health-page.py"
SPEC = importlib.util.spec_from_file_location("health_page", MODULE_PATH)
HEALTH = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(HEALTH)


def record(ts, status, workers=None, **extra):
    """One nightly record. A run states a topology only if it got that far."""
    row = {"ts": ts, "status": status, "commit": "abc1234"}
    if workers is not None:
        row.update(dpu_dpa_threads=32, dpu_rings_per_pod=8, dpu_workers=workers,
                   dpu_l7="off", dpu_lb="rr", dpu_pods_id="deadbeef")
    row.update(extra)
    return row


def render(rows, extra_lines=()):
    with tempfile.TemporaryDirectory() as temp:
        records = Path(temp) / "health.jsonl"
        with records.open("w") as fh:
            for row in rows:
                fh.write(json.dumps(row) + "\n")
            for line in extra_lines:
                fh.write(line + "\n")
        page = Path(temp) / "index.html"
        argv, sys.argv = sys.argv, ["health-page.py", str(records), str(page)]
        try:
            with contextlib.redirect_stdout(io.StringIO()), \
                 contextlib.redirect_stderr(io.StringIO()):
                HEALTH.main()
        finally:
            sys.argv = argv
        return page.read_text()


def marks(html):
    return html.count("<tr class=moved>")


# A redeploy is marked, and the run that introduces the first known topology is
# not: there is nothing before it to differ from.
assert marks(render([
    record("2026-08-19T19:00:00Z", "ok", workers=8, answered="dpumesh"),
    record("2026-08-20T19:00:00Z", "ok", workers=1, answered="dpumesh"),
])) == 1
assert marks(render([record("2026-08-19T19:00:00Z", "ok", workers=8)])) == 0
assert marks(render([
    record("2026-08-19T19:00:00Z", "ok", workers=8),
    record("2026-08-20T19:00:00Z", "ok", workers=8),
])) == 0

# A night that states no topology is a gap in the comparison, not the end of it:
# idle nights are ordinary on a research machine, and a redeploy across one is
# still marked.
assert marks(render([
    record("2026-08-19T19:00:00Z", "ok", workers=8),
    record("2026-08-20T19:00:00Z", "idle"),
    record("2026-08-21T19:00:00Z", "ok", workers=1),
])) == 1
assert marks(render([
    record("2026-08-19T19:00:00Z", "ok", workers=8),
    record("2026-08-20T19:00:00Z", "busy"),
    record("2026-08-21T19:00:00Z", "no_dpu"),
    record("2026-08-22T19:00:00Z", "ok", workers=8),
])) == 0

# Records arrive appended, but the page is ordered by time.
out_of_order = render([
    record("2026-08-21T19:00:00Z", "ok", workers=1),
    record("2026-08-19T19:00:00Z", "ok", workers=8),
])
assert marks(out_of_order) == 1
assert out_of_order.index("08-21") < out_of_order.index("08-19")   # newest first

# Faults. busy and idle are states of the machine, not failures of it; an
# unknown status is a failure, because the page cannot claim it is not.
page = render([
    record("2026-08-17T19:00:00Z", "ok", workers=8),
    record("2026-08-18T19:00:00Z", "idle"),
    record("2026-08-19T19:00:00Z", "busy", workers=8, busy_path="grpc-dpumesh"),
    record("2026-08-20T19:00:00Z", "no_answer", workers=8, answered="dpumesh"),
    record("2026-08-21T19:00:00Z", "something-new"),
])
assert "2 fault(s)" in page
assert "busy, not probed" in page
assert "something-new" in page          # rendered, not swallowed

# The path column names what answered, or what stayed silent.
assert "grpc-dpumesh" in page
assert HEALTH.STATUS["busy"][0] != HEALTH.STATUS["ok"][0]

# A truncated last line is what an interrupted append leaves behind; it must not
# take the rest of the log with it.
salvaged = render([record("2026-08-19T19:00:00Z", "ok", workers=8)],
                  extra_lines=['{"ts": "2026-08-20T19:00:00Z", "stat'])
assert "1 run(s)" in salvaged

# A run that never reached the DPU has no topology to show.
assert "—" in render([record("2026-08-20T19:00:00Z", "idle")])

print("health_page_test: PASS")
