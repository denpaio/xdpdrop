# xdpdrop

Linux XDP program that token-bucket rate-limits TCP SYN packets going to HAProxy frontends on dst port **2000 / 2001 / 2002**.

When HAProxy sits behind GCP GLB / Cloudflare Spectrum, the source IP is always the upstream LB — per-source rate-limit is useless. This program runs in `xdpdrv` (native) mode and unconditionally drops surplus SYN packets at NIC driver layer, capping how many new connections HAProxy must parse PROXY Protocol for.

## Parameters

| Item | Value |
|------|-------|
| Filter | TCP, dst port 2000/2001/2002, SYN=1 ACK=0, IPv4 |
| Algorithm | Token bucket, three ports share one bucket |
| Refill | 10 000 tokens/sec |
| Capacity | 10 000 tokens (≈ 1 second of burst) |
| Over-limit action | `XDP_DROP` (no source-IP awareness) |
| Other traffic | `XDP_PASS` |
| Fail-open | map/boundary errors → `XDP_PASS` |
| Attach mode | native (`xdpdrv`), requires `gve` NIC and `current rx == current tx <= max/2` |

## Files

| File | Purpose |
|------|---------|
| `xdp_synrl.c` | BPF program (token bucket + boundary checks + UDP-port filter) |
| `xdp_synrl_loader.c` | libbpf user-space loader (attach in `XDP_FLAGS_DRV_MODE`, SIGTERM detach) |
| `Makefile` | build + install + uninstall targets |
| `xdp-synrl-prep.sh` | systemd `ExecStartPre`: sets `ethtool -L $IFACE rx 1 tx 1` so gve accepts native XDP |
| `xdp-synrl.service` | systemd unit (Type=simple, Restart=on-failure) |

## Build dependencies (Debian 13 / Ubuntu 24.04)

```
sudo apt-get install -y clang libbpf-dev build-essential ethtool linux-headers-$(uname -r)
```

## Build & install

```
make            # builds xdp_synrl.o + xdp_synrl_loader
sudo make install
# installs to:
#   /usr/local/lib/xdpdrop/xdp_synrl.o
#   /usr/local/sbin/xdp_synrl_loader
#   /usr/local/sbin/xdp-synrl-prep.sh
#   /etc/systemd/system/xdp-synrl.service
# runs `systemctl daemon-reload && systemctl enable xdp-synrl`
sudo systemctl start xdp-synrl
```

## Uninstall

```
sudo make uninstall
```

## Verify

```
# program attached in native mode
ip link show "$(ip route show default | awk '/default/{print $5;exit}')" | grep -i xdp
# expect: ... xdp ... prog/xdp id <N>

# loader running
systemctl status xdp-synrl

# program object visible to bpftool
sudo bpftool prog show | grep xdp_synrl
```

Rate-limit can be exercised from a second host on the same VPC:

```
sudo hping3 -S -p 2001 --flood -q <vm-ip>
```

Observe on the SUT:

```
sudo ethtool -S "$(ip route show default | awk '/default/{print $5;exit}')" \
  | grep -E 'rx_xdp_(drop|pass)'
```

`rx_xdp_drop[0]` should climb at the SYN rate minus ~10 k/s; `rx_xdp_pass[0]` should grow at roughly the configured refill rate.

## Pre-requisites for native XDP on GCP

The current implementation targets GCP VMs with the `gve` driver:

* Machine type with ≥ 4 vCPU (so `max queue` ≥ 2 and `current ≤ max/2` is satisfiable).
* `--network-interface=nic-type=GVNIC` (default for `e2-custom-4-*` is `virtio_net`, which does not support native XDP).
* `xdp-synrl-prep.sh` forces `ethtool -L $IFACE rx 1 tx 1` on every start to satisfy gve's native XDP queue constraint.

Background and platform-specific gotchas (gve queue formats, native vs generic XDP) are well-trodden territory — see the gve driver source and the xdp-tools documentation.

## Known limitations

* IPv4 only — Cloudflare Spectrum / GCP GLB endpoints in this deployment are IPv4. To extend to IPv6, mirror the IPv4 path on `ETH_P_IPV6` and `ipv6hdr`.
* Observed cap currently lands at ~2× the configured refill (e.g. ~20 k/s for a 10 k/s setting) — the token-bucket refill path is under review.
* `xdp-loader` from `xdp-tools` is **not** used: it goes through the libxdp dispatcher, which fails to attach native on `gve`. The loader calls `bpf_xdp_attach()` directly.
