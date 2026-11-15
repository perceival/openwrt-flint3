# RTL8372N P0 build and review report

## Latest result (status checked 2026-10-08)

| Gate | Result | Evidence / scope |
| --- | --- | --- |
| Mainline ARM64 compilation | **Release 6 and 7 PASS** | Release-7 [module CI passed](https://github.com/MNeroba/openwrt-flint3/actions/runs/37454484695): four objects, `W=1`, modpost and module link, with no candidate compiler warnings; module SHA-256 `8163371788badabaf4777f88b24e6de0559634a574c1cc59c3033822df2eeb37`. Release 5 failed on the private `DEFAULT_GPIO_RESET_DELAY` macro. |
| OpenWrt configuration | **PASS** | BE9300 AP configuration; five pinned feeds verified; driver and MDIO-devres packages selected. |
| OpenWrt package, DTB and image | **Release 6 and 7 PASS** | Release-6 [full BE9300 image CI](https://github.com/MNeroba/openwrt-flint3/actions/runs/37397974073) and release-7 [full image CI](https://github.com/MNeroba/openwrt-flint3/actions/runs/37454704524) passed. |
| Source whitespace/style | **Release 6 and 7 PASS** | Release 7 passed `git diff --check` and strict `checkpatch.pl` with 0 findings. |
| BE9300 hardware | **Release 8: T0/T1 PASS; T6 PARTIAL** | [Release-8 report](https://github.com/perceival/openwrt-flint3/pull/104#issuecomment-6048213461) and [raw logs](https://gist.github.com/perceival/3172215cb83c6ea3866fd55bee0808be). Source commit `7e51247b` was built/flashed on kernel 6.18.39. T1 reports four private PHY bindings and all four jacks up at 2.5G/1G/2.5G/1G. All 33 logged setup readbacks matched expected values. The standalone-port test supports no forwarding from one detached port to one bridge observer; reverse evidence is counter-only. No general VLAN/offload sign-off. Release 7 separately supplies T2/T3/T4/T5/T7. |
| Retained source provenance | **OPEN REVIEW** | Restricted header and patch arrays excluded; SDS field/polarity lineage remains unresolved. |
| Replacement acceptance in #100 | **NOT MET** | P0 needs remaining hardware coverage; P1/P2 parity or maintainer-agreed deferrals remain required. |

The startup readback implementation was added in [`e2d5a951`](https://github.com/MNeroba/openwrt-flint3/commit/e2d5a95154c9dce1315a5d888981d400616a5f59);
its [ARM64 module CI](https://github.com/MNeroba/openwrt-flint3/actions/runs/37676023248)
passed. Source commit `7e51247b` passed focused [ARM64/Linux 6.18.39 module CI](https://github.com/MNeroba/openwrt-flint3/actions/runs/37676942253).
The maintainer built and booted an image from that exact commit for Release 8.
The [Release-8 report](https://github.com/perceival/openwrt-flint3/pull/104#issuecomment-6048213461)
records 33/33 expected readbacks and a partial standalone-port test. The broad
PR matrix is no longer queued: 198 checks succeeded, 29 failed and 2 were
skipped; it includes a failed `qualcommbe/ipq53xx` target job and requires
triage. The successful focused module build and bench image do not make the
whole PR matrix green or qualify general VLAN/isolation behavior.

The CI full target-build run passed on 2026-10-02 for its tested source
revision. The maintainer's independent `908810c09b` image on 2026-10-04 is a
historical run: it reached the first T1 failure and did not qualify traffic.
The later release-6 hardware result superseded that status for T0/T1/T2/T4/T7.
The release-7 report and [follow-up](https://github.com/perceival/openwrt-flint3/pull/104#issuecomment-6044579250)
confirm T0/T1/T2/T3/T4/T5/T7, including kernel link events for LAN1's physical
cycle and the full T2 topology and T4 ping evidence. Release 8 adds T6 setup
readbacks and a one-port negative-forwarding scenario; broader isolation,
reserved control-frame and CPU-load evidence remain open. See
[PHY-PROBE-REPORT.md](PHY-PROBE-REPORT.md).

## Release-7 hardware result (2026-10-07)

The [maintainer's release-7 report](https://github.com/perceival/openwrt-flint3/pull/104#issuecomment-6037094988)
identifies source `79afa2c51a3c2396c33ed511ed092d799c52e1bf`, image revision
`r35533+286-3b2bc55dcb`, and flashed-image SHA-256
`a219c20026965027b425b80181abb362377df5450298f86932246033ed480ac9`.
The [full-image CI](https://github.com/MNeroba/openwrt-flint3/actions/runs/37454704524)
and hardware T0/T1 pass. All four PHYs bind to the private driver and the
release-6 PHY power-down warnings on ports 0–2 are absent. Three warm reboots
and one cold power cycle pass with expected PHY bindings, rates and pings.

The report marks T3 PASS for all jacks (LAN1/3 at 2.5G; LAN2/4 at 1G) and
physical unplug/replug. The [T2/T3/T4 follow-up](https://github.com/perceival/openwrt-flint3/pull/104#issuecomment-6044579250)
explains that the SSH sampler ran over LAN1 and was interrupted during its
unplug; the kernel link-event excerpt confirms a down/up transition on all
four jacks. It also confirms four `lanN@lan` netdevs attached to `br-lan` in
forwarding state and T4 at 100/100 pings with 0% loss. T5 passes all six
directly connected pairs with 30-second bidirectional iperf3 runs and 20/20
pings; throughput is about 0.927–1.56 Gbit/s on the CPU/software-bridge path.
The LAN3 RX drop counter
rose by 77 across about 9.27 million packets; LAN1 TX drop 1 and LAN3 TX drops
2 were present before and unchanged after. TCP retransmissions are recorded,
with no P0 throughput threshold. Release 8 verifies the setup readbacks and
partially verifies one-port CPU-only isolation; the reverse-direction result
lacks a packet capture. The `lan` conduit RX/TX error and drop counters were reported as
zero for T4, while a separate `tx_errors` value still reads `2^64-1`; the raw
PPE MIB operands needed to interpret it are still unavailable. The boot log's
WAN PCS message persists, while the DSA CPU link comes up and the tests continue.

## PHY binding correction and release-6 bench result (2026-10-06)

The initial [diagnostic report](https://github.com/perceival/openwrt-flint3/pull/104#issuecomment-6006419562) showed successful C22 ID reads but binding to the in-tree RTL8224 driver. Release 6's pre-registration per-device matcher corrected that failure. The maintainer's [release-6 report](https://github.com/perceival/openwrt-flint3/pull/104#issuecomment-6014637917) confirms private-driver binding on ports 4–7 and passes T0/T1/T2/T4/T7. Follow-up results make T3 partial and T5 partial (3/6 LAN pairs); T6 is software output only. The full cause, logs and warning disposition are in [PHY-PROBE-REPORT.md](PHY-PROBE-REPORT.md).

Release 5 suppresses automatic internal-bus discovery and assigns the private
matcher before PHY registration, but its ARM64 build failed on an inaccessible
kernel macro. Release 6 replaces it with a local named 10 us value matching
upstream OF-MDIO behavior and rejects unsupported `ethernet-phy-package` nodes
with an explicit error. Release-6 module and full-image CI both passed; the
maintainer then passed T0/T1 on hardware. Five starts did not reproduce the
prior boot stop, but the old stop's cause is not established. The new package
release-7 guard prevents PHY access to non-PHY DSA port holes 0–2; its ARM64
module and full-image builds passed, and the hardware log confirms the
power-down warnings disappeared.

Diagnostic release 4 (`1b7a32bef2`) passed [module CI](https://github.com/MNeroba/openwrt-flint3/actions/runs/37273928197) and
[full-image CI](https://github.com/MNeroba/openwrt-flint3/actions/runs/37273960255); those passes do not qualify releases 5 or 6.

## Follow-up research (updated 2026-10-04)

[P1-RESEARCH.md](P1-RESEARCH.md) adds a source-grounded plan for VLAN/L2 table
access, bridge flags, FDB/MDB, CIST and BPDU delivery. No P1 runtime support or
hardware result is added. The module and full target-image builds passed for
the unchanged P0 build inputs. Research findings are not hardware or functional
qualification.

## Revisions and reproduction

- Proposed base: `perceival/openwrt-flint3:flint3-be9300`,
  `2365932733ca8ec3b346621d9cec2eb3df3b2cf3`.
- CI source: `954a84bd7c46dbbb2412eeddfb300aad8b4cff35`.
- Maintainer-side build report for source commit: `908810c09bd9adfbbc7d25437a9d50b55b2de940`
  ([comment](https://github.com/perceival/openwrt-flint3/pull/104#issuecomment-5976007832)).
- Tested `package/kernel/rtl837x/src` Git tree:
  `785d7682936058c86e90af809e16694ac6dc7492`.
- Documentation commits through `7be7f8d541` retain the baseline build inputs.
  The diagnostic revision and release-6 binding correction each have their own
  builds. Release 5 (2026-10-06) failed ARM64 module CI because
  `DEFAULT_GPIO_RESET_DELAY` is private to kernel OF-MDIO. Release 6 fixes the
  compile error and passed both module and full-image CI. Package release 7
  adds the callback guard; module and full-image CI passed, and the hardware
  log confirms the port 0–2 warnings are absent.
- Build hosts: GitHub-hosted Ubuntu 24.04. ARM64 API check uses
  `aarch64-linux-gnu-`; OpenWrt uses the project toolchain/config.

### Mainline API build

[Successful run](https://github.com/MNeroba/openwrt-flint3/actions/runs/37059225390)
completed on 2026-10-02 at 20:20:15 UTC.

[Build artifact](https://github.com/MNeroba/openwrt-flint3/actions/runs/37059225390/artifacts/11248964933)
contains the generated kernel `.config`, kernel/module logs and
`rtl8372n_dsa.ko`. There are no compiler warnings in the candidate compilation
step. Kernel exports and the RTL8_4 tagger were built before modpost.

| Input / output | SHA-256 |
| --- | --- |
| `linux-6.18.39.tar.xz` | `a7a7e3d2ae9d95e74197223a8d4eb5f6be7aac21b6e6de27e9685d001c1f8cb0` |
| Mainline `rtl8372n_dsa.ko` | `d47c1109ab19c30f81f7a7ccd034d787b1fe2e2684acb99c89389cee1b93f1d6` |

This is an API/modpost artifact, not an OpenWrt installation package. The
minimal kernel config selects `KUNIT`/`REGMAP_BUILD` to enable the hidden
regmap core; no KUnit test suite was run. This check does not exercise probe,
PHY transactions, SerDes or forwarding on hardware.

### OpenWrt target build

[Full BE9300 run](https://github.com/MNeroba/openwrt-flint3/actions/runs/37059229177) **completed successfully** on 2026-10-02 for source
revision `954a84bd7c46dbbb2412eeddfb300aad8b4cff35`. It used [configs/ap.config](../../../configs/ap.config)
and [P0-FEEDS.conf](P0-FEEDS.conf), then executed `make defconfig` and
`make -j2 V=s`. Package/DTB/image building and artifact confirmation passed.
The [uploaded build artifact](https://github.com/MNeroba/openwrt-flint3/actions/runs/37059229177/artifacts/11254652883) contains generated configuration,
logs, packages and image outputs.

| Feed | Pinned commit |
| --- | --- |
| packages | `493b2ae11c3148f43b3ab680ac2b2bb78cc8430c` |
| luci | `aa3d48836e90ae0706c8d8f9b46b8371e45cfe1f` |
| routing | `4b9891b9136259f93294a424507ed24c5e8c1cbd` |
| telephony | `5d68d53c160a325ea9d03fce393e051573bcc736` |
| video | `816fa8fe0ca759cc5d1ba71af1a716405bf4dda4` |

The CI image-build and artifact-confirmation gate is **PASS** for the source
revision above. The maintainer separately reproduced revision `908810c09b`
with the documented config adjustments and staged its checksummed image for
bench use. The uploaded CI artifact preserves generated `.config`,
`p0-build-inputs.txt`, installed feed lock, logs, packages and target images.
This establishes successful build/packaging for the baseline. The independently
built `908810c09b` image reached the reported T1 failure on BE9300; it did not
reach traffic tests. A separate minimal OpenWrt dependency-image build has not
been run.

## Feature readiness

The table separates release-6 observations from work still required; passing
one smoke check does not qualify the entire feature.

| Feature | Candidate implementation | Required first-device evidence |
| --- | --- | --- |
| MDIO/regmap, detection and reset | Implemented; checked field helpers and dedicated reset writer | Chip ID, no timeouts, cold/warm consistency |
| Internal PHY transport | Serialized C22/C45/OCP; per-PHY pages | IDs, correct binding, concurrent access and recovery |
| PHY autoneg/status | Private driver; published 10/100/1000/2500 decoding | Supported/local/partner modes; peers at available rates; link cycling |
| 10G CPU PCS/SerDes | Mode and polarity plumbing; no vendor patch arrays | Physical PCS at both ends and bidirectional traffic after reset |
| CPU tags / LAN mapping | Native kernel RTL8_4; users 4–7 | Correct source jack, directed TX and no duplicates |
| VLAN 1 bootstrap / isolation | CPU-only matrix; learning disabled; CPU flood masks | VLAN/PVID, isolation/learning/flood readbacks and negative forwarding tests |
| Untagged software bridge | Intended fallback; hardware bridge callbacks absent | First-port join, all six LAN pairs, leave/rejoin and CPU forwarding |
| General switching offload | VLAN/FDB/MDB/STP/bridge flags not implemented | P1 work after P0; BPDU/RMA behavior remains an explicit open test |
| LAG #47 / policing #49 | Not ported | P2 implementation and current-revision bench results |
| Other baseline features | MTU/jumbo, alternate tags, mirroring, statistics, EEE and GPIO parity not established | Restore or agree explicit deferrals |

Only RTL8372N with `realtek,rtl8372n` is claimed as the source scope. The
BE9300 test uses CPU port 3 at fixed 10GBASE-R and users 4–7. Legacy BE6500
`realtek,rtl837x` nodes are unsupported by this candidate.

## Source scope and remaining decisions

[PROVENANCE.md](PROVENANCE.md) records pinned inputs, symbol/operation lineage
and unresolved rows. Airjinkela's [deprecation notice](https://github.com/RuijieNetworksCommunity/rtl837x-gsw-driver/issues/2#issuecomment-5935360083)
and [SDK disclosure](https://github.com/RuijieNetworksCommunity/rtl837x-gsw-driver/issues/2#issuecomment-5946313878)
are linked explicitly. Public repository licenses are not described as proof
of independent origin or as source-owner permission.

The candidate excludes `rtk-api`, the restricted generated register header and
all vendor PHY/SerDes patch arrays. The existing package `LICENSE` is unchanged;
required author attribution and the MIT notice are retained. Twenty unrelated
local patch-metadata changes are excluded from the published commits.

Before replacement/merge:

1. Re-run the OpenWrt package/DTB/image build if code, configuration or pinned
   feeds change; the full-image gate passed for the revision recorded above.
2. Resolve retained SDS/source lineage questions.
3. Run [FIRST-HARDWARE-TEST.md](FIRST-HARDWARE-TEST.md), including negative
   forwarding, control frames, reset and PHY concurrency checks. A fixed-link
   carrier or one successful ping is insufficient.
4. Restore P1/P2 and the remaining baseline interfaces, or record maintainer
   agreement to each deferral under [Issue #100](https://github.com/perceival/openwrt-flint3/issues/100).
5. Coordinate the final board changes separately. Official IPQ53xx/BE9300
   integration remains tied to [OpenWrt #23161](https://github.com/openwrt/openwrt/pull/23161),
   which was open and unmerged at this check.

Keep the shipping baseline while this Draft is reviewed. Release 7 passes
T0/T1/T2/T3/T4/T5/T7, including the completed T3 link-event evidence, T2
topology report and T4 ping run. Release 8 adds 33/33 setup readbacks and a
one-port negative-forwarding result; reverse-direction evidence is counter-only.
Broader VLAN/isolation and reserved-control-frame behavior, recovery and
concurrency checks, source-owner sign-off and functional parity remain open;
P0 qualification is not claimed.
