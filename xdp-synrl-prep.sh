#!/bin/bash
# ExecStartPre for xdp-synrl.service.
#
# Forces the default-route interface into a queue layout that satisfies
# gve's native XDP precondition (current rx == current tx, both <= max/2).
# 4 vCPU machines have max rx=tx=2, so rx 1 tx 1 always passes the check.
set -eu
IFACE="$(ip route show default | awk '/default/ {print $5; exit}')"
if [ -z "$IFACE" ]; then
    echo "xdp-synrl-prep: no default route, refusing to start" >&2
    exit 1
fi
ethtool -L "$IFACE" rx 1 tx 1 >/dev/null 2>&1 || true
