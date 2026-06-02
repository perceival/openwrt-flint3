# OpenWrt port — GL.iNet Flint 3 (GL-BE9300, IPQ5332)

Work-in-progress mainline **OpenWrt** support for the **GL.iNet Flint 3 (GL-BE9300)** —
Qualcomm **IPQ5332** (Networking Pro 620, quad Cortex-A53) + 2× **QCN6274** Wi‑Fi 7
radios + **RTL8372N** 10G switch + **RTL8221B** 2.5G WAN PHY.

Target: **`qualcommbe/ipq53xx`**, kernel 6.12.

> This is a target **overlay**, not a full OpenWrt tree. Drop it into an OpenWrt
> checkout and build (see below). It carries the board DTS, the kernel patch series,
> an out-of-tree **RTL8372N DSA driver**, and the ath12k patches.

## Status

| Subsystem | State |
|---|---|
| Boot / procd / SSH | ✅ clean procd boot, dropbear up |
| **LAN** (RTL8372N via DSA + EDMA/PPE) | ✅ 100% ping, **949 Mbit/s** iperf3 |
| **WAN** (eth1, RTL8221B USXGMII) | ✅ link up **2.5 Gbps**, line rate |
| **VLANs** (DSA bridge VLANs) | ✅ tagged/untagged validated |
| 2× QCN6274 on PCIe | ⚠️ enumerate (Gen3 x2), but **Wi‑Fi blocked** |
| **Wi‑Fi (ath12k)** | ❌ blocked — see below |

### The Wi‑Fi blocker

`ath12k` can't bind: **any PCIe config-space access after boot stalls the AXI bus**
(config reads work during enumeration, then hang). Traced down to
`pci_read_config_byte(QCN, PCI_INTERRUPT_PIN)` inside `pci_assign_irq()`. Ruled out:
IOMMU, INTx/GIC mapping, clock/PD gating (`clk_ignore_unused pd_ignore_unused` no
effect — clocks incl. the pipe clock are up), nohz. The PCIe support is the upstream
IPQ5332 backport (`qcom,pcie-ipq5332`, `cfg_2_9_0`); no upstream IPQ5332 board exercises
PCIe, so this may be the first real run on this SoC. Discussion / help wanted:
**<FORUM-THREAD-LINK>**.

ath12k is **blacklisted by default** (`base-files/etc/modprobe.d/ath12k.conf`) so the
device boots cleanly as a wired router until this is solved.

## Build

```sh
git clone https://github.com/openwrt/openwrt.git
cd openwrt
# overlay this repo's target/ and package/ onto the checkout:
rsync -a /path/to/this-repo/target/   target/
rsync -a /path/to/this-repo/package/  package/
./scripts/feeds update -a && ./scripts/feeds install -a
make menuconfig    # Target: Qualcomm Atheros BE -> ipq53xx -> GL.iNet GL-BE9300
make -j$(nproc)
```
Flash the resulting `…glinet_gl-be9300-initramfs-uImage.itb` via U‑Boot TFTP, or the
`…squashfs-sysupgrade.bin`.

## Layout

- `target/linux/qualcommbe/dts/ipq5332-gl-be9300.dts` — board DTS
- `target/linux/qualcommbe/files-6.12/drivers/net/dsa/rtl8372n.c` — RTL8372N DSA driver
- `target/linux/qualcommbe/patches-6.12/` — kernel patch series (PCIe/CMN-PLL/NSSCC,
  PPE/EDMA/XGMAC, PCS, DSA, etc.)
- `package/kernel/mac80211/patches/ath12k/` — ath12k patches (dualmac, MLO-disable, …)

### Diagnostic-only patches (remove before any upstream submission)

These are debug instrumentation, kept here because they document the Wi‑Fi
investigation referenced in the forum thread:

- `patches-6.12/0591-debug-pci-device-probe-trace.patch`,
  `patches-6.12/0592-debug-pci-assign-irq-trace.patch`
- `package/kernel/mac80211/patches/ath12k/098-ath12k-pci-init-trace.patch`,
  `…/099-ath12k-pci-probe-step-trace.patch`
- `patches-6.12/0335-…-PPE-debugfs…`, `0561-…-debug-snapshot…`, `0590-…-rx-hexdump-debug…`

## License

GPL-2.0, matching OpenWrt and the Linux kernel.

## Credits

Port by Kamil Bienkiewicz (`perceivalpercy@gmail.com`). Built on upstream OpenWrt and
the in-progress mainline IPQ5332 enablement.
