# xdpdrop

對送往 HAProxy frontend（dst port **2000 / 2001 / 2002**）的 TCP SYN 封包做 token bucket 流量限制的 Linux XDP 程式。

當 HAProxy 位於 GCP GLB / Cloudflare Spectrum 之後，連線的 source IP 永遠是上游 LB 的機器，per-source 限速完全沒用。這個程式跑在 `xdpdrv`（native）模式下，在 NIC 驅動層直接丟棄過量 SYN，把 HAProxy 需要解析 PROXY Protocol 的新連線數量壓在可控範圍內。

## 參數

| 項目 | 值 |
|------|----|
| Filter | TCP，dst port 2000/2001/2002，SYN=1 ACK=0，IPv4 |
| 演算法 | Token bucket，三個 port 共用一個桶 |
| Refill | 10 000 tokens/sec |
| Capacity | 10 000 tokens（約 1 秒 burst 容量） |
| 超額時 | `XDP_DROP`（不分 source IP） |
| 其他流量 | `XDP_PASS` |
| Fail-open | map / 邊界錯誤一律 `XDP_PASS` |
| Attach mode | native (`xdpdrv`)，需要 `gve` NIC 且 `current rx == current tx <= max/2` |

## 檔案

| 檔案 | 用途 |
|------|------|
| `xdp_synrl.c` | BPF 程式（token bucket + 邊界檢查 + port filter） |
| `xdp_synrl_loader.c` | libbpf user-space loader（以 `XDP_FLAGS_DRV_MODE` attach，SIGTERM 時 detach） |
| `Makefile` | build / install / uninstall target |
| `xdp-synrl-prep.sh` | systemd `ExecStartPre`：執行 `ethtool -L $IFACE rx 1 tx 1` 讓 gve 接受 native XDP |
| `xdp-synrl.service` | systemd unit（Type=simple, Restart=on-failure） |

## Build 相依套件（Debian 13 / Ubuntu 24.04）

```
sudo apt-get install -y clang libbpf-dev build-essential ethtool linux-headers-$(uname -r)
```

## Build 與安裝

```
make            # 產生 xdp_synrl.o 與 xdp_synrl_loader
sudo make install
# 會安裝到：
#   /usr/local/lib/xdpdrop/xdp_synrl.o
#   /usr/local/sbin/xdp_synrl_loader
#   /usr/local/sbin/xdp-synrl-prep.sh
#   /etc/systemd/system/xdp-synrl.service
# 並執行 `systemctl daemon-reload && systemctl enable xdp-synrl`
sudo systemctl start xdp-synrl
```

## 移除

```
sudo make uninstall
```

## 驗證

```
# 確認程式以 native mode attach
ip link show "$(ip route show default | awk '/default/{print $5;exit}')" | grep -i xdp
# 預期：... xdp ... prog/xdp id <N>

# loader 還活著
systemctl status xdp-synrl

# bpftool 看得到 program
sudo bpftool prog show | grep xdp_synrl
```

從同一 VPC 內另一台主機驗證 rate-limit 是否觸發：

```
sudo hping3 -S -p 2001 --flood -q <vm-ip>
```

在 SUT 觀察：

```
sudo ethtool -S "$(ip route show default | awk '/default/{print $5;exit}')" \
  | grep -E 'rx_xdp_(drop|pass)'
```

`rx_xdp_drop[0]` 應接近「灌入的 SYN PPS 減掉 ~10 k/s」，`rx_xdp_pass[0]` 應接近設定的 refill 速率。

## 在 GCP 跑 native XDP 的前提

目前實作鎖定使用 `gve` 驅動的 GCP VM：

* 機型至少 4 vCPU（這樣 `max queue` 才會 ≥ 2，`current ≤ max/2` 才有可能滿足）。
* VM 建立時要 `--network-interface=nic-type=GVNIC`（`e2-custom-4-*` 預設是 `virtio_net`，**不支援** native XDP）。
* `xdp-synrl-prep.sh` 在每次啟動時強制 `ethtool -L $IFACE rx 1 tx 1`，確保滿足 gve 對 native XDP 的 queue 限制。

背景知識（gve queue format、native 與 generic XDP 差異）已是熟路 — 直接讀 gve 驅動原始碼與 xdp-tools 文件即可。

## 已知限制

* 只支援 IPv4 — 本實作鎖定的上游（Cloudflare Spectrum / GCP GLB）目前只走 IPv4。要擴到 IPv6 請對 `ETH_P_IPV6` 與 `ipv6hdr` 補一份對應路徑。
* 目前觀察到的實際 cap 大約是設定 refill 的 2 倍（例如設 10 k/s 實測 ~20 k/s）— token bucket 的 refill 邏輯還在 review。
* **不**走 `xdp-tools` 的 `xdp-loader`，因為它經過 libxdp dispatcher，在 `gve` 上 attach native 會失敗。本 repo 的 loader 直接呼叫 `bpf_xdp_attach()`。
