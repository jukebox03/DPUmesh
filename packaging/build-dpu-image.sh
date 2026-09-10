#!/bin/bash
set -euo pipefail
project=${1:?usage: build-dpu-image.sh PROJECT_ROOT IMAGE}
image=${2:?image required}
[ "$(uname -m)" = aarch64 ]
context=$(mktemp -d)
trap 'rm -rf "$context"' EXIT
install -d "$context/rootfs/usr/local/bin" "$context/rootfs/usr/local/lib/dpumesh"
binary="$project/doca/build/dpumesh_dpu"
install -m 0755 "$binary" "$context/rootfs/usr/local/bin/"
install -m 0755 "$project/packaging/dpu-entrypoint.sh" "$context/rootfs/usr/local/bin/dpumesh-dpu-entrypoint"
install -m 0555 "$project/dpu/feed_receiver.py" "$context/rootfs/usr/local/bin/dpumesh-feed-receiver"
install -m 0555 "$project/packaging/linkerd-init.sh" "$context/rootfs/usr/local/bin/dpumesh-linkerd-init"
# Include transitive dependencies resolved by the ARM dynamic loader. The
# loader/glibc remain from the matching Ubuntu base rather than being replaced.
ldd "$binary" > "$context/ldd.txt"
if grep -q 'not found' "$context/ldd.txt"; then cat "$context/ldd.txt" >&2; exit 1; fi
while read -r path; do
    case "$path" in */libc.so.*|*/libm.so.*|*/libpthread.so.*|*/libdl.so.*|*/librt.so.*|*/ld-linux-*) continue;; esac
    cp -L "$path" "$context/rootfs/usr/local/lib/dpumesh/"
done < <(awk '$2 == "=>" && $3 ~ /^\// {print $3}' "$context/ldd.txt")
# verbs loads providers dynamically; they do not appear in ldd.
for path in /usr/lib/aarch64-linux-gnu/libibverbs/libmlx5*.so /usr/lib/aarch64-linux-gnu/libibverbs/libmlx5*.so.*; do
    [ ! -e "$path" ] || { install -d "$context/rootfs/usr/lib/aarch64-linux-gnu/libibverbs"; cp -L "$path" "$context/rootfs/usr/lib/aarch64-linux-gnu/libibverbs/"; }
done
if [ -d /etc/libibverbs.d ]; then cp -a /etc/libibverbs.d "$context/rootfs/etc-libibverbs.d"; install -d "$context/rootfs/etc"; mv "$context/rootfs/etc-libibverbs.d" "$context/rootfs/etc/libibverbs.d"; fi
docker build -f "$project/packaging/dpu.Dockerfile" -t "$image" "$context"
