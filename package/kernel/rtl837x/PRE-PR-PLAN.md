# RTL8372N DSA: review and plan before PR publication

Review date: 2026-10-02.

**Implementation update:** [P0-STATUS.md](P0-STATUS.md) records the source fixes
and remaining gates. Findings below describe the pre-fix audit snapshot; they
are retained as the rationale, not as a claim that every defect still exists.
[BUILD-REPORT.md](BUILD-REPORT.md) records the subsequent ARM64 build evidence;
[P1-RESEARCH.md](P1-RESEARCH.md) is the current 2026-10-03 P1 source/dependency
plan. The pre-fix figures and readiness statements below are historical.

 This is a source-review result and an implementation
plan, not a successful build or hardware qualification report.

## 1. Reviewed candidate and recommendation

- Working branch: `rtl837x-dsa-port`.
- Base: `2365932733ca8ec3b346621d9cec2eb3df3b2cf3`.
- Proposed first review target: `perceival/openwrt-flint3:flint3-be9300`.
  Its GitHub head matched the base at review time.
- The replacement is currently uncommitted. Six candidate files were untracked
  before this plan was added, including the main chip source and register map.
- The tracked diff contains 188 files, 249 added lines and 168,567 deleted lines.
  These figures exclude the untracked sources and documents.
- Twenty modified patch files outside the driver and BE9300 DTS contain earlier
  metadata cleanup. Keep those changes out of the DSA replacement PR.
- The target selects Linux 6.18; the current pinned version is 6.18.39.
- No successful package build, image build or BE9300 hardware run is recorded.

**Recommendation:** repair the source defects and obtain a reproducible P0
build before opening a Draft PR. Keep the existing shipping driver on the
maintainer's branch until the replacement passes the agreed hardware and
feature-parity gates. An early Draft can be useful for architecture/provenance
review, but is not a proposal to merge the current functional reduction.

