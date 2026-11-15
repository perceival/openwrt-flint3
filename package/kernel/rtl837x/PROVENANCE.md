# RTL8372N P0 source and operation ledger

This ledger records the actual public inputs and their use. Public availability,
repository licensing and matching register values do not establish independent
origin by themselves. The candidate excludes the restricted SDK header and
PHY/SerDes patch payloads; review of the remaining lineage is still pending.

## Pinned inputs

| Input | Revision / declared license | Use and limit |
| --- | --- | --- |
| [Airjinkela DSA](https://github.com/airjinkela/rtl837x-dsa-driver/tree/7f3b64c7f3a37db1866fd80ba815d89974d995fd) | `7f3b64c7f3a37db1866fd80ba815d89974d995fd`, GPL-2.0-or-later source headers | Adapted DSA/phylink/MDIO architecture and retained author attribution. Its restricted generated header and patch arrays are excluded. The author's SDK lineage disclosure remains relevant to retained definitions. |
| [RTLPlayground](https://github.com/logicog/RTLPlayground/tree/f0aea3dcac056e3274fd39e1c76a7117471c37da) | `f0aea3dcac056e3274fd39e1c76a7117471c37da`, MIT repository notice | Register/operation comparison; `NOTICE` retains the MIT copyright/permission notice. This is a public corroborating source, not proof of independent research. |
| [ZTE RTL8372N source](https://github.com/cnjn/linux-mainline-zte-zxslc-sr1010/blob/07f8687248578d4be6931c665ff5d08bb6cc3d9d/drivers/net/ethernet/zte/zx279133-rtl8372n.c) | `07f8687248578d4be6931c665ff5d08bb6cc3d9d`, GPL-2.0 source metadata | Compare register facts and operations. Its SDS transaction helper corroborates the indirect command layout; its CPU-port-8 mode, reset, patch and polarity sequence is board-specific and is not imported. Source-owner provenance still needs review. |
| [Linux Realtek PHY](https://github.com/torvalds/linux/blob/v6.18/drivers/net/phy/realtek/realtek_main.c) | `v6.18`, GPL-2.0+ | Published PHY page/capability/status behavior. Original file credits Johnson Leung / Freescale Semiconductor (2004); see its retained upstream attribution and history. Candidate native MMD access uses the actual PHY port, unlike this source's single-PHY address-0 VEND2 helper. |
| [Linux r8169 transport](https://github.com/torvalds/linux/blob/v6.18/drivers/net/ethernet/realtek/r8169_main.c) | `v6.18`, GPL-2.0-only | Published `r8168g_mdio_read/write` page/OCP address semantics. Source credits ShuChen / Realtek (2002), Francois Romieu (2003–2007) and other contributors. No NIC MMIO transport or firmware is imported. |
| [Linux MDIO definitions](https://github.com/torvalds/linux/blob/v6.18/include/uapi/linux/mdio.h) | `v6.18`, GPL-2.0 WITH Linux-syscall-note | Standard MMD/2.5G advertisement constants, used through the kernel header. |
| [Linux MDIO bus matching](https://github.com/gregkh/linux/blob/v6.18.39/drivers/net/phy/mdio_bus.c), [bus registration](https://github.com/gregkh/linux/blob/v6.18.39/drivers/net/phy/mdio_bus_provider.c), [OF registration](https://github.com/gregkh/linux/blob/v6.18.39/drivers/net/mdio/of_mdio.c) and [fwnode helper](https://github.com/gregkh/linux/blob/v6.18.39/drivers/net/mdio/fwnode_mdio.c) | `v6.18.39`, upstream kernel sources | Use public `get_phy_device`, per-device `mdio.bus_match`, `phy_device_register`, OF association and managed bus teardown. New registration orchestration retains real hardware IDs; no vendor PHY data imported. |
| [Linux PHY device lifecycle](https://github.com/gregkh/linux/blob/v6.18.39/drivers/net/phy/phy_device.c) and [driver core](https://github.com/gregkh/linux/blob/v6.18.39/drivers/base/dd.c) | `v6.18.39`, upstream kernel sources | Review `get_phy_c22_id`, `phy_bus_match`, `phy_probe` and `device_is_bound`; use the exported binding API under the device lock. Diagnostic logging is new code; no PHY IDs or vendor payloads are introduced. |

## Code reuse and copyright-risk review

- The P0 implementation does not include Airjinkela's restricted generated
  register header, `rtk-api`, or the PHY/SerDes patch payloads. The module's
  object list is limited to the four files listed below.
- Air-derived portions remain in the candidate. The original Air copyright
  notice is retained in `rtl8372n.c`, `rtl837x_mdio.c` and `rtl837x.h`; the
  shared SDS/register helpers in `rtl837x_common.c` are substantially
  reworked and now carry an explicit “Portions adapted” notice. These files
  declare GPL-2.0-or-later, matching the Air source headers.
- `rtl8372n_phy.c` is a separate PHY integration using documented Linux PHY
  behavior; its source and register mapping are recorded below. The
  RTLPlayground MIT notice is retained in [NOTICE](NOTICE). ZTE is used for
  comparison only; no ZTE implementation block is included.
- This reduces the risk of uncredited copying, but it does not establish that
  Air's retained switch-specific work is independently sourced. The author
  disclosed SDK-derived lineage without identifying exactly which functions
  or definitions were affected. GPL headers and public repository licenses do
  not resolve rights in any third-party SDK-derived material. Keep the source
  owner/maintainer review open and do not describe the candidate as
  provenance-cleared until that lineage has a defensible disposition.

## Feature-to-source map

Locations name functions/symbols in the pinned files to keep references stable
through candidate edits. Hardware qualification remains incomplete for the
register and feature rows. Release 7 passes T0/T1/T2/T3/T4/T5/T7 and the full
six-pair T5 software-bridge matrix. The [Release-7 follow-up](https://github.com/perceival/openwrt-flint3/pull/104#issuecomment-6044579250)
provides kernel link events confirming physical cycles on all four jacks,
full T2 topology output and the 100-packet T4 check. Release 8 on source commit `7e51247b` reports 33/33 matching setup
readbacks and one standalone-port CPU-only isolation case; reverse-direction
evidence is counter-only. Broader register-level and feature qualification
remains open. See the [Release-8 report](https://github.com/perceival/openwrt-flint3/pull/104#issuecomment-6048213461)
and [raw logs](https://gist.github.com/perceival/3172215cb83c6ea3866fd55bee0808be).
See [the release-7 report](https://github.com/perceival/openwrt-flint3/pull/104#issuecomment-6037094988)
and [raw logs](https://gist.github.com/perceival/145d80ee322c48e88870ec011ae1200d).

| Candidate feature/symbols | Specific public source location | Derivation / remaining question |
| --- | --- | --- |
| MDIO registers 21–24, commands `0x19/0x1b`, BUSY bit | ZTE `RTL8372N_SMI_*`, `rtl8372n_wait_ready/read_reg/write_reg`; Air `src/rtl837x_mdio.c` | Adapted 32-bit register transport; new reset writer omits post-command polling. Reset semantics need hardware confirmation. |
| Chip ID `0x0004`, reset `0x0024`, accepted `0x837270xx` | RTLPlayground `rtl837x_regs.h`; ZTE ID/reset definitions and detection | Compared register facts; exact revision/reset defaults need readback. |
| PHY indirect `0x6438`–`0x6444`, status bits 24–26, C45 command encoding | ZTE `rtl8372n_phy_read/write`; RTLPlayground PHY transport routines | Reimplemented serialized access. Hardware polling arbitration and timeout budget need confirmation. |
| Per-port C22 page/OCP translation, page 0 / `0xa40` alias | Linux r8169 `OCP_STD_PHY_BASE`, `r8168g_mdio_read/write` | Fresh per-PHY page state and bounds checking, with phylib page callbacks following Linux Realtek `rtl821x_read_page/write_page` using already-locked bus access; no NIC transport copied. RTL8372N compatibility remains a bench question. |
| PHY power OCP `0xa610`, `0x2058/0x2858` | RTLPlayground `rtl837x_port.c` / `rtl837x_init.c` power operations; Air PHY enable/disable | Public register/value comparison. Whole-register writes and revision dependence remain unverified. |
| Private PHY capability page `0xa61`, reg `0x13`, `MDIO_PMA_SPEED_2_5G` | Linux Realtek `rtlgen_supports_2_5gbps` | Same published predicate with error propagation. Actual integrated PHY IDs/ability readbacks are required. |
| Native MMD access, 2.5G advertisement and BMCR autoneg restart | Linux `mdio.h` constants and `__genphy_config_aneg`; ZTE PHY autoneg behavior for comparison | Uses per-port native C45 and shared kernel C22 autoneg. Forced 2.5G omitted; native AN interoperability still unverified. |
| PHY PHYSR page `0xa43`, reg `0x12`, speed/duplex/master bits | Linux Realtek `rtlgen_read_status`, `rtlgen_decode_physr` | Restricted decoding for 10/100/1000/2500; unsupported encodings fail closed. Partner 2.5G advertisement uses native AN register 0x21 / bit 5, also shown in RTLPlayground `phy.h` and `print_phy_adv`; all status behavior needs readback. |
| SMI polling/MAC type `0x632c/0x6330/0x6334/0x6454` | RTLPlayground `rtl837x_regs.h`, init; ZTE setup | Separate contiguous fields. Reset defaults and automatic MAC tracking must be checked. |
| SDS indirect `0x03f8/0x03fc/0x0400`, command fields | [Air access helper](https://github.com/airjinkela/rtl837x-dsa-driver/blob/7f3b64c7f3a37db1866fd80ba815d89974d995fd/src/rtl837x_common.c#L408-L472) and [generated definitions](https://github.com/airjinkela/rtl837x-dsa-driver/blob/7f3b64c7f3a37db1866fd80ba815d89974d995fd/src/rtl8373_reg_definition.h#L2967-L2985); [ZTE helper](https://github.com/cnjn/linux-mainline-zte-zxslc-sr1010/blob/07f8687248578d4be6931c665ff5d08bb6cc3d9d/drivers/net/ethernet/zte/zx279133-rtl8372n.c#L1761-L1845); [RTLPlayground register map](https://github.com/logicog/RTLPlayground/blob/f0aea3dcac056e3274fd39e1c76a7117471c37da/rtl837x_regs.h#L64-L88) and SDS operations | The address and command layout agree technically across the references. Air's author disclosed SDK-derived lineage; the ZTE lineage is unreviewed; RTLPlayground declares MIT, but the reviewed material does not establish independent derivation. No vendor header or patch array is imported. Resolve source lineage separately from hardware behavior. |
| SDS mode `0x7b20`, mode `0x1a`, polarity pages 0/6 and bits | RTLPlayground [mode constants](https://github.com/logicog/RTLPlayground/blob/f0aea3dcac056e3274fd39e1c76a7117471c37da/rtl837x_regs.h#L64-L88), [SDS init](https://github.com/logicog/RTLPlayground/blob/f0aea3dcac056e3274fd39e1c76a7117471c37da/rtl837x_init.c#L21-L90), [OEM companion settings](https://github.com/logicog/RTLPlayground/blob/f0aea3dcac056e3274fd39e1c76a7117471c37da/machine_init.c#L60-L77); Air [interface-to-mode mapping](https://github.com/airjinkela/rtl837x-dsa-driver/blob/7f3b64c7f3a37db1866fd80ba815d89974d995fd/src/rtl837x_common.c#L207-L220); ZTE [CPU-port-8 setup](https://github.com/cnjn/linux-mainline-zte-zxslc-sr1010/blob/07f8687248578d4be6931c665ff5d08bb6cc3d9d/drivers/net/ethernet/zte/zx279133-rtl8372n.c#L1986-L2040) | `0x1a` is identified as 10GBASE-R/10GR by Air and RTLPlayground. ZTE's value `0x0d` is used with a different USXGMII CPU-port-8 setup and is not a conflicting Flint3 setting. RTLPlayground documents RX swap at page 0/reg 0 bit 9 and page 6/reg 2 bit 13. For TX, RTLPlayground says OEM firmware sets companion SDS0 polarity bits at page 0 bit 8 and page 6 bit 14 for RTL8221B; ZTE also changes these bits in a board-specific sequence. This corroborates behavior but is not an independent clean source. The candidate applies these settings to both SDS lanes from board properties; lane mapping and each flag's effect have not been independently verified. Release-7 link/traffic tests exercise the board configuration as a whole, not each polarity field independently. Generic reset/patch sequences remain excluded; post-reset and polarity-specific coverage are still open. |
| RTL8_4 CPU tag `0x603c/0x6720/0x6724` | RTLPlayground `rtl837x_regs.h` CPU tag symbols; Air `set_tag_rtl`; Linux `net/dsa/tag_rtl8_4.c` | Native tagger; no copied tagger or private 802.1Q protocol. Header format, forwarding reasons and port identity need traffic tests. |
| VLAN 1, table `0x5cac/0x5cb8`, read data `0x5ccc`, PVID `0x4e1c`, filter `0x4e14/0x4e18` | RTLPlayground [`vlan_setup` / `vlan_get`](https://github.com/logicog/RTLPlayground/blob/f0aea3dcac056e3274fd39e1c76a7117471c37da/rtl837x_port.c); ZTE VLAN map | Minimal bootstrap only. The new diagnostic issues the public read transaction and reports the raw word; it does not infer bit-25 semantics. General VLAN callbacks are absent. |
| Isolation `0x50c0 + port * 4` | RTLPlayground isolation register; ZTE isolation operations | CPU-only matrix; unused ports cleared. Hardware bridge join/leave code removed. |
| Learning limit `0x5384 + port * 4`, mask bits 12:0 | ZTE `RTL8372N_L2_LEARN_LIMIT_*`, bridge-flags learning update; RTLPlayground limit address | Zero limit for P0. Confirm actual disable and limit-exceeded behavior on hardware. |
| Flood `0x5360`–`0x5370`, mask bits 9:0 | ZTE `RTL8372N_*FLOOD`, `rtl8372n_flood_port_set` | CPU-only unknown unicast/multicast/broadcast targets; reserved RMA/control frames are a separate unresolved bench gate. |

## P1 research boundary

[P1-RESEARCH.md](P1-RESEARCH.md) maps potential VLAN/L2/CIST/BPDU follow-up
work to the pinned sources above and the Linux v6.18 DSA contract. Those rows
are research only: they do not extend the implemented feature-to-source map
or certify new definitions for import. In particular, conflicting descriptions
of VLAN bit 25 and L2 bit 29 must be resolved before general table APIs are
implemented. Every newly retained field/operation needs a ledger entry and
source disposition; hardware validation remains a separate requirement.

## Excluded material and candidate licensing

The module object list contains `rtl8372n.c`, `rtl837x_common.c`,
`rtl837x_mdio.c` and `rtl8372n_phy.c`. It excludes `rtk-api`, restricted generated
headers, all PHY/SerDes patch arrays, GPIO, swconfig and debug SDK interfaces.
The small register header is confined to used definitions and the ledger above.

The adapted driver files declare GPL-2.0-or-later; the new PHY layer declares
GPL-2.0-only. The combined module's `MODULE_LICENSE("GPL")` is kernel metadata,
not a claim that it resolves the origin of all register facts. The MIT input
notice is retained in [NOTICE](NOTICE). The existing package `LICENSE` file is
unchanged.

## Outstanding disposition

- Review retained Air/ZTE lineage with the source owners and maintainers,
  including [Airjinkela's SDK disclosure](https://github.com/RuijieNetworksCommunity/rtl837x-gsw-driver/issues/2#issuecomment-5946313878)
  and [JiaY-shi's source question](https://github.com/JiaY-shi/rtl83xx/issues/1).
- Confirm or independently source the SDS field/polarity facts still flagged
  above, with special attention to TX polarity semantics and Air-derived
  switch-specific code. Do not call the candidate fully provenance-clean
  until the retained lineage has a defensible disposition.
- Build for the real OpenWrt target and validate every row on BE9300.
- No source-owner Signed-off-by is inferred from a repository license or from
  our attribution. Obtain any required records before upstream submission.
