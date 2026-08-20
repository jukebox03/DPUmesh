#!/usr/bin/env bash
# If a public header changed, require either an ABI_MAJOR bump or an explicit
# "ABI-Impact:" line in the commit messages. The point is a conscious decision,
# not a mandatory bump.
set -u

base=${1:-}
if [ -z "$base" ] || [ "$base" = "0000000000000000000000000000000000000000" ]; then
  echo "no base commit to compare against, skipping"
  exit 0
fi

if ! git cat-file -e "${base}^{commit}" 2>/dev/null; then
  echo "base commit $base not present locally, skipping"
  exit 0
fi

watched="include/dpumesh linkerd/include"

changed=$(git diff --name-only "$base" HEAD -- $watched)
if [ -z "$changed" ]; then
  echo "no public header changes"
  exit 0
fi

echo "changed public headers:"
echo "$changed" | sed 's/^/  /'

if git diff "$base" HEAD -- Makefile | grep -qE '^[-+]ABI_MAJOR'; then
  current=$(grep -E '^ABI_MAJOR' Makefile | awk '{print $3}')
  echo "RESULT: ABI_MAJOR was bumped (now $current)"
  exit 0
fi

if git log --format='%B' "$base..HEAD" | grep -qiE '^ABI-Impact:'; then
  echo "RESULT: ABI impact declared in a commit message:"
  git log --format='%B' "$base..HEAD" | grep -iE '^ABI-Impact:' | sed 's/^/  /'
  exit 0
fi

cat <<'MSG'
::error::Public headers changed without an ABI decision.
Do one of these:
  1. Bump ABI_MAJOR in the Makefile if the change breaks binary compatibility
     (field added or reordered in a public struct, signature change, symbol removed).
  2. Add a line to the commit message stating why it is safe, e.g.
     ABI-Impact: none (comment only)
     ABI-Impact: none (new function appended, no layout change)
MSG
exit 1
