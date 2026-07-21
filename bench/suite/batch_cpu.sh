#!/bin/bash
# ABI 3 exposes one automatic-batching policy, so this comparison is unavailable.
echo "batch_cpu: unavailable on ABI 3 (automatic transport batching); use the historical report data or checkout commit af19365" >&2
exit 2
