#!/bin/sh
set -eu

if [ "$#" -ne 3 ]; then
    echo "usage: $0 LIBDPUMESH PRELOAD ABI_MAJOR" >&2
    exit 2
fi

lib=$1
preload=$2
abi_major=$3
expected_soname="libdpumesh.so.$abi_major"

fail() {
    echo "abi_contract_test: $*" >&2
    exit 1
}

for tool in readelf nm awk grep; do
    command -v "$tool" >/dev/null 2>&1 || fail "required tool not found: $tool"
done
[ -f "$lib" ] || fail "library not found: $lib"
[ -f "$preload" ] || fail "preload library not found: $preload"

soname=$(readelf -d "$lib" | awk '/Library soname:/ {
    sub(/^.*\[/, ""); sub(/\].*$/, ""); print
}')
[ "$soname" = "$expected_soname" ] ||
    fail "expected SONAME $expected_soname, found ${soname:-<none>}"

symbols=$(nm -D --defined-only "$lib" | awk '{ print $3 }')
for symbol in \
    dmesh_create_channel dmesh_destroy_channel dmesh_pod_id dmesh_msg_max \
    dmesh_post_max dmesh_create_eq dmesh_destroy_eq dmesh_eq_fd \
    dmesh_create_qp dmesh_destroy_qp dmesh_abort_qp dmesh_alloc \
    dmesh_post_send dmesh_flush dmesh_get_tx_stats dmesh_poll_eq \
    dmesh_release_rx_buffer
do
    if ! printf '%s\n' "$symbols" | grep -Fqx "$symbol"; then
        fail "missing public symbol: $symbol"
    fi
done

for symbol in \
    dmesh_create_cq dmesh_destroy_cq dmesh_cq_fd dmesh_poll_cq \
    dmesh_wc_release
do
    if printf '%s\n' "$symbols" | grep -Fqx "$symbol"; then
        fail "legacy public symbol still exported: $symbol"
    fi
done

needed=$(readelf -d "$preload" | awk '/Shared library:/ {
    sub(/^.*\[/, ""); sub(/\].*$/, ""); print
}')
if ! printf '%s\n' "$needed" | grep -Fqx "$expected_soname"; then
    fail "$preload does not require $expected_soname"
fi

echo "abi_contract_test: PASS"
