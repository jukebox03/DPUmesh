#!/bin/sh
set -eu

if [ "$(id -u)" -ne 0 ]; then
    echo "install-host.sh must run as root" >&2
    exit 1
fi

project=${1:-}
if [ -z "$project" ] || [ ! -f "$project/node/dpumeshd.py" ]; then
    echo "usage: install-host.sh PROJECT_ROOT" >&2
    exit 2
fi

# Never replace the Python package while systemd is concurrently retrying a
# failed daemon. The caller starts/restarts the service after installing its
# environment and credentials.
if systemctl is-active --quiet dpumeshd.service 2>/dev/null ||
   systemctl is-failed --quiet dpumeshd.service 2>/dev/null; then
    systemctl stop dpumeshd.service
fi

install -d -o root -g root -m 0755 /opt/dpumesh/bin /opt/dpumesh/lib \
    /opt/dpumesh/python /etc/dpumesh/tls
install -o root -g root -m 0555 "$project/build/bin/dmesh_broker" \
    /opt/dpumesh/bin/dmesh_broker
install -o root -g root -m 0555 "$project/build/lib/libdpumesh.so.5" \
    /opt/dpumesh/lib/libdpumesh.so.5
rm -rf /opt/dpumesh/python/node
cp -a "$project/node" /opt/dpumesh/python/node
chown -R root:root /opt/dpumesh/python/node
find /opt/dpumesh/python/node -type d -exec chmod 0555 {} \;
find /opt/dpumesh/python/node -type f -exec chmod 0444 {} \;

if [ ! -x /opt/dpumesh/venv/bin/python ]; then
    python3 -m venv /opt/dpumesh/venv
fi
/opt/dpumesh/venv/bin/pip install --disable-pip-version-check \
    'grpcio>=1.83,<2' 'protobuf>=7.35,<8'

install -o root -g root -m 0644 "$project/packaging/dpumeshd.service" \
    /etc/systemd/system/dpumeshd.service
if [ ! -e /etc/dpumesh/dpumeshd.env ]; then
    install -o root -g root -m 0600 "$project/packaging/dpumeshd.env.example" \
        /etc/dpumesh/dpumeshd.env
fi
systemctl daemon-reload
echo "installed; provision /etc/dpumesh/tls and dpumeshd.env before enable" >&2
