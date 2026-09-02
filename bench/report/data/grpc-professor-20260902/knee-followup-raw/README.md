# Knee follow-up raw receipts

All points use the same A8 expanded binary, `N/K/A=32/8/8`, Host 9+9 pin,
eight client threads/channels/reactors, constant open-loop arrivals, 10 seconds
and symmetric logical request/response frames. A point is attempted three times
until the first overload; an overload ends that monotonic axis.

- `fresh-64-*`: fresh-deployment 64 B repetitions. The 92k mixed point has one
  clean repetition and one worker-5 stall/drop repetition. The 98k and 99k
  observations are first-bad points on independent fresh deployments.
- `aged-64-99`: the excluded approximately 11-hour deployment observation.
- `fresh-1k-*`: 75.25k mixed and 76k first-bad observations.
- `fresh-8k-*`: the uninterrupted clean 25.25--29.75k progression and two 30k
  repetitions, one clean and one failed. Together they make 30k a mixed point.

Each directory contains the generator's `points.csv` and timestamped
`sweep.log`. The report-level `knee-followup-summary.csv` contains only medians
and the clean/mixed/bad classification; these files are the source receipt.
