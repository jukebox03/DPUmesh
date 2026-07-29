#!/bin/bash
set -euo pipefail

SUITE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec "$SUITE_DIR/l4_proxy_data.sh" "$@"
