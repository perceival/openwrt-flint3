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
| 2× QCN6274 on PCIe | ✅ enumerate + probe (Gen3 x2) |
| **Wi‑Fi (ath12k)** | ✅ both radios — real clients, SAE + DHCP, 5 GHz + 6 GHz |

### Wi‑Fi (solved)

`ath12k` originally couldn't bind: **any PCIe config-space access after boot stalled the
AXI bus** (config reads worked during enumeration, then hung in
`pci_read_config_byte(QCN, PCI_INTERRUPT_PIN)` inside `pci_assign_irq()`).

**Root cause:** the `pcie@18000000` node was missing its **`interconnects` (NoC) property**.
`drivers/pci/controller/dwc/pcie-qcom.c` (`qcom_pcie_icc_init`) calls
`devm_of_icc_get(dev, "cpu-pcie")` + `icc_set_bw()`; with no DT property the CPU↔PCIe
fabric path is never given bandwidth, so config TLPs stall once the link goes idle. The
3-line DT fix is `patches-6.12/0594-…-add-pcie-interconnects.patch`.

With that, ath12k probes both QCN6274 radios and registers two PHYs (non-MLO multi-pdev,
per patch `402`). Verified on hardware: multiple clients associating end-to-end with SAE +
DHCP on **5 GHz (DFS ch100) and 6 GHz** simultaneously. 2.4 GHz (the IPQ5332-integrated
radio) is not yet enabled. Note: the 5 GHz radio is **self-managed regulatory** and only
permits DFS channels here. Discussion:
**https://forum.openwrt.org/t/gl-inet-flint-3-exploration-gl-be9300-ipq5332/250267**.

## Deployed

Running as a **live home AP** — moodsy on **5 + 6 GHz** with **802.11r (FT‑SAE) + dawn** roaming in a multi‑AP network, dual‑VLAN trunk uplink, **eMMC** f2fs overlay (sysupgrade keeps config; boots standalone). >1 Gbit/s Wi‑Fi to the device; line‑rate to anything behind the uplink.

## What's left

- **2.4 GHz on‑SoC radio** — the biggest piece. Needs the Q6/WCSS root‑PD remoteproc (the unmerged *WCSS secure PIL* series; the older multi‑PD approach was dropped) + `ATH12K_AHB` (merged upstream, just disabled here) + IPQ5332 firmware (not in upstream linux‑firmware).
- **6 GHz downlink TX** — associates and works, but AP→client TX is lossy. Pick a 6 GHz channel with room for the width (ch5 is too close to the band edge → association fails).
- **WAN‑on‑trunk boot stall** — booting with a *live* trunk on `eth1` intermittently RCU‑stalls pre‑userspace (a clock/idle wedge); use a LAN port for the uplink.
- **Upstreaming** — the RTL8372N DSA driver especially.

## Board notes / gotchas

- **Switch port map ≠ silkscreen.** The 4 LAN jacks are RTL8372N chip ports **5, 6, 7, 8** (LAN4 = port **8**); **port 4 is a 1G secondary CPU/trunk**, not a jack. Use the chip port numbers for the DT `reg=` fields.
- **DSA conduit MTU + vlan‑aware bridge:** DSA sizes the CPU conduit for the 8‑byte CPU tag but **not** the 4‑byte VLAN tag → full‑1500 frames are silently dropped on the CPU/Wi‑Fi path (pings/DNS fine, TCP/SSH stall). Bump the conduit MTU.

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
