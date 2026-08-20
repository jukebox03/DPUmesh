#!/usr/bin/env python3
"""Render the accumulated benchmark history as one self-contained HTML page.

    ci/bench-page.py history.jsonl index.html

Every point in the history was measured at an operating point named in
ci/bench-frozen.txt, so a line here is comparable along its own length. Points
are grouped into a family (the label up to the last dash) and drawn with the
dpumesh and tcp series together: when both move on the same day the machine
moved, not DPUmesh.

No threshold is applied. The page records; reading it is a person's job until
the noise band of this machine is known.
"""
import html
import json
import sys
from collections import OrderedDict

METRICS = [
    ("mrps", "throughput (Mrps)", "higher is better"),
    ("p50", "p50 latency (us)", "lower is better"),
    ("p99", "p99 latency (us)", "lower is better"),
]
COLORS = {"dpumesh": "#2f6fdb", "tcp": "#c2632b"}

W, H = 420, 190          # one chart
PAD_L, PAD_R, PAD_T, PAD_B = 52, 12, 10, 28


def load(path):
    rows = []
    with open(path) as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            try:
                rows.append(json.loads(line))
            except json.JSONDecodeError:
                print(f"skipping malformed history line: {line[:80]}", file=sys.stderr)
    rows.sort(key=lambda r: (r.get("ts", ""), r.get("label", "")))
    return rows


def family_of(label):
    return label.rsplit("-", 1)[0] if "-" in label else label


def chart(runs, family, metric, title, note):
    """runs: ordered list of timestamps -> the shared x axis of every series."""
    series = OrderedDict()
    for row in runs["rows"]:
        if family_of(row.get("label", "")) != family or metric not in row:
            continue
        series.setdefault(row.get("solution", "?"), []).append(
            (runs["index"][row["ts"]], row[metric], row))

    values = [v for pts in series.values() for _, v in [(p[0], p[1]) for p in pts]]
    if not values:
        return ""
    lo, hi = min(values), max(values)
    if hi == lo:
        hi = lo + (abs(lo) or 1) * 0.05
    span = hi - lo
    lo -= span * 0.08
    hi += span * 0.08
    n = max(len(runs["order"]) - 1, 1)

    def x(i):
        return PAD_L + (W - PAD_L - PAD_R) * (i / n)

    def y(v):
        return PAD_T + (H - PAD_T - PAD_B) * (1 - (v - lo) / (hi - lo))

    body = [
        f'<rect x="{PAD_L}" y="{PAD_T}" width="{W-PAD_L-PAD_R}" height="{H-PAD_T-PAD_B}" '
        f'class="plot"/>',
        f'<text x="{PAD_L-6}" y="{y(hi)+4:.1f}" class="tick" text-anchor="end">{hi:.4g}</text>',
        f'<text x="{PAD_L-6}" y="{y(lo)+4:.1f}" class="tick" text-anchor="end">{lo:.4g}</text>',
    ]
    for sol, pts in series.items():
        color = COLORS.get(sol, "#777")
        path = " ".join(f"{x(i):.1f},{y(v):.1f}" for i, v, _ in pts)
        body.append(f'<polyline points="{path}" fill="none" stroke="{color}" stroke-width="1.6"/>')
        for i, v, row in pts:
            tip = html.escape(f'{row.get("label")} {metric}={v} {row.get("commit","")} {row.get("ts","")}')
            body.append(
                f'<circle cx="{x(i):.1f}" cy="{y(v):.1f}" r="2.6" fill="{color}">'
                f"<title>{tip}</title></circle>")
    legend = " ".join(
        f'<span class="key"><i style="background:{COLORS.get(s, "#777")}"></i>{html.escape(s)}</span>'
        for s in series)
    first = runs["order"][0][:10] if runs["order"] else ""
    last = runs["order"][-1][:10] if runs["order"] else ""
    body.append(f'<text x="{PAD_L}" y="{H-8}" class="tick">{first}</text>')
    body.append(f'<text x="{W-PAD_R}" y="{H-8}" class="tick" text-anchor="end">{last}</text>')
    return (f'<figure><figcaption>{html.escape(family)} &middot; {html.escape(title)}'
            f'<small>{html.escape(note)}</small></figcaption>'
            f'<svg viewBox="0 0 {W} {H}" role="img">{"".join(body)}</svg>'
            f'<div class="legend">{legend}</div></figure>')


def main():
    if len(sys.argv) != 3:
        sys.exit("usage: bench-page.py <history.jsonl> <index.html>")
    rows = load(sys.argv[1])
    order = sorted({r["ts"] for r in rows if "ts" in r})
    runs = {"rows": rows, "order": order, "index": {ts: i for i, ts in enumerate(order)}}
    families = sorted({family_of(r.get("label", "")) for r in rows if r.get("label")})

    charts = "".join(
        chart(runs, fam, metric, title, note)
        for fam in families
        for metric, title, note in METRICS)
    latest = order[-1] if order else "never"
    commits = len({r.get("commit") for r in rows})

    page = f"""<!doctype html>
<meta charset="utf-8">
<title>DPUmesh benchmark history</title>
<style>
  :root {{ color-scheme: light dark; }}
  body {{ font: 14px/1.5 -apple-system, system-ui, sans-serif; margin: 2rem auto; max-width: 1000px;
          padding: 0 1rem; }}
  h1 {{ font-size: 1.3rem; margin-bottom: .2rem; }}
  p.meta {{ color: #666; margin-top: 0; }}
  .grid {{ display: grid; grid-template-columns: repeat(auto-fit, minmax(320px, 1fr)); gap: 1rem; }}
  figure {{ margin: 0; border: 1px solid #8883; border-radius: 6px; padding: .6rem; }}
  figcaption {{ font-weight: 600; font-size: .85rem; margin-bottom: .3rem; }}
  figcaption small {{ font-weight: 400; color: #888; margin-left: .4rem; }}
  svg {{ width: 100%; height: auto; }}
  .plot {{ fill: #8881; stroke: #8884; }}
  .tick {{ font-size: 9px; fill: #888; }}
  .legend {{ font-size: .75rem; color: #888; }}
  .key {{ margin-right: .6rem; }}
  .key i {{ display: inline-block; width: .6rem; height: .6rem; border-radius: 50%; margin-right: .25rem; }}
</style>
<h1>DPUmesh benchmark history</h1>
<p class="meta">{len(rows)} points across {commits} commits &middot; last run {html.escape(latest)}
&middot; operating points frozen in <code>ci/bench-frozen.txt</code> &middot; no regression threshold is
applied: this page records, it does not judge.</p>
<div class="grid">{charts}</div>
"""
    with open(sys.argv[2], "w") as fh:
        fh.write(page)
    print(f"bench-page: {len(rows)} points, {len(families)} families -> {sys.argv[2]}")


if __name__ == "__main__":
    main()
