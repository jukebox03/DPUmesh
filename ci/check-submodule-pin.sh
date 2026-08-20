#!/usr/bin/env bash
# Verify the linkerd2-proxy submodule pin is still reachable upstream.
# Being behind the branch tip is fine; an unreachable pin is not.
set -u

sub="linkerd/port/linkerd2-proxy"

pinned=$(git ls-tree HEAD "$sub" | awk '{print $3}')
url=$(git config -f .gitmodules --get "submodule.$sub.url")
branch=$(git config -f .gitmodules --get "submodule.$sub.branch")

if [ -z "$pinned" ] || [ -z "$url" ] || [ -z "$branch" ]; then
  echo "::error::cannot read submodule config for $sub"
  exit 1
fi

echo "submodule : $sub"
echo "url       : $url"
echo "branch    : $branch"
echo "pinned    : $pinned"

tip=$(git ls-remote "$url" "refs/heads/$branch" | awk '{print $1}')
if [ -z "$tip" ]; then
  echo "::error::branch '$branch' no longer exists at $url"
  exit 1
fi
echo "tip       : $tip"

if [ "$pinned" = "$tip" ]; then
  echo "RESULT: in sync"
  exit 0
fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

if ! git clone --filter=blob:none --bare --quiet --branch "$branch" "$url" "$tmp/up.git"; then
  echo "::error::cannot clone $url"
  exit 1
fi

if ! git -C "$tmp/up.git" cat-file -e "${pinned}^{commit}" 2>/dev/null; then
  echo "::error::pinned commit $pinned is GONE from upstream (force-push + gc); the build is no longer reproducible"
  exit 1
fi

if git -C "$tmp/up.git" merge-base --is-ancestor "$pinned" "$tip"; then
  behind=$(git -C "$tmp/up.git" rev-list --count "$pinned..$tip")
  echo "RESULT: behind by $behind commit(s), pin still reachable"
  exit 0
fi

echo "::error::pinned commit exists but is not an ancestor of '$branch'; upstream history was rewritten"
exit 1