The official OpenWrt driver/package submission and the BE9300 board submission
remain separate. [OpenWrt #23161](https://github.com/openwrt/openwrt/pull/23161)
was still OPEN and unmerged at review time. It remains the agreed gate for the
final official IPQ53xx/BE9300 integration; local source fixes and candidate
builds can proceed against this fork before that gate is resolved.

## 2. Findings

Line references below describe the reviewed source before corrective edits.
Severity distinguishes a confirmed source defect from a hardware or provenance
question. None of the hardware risks below is claimed to have been reproduced.

| ID | Priority and evidence | Finding | Required disposition |
| --- | --- | --- | --- |
| R01 | P0; confirmed source-level build blocker | `src/rtl837x_common.c:23,33` uses `FIELD_GET(mask, ...)` and `FIELD_PREP(mask, ...)` inside externally defined functions with runtime masks. Linux bitfield macros require constant masks. | Replace the helpers with a documented runtime contiguous-field implementation, or move constant field operations to call sites. Separate field values from raw bitmaps. Compile the resulting objects with the target kernel. |
| R02 | P0; confirmed first-port failure | `src/rtl8372n.c:472` passes `port_bitmap == 0` when the first port joins an empty bridge. The write helper rejects zero masks at `src/rtl837x_common.c:30`, returning `-EINVAL`. | Make an empty isolation update a successful no-op at the bitmap API/call site. Test first join, second join, leave, rejoin and rollback. This finding matters if hardware bridge join is retained for P0. |
| R03 | P0; confirmed API misuse | Isolation masks can be sparse; setup also combines the disjoint port-3 and port-8 MAC-type fields at `src/rtl8372n.c:688`. They are passed through an API intended to prepare one field. | Use raw `regmap_update_bits()` semantics for set/clear bitmaps and separate writes for independent fields. Do not merely replace the macros while leaving their mixed contracts unchanged. |
| R04 | P0; confirmed missing transaction protection | `src/rtl837x_common.c:135-192` stages PHY commands through several regmap operations without a PHY-engine mutex. Per-register transport locks do not serialize the complete command against direct port-enable/disable operations. | Serialize staging, execution, polling and readback with one PHY-engine lock; verify idle before staging and error/timeout behavior. Document lock order. |
| R05 | P0; confirmed incomplete offload contract | Bridge join sets `tx_fwd_offload = true` at `src/rtl8372n.c:478`; setup enables learning, while switch ops have no STP, bridge-flags, VLAN, FDB or MDB callbacks. | For P0, prefer explicit standalone/software bridging with safe CPU-only isolation and audited trap/learning behavior. Otherwise implement and verify the required minimum hardware bridge controls. Clearing the TX flag alone is insufficient. |
| R06 | P0; confirmed packaging omission | `src/rtl8372n.c:134,155` uses MDIO devres helpers. The package declares DSA/regmap dependencies but no `kmod-mdio-devres` dependency, unlike other 6.18 DSA packages. | Add the appropriate dependency for the supported kernel; verify modpost, package dependency metadata and loading on a minimal image without accidental transitive dependencies. |
| R07 | P0; confirmed failure-state weakness | PCS state reads return early after partial updates at `src/rtl8372n.c:190-217`, potentially leaving link marked up when a later read fails. | Initialize a fail-closed state and commit a complete snapshot only after successful reads; retain link-down/unknown speed on failure. |
| R08 | P0; confirmed reset implementation mismatch, hardware impact pending | `src/rtl837x_mdio.c:161` assigns the normal post-polling writer to `write_reg_noack`. Software reset calls it before the reset settle delay. | Establish reset-safe transaction semantics from documented sources. Implement a dedicated path if required; do not silently ignore unrelated transport errors. Validate cold and warm reset. |
| R09 | P0; PHY support not established | The candidate provides PHY transport and power control, but no dedicated PHY driver. Advertising `MAC_2500FD` does not prove PHY advertisement, autonegotiation or 2.5G status decoding. | Check actual PHY IDs and binding against the existing Linux Realtek Internal NBASE-T driver before adding custom code. Audit page/MMD/OCP semantics and implement a narrowly scoped clean PHY driver only if needed. |
| R10 | P0; SerDes behavior unverified | Mode selection and polarity writes replace a much larger initialization path. Removing patch data does not prove a working 10G CPU link after reset. | Document each required initialization operation and its source. Validate physical PCS state and bidirectional traffic; a fixed-link carrier indication alone is insufficient. |
| R11 | P0; confirmed late/incomplete topology validation | Reset occurs before cascade validation; the single-CPU check occurs at the end of setup. Port capabilities treat every non-SerDes port as an internal PHY, including ports outside 4-7. | Validate one supported CPU port, user-port range and interface modes before the setup software reset; audit whether probe GPIO reset also needs earlier DT validation. Reject unsupported layouts and restrict PHY address/mask operations. |
| R12 | P0; confirmed API boundary defect | `src/rtl8372n.c:675,681` accesses removed `phylink_pcs.neg_mode` for exactly 6.18.0 due to an inclusive comparison. It is excluded on the target 6.18.39. | Prefer one explicit supported API for 6.18.39. If retaining compatibility code, fix and build each claimed version, including the separate 6.12.44 bus-field boundary. |
| R13 | P0; provenance documentation incomplete | `PROVENANCE.md` lists repository revisions and labels sources independent, but does not establish a symbol-level lineage. A public GPL/MIT repository and matching register values alone do not establish independent origin. | Build a feature/symbol/source/license/provenance/evidence matrix; distinguish adapted architecture, public register facts, reused expressions and unresolved vendor-derived data. Resolve every carried block before calling the candidate provenance-clean. |
| R14 | P0; confirmed review/scope gap | New implementation files are untracked; unrelated metadata edits are mixed into the worktree, and the replacement removes the SDK plus many callbacks at once. | Prepare an explicit PR file list and coherent commits, include all new sources, exclude unrelated cleanup, and document removed behavior. Preserve the separate local cleanup changes. |
| R15 | P1; board compatibility unverified | The legacy `realtek,rtl837x` match also applies to BE6500, whose DTS still carries legacy mode/GPIO declarations. The BE9300 conduit has both a switch `phy-handle` and a fixed-link. | Establish a board support matrix. Check the conduit binding and schema. Coordinate DTS/binding changes with the board author; do not silently claim a generic RTL837x replacement. |
| R16 | P1; documentation/test coverage gap | The first-test guide assumes a bridge path that currently fails, searches only `.ipk` although APK is the default, and relies on host bridge output for some observations. | Update the guide after choosing P0 semantics; record actual package format, full logs, PHY binding, register evidence and exact build inputs. Distinguish software configuration from hardware programming. |

### Primary API evidence checked through CLI

- [Linux v6.18 bitfield macros](https://github.com/torvalds/linux/blob/v6.18/include/linux/bitfield.h): constant/nonzero mask checks and contiguous-field requirements.
- [Linux v6.18.39 phylink header](https://github.com/gregkh/linux/blob/v6.18.39/include/linux/phylink.h): PCS callback signature and removed `neg_mode` member.
- [Linux v6.18.39 MDIO devres](https://github.com/gregkh/linux/blob/v6.18.39/drivers/net/phy/mdio_devres.c): the helpers used by this driver are exported from that module.
- [Linux v6.18 DSA port handling](https://github.com/torvalds/linux/blob/v6.18/net/dsa/port.c): bridge offload synchronization and STP/bridge-flags contracts.
- [Linux v6.18 RTL8_4 tagger](https://github.com/torvalds/linux/blob/v6.18/net/dsa/tag_rtl8_4.c): directed CPU transmission and receive forwarding marks. Software fallback must account for both.
- [Linux v6.18 Realtek PHY driver](https://github.com/torvalds/linux/blob/v6.18/drivers/net/phy/realtek/realtek_main.c): the Internal NBASE-T driver matches selected C22 IDs plus capability predicates, not every RTL8372N PHY automatically.

## 3. Ordered implementation plan

### A. Freeze scope, behavior and provenance

1. Keep a complete backup/patch of the worktree, including untracked files.
2. Record the shipping baseline commit, target branch, config and source pins.
3. Compare old/new callback sets and mark each feature retained, replaced,
   deliberately deferred or still unknown. Include both tag protocols, MTU,
   mirroring, bridge flags, statistics, EEE/GPIO and lifecycle behavior.
4. Prepare a register/operation ledger with these columns:
   `feature -> symbol/value -> source revision/file/location -> derivation ->
   license/attribution -> hardware evidence -> unresolved question`.
5. Cover transport, chip ID/reset, PHY C22/C45/OCP, PHY power values, SDS
   command layout/mode/polarity, VLAN tables/PVID, isolation, learning and CPU
   tagging individually. A match across two SDK-derived implementations does
   not independently establish origin.
6. Record [Airjinkela's lineage comment](https://github.com/RuijieNetworksCommunity/rtl837x-gsw-driver/issues/2#issuecomment-5946313878)
   and the outstanding [JiaY-shi source question](https://github.com/JiaY-shi/rtl83xx/issues/1).
   Seek clarification only for blocks we actually need to retain.
7. Keep restricted headers, generated SDK lists and PHY/SerDes patch payloads
   outside the candidate. If a required operation lacks a documented source,
   mark the dependency unresolved instead of relabeling it.
8. Preserve required author attribution and any applicable notice for reused
   MIT material in an appropriate source/notice file. This task does not
   change the existing package `LICENSE` file.

**Completion evidence:** a complete callback comparison and provenance ledger,
with every P0 block traceable or explicitly excluded. Replace unsupported
claims of independent origin in the existing manifest.

### B. Repair the P0 access and control path

1. Resolve R01/R03 first. Define separate APIs for an unshifted contiguous
   field value and a raw set/clear bitmap. Validate masks and value range;
   document whether zero bitmaps are allowed. Audit every caller.
2. Add a PHY-engine mutex, initialized in probe. Keep the whole transaction
   protected across idle check, staging, command, completion and result.
   Define ordering between child MII-bus lock, engine lock, regmap lock and
   parent MII-bus lock; do not recursively acquire the parent lock.
3. Restrict PHY operations to supported ports 4-7. Validate selection masks,
   MMD and register ranges; ensure failed reads never return stale data.
4. Review timeout budgets, stale command status and hardware autopolling
   interactions. Keep PHY and SDS transaction errors visible to callers.
5. Separate reset from ordinary writes and document the post-reset readiness
   sequence. Avoid an unconditional reset error suppression workaround.
6. Validate topology before setup reset/programming. Restrict P0 to one CPU
   on a supported SerDes MAC, internal user ports 4-7 and no cascade. The
   initial BE9300 test topology remains CPU 3 and user ports 4-7.
7. Fix PCS error handling; remove unused variables and unnecessary exports.
8. Audit every error exit after PHY-bus registration, teardown, shutdown and
   repeated bind/unbind for resources and safe hardware state. Review the
   process-wide MII-bus ID counter and replace it with a collision-safe ID.
9. Align phylink and DSA API usage with the exact supported target. Do not
   advertise 6.12 compatibility merely because preprocessor branches exist.

**Completion evidence:** source review of the corrected helpers, caller audit,
lock-order description, failure-path review and a successful target build.

### C. Make the P0 forwarding scope safe and explicit

Recommended first-test mode: standalone DSA user ports with isolated
user-to-CPU forwarding. Establish software bridge fallback before promising
hardware bridge support.

1. Ensure hardware cannot bypass software STP or VLAN decisions in fallback
   mode. Audit learning, flood/trap behavior and the RTL8_4 forwarding mark;
   select a correctly supported fallback contract.
2. Disable unsupported bridge offload explicitly and check the DSA core's
   software fallback on the target kernel. Do not rely on removing only
   `tx_fwd_offload = true`.
3. If hardware bridge join remains implemented, fix R02 and rollback and
   implement the minimum required STP, BPDU trap, learning and bridge-flags
   behavior before allowing the associated bridge mode.
4. Reject or deliberately support VLAN-aware requests according to the
   chosen fallback. Hardware VLAN 1 bootstrap is not general VLAN offload.
5. Confirm CPU tag format/placement and port identity against the existing
   RTL8_4 tagger. Verify no duplicate forwarding or incorrect offload marks.
6. Test port isolation before bridge creation and after bridge destruction;
   include disabled/unconfigured ports, unknown unicast, broadcast and
   multicast. A single successful ping is insufficient.

**Completion evidence:** documented semantics, host-side regression checks and
bench isolation/forwarding evidence before treating that mode as supported.
Hardware checks can occur after Draft publication, but the intended contract
and its source implementation must be coherent before the Draft.

### D. Complete PHY and SerDes plumbing from documented sources

1. Record C22 PHY IDs and available C45 IDs/capabilities for ports 4-7;
   identify which driver would bind and what it needs to probe successfully.
2. First evaluate the kernel Realtek Internal NBASE-T support. Its page and
   MMD callbacks need scrutiny: providing native bus C45 callbacks alone does
   not prove that a C22 PHY's driver will use them as intended.
3. Check C22 page selection, OCP mapping, autonegotiation advertisement,
   restart, status, pause and 2.5G decoding. If existing kernel support cannot
   operate through this transport, implement a small chip-specific PHY layer
   using verified public sources, with constrained matching and safe lifetime.
   The ZTE implementation is a comparison source, not an automatic import.
4. Define the post-reset initialization order: chip readiness, PHY access,
   PHY power policy, SDS mode/polarity and CPU-tag/forwarding setup. Identify
   operations that depend on reset defaults or bootloader state.
5. Check whether mode changes overwrite earlier polarity writes. Compare
   readbacks before/after phylink configuration rather than assuming order.
6. Capture physical switch/SoC PCS state and traffic after cold boot. A DT
   fixed-link reporting 10,000 Mb/s does not prove the physical CPU link works.
7. If operation without vendor patch data fails, obtain narrowly scoped
   before/after readbacks and public documentation. Do not import patch arrays
   merely to make the smoke test pass.

**Completion evidence:** justified PHY binding and source-complete minimum
initialization; hardware bring-up determines whether further clean work is
required. Stable 2.5G and 10G operation remain unverified until tested.

### E. Package, bindings and reproducible build

1. Add MDIO devres dependency and determine whether the selected PHY path
   requires `kmod-phy-realtek` explicitly. Its presence in `configs/ap.config`
   is not sufficient package dependency documentation.
2. Bump package release/version for the changed implementation and module
   name. Verify FILES, autoload, Kconfig and the tagger's ownership/dependencies.
3. Build a minimal dependency configuration as well as `configs/ap.config`;
   this catches dependencies hidden by the large normal image.
4. Document exactly supported chips/boards. Audit BE6500 before keeping a
   generic compatibility claim. Coordinate explicit compatible/binding changes
   and custom polarity property definitions with the board PR.
5. Check BE9300 conduit `phy-handle` plus fixed-link against phylink/PPE
   handling. Do not change it based on appearance alone; use binding and driver
   evidence and validate the resulting DTS.
6. Use a supported Linux host/toolchain, pin feed revisions, preserve the
   generated `.config` and build source revision. Compile the package, run
   modpost, build the DTB and full GL-BE9300 image, and verify installed modules.
7. Run available `checkpatch` and whitespace checks. Classify warnings and
   repair API, signedness, format, unused-variable and lock issues.
8. Keep logs, package dependency output and image checksums with results.
   If no supported host is available, report the build gate pending.

**Completion evidence:** reproducible package and image artifacts for the
recorded revision, correct dependencies and no unresolved build/link errors.

### F. Update docs and assemble the Draft PR

1. Revise [README.md](README.md), [PROVENANCE.md](PROVENANCE.md) and
   [FIRST-HARDWARE-TEST.md](FIRST-HARDWARE-TEST.md) to reflect the corrected
   implementation and chosen bridge mode.
2. Fix APK/IPK discovery based on the actual generated configuration. Preserve
   complete boot/dmesg logs alongside filtered summaries.
3. Add PHY IDs, bound driver, supported/advertised/partner link modes, carrier
   transitions, PCS readbacks and tag/port identity to the report template.
4. State that `bridge vlan show` describes host configuration; require switch
   readbacks or behavioral isolation tests for hardware programming claims.
5. Include first-port bridge join and rollback, simultaneous PHY operations,
   failed command recovery and warm/cold reset cases. Mark the existing
   `tests/bridge-flags.sh` as a later parity test until its callbacks exist.
6. Prepare coherent commits: source/map and transport; chip/PHY/PCS and safe
   forwarding; packaging; provenance/test documentation. Keep board changes
   as a separate review unit, coordinated with the board author.
7. Include the new files explicitly. Use a file allowlist; exclude the twenty
   unrelated patch-metadata edits while preserving that local cleanup.
8. Review the complete prospective PR diff against the target branch, not
   just `git diff` of tracked worktree files. Ensure restricted data is absent
   from every introduced commit as well as the final snapshot.
9. Use a branch name without the prohibited prefix and ordinary technical
   commit messages. Preserve required copyright and author attributions.
10. The description should explain the old-source problem, link #99/#100 and
    Airjinkela's source/deprecation comments, identify the exact source pins,
    list retained/deferred features and attach real build evidence. Include
    Signed-off-by only where the contributor has actually provided it.

**Draft publication gate:** A-F source/documentation work is complete, P0 has
built reproducibly, and unresolved hardware validation is clearly described.
Do not claim working hardware or full replacement parity at this point.

## 4. Hardware and parity gates after the baseline builds

| Stage | Work | Acceptance evidence |
| --- | --- | --- |
| P0 hardware | Probe/reset, PHY IDs/binding, CPU 3 10GBASE-R, all four LAN jacks, native tags, isolated and chosen fallback traffic, link transitions, cold/warm boot and failure recovery | Exact image/config, complete logs and readbacks; bidirectional traffic with correct port identity; no unexpected port leakage, stale state or reset dependence |
| P1 switching | Hardware bridge flags/learning/flooding, VLAN filtering/add/delete/PVID, FDB/MDB, STP and BPDU handling, fast age and rollback | Tagged/untagged traffic matrix, cross-VLAN negative tests, FDB/MDB readback, controlled STP loop/BPDU test and bridge lifecycle results |
| P2 #47 | LAG join/leave/change, member state, hash selection, failover/recovery and link-up/TX-disabled standby | Current-revision LACP/member evidence; standby receives no prohibited transmission; regression traffic and throughput results |
| P2 #49 | Meter units/range, allocation, setup/reset/delete and failed cleanup | Actual measured rates, register evidence and restored baseline after deletion/failure |
| Remaining baseline interfaces | MTU/jumbo, both prior tag modes or an agreed narrower replacement, mirroring, MIB/ethtool, EEE, GPIO/LED consumers and diagnostics | Callback comparison with restored behavior or explicit maintainer-approved deferral; board regressions covered |

[Issue #100](https://github.com/perceival/openwrt-flint3/issues/100) currently
requires feature parity or explicitly agreed deferral. P0 must not silently
redefine that acceptance criterion. Record the P0/P1/P2 split with the maintainer
before proposing a merge with deferred features.

The maintainer's [bench scheduling note](https://github.com/perceival/openwrt-flint3/issues/99#issuecomment-5945585992)
expects BE9300 availability around 2026-10-09; that is an estimate, not a passed
test or guaranteed date.

## 5. What can be completed without the router

**Can proceed now:** helper/API fixes, transaction locks and error handling,
topology validation, package dependencies, source/provenance ledger, callback
inventory, DTS/binding review, documentation, focused host-side regression
checks and build execution on an available supported Linux host.

**Needs hardware or exact readbacks:** reliable post-reset initialization,
PHY-driver binding, 2.5G negotiation, physical 10G CPU link, polarity behavior,
RTL8_4 forwarding semantics, port/VLAN isolation, STP/BPDU behavior, LAG member
selection and policer units/cleanup behavior.

**Needs maintainer/source-owner input:** unresolved source lineage,
contributor sign-offs, accepted feature deferrals, board scope and the final
OpenWrt dependency/base decision.

There is no defensible single percentage for readiness yet. Track concrete
completed gates: source review, source fixes, provenance, build, P0 hardware,
P1 parity, P2 features and submission scope. The current candidate has passed
source inspection only, and that inspection found blocking defects.

## 6. Definition of ready for replacement / official submission

- All P0 source defects are fixed and the exact revision builds reproducibly.
- Every retained code/data block has recorded provenance and attribution;
  excluded restricted data is absent from introduced candidate commits.
- BE9300 P0 hardware tests pass, including reset and PHY/CPU-link behavior.
- P1/P2 and other baseline interfaces pass, or the maintainer explicitly agrees
  and records their deferral with a safe supported scope.
- Board compatibility and recovery/regression behavior are documented.
- The PR contains only the intended driver/package and coordinated board work,
  with build/test evidence and honest limitations.
- For the official BE9300 integration, #23161 has the agreed disposition and
  the final candidate is built and tested against the accepted base.
