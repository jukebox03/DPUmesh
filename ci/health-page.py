#!/usr/bin/env python3
"""Render the accumulated health records as one self-contained HTML page.

    ci/health-page.py health.jsonl index.html

Two things are on this page and neither is a performance number. The strip says
whether the deployed campaign answered on a given night. The table says what was
deployed, which on a machine whose configuration is a variable of the research is
the more useful half: it is a log of the states this node has been in.

A row is marked where the DPU's topology differs from the last run that stated
one. Runs that state no topology — nothing deployed, a busy machine, a DPU that
did not answer — are gaps in that comparison rather than ends of it, so a
redeploy is still marked when idle nights sit on either side of it.
"""
import html
import json
import sys

# ok: a campaign answered. idle: none deployed, which is a normal state between
# experiments and not a fault. Everything else is a fault.
# busy: the machine was working, so nothing was probed. Not a fault — but a
# client that is wedged rather than occupied also lands here, and the only thing
# that tells them apart is that a wedged one stays here run after run.
STATUS = {
    "ok":          ("#2f8f4e", "answered"),
    "idle":        ("#8a8a8a", "nothing deployed"),
    "busy":        ("#d9a441", "busy, not probed"),
    "no_answer":   ("#c33", "deployed, did not answer"),
    "no_dpu":      ("#c33", "DPU did not state its topology"),
    "no_path":     ("#c33", "no known path in this campaign"),
    "unreachable": ("#c33", "namespace unreachable"),
}
TOPOLOGY = ("dpu_dpa_threads", "dpu_rings_per_pod", "dpu_workers",
            "dpu_l7", "dpu_lb", "dpu_pods_id")


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
                print(f"skipping malformed record: {line[:80]}", file=sys.stderr)
    rows.sort(key=lambda r: r.get("ts", ""))
    return rows


def topology(row):
    return tuple(row.get(k) for k in TOPOLOGY)


def known(signature):
    """A run that never reached the DPU has no topology to compare."""
    return all(part is not None for part in signature)


def nka(row):
    parts = [row.get("dpu_dpa_threads"), row.get("dpu_rings_per_pod"), row.get("dpu_workers")]
    if any(p is None for p in parts):
        return "\u2014"
    return "/".join(str(p) for p in parts)


def main():
    if len(sys.argv) != 3:
        sys.exit("usage: health-page.py <health.jsonl> <index.html>")
    rows = load(sys.argv[1])

    strip = "".join(
        f'<i style="background:{STATUS.get(r.get("status"), ("#c33", "?"))[0]}" '
        f'title="{html.escape(r.get("ts", "?"))} — '
        f'{html.escape(STATUS.get(r.get("status"), ("", r.get("status", "?")))[1])}"></i>'
        for r in rows)

    # In time order, each run that states a topology against the last one that
    # did. A run that states none is skipped over rather than compared against,
    # because it is an absence of information and not a change of state: on a
    # research machine idle nights are ordinary, and a redeploy across one is
    # exactly what this page exists to show.
    moved_at = set()
    previous = None
    for index, row in enumerate(rows):
        signature = topology(row)
        if not known(signature):
            continue
        if previous is not None and signature != previous:
            moved_at.add(index)
        previous = signature

    body = []
    for index in range(len(rows) - 1, -1, -1):        # newest first
        row = rows[index]
        colour, label = STATUS.get(row.get("status"), ("#c33", row.get("status", "?")))
        moved = index in moved_at
        clients = row.get("clients") or []
        body.append(
            f'<tr{" class=moved" if moved else ""}>'
            f'<td class="ts">{html.escape(row.get("ts", "?")[:16].replace("T", " "))}</td>'
            f'<td><b style="color:{colour}">●</b> {html.escape(label)}</td>'
            f'<td>{html.escape(nka(row))}</td>'
            f'<td>{html.escape(str(row.get("dpu_l7") or "—"))}</td>'
            f'<td>{len(clients) or "—"}</td>'
            f'<td class="ts">{html.escape(row.get("answered") or row.get("busy_path") or "—")}</td>'
            f'<td class="ts">{html.escape(str(row.get("commit") or "—"))}</td></tr>')

    latest = rows[-1] if rows else {}
    faults = sum(1 for r in rows if r.get("status") not in ("ok", "idle", "busy"))
    colour, label = STATUS.get(latest.get("status"), ("#8a8a8a", "no runs yet"))

    page = f"""<!doctype html>
<meta charset="utf-8">
<title>DPUmesh node health</title>
<style>
  :root {{ color-scheme: light dark; }}
  body {{ font: 14px/1.6 -apple-system, system-ui, sans-serif; margin: 2rem auto; max-width: 900px;
          padding: 0 1rem; }}
  h1 {{ font-size: 1.3rem; margin-bottom: .2rem; }}
  h2 {{ font-size: 1rem; margin: 2rem 0 .4rem; }}
  p.meta {{ color: #888; margin-top: 0; }}
  .strip {{ display: flex; flex-wrap: wrap; gap: 3px; margin: 1rem 0; }}
  .strip i {{ width: 12px; height: 26px; border-radius: 2px; display: block; }}
  table {{ border-collapse: collapse; width: 100%; font-size: .85rem; }}
  th, td {{ text-align: left; padding: .3rem .5rem; border-bottom: 1px solid #8883; }}
  th {{ color: #888; font-weight: 600; }}
  td.ts {{ font-family: ui-monospace, monospace; font-size: .8rem; }}
  tr.moved td {{ border-top: 2px solid #d9a441; }}
  tr.moved td:first-child::before {{ content: "▲ "; color: #d9a441; }}
  .wrap {{ overflow-x: auto; }}
</style>
<h1>DPUmesh node health</h1>
<p class="meta"><b style="color:{colour}">●</b> {html.escape(label)} &middot; {len(rows)} run(s)
&middot; {faults} fault(s) &middot; last {html.escape(latest.get("ts", "never"))}</p>
<div class="strip">{strip}</div>
<h2>What was deployed</h2>
<p class="meta">A gold rule marks a run where the DPU's topology differs from the last run that
stated one; runs that state none are skipped over, not compared against.
No latency or throughput is recorded here: a single request against whatever happened to be
deployed proves the path carries bytes and nothing else. Performance is measured by hand.</p>
<div class="wrap"><table>
<thead><tr><th>run (UTC)</th><th>result</th><th>N/K/A</th><th>L7 layer</th>
<th>clients</th><th>path</th><th>commit</th></tr></thead>
<tbody>{"".join(body)}</tbody></table></div>
"""
    with open(sys.argv[2], "w") as fh:
        fh.write(page)
    print(f"health-page: {len(rows)} runs, {faults} faults -> {sys.argv[2]}")


if __name__ == "__main__":
    main()
