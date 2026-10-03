# GL-BE9300 feature and validation status

**Snapshot checked: 2026-10-03.** This page tracks board-specific behavior and switch/acceleration support. It is not a list of every package or generic feature available in OpenWrt. The `vanilla` name means the minimal profile built from this project’s `flint3-be9300` tree.

## How to read the statuses

- **Hardware-reported** means the maintainer reported a device test. It does not imply every profile, client, topology or PR revision received that test.
- **Image manifest** means the feature, patch series or package is recorded for that exact image. Inclusion alone is not a hardware pass.
- **Build-passed** means compilation/image assembly completed. It does not demonstrate device behavior.
- **Not in release** means absent from the named reference image.
- **Not verified** means the cited project records do not establish a test result. Do not infer that the feature is broken or supported.

## Reference image profiles

[`ref-20260930`](https://github.com/perceival/openwrt-flint3/releases/tag/ref-20260930) is the latest published reference batch checked for this page. All three profiles use Linux 6.18.39 and the existing RTL837x driver, but their manifests identify different revisions/configurations.

| Profile | Manifest revision | Image configuration | FT-over-MLO patches | Release test evidence |
| --- | --- | --- | --- | --- |
| `vanilla` | `88d31e06f9988266da0467e417cca9c3f4f0f6c7` | Target/device `make defconfig`; no LuCI; minimal default packages | None | Exact image: wiped-overlay first boot, `procd` at 12.6 s, switch up at ~16 s, LAN at 192.168.1.1 and DHCP listening |
| `router` | `88d31e06f9988266da0467e417cca9c3f4f0f6c7` | LuCI/Argon, software nftables flowtable, opt-in PPE offload, WireGuard, odhcpd, unbound, chrony, `mdns-repeater`; WAN on LAN4 | None | Exact image: wiped-overlay first boot, `procd` at 12.9 s, switch up at ~16 s, LAN at 192.168.1.1 and DHCP listening; `ksmbd` enabled and `wsdd2` stopped |
| `ap` | `11e70554a087ff973de10b73981467467fee682a` | Full household AP profile; LuCI/Argon, `wpad-openssl`, tri-band MLO | 992–999a | The manifest records the separate `mlo-build` revision. Release notes report the production AP running the AP configuration; that statement is not a test report for every function on the exact downloadable image |

All manifests record the default `rtl8_4` switch tagger, the PPE 0448 security fix and `disassoc_low_ack=0`. They also record automatic ath12k device-coredump release for recovery. For package lists, exact build inputs, image hashes and limitations, use the [release assets and notes](https://github.com/perceival/openwrt-flint3/releases/tag/ref-20260930).

## Shipping firmware: existing RTL837x driver

| Feature | Status in the published baseline | Evidence and limitation |
| --- | --- | --- |
| Boot and basic LAN services | **Hardware-reported; exact `vanilla` and `router` images checked** | The release records first-boot tests after wiping overlay, LAN reachability, DHCP and switch readiness. The AP manifest is a separate revision/profile. |
| Installation, sysupgrade and return to stock | **Reported working** | See the [GL-BE9300 installation/recovery procedure](https://openwrt.org/toh/gl.inet/gl-be9300). The README retains the stock factory-image procedure and eMMC/ART backup warning. |
| Wired LAN switch and WAN PHY | **Hardware-reported working** | Existing RTL8372N DSA package, 10GBASE-R SoC link and RTL8221B 2.5G USXGMII WAN. A two-unit 2.5G-trunk result of ~1.8–1.9 Gbit/s is reported; the README does not describe a complete link/rate matrix for every jack and peer. |
| Basic bridge/VLAN | **Maintainer-reported working** | The project reports bridge VLAN operation on the existing driver. Full FDB/MDB, STP/BPDU and multi-bridge acceptance evidence is not bundled with the reference release. |
| Wi-Fi 7 radios | **Maintainer-reported working** | The project reports the 2.4 GHz on-SoC radio and two QCN9274 PCIe radios working across all three bands. Per-client throughput/interoperability is not implied. |
| Tri-band MLO, DFS, 802.11k/v | **Maintainer-reported working; AP-specific** | Reported for the AP configuration. DFS includes multiple BSSes started together on a DFS radio. The exact test/client topology is not recorded in the release manifest. |
| 802.11r over-the-air with MLO | **Experimental and image-specific** | AP image manifest contains hostapd patches 992–999a; `vanilla`/`router` do not. Patch 999a says **NOT HARDWARE-VERIFIED**. A separate post-FT data-path failure is reported, and patch 992 does not change FT-over-the-DS for AP MLDs. Do not treat this as qualified roaming. |
| PPE IPv4 LAN→WAN offload | **Opt-in; hardware-reported for the existing driver** | Release notes report TCP/UDP SNAT, untagged or 802.1Q-tagged WAN, ~2.3 Gbit/s and ~1% CPU. The measured path/protocols are those stated; no general line-rate claim is made. |
| PPE WAN→LAN and IPv6 offload | **Not in `ref-20260930`** | These directions/protocols remain follow-up work. Tagged-WAN download traffic uses the software path in the release notes. |
| PPE source-port rewrite security fix | **Included in `ref-20260930`** | Patch 0448 fixes the hardware NAT byte-order error that could reuse another client’s mapping after a firewall source-port remap. The release directs users of affected 2026-09-23-through-09-30 router builds with hardware offload enabled to update. |
| ath12k crash recovery | **Recovery included; crash cause open** | The release records automatic firmware-coredump release and radio recovery in seconds. Sustained-load firmware crashes remain a known issue. |
| LAG link-state synchronization | **Open PR; not in the reference release** | [#47](https://github.com/perceival/openwrt-flint3/pull/47) targets the existing driver. A two-port LAG/failover test is reported for a prior PR head. The current head has not had its own package build or hardware run; a link-up LACP standby with TX disabled is explicitly outside validated behavior. |
| Egress port rate policing | **Open PR; not in the reference release** | [#49](https://github.com/perceival/openwrt-flint3/pull/49) targets the existing driver. Earlier-head measurements reported 476 Mbit/s for a 500 Mbit/s limit, 95 Mbit/s for 100 Mbit/s, and restoration after deletion. The current head has not had its own package build or hardware run. |
| Fan control around the first trip | **Hardware behavior measured; tuning change not merged** | [Issue #8](https://github.com/perceival/openwrt-flint3/issues/8) records two idle units on opposite sides of the 50 °C trip: 48.7 °C at 14%/1126 rpm and 50.4 °C at 50%/3493 rpm. The finer trip table was still unapplied in the latest update. |

The source includes additional legacy-driver interfaces; the table above deliberately distinguishes reported end-to-end behavior from source presence. Check the PRs and test reports before relying on a feature outside the evidence shown here.

## Replacement candidate: P0 #104 and dependent P1 work

These results belong to the replacement code only and must not be combined with tests of the shipping SDK-backed driver.

| Area | Candidate status | Build and hardware evidence |
| --- | --- | --- |
| P0 probe, reset, register transport, internal PHY, 10G PCS and native tags | **Implemented in Draft #104 source** | ARM64/Linux 6.18.39 module build passed with W=1/modpost. A full BE9300 AP-config OpenWrt image build also passed for source revision `954a84bd7c46dbbb2412eeddfb300aad8b4cff35` ([module run](https://github.com/MNeroba/openwrt-flint3/actions/runs/37059225390), [image run](https://github.com/MNeroba/openwrt-flint3/actions/runs/37059229177)). **No BE9300 hardware test is reported.** |
| P0 LAN ports, CPU link and software forwarding/isolation | **Implemented as the candidate’s intended fallback; not qualified** | The full first-device procedure is in [#104](https://github.com/perceival/openwrt-flint3/pull/104). Port mapping, PHY negotiation, native-tag behavior, reset recovery and software bridge forwarding all require the documented hardware run. |
| P1-A VLAN table transaction foundation | **Partially implemented in dependent Draft PR #1** | Adds shared serialization, checked VID/masks, raw readback and routes VLAN 1 bootstrap through the table path. Focused five-object ARM64 module build passed. Target image and hardware checks for this revision remain open; it does **not** add general DSA bridge/VLAN callbacks. |
| Hardware bridge/VLAN offload | **Not implemented in #104** | P1 research records VLAN/PVID and bridge-flag work, source conflicts and required table/error semantics. Only a staged plan and partial table foundation exist. |
| FDB/MDB learning/dump/flush and STP/BPDU handling | **Not implemented/qualified in #104** | CPU-only BPDU delivery with decodable source-port metadata, CIST state enforcement, dynamic-only flush and forwarding isolation are explicit hardware gates in [#100](https://github.com/perceival/openwrt-flint3/issues/100). |
| LAG #47 and rate policing #49 | **Not ported to the replacement** | P2 follows P0 hardware bring-up and P1 parity under the agreed plan. Measurements on the existing driver do not validate the replacement. |
| MTU/jumbo, alternate tags, mirroring, MIB/ethtool, EEE and GPIO parity | **Not established for the replacement** | #104 calls these out as baseline interfaces to restore or explicitly defer. Source presence in the old driver is not replacement support. |
| PPE/NAT and 802.11r | **Outside #104 scope** | Track separately from the switch replacement. The AP image’s FT series and the existing driver’s PPE results do not validate #104. |

The P0 results establish compilation and image assembly only. The exact-device test and recovery steps in the [first-hardware-test matrix](https://github.com/MNeroba/openwrt-flint3/blob/rtl837x-dsa-port/package/kernel/rtl837x/FIRST-HARDWARE-TEST.md) remain necessary before calling the candidate functional on a GL-BE9300.

## Evidence references

- [Latest reference images, build manifests and release test report](https://github.com/perceival/openwrt-flint3/releases/tag/ref-20260930)
- [Draft replacement PR #104, scope and status](https://github.com/perceival/openwrt-flint3/pull/104)
- [Replacement plan and acceptance criteria, Issue #100](https://github.com/perceival/openwrt-flint3/issues/100)
- [LAG PR #47](https://github.com/perceival/openwrt-flint3/pull/47) · [egress policing PR #49](https://github.com/perceival/openwrt-flint3/pull/49)
- [Fan/thermal-trip report, Issue #8](https://github.com/perceival/openwrt-flint3/issues/8)
- [PPE parser/offload bench notes](rtl8_4-ppe-alias-20260927.md)
- [Full BE9300 first-hardware test procedure for the replacement](https://github.com/MNeroba/openwrt-flint3/blob/rtl837x-dsa-port/package/kernel/rtl837x/FIRST-HARDWARE-TEST.md)
