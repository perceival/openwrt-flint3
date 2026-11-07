# OpenWrt for the GL.iNet Flint 3 (GL-BE9300)

<a href="https://ko-fi.com/C8Y320ZTT2"><img src="https://storage.ko-fi.com/cdn/kofi6.png?v=6" height="36" alt="Buy Me a Coffee at ko-fi.com"></a>

Mainline **OpenWrt** support for the **GL.iNet Flint 3 (GL-BE9300)** — Qualcomm
**IPQ5332** (quad Cortex-A53) with tri-band Wi-Fi 7, a Realtek **RTL8372N** 10G
switch and a **RTL8221B** 2.5G WAN PHY.

> **This branch (`flint3-be9300`) is a complete, buildable OpenWrt tree.**
> Clone it and build — there is nothing to drop into another checkout.
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
| PPE hardware flow offload | IPv4 LAN→WAN NAT (TCP/UDP, untagged or 802.1Q WAN) ~2.3 Gbit/s at ~1% CPU; opt-in via the firewall's hardware flow offloading. WAN→LAN and IPv6 in the next release |
| Wi-Fi 7, all three bands | working |
| MLO (AP MLD across 2.4/5/6 GHz) | working |
| DFS | working, including several BSSes per DFS radio started together |
| 802.11k / 802.11v | working |
| eMMC sysupgrade + return to stock | working |

Throughput measured between two units over a 2.5G trunk: **~1.8–1.9 Gbit/s**.

## Known issues

- **ath12k firmware hang under sustained load.** After hours with many clients
  the Q6 can take a fatal error. Since 2026-09-28 the firmware coredump is
  released automatically and the radios recover in seconds instead of staying
  down; the cause of the crash itself is still open. Reported upstream.
