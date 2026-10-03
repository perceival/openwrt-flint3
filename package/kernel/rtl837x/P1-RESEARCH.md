# RTL8372N P1 feasibility and implementation plan

Updated: 2026-10-03. Applies to the P0 candidate in [PR #104](https://github.com/perceival/openwrt-flint3/pull/104), with the replacement requirements tracked in [Issue #100](https://github.com/perceival/openwrt-flint3/issues/100).

## 1. Conclusion and current boundary

There is enough public material to design a substantial part of P1: serialized
VLAN/L2 table transactions, VLAN membership/PVID operations, learning/flood
controls, CIST port states and a static multicast entry encoder. This does not
establish BE9300 operation, resolve all table semantics or settle source lineage.
No P1 runtime callbacks are added by this documentation revision.

Keep the agreed order: **P0 build and hardware bring-up → P1 switching parity →
P2 LAG/rate limiting**. The maintainer's [scheduling and order comment](https://github.com/perceival/openwrt-flint3/issues/99#issuecomment-5945585992)
allows source/build work while the board is unavailable and estimates bench
availability around 2026-10-09. That date is not a hardware result or commitment.

Source preparation can proceed now. Enabling hardware bridge offload in the
candidate requires the P0 bench gates plus the control-frame, database and
failure-handling requirements below. Merely adding a successful
`port_bridge_join` callback would change DSA forwarding semantics before those
requirements are met.

## 2. Pinned research inputs

These are research references; they are not a list of newly imported code.
[PROVENANCE.md](PROVENANCE.md) remains the ledger of the implemented P0 inputs.
Public repository licenses and agreement between sources do not, by themselves,
prove independent origin. Air/ZTE lineage remains subject to owner review.

| Reference | Declared license / role | Specific material |
| --- | --- | --- |
| [RTLPlayground](https://github.com/logicog/RTLPlayground/tree/f0aea3dcac056e3274fd39e1c76a7117471c37da) at `f0aea3dcac056e3274fd39e1c76a7117471c37da` | MIT repository notice; public register/operation reference | `rtl837x_regs.h`; `rtl837x_port.c` functions `vlan_get/create/delete`, `port_pvid_get/set`, `port_l2_forget_port`, `port_l2_learned`, `port_l2mc_set`; `doc/vlan.md`, `doc/l2.md`, `doc/stp.md`; `rtl837x_stp.c` |
| [Public ZTE RTL8372N implementation](https://github.com/cnjn/linux-mainline-zte-zxslc-sr1010/blob/07f8687248578d4be6931c665ff5d08bb6cc3d9d/drivers/net/ethernet/zte/zx279133-rtl8372n.c) at `07f8687248578d4be6931c665ff5d08bb6cc3d9d` | GPL-2.0 source metadata; comparison only, lineage unresolved | `rtl8372n_l2_encode_key/write_words/lookup/clear/next_uc`; VLAN, bridge-flags, CIST, FDB/MDB and dynamic-flush callbacks |
| [Airjinkela DSA](https://github.com/airjinkela/rtl837x-dsa-driver/tree/7f3b64c7f3a37db1866fd80ba815d89974d995fd) at `7f3b64c7f3a37db1866fd80ba815d89974d995fd` | GPL source declarations with the already documented SDK lineage; comparison only | `src/rtl837x.h::rtl837x_vlan_data`; `src/rtl837x_common.c::rtl837x_vlan_get/set`; generated restricted header is not an input to the replacement |
| [Linux v6.18 DSA contract](https://github.com/torvalds/linux/blob/v6.18/Documentation/networking/dsa/dsa.rst) and [RTL8_4 tagger](https://github.com/torvalds/linux/blob/v6.18/net/dsa/tag_rtl8_4.c) | Upstream API/behavior reference | Address databases; bridge join/leave; VLAN/CPU membership; learning/flood/STP/fast-age callbacks; forwarded versus trapped RX tags |

RTLPlayground describes embedded-8051 switches, including tests on SWTGW218AS.
The ZTE implementation uses an external CPU on port 8 and a private S-VLAN/NPPT
transport. Neither is a BE9300 test. Flint 3 uses external CPU port **3**, native
RTL8_4 tags and user ports **4–7**. Do not import port 9 MCU assumptions, ZTE
transport VLAN reservations, board control or vendor PHY/SerDes arrays.

## 3. What can be prepared and what is still missing

All rows have **no BE9300 hardware verification**. “Can prepare” means source
work is feasible, not that a feature is implemented or ready to advertise.

| P1 component | Public basis | Can prepare without the board | Remaining requirement before enabling support |
| --- | --- | --- | --- |
| Shared VLAN/L2 table engine | Control `0x5cac`; status/method `0x5cb0`; write words `0x5cb8/0x5cbc/0x5cc0`; read words `0x5ccc/0x5cd0/0x5cd4`; VLAN selector `3`, L2 selector `4` in RTLPlayground code | Common transaction lock, bounded idle/execute polling, checked staging/readback and explicit error returns | Validate timeout budgets, selector/method restoration and readback on RTL8372N; source-map each retained field |
| VLAN/PVID | Membership bits 9:0, untag bits 19:10; PVID base `0x4e1c` with two 12-bit port fields; admission `0x4e10`; filter `0x4e18` | Read/modify/write design; port/VID bounds; tagged/untagged/PVID lifecycle and deletion rollback | Resolve bit 25 interpretation; prove no-PVID rejection, filtering-on/off behavior and CPU VLAN/tag semantics |
| Bridge join/leave and isolation | Isolation `0x50c0 + port * 4`; Linux DSA database contract | Per-bridge port matrix, standalone CPU-only restore, explicit unsupported-domain fallback | Prove database separation and tagger forwarding marks; initially propose one hardware bridge until additional domains are established |
| Learning and flood flags | Learning limit `0x5384 + port * 4`; flood masks `0x5360`–`0x5370` | Checked `BR_LEARNING`, `BR_FLOOD`, `BR_MCAST_FLOOD`, `BR_BCAST_FLOOD`; isolation policy | Confirm zero-limit behavior, limit-exceeded action, CPU delivery and combined flood/VLAN/isolation effects; unsupported flags must fail explicitly |
| Dynamic FDB flush | `0x53d4` command / `0x53dc` mode in RTLPlayground; additional busy/mode fields in ZTE | Bounded per-port flush design and static-entry preservation policy | Establish completion/error behavior and dynamic-only semantics; static MDB/BPDU entries must survive |
| FDB add/delete/dump | MAC/VID/port/static/age layout and next-entry iteration; ZTE lookup/hit/delete example | Checked codecs, finite iteration, CPU/local-address and `dsa_db` design | Resolve IVL versus validity, lookup-hit/capacity/delete semantics and VID 0 versus nonzero databases; no fabricated success on unsupported CPU entries |
| MDB add/delete | RTLPlayground `port_l2mc_set` describes MAC + VID + IVL and a 10-bit port mask | Codec and serialized membership/refcount design including CPU subscriptions | Confirm lookup/delete/full-table behavior, lifecycle across VLAN changes and control-entry ownership |
| CIST port states | `0x5310`, two bits per port; hardware states disabled=0, blocking=1, learning=2, forwarding=3 in RTLPlayground | Linux DISABLED/BLOCKING/LISTENING/LEARNING/FORWARDING mapping and readback plan | Verify control-frame exceptions, CPU port state and fast-age transitions; a state-register write alone is not STP support |
| BPDU CPU delivery | RTLPlayground documents reserved multicast plus a per-VLAN static-L2MC workaround | Two explicit implementation options and non-loop capture procedure | Preferred external-CPU trap needs suitably sourced action fields and measured RTL8_4 trap reason; L2MC alternative needs priority/admission/STP/capacity proof on BE9300 |

### Source inconsistencies that must not become API assumptions

1. **VLAN selector:** RTLPlayground `doc/vlan.md` says `0x02`, while its
   `rtl837x_regs.h`, `vlan_get/create` code and `doc/l2.md` use **`0x03`**.
   P0 uses `0x03`. The conflicting prose is not a reason to change it.
2. **VLAN bit 25:** RTLPlayground prose calls it a validity indicator.
   Air's `rtl837x_vlan_data` calls it `ivl_en`; ZTE calls it `VLAN_IVL`.
   P0 sets that bit through `RTL837X_VLAN_DATA_VALID`, but that name must not
   be used to infer general entry validity. Resolve the interpretation, then
   rename/document the field before building general VLAN APIs around it.
3. **L2 word B bit 29:** RTLPlayground's older dump explanation calls it
   valid/stale; its multicast encoder and ZTE's FDB decoder identify it as
   **IVL**. Do not discard entries or terminate a dump based solely on that bit.
   Establish hit/empty/end-of-table rules and SVL/IVL behavior separately.

These are meaningful semantic conflicts, even where the existing bootstrap
writes happen to match. Unicast static-entry flags, aging units and full-table
status are also not justified by a matching register address alone.

## 4. DSA requirements to apply in the implementation

### Transactions and failures

- Add a shared table-engine mutex around the entire idle → method/staging →
  execute → completion → status/readback sequence. A per-register regmap lock
  cannot protect a multi-register transaction.
- Define lock order as bridge/VLAN state lock (when needed) → table lock →
  regmap/map lock → parent MDIO lock. Do not enter regmap while already holding
  its map lock or manually hold the parent MDIO lock around regmap calls.
  Keep table transactions out of PHY/SDS engine critical sections.
- Serialize associated software membership updates as well as hardware writes;
  commit cached state only after successful completion. Specify rollback or
  fail-closed port isolation after partial multi-register updates. If rollback
  fails, report that separately and do not leave an optimistic software state.
- Restore temporary L2 method/clear controls even on error. Keep iteration
  finite and detect non-progress/wrap; return timeout/capacity/lookup errors
  according to the established hardware contract.
- Void callbacks such as STP state/bridge leave/fast-age need logged failures
  and an isolation/recovery policy; they cannot return success by omission.

### VLAN, databases and CPU path

- Keep VLAN-table member and untag masks within usable ports; untag must be a
  subset of membership. Preserve unrelated fields when updating entries.
- Follow DSA shared-port VLAN notifications/refcounts. Do not copy the MCU
  firmware policy of automatically adding CPU port 9 to every VLAN. Any
  additional CPU membership needed for this hardware must be explicit and
  tested, including bridge `self` and foreign/software ports.
- Filtering enabled with no PVID must reject ordinary untagged data; filtering
  disabled must preserve VLAN-unaware bridge semantics, including tagged
  traffic. A fixed VID 1/PVID 1 assignment is not sufficient for both modes.
- Define `DSA_DB_PORT` and `DSA_DB_BRIDGE`, VID 0 and nonzero VID handling.
  A single global MAC/VID key does not establish separation between two
  bridges using the same MAC/VID. Initially propose one offloaded bridge
  (`max_num_bridges = 1`); keep unsupported bridges in the DSA standalone
  CPU-only configuration. Do not declare `fdb_isolation` until implemented.
- Handle CPU/local FDB and MDB entries or document a deliberate CPU-flood
  strategy consistent with the DSA configuration. Do not copy an unconditional
  successful return for a CPU FDB request from the ZTE comparison source.
- Keep TX forwarding offload disabled until its separate tagger/domain
  contract is proved. RX RTL8_4 marks depend on the reason field: upstream
  treats the trap reason differently from already-forwarded traffic.

### STP and control frames

- Linux bridge owns the STP state machine. Implement CIST state enforcement
  and dynamic fast-age, not the firmware's separate STP daemon.
- Preferred path: a properly sourced reserved-multicast **trap to external
  CPU port 3**, with readback and captured tag reason. The existing proposed
  [Airjinkela BPDU PR #2](https://github.com/airjinkela/rtl837x-dsa-driver/pull/2)
  uses that project's generated-header definitions. It is an unverified
  proposal, not a provenance-cleared implementation to transplant into #104.
- Alternative: static L2 multicast membership limited to CPU port 3 for each
  applicable VLAN/PVID. RTLPlayground uses this because its embedded MCU is
  not the external trap destination. That limitation does not establish that
  BE9300 needs the same workaround.
- If evaluating the alternative, prove lookup priority relative to reserved
  multicast, delivery from BLOCKING/LISTENING ports, tag/admission handling,
  BPDU visibility to the Linux bridge and the CPU forwarding mark. Maintain
  entries across VLAN/PVID changes and refcount them separately from MDB
  subscriptions; preserve them on dynamic flush and handle capacity errors.
- Do not copy the firmware's STP-off policy of flooding reserved BPDUs to all
  ports. Verify Linux bridge policy and control-frame behavior independently.
  Confirm neighboring reserved groups are unaffected by a BPDU-specific action.

## 5. Proposed implementation sequence

This is a follow-up series, not a claim that P1 is present in the P0 Draft.
Source design can advance now; runtime changes should be reviewed/built and
bench-qualified in the stated dependency order.

| Step | Deliverable | Required gate |
| --- | --- | --- |
| P1-A | Narrow register ledger, corrected field meanings, common serialized VLAN/L2 transaction helpers, checked codecs | Explain the three conflicts above; record provenance for every new field; build the exact revision; test P0 again after bootstrap refactoring |
| P1-B | BPDU delivery decision and per-port CIST/learning/dynamic-flush primitives | P0 CPU/PHY/tag/isolation passes; demonstrate CPU-only BPDU reception without a physical loop; dynamic flush preserves static entries |
| P1-C | One-bridge hardware forwarding plus VLAN add/delete/filter/PVID and supported bridge flags | B is proved; tagged/untagged/no-PVID and filtering changes pass; leave/failure restores CPU-only isolation; database scope is explicit |
| P1-D | FDB add/delete/dump and MDB membership, including CPU/local entries | Lookup hit, empty/end, deletion, capacity and VID/database semantics established; shared-entry lifecycle and rollback verified |
| P1-E | Integrated bridge/VLAN/STP regression report and agreed remaining deferrals | Isolated loop test only after non-loop BPDU gate; all bench cases below recorded for the exact revision |
| P2 | LAG #47 and rate limiting #49 | P1 parity passes, or narrower scope is explicitly agreed by the maintainer |

CIST and FDB flush appear before enabling bridge offload because bridge state
changes must already have enforceable forwarding/learning behavior. General
FDB/MDB management follows the shared engine and database design; do not
expand P0 while those semantics are still guesses. MST instances, multiple
hardware bridge domains and advanced offloads are separate extensions.

## 6. Acceptance matrix for a future P1 revision

This matrix plans future tests. **Every row is currently NOT RUN for P1.**
Use [FIRST-HARDWARE-TEST.md](FIRST-HARDWARE-TEST.md) for P0 first; successful
shipping-SDK results do not validate the new driver.

| ID | Test | Required evidence / result |
| --- | --- | --- |
| P1-0 | Exact-source target build and P0 regression | Source SHA, image/module checksums, config/feeds; P0 T0–T8 report, no new MDIO/lock/PHY/CPU-link failures |
| P1-1 | Bridge lifecycle and unsupported second bridge | Join/leave/rejoin/destroy; standalone ports deliver only to CPU; rejected offload keeps functional software fallback and no cross-bridge leakage |
| P1-2 | Tagged/untagged VLANs, PVID and admission | Two VIDs, access and trunk ports, bridge `self`; captures show correct tag preservation/removal, CPU reachability and no cross-VLAN traffic; no-PVID ordinary untagged frames rejected |
| P1-3 | VLAN filtering changes and deletion | Enable/disable, remove PVID/last member, leave/rejoin; preserve VLAN-unaware tagged traffic where supported; no stale member/PVID or unexplained management loss |
| P1-4 | Learning/flood/isolation flags | Each supported flag on/off and combinations; unknown UC, non-IP MC, IPv4/IPv6 MC and broadcast destinations verified; standalone/CPU behavior remains correct |
| P1-5 | FDB identity and lifecycle | Static add/replace/delete, dynamic learning/dump, same MAC on distinct VIDs, VID 0 handling, CPU/local addresses and foreign bridge ports; bounded dump and correct database interpretation |
| P1-6 | MDB and capacity | Shared group add/remove on two ports plus CPU subscription; last removal, VLAN changes and dynamic flush; no clobbering of protocol entries; full-table behavior reported accurately |
| P1-7 | BPDU path without a loop | Inject/capture standard BPDUs from FORWARDING and BLOCKING/LISTENING user ports, with VLAN/PVID variations; CPU receives, other LAN jacks do not relay ingress BPDUs; capture RTL8_4 reason where possible |
| P1-8 | CIST transitions and isolated loop | State readbacks, learning/fast-age transitions, bounded reconvergence and no ingress-BPDU relay/resource growth; first prove P1-7 and use an independent recovery path |
| P1-9 | Concurrency, failures and reset | Concurrent table operations/PHY reads, timeout/partial-update evidence where practical, warm/cold restart; no mixed transactions or falsely committed state; repeat forwarding negatives |

Record PASS / FAIL / BLOCKED / NOT RUN individually. Include topology, silicon
revision, port identities, raw readbacks and packet captures. If a suitable
readback method, controllable peer or fault-injection mechanism is unavailable,
mark the corresponding case BLOCKED; do not infer a pass from register writes.

## 7. Open decisions and evidence requests

- **Maintainer:** confirm the proposed initial one-bridge scope and whether
  multi-bridge/MST behavior is required for the first P1 submission. Confirm
  any deferral under #100; existing replacement acceptance remains unchanged.
- **Hardware owner:** first provide the P0 report; then BPDU captures and
  readbacks for table control/status, VLAN 1 plus a test VID, PVID/admission,
  isolation/learning/flood and CIST before/after controlled changes. Use an
  agreed read-only method; no unreviewed register-write script is prescribed.
- **Source owners:** clarify the VLAN/L2 field conflicts, reserved-multicast
  action fields and table hit/clear/capacity semantics with source locations
  and redistribution lineage. [JiaY-shi's source question](https://github.com/JiaY-shi/rtl83xx/issues/1)
  remains unanswered at this check; no permission or sign-off is inferred.
- **Integration:** keep the full OpenWrt build, retained SDS/source review and
  [OpenWrt #23161](https://github.com/openwrt/openwrt/pull/23161) disposition as
  separate gates. P1 research does not remove them.
