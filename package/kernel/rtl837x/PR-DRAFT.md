# rtl8372n: add P0 DSA bring-up candidate for GL-BE9300

## Problem

The shipping RTL837x package uses the old SDK-backed source. In his
[deprecation comment](https://github.com/RuijieNetworksCommunity/rtl837x-gsw-driver/issues/2#issuecomment-5935360083),
Airjinkela recommends the DSA refactor and plans to remove the retained
register-definition header. This PR starts the replacement tracked in
[#100](https://github.com/perceival/openwrt-flint3/issues/100), following the discussion in
[#99](https://github.com/perceival/openwrt-flint3/issues/99).

## Summary

Prepare a minimal RTL8372N DSA bring-up candidate for GL-BE9300:

- Adapt the Airjinkela DSA/phylink/MDIO architecture with retained attribution.
  Remove the SDK object tree, restricted generated header and vendor PHY/SerDes
  patch arrays from the candidate.
- Add a small register map with a pinned source/operation ledger; retain the
  RTLPlayground MIT notice and identify unresolved SDS lineage explicitly.
- Fix runtime register-field handling, serialize complete PHY/SDS transactions
  and document child/parent MDIO lock ordering.
- Add per-PHY C22 pages and a private internal-PHY driver using actual-port
  native MMD access, published capability/status definitions and 2.5G autoneg.
  Missing/wrong PHY binding and unsupported speed encodings fail closed;
  forced 2.5G is unsupported.
- Use a dedicated reset writer, fail-closed PCS reads, early topology validation
  and setup-failure/teardown quiescing.
- Use CPU-only isolation/flood masks with hardware learning disabled. Incomplete
  hardware bridge callbacks are removed; DSA software bridging is the P0 path.
  Release 7 passed all six LAN-pair tests over that path. Release 8 on source
  commit `7e51247b` passed 33/33 setup readbacks and partially verified isolation
  for one standalone port; this does not establish general VLAN offload or a
  throughput target.
- Target Linux 6.18, declare MDIO devres dependency and build `rtl8372n_dsa.ko`
  with the kernel RTL8_4 tagger. Coordinate BE9300's switch compatible to
  `realtek,rtl8372n` and remove unsupported legacy switch properties.
- Include reproducible CI inputs, a build/review report and the first-device
  procedure with a result template.

## Validation and build evidence

The package baseline at `954a84bd7c46dbbb2412eeddfb300aad8b4cff35` and
diagnostic `1b7a32bef2` built successfully, but the diagnostic hardware run
failed T1 because RTL8224 bound first. Release 5 failed module CI on a private
kernel macro. Release 6 [module CI](https://github.com/MNeroba/openwrt-flint3/actions/runs/37397943227)
and [full image CI](https://github.com/MNeroba/openwrt-flint3/actions/runs/37397974073)
passed; its module SHA-256 is
`9ccd428ae58f7650d8f7e47455c24250349e840758208e800146663a44037263`.

Package release 7 (`79afa2c51a3c2396c33ed511ed092d799c52e1bf`) passed both
[ARM64 module CI](https://github.com/MNeroba/openwrt-flint3/actions/runs/37454484695)
and [full BE9300 image CI](https://github.com/MNeroba/openwrt-flint3/actions/runs/37454704524).
The maintainer's [release-7 hardware report](https://github.com/perceival/openwrt-flint3/pull/104#issuecomment-6037094988)
records T0/T1 and T7 PASS, T3 reported PASS, T5 PASS, and T6 unverified. All
four internal PHYs bind to the private driver; the ports 0–2 power-down
warnings are absent. Three warm reboots and one cold cycle preserved binding,
link speeds and reachability.

T3 reports physical unplug/replug on all four jacks and rates of 2.5G/1G/2.5G/1G.
The [release-7 follow-up](https://github.com/perceival/openwrt-flint3/pull/104#issuecomment-6044579250)
explains the LAN1 sample gap: the sampler ran over SSH through LAN1 and was
interrupted during the unplug. The kernel link-event excerpt confirms physical
down/up on all four jacks; LAN2 briefly flapped a second time during reinsert.
T5 now covers all six jack pairs with 30-second bidirectional
iperf3 and 20/20 pings. Throughput ranged from about 0.927 to 1.56 Gbit/s on
the CPU/software-bridge path. The run recorded 77 new RX drops on LAN3 (out of
about 9.27 million packets) and TCP retransmissions; no throughput threshold is
defined. LAN1's one TX drop and LAN3's two TX drops were already present before
the run. The aggregate CPU snapshot omits softirq time and is not a forwarding
CPU measurement. Release 8 now supplies 33/33 setup readbacks and a limited
one-port negative-forwarding result; reverse-direction evidence is counter-only.

The follow-up confirms Release-7 T2: four `lanN@lan` netdevs in `br-lan`,
forwarding state, expected DSA modules loaded, and LAN4 linked at 1G/Full.
Release-7 T4 passes 100/100 pings with 0% loss. The reported conduit RX/TX
errors and drops are zero; the separate `tx_errors=2^64-1` anomaly remains
uninterpreted without raw PPE MIB operands. T8 remains an observation only, not
a switching-performance qualification.

| Check | Result | Evidence |
| --- | --- | --- |
| ARM64 / Linux 6.18.39 | **Release 6, 7 and Release-8-tested source module CI PASS** | [Release 4 module CI](https://github.com/MNeroba/openwrt-flint3/actions/runs/37273928197) passed. [Release 5 CI](https://github.com/MNeroba/openwrt-flint3/actions/runs/37395423769) failed because `DEFAULT_GPIO_RESET_DELAY` is private to `of_mdio.c`. Release 6 [module CI passed](https://github.com/MNeroba/openwrt-flint3/actions/runs/37397943227). Release 7 [module CI passed](https://github.com/MNeroba/openwrt-flint3/actions/runs/37454484695). Source commit `7e51247b` [focused module CI passed](https://github.com/MNeroba/openwrt-flint3/actions/runs/37676942253); the earlier release-7 module SHA-256 is `8163371788badabaf4777f88b24e6de0559634a574c1cc59c3033822df2eeb37`. |
| BE9300 OpenWrt configuration | **PASS** | AP config, pinned-feed verification and driver/MDIO-devres selection in the [target run](https://github.com/MNeroba/openwrt-flint3/actions/runs/37059229177) |
| OpenWrt package / DTB / full image | **Release 6 and 7 CI PASS; Release 8 image for tested source commit built and booted on BE9300** | Baseline, diagnostic, release-6 and release-7 image builds passed. The [release-7 full-image run](https://github.com/MNeroba/openwrt-flint3/actions/runs/37454704524) built the candidate for its hardware report. Perceival's [Release-8 report](https://github.com/perceival/openwrt-flint3/pull/104#issuecomment-6048213461) identifies and boots a full image from source commit `7e51247b`; the broad affected-target PR matrix has failures, including `qualcommbe/ipq53xx`, and still needs triage. The [release-5 image CI](https://github.com/MNeroba/openwrt-flint3/actions/runs/37395426353) was cancelled after its module compile failed. |
| Whitespace | **Release 7 PASS** | `git diff --check` passed for the release-6 to release-7 change. |
| checkpatch | **Release 7: 0 findings** | The release-7 source diff has 0 errors, warnings or checks under strict `checkpatch.pl`. |
| BE9300 hardware | **Release 7: T0/T1/T2/T3/T4/T5/T7 PASS; Release 8: T1 PASS, T6 PARTIAL** | [Release-7 report](https://github.com/perceival/openwrt-flint3/pull/104#issuecomment-6037094988), [T2/T3/T4 follow-up](https://github.com/perceival/openwrt-flint3/pull/104#issuecomment-6044579250), and [Release-8 report](https://github.com/perceival/openwrt-flint3/pull/104#issuecomment-6048213461) with [raw logs](https://gist.github.com/perceival/3172215cb83c6ea3866fd55bee0808be). Release 8 tests source commit `7e51247b`: 33/33 setup readbacks pass; one standalone-port test supports CPU-only isolation in the measured direction, while reverse evidence is counter-only. No general VLAN/offload sign-off. |

### P0 setup readback follow-up (tested source commit `7e51247b`)

The follow-up adds boot-time readbacks for the VLAN 1 table word, available
PVID fields, isolation masks, learning limits, CPU flood destinations and VLAN
filter controls. The VLAN table operation issues a read command and does not
write the table entry. Perceival's Release-8 log reports 33/33 matching
readbacks. The negative-forwarding test supports CPU-only isolation for one
detached port and one measured direction; the reverse-direction result is
counter-only, with no endpoint packet capture. T6 is therefore **PARTIAL**:
the readback is verified, but this does not establish general VLAN offload,
all-port isolation, VLAN-aware behavior or reserved control-frame handling.

The implementation commit [`e2d5a951`](https://github.com/MNeroba/openwrt-flint3/commit/e2d5a95154c9dce1315a5d888981d400616a5f59)
passed [ARM64/Linux 6.18.39 module CI](https://github.com/MNeroba/openwrt-flint3/actions/runs/37676023248).
Source commit `7e51247b` passed focused [ARM64 module CI](https://github.com/MNeroba/openwrt-flint3/actions/runs/37676942253),
and its image was built and booted for Release 8. The broad PR
matrix has 198 successful, 29 failed and 2 skipped checks, including a failed
`qualcommbe/ipq53xx` target job; this needs separate triage. The focused module
build and Release-8 image do not make that matrix green. Release-7 results do
not validate the later source revision.

The [ARM64 artifact](https://github.com/MNeroba/openwrt-flint3/actions/runs/37059225390/artifacts/11248964933)
contains the generated config, complete build logs and module. Its module
SHA-256 is `d47c1109ab19c30f81f7a7ccd034d787b1fe2e2684acb99c89389cee1b93f1d6`.
This is an API-check artifact, not an OpenWrt installation package.

The full target workflow uses `configs/ap.config` and five pinned feeds. It
retains build inputs, generated config, logs, target packages/images and image
checksums when available. A partial artifact upload does not establish success.

### Release-6 warning diagnosis

The serial log's `failed to power down PHY on port 0/1/2: -22` comes from our
DSA `port_disable` callback. Release 6 treated all non-SerDes ports as PHYs,
while the accessor accepts only ports 4–7 and returns `-EINVAL` before any PHY
transaction. Package release 7 limits both enable and disable callbacks to the
supported PHY-port mask. Release-7 module and full-image CI passed, and the
release-7 hardware log confirms that the port 0–2 power-down warnings no longer
appear.

The conduit `lan` counter remains anomalous and separate from the switch
driver. Release 6 showed `tx_errors=2^64-2`; the release-7 T5 baseline and
final sample show `2^64-1` unchanged. Qualcomm PPE computes it with an unsigned
subtraction of `tx_frames_g` from `tx_packets`; these values are consistent
with underflow, but the report does not include both raw MIB operands. This
does not establish packet loss and still needs separate raw-MIB validation.

The `10GBASE-R link not up before USXG_EN` message remains in the release-7
boot log after `wan` inband/USXGMII setup. Boot continues, the DSA CPU link
reports 10 Gbit/s, and the hardware tests pass. It is not evidence of a failed
DSA CPU link and does not explain the earlier release-4 log ending. That older
stop remains unexplained because the log contains no panic or hung-task evidence.

## Confirmed PHY binding cause and registration correction (2026-10-06)

Perceival's [diagnostic report](https://github.com/perceival/openwrt-flint3/pull/104#issuecomment-6006419562) confirms all four PHYs read
ID `0x001ccad0`, but the in-tree `RTL8224 2.5Gbps PHY` driver wins binding.
Adding another matching ID or relying on module ordering provides no priority.

Release 5 failed ARM64 compilation because `DEFAULT_GPIO_RESET_DELAY` is
private to kernel `of_mdio.c`. Release 6 replaces it with a named local 10 us
default matching upstream. It reports unsupported `ethernet-phy-package` nodes
explicitly.

The release-6 correction:

- Registers the private bus with automatic scanning disabled, discovers actual
  PHY IDs and sets a device-specific matcher before registering each PHY.
  The per-device MDIO callback admits the private driver on enabled ports 4–7.
  Linux checks OF matches before invoking this callback; vendor-specific child
  compatibles need separate review. BE9300 has no child MDIO PHY nodes.
- Preserves optional MDIO-node/PHY-node association and reset delays; rejects
  invalid, duplicate or missing explicit internal PHY addresses and unsupported
  C45 PHYs/package nodes. The BE9300 path needs no DT compatible change. WAN
  Realtek remains available.
- Retains completed-binding checks and all probe/read-error diagnostics.
  Registered PHYs are owned by managed bus teardown; failed registration frees
  the unregistered device. No ID spoofing or post-probe rebind is used.
- Leaves PHY/SerDes/reset/forwarding register programming unchanged.

The [failure/fix report](https://github.com/MNeroba/openwrt-flint3/blob/rtl837x-dsa-port/package/kernel/rtl837x/PHY-PROBE-REPORT.md)
records the revision-specific hardware evidence. Release 7 confirms private
PHY binding, reset repeatability, all six software-bridge pairs, T2/T3/T4 and
the absence of port 0–2 power-down warnings. Release 8 adds exact-revision T1 and
partial T6 setup/isolation evidence. Broader VLAN/isolation, reserved control
frames and the CPU softirq forwarding load remain open. P1-A remains separate.

## Scope and remaining work

| Stage | Scope | Current status |
| --- | --- | --- |
| P0 | Probe/reset, register access, internal PHY, 10G CPU PCS, native tags, four LAN jacks and CPU/software forwarding | Release 7 passes T0/T1/T2/T3/T4/T5/T7, including all six LAN pairs. Release 8 tests source commit `7e51247b`: T1 and T6 setup readbacks pass; the one-port negative-forwarding result is partial, with reverse evidence counter-only. Broader isolation/control-frame behavior remains open. T5 recorded 77 new LAN3 RX drops; no throughput target is defined. |
| P1 | Hardware bridge/VLAN, FDB/MDB, STP/BPDU and bridge flags | Not implemented; [source/dependency plan](https://github.com/MNeroba/openwrt-flint3/blob/rtl837x-dsa-port/package/kernel/rtl837x/P1-RESEARCH.md) prepared; BPDU/database/table semantics remain gates |
| P2 | LAG #47 and rate limiting #49 | Not ported; prior feature requirements remain applicable |
| Other baseline interfaces | MTU/jumbo, alternate tags, mirroring, MIB/ethtool, EEE and GPIO parity | Not established; restore or agree individual deferrals |

Only RTL8372N with the explicit compatible is included in the candidate scope.
BE9300 uses CPU port 3 at fixed 10GBASE-R and internal PHY/user ports 4–7.
Legacy BE6500 `realtek,rtl837x` nodes are unsupported by this candidate.

This Draft is for technical/provenance review and staged hardware bring-up.
Release 7 confirms T0/T1, reset repeatability and the complete software-bridge
pair matrix. Release 8 closes the setup-readback gap for its listed registers
and partially tests one-port isolation; P0 qualification and provenance review
remain open. Keep the shipping baseline until the acceptance criteria in #100
pass or the maintainer explicitly agrees the corresponding feature deferrals.
PPE/NAT and 802.11r remain outside this PR. Final official driver and board submissions
should remain separate. OpenWrt [PR #23161](https://github.com/openwrt/openwrt/pull/23161),
the IPQ53xx prerequisite, remains open and unmerged as of 2026-10-08. The
independent `908810c09b` image failed T1 in the earlier diagnostic run.
Release 7 now passes T0/T1/T2/T3/T4/T5/T7 on hardware. Complete remaining P0
checks and source/provenance gates before the separate P1-A image and A0–A6
tests; broader T6 isolation and reserved-control-frame coverage remain open.

### P1 research and maintainer scope update (2026-10-04)

Public sources cover substantial VLAN/L2/CIST operations, but they disagree
on the VLAN selector description, VLAN bit 25 and L2 bit 29. The new plan
records these conflicts, the Linux DSA CPU/database requirements, two BPDU
delivery options and a staged implementation/bench matrix. No P1 runtime
callbacks were added by that research update; it left the baseline P0 build
inputs unchanged. The later diagnostic source revision is tracked separately.

Perceival agrees with the P0 hardware → P1 parity → P2 order and accepts one
initially offloaded hardware bridge for P1. Multiple bridge domains/MST remain
deferred pending semantics work; unsupported domains require CPU-only/software
fallback and isolation tests. Keep P1-A separate until the remaining P0 matrix and provenance gates are
resolved. Prepare shared table transactions and checked codecs first;
prove BPDU CPU delivery, CIST and dynamic flush before enabling hardware
bridge/VLAN/flags; then add FDB/MDB with explicit database/CPU-entry semantics.

## Provenance review

The restricted header and vendor patch arrays are excluded. The existing
package `LICENSE` is unchanged. Remaining public-source lineage is recorded
rather than described as fully cleared: Airjinkela's
[SDK disclosure](https://github.com/RuijieNetworksCommunity/rtl837x-gsw-driver/issues/2#issuecomment-5946313878)
still matters, particularly for SDS command fields and polarity definitions.
Repository licensing and matching register values are not presented as proof
of independent origin or as source-owner authorization. No third-party
Signed-off-by is inferred.

## Review documents

- [Build/review report and exact inputs](https://github.com/MNeroba/openwrt-flint3/blob/rtl837x-dsa-port/package/kernel/rtl837x/BUILD-REPORT.md)
- [Current implementation status](https://github.com/MNeroba/openwrt-flint3/blob/rtl837x-dsa-port/package/kernel/rtl837x/P0-STATUS.md)
- [Source and operation ledger](https://github.com/MNeroba/openwrt-flint3/blob/rtl837x-dsa-port/package/kernel/rtl837x/PROVENANCE.md)
- [PHY probe failure analysis and diagnostic rerun](https://github.com/MNeroba/openwrt-flint3/blob/rtl837x-dsa-port/package/kernel/rtl837x/PHY-PROBE-REPORT.md)
- [First hardware test matrix and report template](https://github.com/MNeroba/openwrt-flint3/blob/rtl837x-dsa-port/package/kernel/rtl837x/FIRST-HARDWARE-TEST.md)
- [P1 feasibility, source conflicts and staged acceptance matrix](https://github.com/MNeroba/openwrt-flint3/blob/rtl837x-dsa-port/package/kernel/rtl837x/P1-RESEARCH.md)
- [Pre-PR audit and phased remediation plan](https://github.com/MNeroba/openwrt-flint3/blob/rtl837x-dsa-port/package/kernel/rtl837x/PRE-PR-PLAN.md)

@perceival, thank you for the detailed Release-7 and Release-8 reports and
for publishing the raw logs. The latest evidence is linked here:
[Release-8 bench report](https://github.com/perceival/openwrt-flint3/pull/104#issuecomment-6048213461).
