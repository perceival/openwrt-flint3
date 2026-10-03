# RTL8372N DSA driver candidate

This package contains an early, RTL8372N-only DSA bring-up candidate for
OpenWrt, with the limited P0 scope described below. See
[PROVENANCE.md](PROVENANCE.md) for source and register origins. The detailed
[first-hardware test plan](FIRST-HARDWARE-TEST.md) covers the P0 build gate,
Flint 3 smoke tests, expected results, failure evidence, and a maintainer report template.
The [pre-PR review and remediation plan](PRE-PR-PLAN.md) records blocking
source findings, build requirements and the staged publication gates.
The [P1 feasibility and implementation plan](P1-RESEARCH.md) maps public
VLAN/L2/STP sources, conflicting field descriptions, dependencies and future
acceptance tests. It does not add P1 runtime support.

## P0 scope

The current code provides:

- MDIO-backed 32-bit register access, chip identification, and software reset.
- Serialized Clause 22, Clause 45, and OCP access to integrated PHY ports 4–7,
  including per-PHY C22 page selection.
- A private PHY driver with native MMD access to the actual port, standard
  autonegotiation and published Realtek 2.5G status decoding. Forced 2.5G is
  explicitly unsupported; actual negotiation remains hardware-unverified.
- Phylink plumbing for internal PHY ports 4–7 and the 10GBASE-R SerDes MACs
  on ports 3 and 8.
- SerDes mode selection and the optional `sds0/1-{rx,tx}-swap` device-tree
  properties, using the small set of open register operations listed in the
  provenance manifest.
- Native RTL8_4 CPU tagging, a VLAN 1/PVID 1 bootstrap, and CPU-only port
  isolation/flooding with learning disabled. Hardware bridge offload is omitted;
  DSA's software bridge fallback is the intended P0 path and needs bench checks.

The driver currently supports one CPU port and rejects cascaded DSA ports.
Only RTL8372N is accepted by chip-ID detection. The candidate matches only
`realtek,rtl8372n`; the BE9300 candidate DTS uses that explicit compatible.
Legacy `realtek,rtl837x` nodes, including the current BE6500 node, are not
supported by this P0 replacement. Keep the shipping package on the maintainer's
branch until board compatibility and feature deferrals have been agreed.

## Not in P0

The candidate has no DSA VLAN add/delete callbacks, FDB or MDB offload, STP
offload, LAG, rate limiting, hardware MIB/ethtool counters, GPIO controller,
or EEE support. The initial VLAN 1 setup only bootstraps the switch; it does
not provide general VLAN offload. LAG and rate limiting remain the work
tracked by Issues #47 and #49.

The SerDes path selects the 10GBASE-R mode and applies the board's optional
polarity swaps. It does not contain PHY firmware, vendor patch arrays, or the
full SerDes initialization sequence used by vendor SDKs. Internal PHY
power-control and link operation, and the 10G CPU link after reset, still need
hardware validation. Flint 3 hardware bring-up results are pending.

## Device tree

The current Flint 3 node uses `compatible = "realtek,rtl8372n"`, `reg = <29>`,
CPU port 3, and user ports 4–7. The driver consumes these top-level boolean
properties when present:

```dts
sds0-rx-swap;
sds0-tx-swap;
sds1-rx-swap;
sds1-tx-swap;
```

The P0 driver does not provide a GPIO controller and ignores the legacy
`rtl837x,sds0mode`, MDI-reverse, and PHY-TX-polarity properties. SerDes mode is
selected by phylink from the port's `phy-mode`; only `10gbase-r` is accepted
for ports 3 and 8.

## Build output

The OpenWrt package remains `kmod-rtl837x-dsa`. It builds
`rtl8372n_dsa.ko` (including its private PHY layer) and enables the kernel's
`tag_rtl8_4` DSA tagger. It explicitly depends on MDIO devres. Linux 6.18 is the
supported API; 6.12 compatibility is not claimed.

See [P0-STATUS.md](P0-STATUS.md) and [BUILD-REPORT.md](BUILD-REPORT.md) for
the current evidence and remaining gates. The ARM64 module build and full
BE9300 AP-config OpenWrt package/DTB/image build passed for source revision
`954a84bd7c46dbbb2412eeddfb300aad8b4cff35` ([module run](https://github.com/MNeroba/openwrt-flint3/actions/runs/37059225390),
[target image run](https://github.com/MNeroba/openwrt-flint3/actions/runs/37059229177)).
These are build results only; no BE9300 hardware checks have passed.

## Staged follow-up

1. Run the P0 first-device matrix on a BE9300 using the exact built image:
   real CPU/PHY links, tags, CPU-only isolation and software forwarding. The
   current image build passed; repeat it if source or build inputs change.
2. Prepare the shared table engine and resolve VLAN/L2 field meanings and
   source lineage, following [P1-RESEARCH.md](P1-RESEARCH.md). Source design can
   proceed while P0 hardware results are pending.
3. Prove BPDU CPU delivery, CIST states and dynamic fast-age before enabling
   hardware bridge/VLAN/flags; then add database-correct FDB/MDB management.
   Each runtime step needs a build and its documented bench gates.
4. Reach P1 parity or record agreed deferrals before porting LAG (#47) and
   rate limiting (#49). Restore other baseline interfaces in separate, sourced
   and tested changes or obtain explicit maintainer agreement to defer them.
