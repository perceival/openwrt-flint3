# OpenWrt for the GL.iNet Flint 3 (GL-BE9300)

Mainline **OpenWrt** support for the **GL.iNet Flint 3 (GL-BE9300)** — Qualcomm
**IPQ5332** (quad Cortex-A53) with tri-band Wi-Fi 7, a Realtek **RTL8372N** 10G
switch and a **RTL8221B** 2.5G WAN PHY.

> **This branch (`flint3-be9300`) is a complete, buildable OpenWrt tree.**
> Clone it and build — there is nothing to drop into another checkout.
> (An earlier `main` branch held a target *overlay*; it is retired and
> preserved at the tag `archive/main-overlay`.)

Target: **`qualcommbe/ipq53xx`**, kernel **6.18**.

## Hardware

| Block | Detail |
|---|---|
| SoC | Qualcomm IPQ5332, 4× Cortex-A53 |
| Wi-Fi 2.4 GHz | on-SoC radio, ath12k over AHB |
| Wi-Fi 5 / 6 GHz | 2× QCN9274, ath12k over PCIe |
| Switch | RTL8372N, out-of-tree DSA driver (`realtek,rtl837x`); SoC↔switch link is 10GBASE-R |
| WAN | RTL8221B 2.5G, USXGMII |
| Storage | eMMC |

## Status

| Subsystem | State |
|---|---|
| Boot / procd / SSH | working |
| LAN (RTL8372N via DSA + EDMA/PPE) | working |
| WAN (2.5G, USXGMII) | working, links at 2.5 Gbps |
| VLANs (bridge-vlan on DSA) | working |
| Wi-Fi 7, all three bands | working |
| MLO (AP MLD across 2.4/5/6 GHz) | working |
| DFS | working (needs the cfg80211 secondary-AP-after-CAC patch, included) |
| 802.11k / 802.11v | working |
| eMMC sysupgrade + return to stock | working |

Throughput measured between two units over a 2.5G trunk: **~1.8–1.9 Gbit/s**.

## Known issues

- **ath12k firmware hang under sustained load.** After hours with many clients
  the Q6 can take a fatal error; radios stay down until reboot. Reported
  upstream.
- **PPE WAN RX FIFO overruns.** Roughly 0.07–0.09 % of packets at ~1.9 Gbit/s.
  No longer the hard ~600 Mbit/s cap earlier builds had, but not zero.
- **802.11r is incompatible with MLO.** hostapd's FT code has no MLD
  awareness — do not enable 11r on an MLD SSID. 11k/11v are fine.

## Building

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

## Installing

Full, hardware-verified instructions — including the round trip back to stock —
are on the device page:

**https://openwrt.org/toh/gl.inet/gl-be9300**

Short version: from stock firmware, use the **factory** image with
`sysupgrade -F -n`. The stock image check requires a QSDK FIT, so `-F` is
required and the "missing section" warnings for `u-boot`/`tz`/`sb11` are
expected. Do **not** force the plain sysupgrade image from stock.

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
