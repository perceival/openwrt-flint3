# OpenWrt port — GL.iNet Flint 3 (GL-BE9300, IPQ5332)

Work-in-progress mainline **OpenWrt** support for the **GL.iNet Flint 3 (GL-BE9300)** —
Qualcomm **IPQ5332** (Networking Pro 620, quad Cortex-A53) + 2× **QCN6274** Wi‑Fi 7
radios (5 + 6 GHz, on PCIe) + an **on-SoC 2.4 GHz** radio + **RTL8372N** 10G switch +
**RTL8221B** 2.5G WAN PHY.

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
| **Wi‑Fi 5 GHz + 6 GHz** (2× QCN6274, PCIe / ath12k) | ✅ real clients, SAE + DHCP, simultaneous |
| **Wi‑Fi 2.4 GHz** (on-SoC Q6/WCSS / ath12k AHB) | 🚧 WIP — driver + DT + firmware done, blocked on a TrustZone PAS step (see below) |
| **eMMC** persistence (f2fs overlay, sysupgrade) | ✅ config persists across reboots |
| Standalone cold-boot from eMMC | ⚠️ flaky (~50%) — a U-Boot→Linux SDHCI handoff issue (see *What's left*) |

### Wi‑Fi 5/6 GHz (solved)

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
DHCP on **5 GHz (DFS ch100) and 6 GHz** simultaneously. The 5 GHz radio is **self-managed
regulatory** and only permits DFS channels here.

### Wi‑Fi 2.4 GHz (in progress)

The 2.4 GHz radio is **integrated in the IPQ5332** and runs on the on-SoC **Q6/WCSS**
coprocessor (not a PCIe card). The driver side is complete and binds on hardware:
the **multi-PD remoteproc** (`qcom_q6v5_mpd`, patches `0595–0605`), **ath12k AHB**
(`ATH12K_AHB` + patch `201`), the DT nodes (`remoteproc@d100000`, `wifi@c000000`,
`wcss-smp2p`, reserved-mem), and firmware staging are all in place. `ath12k_ahb` recognises
the radio (`Hardware name: ipq5332 hw1.0`) and the Q6 firmware loads.

**The blocker:** Q6 bring-up fails at the final TrustZone step —
`qcom_scm_pas_auth_and_reset(pasid=0xD)` returns **-22** (`res[0] = -32`). `init_image`,
`mem_setup` and the segment load all succeed; only the PAS auth is refused. This persists
with the **correct ath12k firmware** (TIP `wlan-ap` `qca-wifi-7/ath12k-firmware/files/IPQ5332`
— *not* the vendor WIFIFW-partition blob, which is for the proprietary cnss driver). GL.iNet
and QCA are looking into the TZ side. The nodes ship **disabled** by default; set
`remoteproc@d100000` and `wifi@c000000` to `okay` (and stage the firmware) to reproduce.

Discussion: **https://forum.openwrt.org/t/gl-inet-flint-3-exploration-gl-be9300-ipq5332/250267**.

## Deployed

Runs as a **live home AP** — 5 + 6 GHz with **802.11r (FT‑SAE) + dawn** roaming in a
multi‑AP network, dual‑VLAN trunk uplink, **eMMC** f2fs overlay (sysupgrade keeps config).
>1 Gbit/s Wi‑Fi to the device; line‑rate to anything behind the uplink. (Standalone
cold-boot from eMMC is currently flaky — see below; TFTP/initramfs boot is 100% reliable.)

## What's left

- **2.4 GHz on‑SoC radio** — blocked on the TrustZone `pas_auth_and_reset(-22)` above
  (GL.iNet/QCA engaged). Everything else for it is done.
- **Standalone eMMC cold-boot (~50% flaky)** — when U‑Boot reads the kernel off eMMC, the
  SDHCI controller is left in a state Linux's reset only recovers ~half the time
  (`mmc0: Reset 0x1 never completed` → `mmcblk0` never enumerates → rootwait hangs). The
  same kernel over TFTP/initramfs (U‑Boot never touches mmc) is 100% reliable. A
  `GCC_SDCC_BCR` reset on the `&sdhc` node helps but isn't deterministic; the robust fix is
  kernel-side SDHCI re-init. Workaround under test: `mmc rescan` in U‑Boot before `bootm`.
- **6 GHz downlink TX** — associates and works, but AP→client TX is lossy on some channels;
  pick a 6 GHz channel with room for the width.
- **Upstreaming** — the RTL8372N DSA driver especially.

## Board notes / gotchas

- **Switch port map ≠ silkscreen.** On the RTL8372N, **port 3 is the CPU port**; the four
  LAN jacks are chip ports **7, 5, 6, 8** (LAN1=7, LAN2=5, LAN3=6, **LAN4=8**), and port 4
  is a 1G secondary CPU/trunk, not a jack. Use the chip port numbers for the DT `reg=`
  fields; the silkscreen jack numbers don't match the DSA port names. (The vendor's
  `swconfig` tool numbers ports differently again.)
- **DSA conduit MTU + vlan‑aware bridge:** DSA sizes the CPU conduit for the 8‑byte CPU tag
  but **not** the 4‑byte VLAN tag → full‑1500 frames are silently dropped on the CPU/Wi‑Fi
  path (pings/DNS fine, TCP/SSH stall). Bump the conduit MTU (see the hotplug helper).

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

**Note on firmware:** proprietary QCA Wi‑Fi firmware is **not** included. The 5/6 GHz
radios use the standard `ath12k-firmware-qcn9274` OpenWrt package. To test the 2.4 GHz
radio, stage the open-source ath12k IPQ5332 firmware from
[TIP wlan-ap](https://github.com/Telecominfraproject/wlan-ap/tree/main/feeds/qca-wifi-7/ath12k-firmware/files/IPQ5332)
into `/lib/firmware/IPQ5332/`.

## Layout

- `target/linux/qualcommbe/dts/ipq5332-gl-be9300.dts` — board DTS
- `target/linux/qualcommbe/files-6.12/drivers/net/dsa/rtl8372n.c` — RTL8372N DSA driver
- `target/linux/qualcommbe/patches-6.12/` — kernel patch series (PCIe/CMN‑PLL/NSSCC,
  PPE/EDMA/XGMAC, PCS, DSA, the multi‑PD Q6 remoteproc `0595–0605`, etc.)
- `package/kernel/mac80211/patches/ath12k/` — ath12k patches (dualmac, MLO‑disable, AHB, …)

## License

GPL-2.0, matching OpenWrt and the Linux kernel.

## Credits

Port by Kamil Bienkiewicz (`perceivalpercy@gmail.com`). Built on upstream OpenWrt and
the in-progress mainline IPQ5332 enablement. With help from the OpenWrt community and
GL.iNet's engineering team.
