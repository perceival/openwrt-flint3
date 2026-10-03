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
| [ZTE RTL8372N source](https://github.com/cnjn/linux-mainline-zte-zxslc-sr1010/blob/07f8687248578d4be6931c665ff5d08bb6cc3d9d/drivers/net/ethernet/zte/zx279133-rtl8372n.c) | `07f8687248578d4be6931c665ff5d08bb6cc3d9d`, GPL-2.0 source metadata | Compare register facts and operations. No ZTE private-VLAN CPU protocol, board control or PHY/SerDes patch sequence is imported. Source-owner provenance still needs review. |
| [Linux Realtek PHY](https://github.com/torvalds/linux/blob/v6.18/drivers/net/phy/realtek/realtek_main.c) | `v6.18`, GPL-2.0+ | Published PHY page/capability/status behavior. Original file credits Johnson Leung / Freescale Semiconductor (2004); see its retained upstream attribution and history. Candidate native MMD access uses the actual PHY port, unlike this source's single-PHY address-0 VEND2 helper. |
| [Linux r8169 transport](https://github.com/torvalds/linux/blob/v6.18/drivers/net/ethernet/realtek/r8169_main.c) | `v6.18`, GPL-2.0-only | Published `r8168g_mdio_read/write` page/OCP address semantics. Source credits ShuChen / Realtek (2002), Francois Romieu (2003–2007) and other contributors. No NIC MMIO transport or firmware is imported. |
| [Linux MDIO definitions](https://github.com/torvalds/linux/blob/v6.18/include/uapi/linux/mdio.h) | `v6.18`, GPL-2.0 WITH Linux-syscall-note | Standard MMD/2.5G advertisement constants, used through the kernel header. |

## Feature-to-source map

Locations name functions/symbols in the pinned files to keep references stable
through candidate edits. Hardware verification is **pending for every row**.

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
| SDS indirect `0x03f8/0x03fc/0x0400`, command fields | Air `src/rtl837x_common.c` SDS command construction; ZTE SDS operations | Narrow command facts are retained, no patch arrays. This row still needs a specific lineage disposition; excluding the vendor header does not alone settle it. |
| SDS mode `0x7b20`, mode `0x1a`, polarity pages 0/6 and bits | RTLPlayground `rtl837x_init.c::sds_init`; Air mode/polarity operations; ZTE SDS mode comparison | Only mode/polarity operations. Source and mode-change ordering review remain pending, especially TX polarity definitions and post-reset completeness. |
| RTL8_4 CPU tag `0x603c/0x6720/0x6724` | RTLPlayground `rtl837x_regs.h` CPU tag symbols; Air `set_tag_rtl`; Linux `net/dsa/tag_rtl8_4.c` | Native tagger; no copied tagger or private 802.1Q protocol. Header format, forwarding reasons and port identity need traffic tests. |
| VLAN 1, table `0x5cac/0x5cb8`, PVID `0x4e1c`, filter `0x4e14/0x4e18` | RTLPlayground `rtl837x_port.c::vlan_setup` and table definitions; ZTE VLAN map | Minimal bootstrap only. General VLAN callbacks are absent. |
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
  above. Do not call the candidate fully provenance-clean until that is done.
- Build for the real OpenWrt target and validate every row on BE9300.
- No source-owner Signed-off-by is inferred from a repository license or from
  our attribution. Obtain any required records before upstream submission.
