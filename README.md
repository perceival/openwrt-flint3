# OpenWrt for the GL.iNet Flint 3 (GL-BE9300)

Mainline **OpenWrt** support for the **GL.iNet Flint 3 (GL-BE9300)** — Qualcomm
**IPQ5332** (quad Cortex-A53) with tri-band Wi-Fi 7, a Realtek **RTL8372N** 10G
switch and a **RTL8221B** 2.5G WAN PHY.

> The `flint3-be9300` base is a complete OpenWrt tree. This RTL8372N
> driver-replacement candidate is experimental: it has not passed a successful
> package build or hardware test. Follow the [P0 first-test plan](package/kernel/rtl837x/FIRST-HARDWARE-TEST.md)
> before building or installing this candidate.
> (An earlier `main` branch held a target *overlay*; it is retired and
> preserved at the tag `archive/main-overlay`.)

Target: **`qualcommbe/ipq53xx`**, kernel **6.18**.

> [!WARNING]
> **Unofficial, community-maintained port — not affiliated with, endorsed by, or supported
> by GL.iNet or the OpenWrt project.** Provided **as-is, with no warranty of any kind**.
> Flashing third-party firmware carries real risk, including bricking the device, and may
> void your hardware warranty. **Back up your eMMC first** — the ART partition holds your
> unit's unique radio calibration data and MAC addresses and cannot be recovered from
> anywhere else. If this router matters to you, test on a spare unit before relying on it.

## Hardware

| Block | Detail |
|---|---|
| SoC | Qualcomm IPQ5332, 4× Cortex-A53 |
| Wi-Fi 2.4 GHz | on-SoC radio, ath12k over AHB |
| Wi-Fi 5 / 6 GHz | 2× QCN9274, ath12k over PCIe |
| Switch | RTL8372N, out-of-tree DSA candidate (`realtek,rtl8372n`); SoC↔switch link is 10GBASE-R |
| WAN | RTL8221B 2.5G, USXGMII |
| Storage | eMMC |

## Status

This branch is being used to prepare a replacement RTL8372N DSA driver. The P0 candidate has not yet completed a successful OpenWrt build or a Flint 3 hardware run. See the [first-hardware test plan](package/kernel/rtl837x/FIRST-HARDWARE-TEST.md) before building or testing it.

| Subsystem | State |
|---|---|
| Boot / procd / SSH | working on the previous known-good image; candidate image not hardware-tested |
| LAN (RTL8372N via DSA + EDMA/PPE) | P0 replacement candidate; build and hardware validation pending |
| WAN (2.5G, USXGMII) | working on the previous known-good image; candidate regression not checked |
| VLANs (bridge-vlan on DSA) | P0 seeds VLAN 1/PVID only; general VLAN offload is not implemented |
| PPE hardware flow offload | Previous-driver baseline: IPv4 LAN→WAN NAT reached ~2.3 Gbit/s at ~1% CPU. Candidate behavior is not yet validated. |
| Wi-Fi 7, all three bands | working |
| MLO (AP MLD across 2.4/5/6 GHz) | working |
| DFS | working, including several BSSes per DFS radio started together |
| 802.11k / 802.11v | working |
| eMMC sysupgrade + return to stock | established for the previous known-good image; candidate recovery path not tested |

The **~1.8–1.9 Gbit/s** result was measured with the previous working switch-driver build. It is historical baseline data and does not validate this P0 candidate.

## Known issues

- **ath12k firmware hang under sustained load.** After hours with many clients
  the Q6 can take a fatal error. Since 2026-09-28 the firmware coredump is
  released automatically and the radios recover in seconds instead of staying
  down; the cause of the crash itself is still open. Reported upstream.
- **Kernel panic in netlink socket release**, seen six times since August on
  both APs after hours of uptime (sockets of a bridge notification, hostapd
  or wsdd2). The AP reboots itself in ~90 s. wsdd2 is kept disabled as one
  trigger; root cause under investigation with a KASAN kernel.
- **Client kicks for "excessive missing ACKs"**: the driver's packet-loss
  events are unreliable for multi-link stations, so hostapd's
  `disassoc_low_ack` now defaults to 0 in these images (set it to 1 on a
  wifi-iface to restore the old behaviour).
- **PPE WAN RX FIFO overruns.** Roughly 0.07–0.09 % of packets at ~1.9 Gbit/s.
  No longer the hard ~600 Mbit/s cap earlier builds had, but not zero.
- **802.11r is incompatible with MLO.** hostapd's FT code has no MLD
  awareness — do not enable 11r on an MLO SSID. 11k/11v are fine.

## Building

For the RTL8372N P0 package build and first-device test sequence, see [FIRST-HARDWARE-TEST.md](package/kernel/rtl837x/FIRST-HARDWARE-TEST.md). The package has not yet passed that build gate in this worktree.

```sh
git clone -b flint3-be9300 https://github.com/perceival/openwrt-flint3.git
cd openwrt-flint3
./scripts/feeds update -a
./scripts/feeds install -a
make menuconfig     # Target System: Qualcomm Atheros 802.11be
                    # Subtarget:     ipq53xx
                    # Target Profile: GL.iNet GL-BE9300
make -j"$(nproc)"
```

Images land in `bin/targets/qualcommbe/ipq53xx/`.

### Don't want to build from source?

