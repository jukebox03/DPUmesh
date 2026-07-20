#!/bin/bash
# The unbatched-vs-SEND_MORE CPU experiment was an ABI-1 experiment. ABI 2 has one
# send contract: post commits and submits complete units; flush submits the tail.
# Do not silently compare two labels that now execute identical code.
echo "batch_cpu: unavailable on ABI 2 (automatic transport batching); use the historical report data or checkout commit af19365" >&2
exit 2