- **Kernel panic in netlink socket release**, seen six times since August on
  both APs after hours of uptime (sockets of a bridge notification, hostapd
  or wsdd2). The AP reboots itself in ~90 s (the panic log is kept, see
  [Crash logs](#crash-logs)). wsdd2 is kept disabled as one
  trigger; root cause under investigation with a KASAN kernel.
- **Client kicks for "excessive missing ACKs"**: the driver's packet-loss
  events are unreliable for multi-link stations, so hostapd's
  `disassoc_low_ack` now defaults to 0 in these images (set it to 1 on a
  wifi-iface to restore the old behaviour).
- **PPE WAN RX FIFO overruns.** Roughly 0.07–0.09 % of packets at ~1.9 Gbit/s.
  No longer the hard ~600 Mbit/s cap earlier builds had, but not zero.
- **802.11r is incompatible with MLO.** hostapd's FT code has no MLD
  awareness — do not enable 11r on an MLO SSID. 11k/11v are fine.

## Testing the stock 2.4 GHz board file

The image uses 2.4 GHz board data that is known to work with this firmware. GL's
own firmware loads another board file for this board (`bdwlan.b12`), which has
not yet been tried with the ath12k firmware. To try it, install the optional
package `ipq-wifi-glinet_gl-be9300-bdf-test`: select it in `make menuconfig`
(Firmware, ath10k Board-Specific Overrides), or copy the `.apk` from your build's
`packages` directory to the unit and run `apk add --allow-untrusted` on it. It is
not in the default image. It installs a `board-2.bin` container holding that board
file, with the board name and regdb of the normal one, as
`/lib/firmware/ath12k-test/ath12k/IPQ5332/hw1.0/board-2.bin`, which nothing loads
unless selected. To select it until the next reboot:

```sh
wifi down
echo -n /lib/firmware/ath12k-test > /sys/module/firmware_class/parameters/path
rmmod ath12k_wifi7 ath12k
modprobe ath12k && modprobe ath12k_wifi7
sleep 30; wifi up
```

Then check that the 2.4 GHz network comes up (`iwinfo`, and `dmesg | grep ath12k`
shows no board-data or firmware errors), and compare `iw dev <2.4 GHz interface> info`
(txpower) and 2.4 GHz throughput with the normal file. Nothing is changed on
flash: if Wi-Fi does not come back, just reboot and the normal board file is
used again. Please report the result in the issue tracker.

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

Short version: from stock firmware, use the **factory** image with
`sysupgrade -F -n`. `-F` is required because stock does not recognise the
image's board name (see below); the "missing section" warnings for
`u-boot`/`tz`/`sb11` are expected. Do **not** force the plain sysupgrade
image from stock.

`-n` is **required**, not optional. Without it, stock's own
`platform_copy_config` formats an ext4 filesystem inside the `rootfs`
partition and copies GL's configuration into it, where OpenWrt can pick it
up as its overlay. If the file is not a QSDK FIT (for example the sysupgrade
image forced by mistake), it formats from offset 0 and destroys the stock
rootfs, and only U-Boot recovery brings the unit back. Check the image's
`sha256sum` against the build's `sha256sums` file before forcing it.

Back to stock from OpenWrt: `sysupgrade -n be9300-<version>.bin` (no `-F`
needed; `-n` is required here too).

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

**Upgrading with kept settings from an older build.** Older images pinned every
AP to an address derived from ART in `/etc/config/wireless`. On the first boot,
`99-gl-be9300-wifi-macs-cleanup` removes those `macaddr` options, but only the
exact values that script wrote, in the order it wrote them, and only if at least
the first two are there. Anything else is taken for your own and stays, including
a single AP pinned to the ART address, or pinned addresses after you added,
removed or reordered APs. To go back to the defaults for such an interface:

```sh
uci delete wireless.<iface>.macaddr
uci commit wireless
wifi reload
```

## Recovery

From least to most invasive:

1. **Settings broken, system boots:** hold the reset button for more than
   5 seconds while OpenWrt is running and release it. That is a factory reset
   (`firstboot`), and the unit comes back at 192.168.1.1.
2. **System does not finish booting:** failsafe. Power on and watch the white
   LED. While it blinks during early boot, press reset briefly, and it switches
   to a fast blink. Connect a PC with a static 192.168.1.2/24 to **LAN1** and
   run `ssh root@192.168.1.1`. Then `mount_root` to get at the configuration,
   or `firstboot -y && reboot`.
3. **No kernel at all (bad flash):** the U-Boot recovery server. Power off,
   hold reset, power on and keep holding for about 5 seconds. A web server
   then answers at 192.168.1.1 (PC on 192.168.1.2/24). Upload a **stock GL.iNet
   firmware** (`be9300-<version>.bin` from https://dl.gl-inet.com/router/be9300/),
   let it flash and reboot into stock, and then reinstall OpenWrt from stock as
   described above. The same server also has upload forms that write U-Boot
   itself and the `0:ART` partition, with no authentication or check. Never use
   those: ART holds this unit's calibration and MACs.

What the recovery server does with *non-stock* images is GL's own code (the
Qualcomm U-Boot it is based on has no web server), and GL ships no U-Boot in
its firmware images, so it can only be read from a unit's bootloader:

- `…-squashfs-factory.bin` carries the same U-Boot script as GL's images
  (`flashinit mmc; flupdate set mmc; imxtract … hlos && flash 0:HLOS;
  imxtract … rootfs && flash rootfs; flupdate clear`; in Qualcomm's U-Boot
  `flupdate` only selects the flash type). If the server runs it, the image
  is flashed. That is reported from the bootloader in PR #114, not tested.
- the `…-initramfs-uImage.itb` is **not** flashed. The server reports "HTTP
  ugrade is done" but the eMMC is unchanged. The image is left in RAM at
  0x50000000, where `bootm 0x50000000` from the serial console can start it
  (see the device page). This is what the device page measured, and it
  matches the PR's analysis of the `.itb`.
- `…-squashfs-sysupgrade.bin` is rejected.

For a guaranteed result, use a stock image there.

### U-Boot environment (`fw_printenv`)

The images set up `fw_env.config` for the GL-BE9300 with stock's layout:
`0:APPSBLENV` at offset 0, 256 KiB, the environment size of Qualcomm's
U-Boot for IPQ5332. It was checked on a unit running stock 4.10.1, where
`0:APPSBLENV` is 256 KiB, stock's own `/etc/fw_env.config` reads
`/dev/mmcblk0p6 0x0 0x40000`, and `fw_printenv` shows no "Bad CRC" warning.
To repeat the check on your own unit, run this **while it still runs
stock**:

```sh
cat /etc/fw_env.config      # must read: /dev/mmcblk0p6 0x0 0x40000
fw_printenv >/dev/null      # must print no "Bad CRC" warning
```

The entry is only created on a fresh install (or after
`/etc/config/ubootenv` is removed); a keep-settings upgrade from an older
build keeps its empty configuration. To add it by hand on a running unit:

```sh
echo "$(. /lib/functions.sh; find_mmc_part 0:APPSBLENV) 0x0 0x40000" > /etc/fw_env.config
fw_printenv >/dev/null      # again: no "Bad CRC"
```

**Never run `fw_setenv` after `fw_printenv` has warned "Bad CRC, using
default environment".** `fw_setenv` would then write the tools' generic
default environment (`bootcmd=run distro_bootcmd`) with a valid CRC, and
this U-Boot would no longer boot the firmware. Remove `/etc/fw_env.config`
instead and report the output of the stock check above.

## Crash logs

A kernel panic reboots the unit after `kernel.panic` (3 s). If the panic path
hangs instead, the watchdog bites at most 5 s later. Why a crashed AP takes
~90 s to answer again cannot be settled from source. Probably most of it is
the normal boot (U-Boot, kernel, and loading the Wi-Fi firmware for three
radios), but a panic path that hangs until a watchdog bite would look the
same from outside. The timestamps in the saved crash logs, or a serial
console, will tell.

The kernel log of the crash survives that reboot in RAM (ramoops, 1 MiB
reserved at 0x4da00000). On the next boot `/etc/init.d/save-pstore` copies
it to `/root/pstore/NNNN-<date>/` (`dmesg-ramoops-*` is the panic,
`console-ramoops-*` the console of the crashed boot), keeps the last 10
crashes, and says so in the kernel log (`dmesg | grep pstore`, also in
`logread`). The RAM copy is only cleared once every record has been copied
to the flash-backed overlay, synced and compared with the original. If the
overlay is not on flash (it fell back to tmpfs, for example because the
crash damaged it) or a copy fails, the records stay in `/sys/fs/pstore` and
can be copied by hand. `/root/pstore` is kept across sysupgrades that keep
settings. A power cut, or pulling the plug instead of waiting for the
reboot, loses the RAM copy.

U-Boot only collects its own crash dump (and waits for "any key within
10s") when the download-mode cookie in TCSR (0x193D100, bit 4) is set. Linux
clears it at boot (`qcom_scm.download_mode=off`, the default), so a panic
reboots straight into a normal boot.

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