Pre-built reference images are published periodically on the
**[Releases page](https://github.com/perceival/openwrt-flint3/releases)**, in three flavours:

- **`vanilla`** — the exact, unmodified default this tree produces with zero customization
  (no LuCI, `wpad-basic-mbedtls`) — what you'd get building it yourself with no changes
- **`ap`** — full config (LuCI, tri-band MLO) plus the FT-over-MLO roaming series; what the
  maintainer's own household runs
- **`router`** — gateway role: LuCI, WireGuard, unbound, chrony, mDNS reflection; nftables
  flowtable offload in software by default, PPE hardware offload opt-in (see Status)

See the disclaimer above before flashing any of them.

## Installing

Full, hardware-verified instructions — including the round trip back to stock —
are on the device page:

**https://openwrt.org/toh/gl.inet/gl-be9300**

> [!CAUTION]
> The instructions below describe the established image path, not a validated
> installation of the P0 switch-driver candidate. Do not flash the candidate
> on your only router. Use a recoverable test unit, serial console, known-good
> image, and the first-hardware test plan linked above.

Short version for the established image: from stock firmware, use the
**factory** image with `sysupgrade -F -n`. The stock image check requires a
QSDK FIT, so `-F` is required and the "missing section" warnings for
`u-boot`/`tz`/`sb11` are expected. Do **not** force the plain sysupgrade image
from stock.

Stock QSDK firmware may report `qcom,ipq5332-ap-mi01.6` as its board name.
That is the generic Qualcomm MI01.6/RDP468 identity used by the vendor path,
not the OpenWrt GL-BE9300 device identifier. The same compatible is used by
the upstream Qualcomm RDP468 device tree, so it is intentionally not added to
this profile's `SUPPORTED_DEVICES`: doing so would advertise the Flint 3 image
as compatible with other hardware using that generic identity. The resulting
stock compatibility warning is therefore expected; use the documented `-F`
factory-image path instead (see [issue #9](https://github.com/perceival/openwrt-flint3/issues/9)).

**Back up your eMMC first** — the ART partition holds this unit's radio
calibration and MAC addresses and cannot be recovered from anywhere else.

## Other boards

Only the GL-BE9300 is tested here. The tree is a `qualcommbe/ipq53xx` target, and anything that
is not board-specific — the IPQ5332 clocks, PCIe, PPE/EDMA Ethernet and hardware offload, ath12k
Wi-Fi and the firmware-recovery handler — is shared by every IPQ53xx device. What a new board
needs is a device tree, an image definition and, if its switch is not a Realtek RTL837x, a
matching switch driver.

**Defined in this tree, untested by me** (images are not published for them):

| Board | SoC | Switch | Flash | Origin | Notes |
|---|---|---|---|---|---|
| GL.iNet GL-BE6500 | IPQ5332 + QCN9274 | RTL837x (same driver) | NAND (UBI) | [JiaY-shi](https://github.com/JiaY-shi/openwrt) | closest relative; builds from this tree |
| Ubiquiti UniFi 7 Pro XGS | IPQ5332 | none (single 10G PHY) | eMMC + SPI-NOR | Til Kaiser, upstream [#25185](https://github.com/openwrt/openwrt/pull/25185) | needs a bootloader downgrade (newer ones enforce signatures) |

**Other IPQ53xx devices with community work** (not in this tree):

| Device | SoC | Switch | Status |
|---|---|---|---|
| Xiaomi BE3600 Pro (RN01) | IPQ5312 | Motorcomm YT9215S | ported on top of the upstream ipq53xx PR (Ethernet, storage, boot reported working) — [#23161](https://github.com/openwrt/openwrt/pull/23161) |

**Upstream:** OpenWrt main has no IPQ53xx support yet. The subtarget is proposed in
[openwrt/openwrt#23161](https://github.com/openwrt/openwrt/pull/23161) (open). The RTL8372N
switch driver used here exists only in this tree.

**Testers wanted.** If you own a GL-BE6500, a UniFi 7 Pro XGS or another IPQ53xx device and are
comfortable with a serial console and an eMMC/NAND backup, I would like to hear from you: open an
issue with the model, a boot log from stock and a photo of the board. Board support that nobody
can test stays unpublished, and a second set of hands is the fastest way to change that. PRs
bringing up another board on top of this branch are welcome too.

## Upstream

Patches from this work that have gone upstream or are in review:

- `wifi: ath12k: advertise AP_VLAN interface mode for IPQ5332` (linux-wireless)
- hostapd WDS/AP_VLAN `bss->ctx` fix (applied by Jouni Malinen)
- ath12k `hw_scan` NULL-deref report (with the Qualcomm dev team)

## Links

- Forum thread: https://forum.openwrt.org/t/gl-inet-flint-3-exploration-gl-be9300-ipq5332/250267
- Device page: https://openwrt.org/toh/gl.inet/gl-be9300
- Q6/PAS research notes: [`2.4GHZ-Q6-PAS-FINDINGS.md`](2.4GHZ-Q6-PAS-FINDINGS.md)

## Credits

Built on [JiaY-shi's](https://github.com/JiaY-shi/openwrt) GL-BE6500 tree, which
provided the working IPQ5332 Wi-Fi and RTL837x DSA foundation. Thanks to
everyone contributing hardware findings and testing via the issue tracker and
the forum thread.
