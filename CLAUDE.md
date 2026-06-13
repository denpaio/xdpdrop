# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

> **Editing this file:** Consider the whole document before changing it — the right section, the right wording, the most essential form for every sentence. **Length limit: 200 lines** — trim or consolidate before adding.

## What this is

An XDP (eXpress Data Path) program that token-bucket rate-limits TCP SYN packets
destined for HAProxy frontend ports (dst 2000/2001/2002), dropping excess SYNs in
the NIC driver layer. It exists because the HAProxy here sits behind a GCP GLB /
Cloudflare Spectrum, so every connection's source IP is the upstream LB — per-source
limiting is useless, and the only lever left is a global SYN cap that keeps HAProxy's
PROXY-Protocol parse load bounded. See `README.md` for the operational/deployment story.

## Build / install (Linux only)

```sh
make                 # builds xdp_synrl.o (BPF) and xdp_synrl_loader (userspace)
make clean
sudo make install    # installs object + loader + prep script + unit, enables service
sudo make uninstall
```

**The target is Linux; this repo is edited from macOS.** You cannot fully build or run
here — the BPF object needs `clang -target bpf` plus Linux kernel headers, and the loader
links `-lbpf` (libbpf-dev). Do not assume `make` succeeds in this dev environment. There
is no test suite; validation is manual on a deployed GCP VM (`hping3` flood + `ethtool -S`
`rx_xdp_drop`/`rx_xdp_pass` counters — see README "驗證").

## Architecture

Two compiled artifacts from two source files:

- **`xdp_synrl.c` → `xdp_synrl.o`** — the in-kernel BPF program (`SEC("xdp")`,
  prog name `xdp_synrl`). Parses eth→IPv4→TCP, filters to the three SYN-only flows,
  and consumes from a shared token bucket stored in a single-entry `BPF_MAP_TYPE_ARRAY`
  guarded by a `bpf_spin_lock`.
- **`xdp_synrl_loader.c` → `xdp_synrl_loader`** — userspace libbpf loader. Finds the
  default-route interface (parses `/proc/net/route`), attaches with libbpf, blocks on
  `pause()`, and detaches on SIGTERM/SIGINT. `--iface` / `--obj` override the defaults.

Deployment glue: `xdp-synrl.service` (systemd unit, `Restart=on-failure`) runs
`xdp-synrl-prep.sh` as `ExecStartPre` then the loader. The prep script's whole job is
`ethtool -L $IFACE rx 1 tx 1` to satisfy gve's native-XDP queue precondition.

## Invariants to preserve when editing

These three properties are load-bearing — breaking any of them silently breaks the deploy:

1. **Fail-open.** Every error and bounds-check path in the BPF program returns `XDP_PASS`,
   never `XDP_DROP`. A runtime bug must never blackhole HAProxy. Only an explicit
   "bucket empty" decision yields `XDP_DROP`.
2. **Native mode only.** The loader attaches with `XDP_FLAGS_DRV_MODE` and must never fall
   back to skb/generic mode. This is deliberate — it does not use xdp-tools' `xdp-loader`
   because libxdp's dispatcher fails to attach native on gve; the loader calls
   `bpf_xdp_attach()` directly. Native attach also depends on the gve queue layout the
   prep script enforces (needs ≥4 vCPU so `max rx=tx≥2`, and `current ≤ max/2`).
3. **Verifier-passable packet parsing.** Every pointer dereference is preceded by a
   `> data_end` bounds check, and `iph->ihl*4` is clamped to `[20, 60]` so the TCP-header
   offset is provably in range. Keep this shape when touching the parse path.

## Domain notes

- **IPv4 only** by design (upstream is IPv4-only). Extending to IPv6 means adding an
  `ETH_P_IPV6` / `ipv6hdr` path.
- **Hardcoded:** ports 2000/2001/2002 (`PORT_A/B/C`), refill 10000 tokens/s
  (`NS_PER_TOKEN 100000`), capacity 10000 (`BUCKET_CAP`). All three ports share one bucket.
- **Known issue:** observed effective cap is ~2× the configured refill (set 10k/s, measure
  ~20k/s); the refill math is still under review. If you touch the refill block, this is
  the thing to fix — note it preserves fractional tokens by advancing `last_ns` by exactly
  `add * NS_PER_TOKEN` rather than to `now`.
