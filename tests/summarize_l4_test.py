#!/usr/bin/env python3

import importlib.util
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MODULE_PATH = ROOT / "bench" / "suite" / "summarize_l4.py"
SPEC = importlib.util.spec_from_file_location("summarize_l4", MODULE_PATH)
SUMMARIZE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(SUMMARIZE)


with tempfile.TemporaryDirectory() as temp:
    meta = Path(temp) / "meta.txt"
    meta.write_text(
        "params=CONFIGS={dpumesh-preload dpumesh-native} "
        "REPS=3 IDLE_REPS=2 DUR=10\n"
    )
    assert SUMMARIZE.parse_meta(meta) == (
        ["dpumesh-preload", "dpumesh-native"],
        3,
        2,
    )
    meta.write_text("params=FRAME_SIZES={64} REPS=3 DUR=10\n")
    assert SUMMARIZE.parse_meta(meta) == ([], 3, 3)
    assert SUMMARIZE.perf_enabled(meta)
    meta.write_text("params=FRAME_SIZES={64} ENABLE_PERF=0\n")
    assert not SUMMARIZE.perf_enabled(meta)

print("summarize_l4_test: PASS")
