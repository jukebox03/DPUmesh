#!/bin/bash
# The four-way SEND_MORE/reply_batch ablation belonged to the ABI-1 transport.
# ABI 2 automatically submits complete transport units and explicit flush submits
# the trailing partial, so the old switches no longer describe different
# implementations. Preserve the raw historical data in bench/report/data, but
# refuse to generate mislabeled rows.
echo "batch_ablation: unavailable on ABI 2 (automatic transport batching); use the historical report data or checkout commit af19365" >&2
exit 2
