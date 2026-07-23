#!/bin/bash
# ABI 4 exposes one automatic-batching policy, so this ablation is unavailable.
echo "batch_ablation: unavailable on ABI 4 (automatic transport batching); use the historical report data or checkout commit af19365" >&2
exit 2
