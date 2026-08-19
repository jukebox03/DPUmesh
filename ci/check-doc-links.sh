#!/usr/bin/env bash
fail=0
for md in $(git ls-files '*.md'); do
  dir=$(dirname "$md")
  links=$(grep -oE '\]\([^)#][^)]*\)' "$md" | sed 's/^](//; s/)$//')
  for link in $links; do
    case "$link" in
      http://*|https://*|mailto:*) continue ;;
    esac
    target="${link%%#*}"
    [ -z "$target" ] && continue
    if [ ! -e "$dir/$target" ]; then
      echo "BROKEN  $md  ->  $link"
      fail=1
    fi
  done
done
[ "$fail" -eq 0 ] && echo "all relative links resolve"
exit $fail
