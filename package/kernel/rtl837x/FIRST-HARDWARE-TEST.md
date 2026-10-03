# RTL8372N P0 first-hardware test plan

**Status:** test procedure for an unverified candidate. No successful OpenWrt
module build or Flint 3 hardware run has been recorded for this P0 revision.
See [BUILD-REPORT.md](BUILD-REPORT.md) for exact CI evidence. Start the hardware
run only after T0 passes for the image revision under test.

This plan is intended for the driver maintainer and the Flint 3 owner running
the test. It is not a claim that any step has passed.

The candidate scope and sources are documented in [README.md](README.md) and
[PROVENANCE.md](PROVENANCE.md). The board description is
[`ipq5332-gl-be9300.dts`](../../../target/linux/qualcommbe/dts/ipq5332-gl-be9300.dts).

## 1. What this first test should prove

The first run is a narrow smoke test of the P0 path:

1. The MDIO-attached switch is detected at address 29 and the driver accepts
   the RTL8372N chip ID.
2. The switch resets and the minimum register setup completes without MDIO,
   regmap, SDS-command, or DSA registration errors.
3. DSA creates the CPU link and four user ports with the Flint 3 jack mapping.
4. The four board SerDes polarity properties are applied and the CPU link
   carries traffic after the driver's software reset.
5. Internal PHY ports bind to `RTL8372N internal PHY (P0)`, report link
   changes and carry untagged traffic to the router. LAN-to-LAN traffic runs
   through the software bridge and CPU, with no hardware bridge offload.
6. These results survive warm reboot and a cold power cycle.

The run does **not** qualify general VLAN offload, FDB/MDB offload, STP, LAG,
rate limiting, GPIO, EEE, or performance parity. Do not use tests for those
features as P0 acceptance criteria.

The future P1 acceptance matrix is in [P1-RESEARCH.md](P1-RESEARCH.md). Run it
only against a revision that implements the corresponding P1 step, after P0
passes. In particular, establish CPU-only BPDU reception without a loop before
a future hardware-STP loop test; the current P0 does not implement STP offload.

## 2. Safety and test setup

Use a spare GL-BE9300 and establish the project's documented recovery path
with a known-good image before the test. The AP config documents no usable
normal serial console on this board: do not assume UART access. Use the known
TFTP/initramfs recovery path and `ramoops`/`/sys/fs/pstore` crash evidence. If a
working console or independent management path is available, keep it
connected. Follow the backup and installation
instructions in the [top-level README](../../../README.md); do not test this
candidate on the only router you can recover remotely. The P0 code issues a
switch software reset during probe, and full post-reset PHY/SerDes initialization
has not yet been demonstrated.

Prepare:

- One Flint 3 with the candidate image built from the exact source revision
  under test.
- Tested recovery/initramfs access, a known-good image and persistent crash
  log collection. Record any actually usable console or independent management
  path; access through the candidate's LAN ports alone is insufficient.
- Two independent Ethernet hosts. For link-rate checks, use a peer and cable
  that support 2.5 Gb/s; a 1 Gb/s peer is still useful for basic link and
  forwarding checks.
- `ip`, `bridge`, `ping`, and `ethtool` on the router where available;
  `iperf3` on both external hosts for optional throughput measurements.
- A way to identify which physical LAN jack is connected to each host.

Do not connect the two hosts through an unmanaged switch for a LAN-to-LAN
forwarding test: it may forward frames locally without exercising the Flint 3.
Connect each host directly to a different Flint 3 LAN jack.

## 3. Device-tree configuration to use

For the first run, retain the current board topology and all four polarity
flags. The P0 driver consumes `compatible = "realtek,rtl8372n"`, MDIO address
29, port 3 as the 10GBASE-R CPU port, and the boolean properties shown below.
It does not implement the switch GPIO controller or the legacy
`rtl837x,sds0mode` property; those declarations have been removed from the
Flint 3 candidate DTS so that the example matches the driver.

```dts
rtl837x: rtl837x-dsa@29 {
	compatible = "realtek,rtl8372n";
	reg = <29>;
	#address-cells = <1>;
	#size-cells = <0>;

	sds0-rx-swap;
	sds0-tx-swap;
	sds1-rx-swap;
	sds1-tx-swap;

	ports {
		#address-cells = <1>;
		#size-cells = <0>;

		port@3 {
			reg = <3>;
			label = "cpu";
			ethernet = <&xgmac1>;
			phy-mode = "10gbase-r";
			fixed-link {
				speed = <10000>;
				full-duplex;
			};
		};

		/* Front-panel mapping: LAN1=7, LAN2=6, LAN3=5, LAN4=4. */
		port@4 { reg = <4>; label = "lan4"; phy-mode = "internal"; };
		port@5 { reg = <5>; label = "lan3"; phy-mode = "internal"; };
		port@6 { reg = <6>; label = "lan2"; phy-mode = "internal"; };
		port@7 { reg = <7>; label = "lan1"; phy-mode = "internal"; };
	};
};
```

The board DTS records that polarity swaps were required on the previous
working driver. Keep all four enabled for the candidate's baseline run; the
new driver resets the switch before applying them, so the old result must be
reconfirmed after reset. Do not start by sweeping polarity combinations.

## 4. Build and identify the exact candidate

Build on a supported Linux OpenWrt build host. The current checkout's macOS
host is not a successful build environment for this package.

```sh
cp package/kernel/rtl837x/P0-FEEDS.conf feeds.conf
./scripts/feeds update -a
./scripts/feeds install -a
cp configs/ap.config .config
make defconfig
make package/kernel/rtl837x/clean
make package/kernel/rtl837x/compile V=s
make -j"$(nproc)"
```

Before the hardware run, record the exact source revision used for the
image. A commit hash alone is insufficient if the build includes uncommitted
changes; attach the corresponding complete patch or commit the candidate first.
Record the installed feed revisions and image checksum on the build host:

```sh
git rev-parse HEAD
./scripts/feeds list -s -f > p0-feeds.conf
sha256sum bin/targets/qualcommbe/ipq53xx/*glinet_gl-be9300*sysupgrade.bin
find bin/packages -type f \( -name '*rtl837x-dsa*.apk' -o -name '*rtl837x-dsa*.ipk' \) -print
```

The build gate is successful only if the package and full target image finish
without errors, and the image contains `rtl8372n_dsa.ko` plus the kernel's
`tag_rtl8_4` module. Keep the build log with the test report. Install only by
the project's established recovery/install procedure on a recoverable test
unit; this document deliberately gives no unverified flash command.

Before installation, configure an untagged LAN software bridge for this P0.
Do not carry a VLAN-aware production bridge configuration into the first run.
The driver does not implement bridge offload; DSA should fall back to software
bridging without opening user-to-user isolation. A weak "Offloading not
supported" message is expected; failure to attach the port is not expected.

## 5. Capture a baseline after boot

Run this through the verified management path before starting traffic. Save the
output on the router or copy it to the host; do not publish ART contents,
unique MAC addresses, serial numbers, or wireless calibration data.

```sh
{
	date -u
	uname -a
	ubus call system board
	cat /etc/openwrt_release
	dmesg
	dmesg | grep -iE 'rtl837|dsa|phylink|mdio|smi|regmap|timeout|error|fail|oops|call trace'
	lsmod | grep -E 'rtl8372n|tag_rtl8_4|dsa' || true
	ip -br link
	ip -d link
	ip -s link
	bridge link
	bridge vlan show
	command -v ethtool >/dev/null && for d in lan1 lan2 lan3 lan4; do ethtool "$d" 2>&1; done
} 2>&1 | tee /tmp/rtl8372n-p0-baseline.txt
```

If an interface name differs, record the name and use it consistently. The
board's expected user-port names are `lan1` through `lan4`; WAN is a separate
RTL8221B path and is not part of this switch test.

## 6. Test matrix

Record **PASS**, **FAIL**, or **BLOCKED** for each row, with the relevant log
or command output. A blocked test is not a pass.

| ID | Scenario and procedure | Expected result |
| --- | --- | --- |
| T0 | Build/package gate above | Package and full image build cleanly; both required modules are present. |
| T1 | Cold boot with the verified recovery/management path available; save the full boot log and any pstore output. Search for the chip-ID log and driver errors. | Chip-ID read succeeds and passes RTL8372N detection; no probe failure, MDIO timeout, SDS timeout, DSA registration failure, kernel warning, or oops. |
| T2 | Check `ip -d link`, `bridge link`, and `lsmod`. | DSA ports `lan1`–`lan4` exist and join the configured LAN bridge; the CPU conduit is `xgmac1`/the board's `lan` path. `rtl8372n_dsa` and `tag_rtl8_4` are loaded. |
| T3 | Connect one peer to each physical LAN jack, one at a time. For each, record interface carrier and `ethtool` speed/duplex, then unplug and reconnect. | Jack map is LAN1→port 7, LAN2→6, LAN3→5, LAN4→4. Carrier follows the cable; no repeated flap or stuck port. Test 1G and 2.5G only when the peer supports those rates. |
| T4 | From one directly connected host, obtain the expected LAN address and ping the router's LAN address for at least 100 packets. Record loss and `ip -s link` before/after. | Router CPU path works through the switch; no persistent loss, growing error counters, or DSA/tagger errors. |
| T5 | Connect two independent hosts directly to two different LAN jacks, on the same untagged LAN. Confirm they are not connected through another bridge. Ping between them, then run `iperf3` for 30 seconds in both directions. Repeat across all six jack pairs; record any unavailable pair as BLOCKED. | ARP and bidirectional untagged LAN-to-LAN traffic work; no link reset, kernel warning, or persistent packet loss. Record rates as observations, not P0 pass thresholds. |
| T6 | Record `bridge vlan show` and switch VLAN/PVID readbacks using an agreed read-only access method. | Host output records software configuration only. Successful traffic plus hardware readback is needed to confirm the VLAN 1/PVID bootstrap. General VLAN offload is not implemented. |
| T7 | Reboot normally three times, then perform one full power-off/power-on with the verified recovery/management path available. Repeat T1–T4 after each boot. | Probe, link mapping, and basic CPU/LAN traffic remain consistent. No boot relies on stale switch state left by the previous run. |
| T8 | Optional: after all smoke tests pass, measure `iperf3` through a 2.5G-capable LAN peer in each direction. | Record peer, link speed, command, throughput, loss, and counters. No throughput target is defined for this unvalidated P0 candidate. |

### Additional P0 regression checks

- Capture each `/sys/bus/mdio_bus/devices/*/phy_id` and its `driver` symlink.
  Ports 4–7 must bind to the private PHY driver; probe deliberately fails on
  missing/wrong binding. Record supported, advertised and partner modes from
  `ethtool`, including a 2.5G-capable peer and 1G/100M peers where available.
- Before forming a bridge, verify a pair of standalone LAN ports does not
  forward between the hosts. Then attach the first port to an empty software
  bridge, attach the second, verify bidirectional traffic, remove each port
  and repeat. Removal must restore isolation. Capture errors and register
  masks at each stage. CPU load/traffic should reflect software forwarding.
- Run concurrent `ethtool` reads and port down/up cycles from separate shells.
  Save PHY command errors, link recovery and all kernel/lockdep output. This
  exercises the shared PHY command engine; do not infer race freedom from a
  single pass.
- Capture physical switch and SoC PCS state and bidirectional CPU-link traffic.
  A DT fixed-link's 10,000 Mb/s carrier alone is not physical link evidence.
- After cold and warm boot, read learning limits (`0x5384 + port * 4`), flood
  masks (`0x5360`–`0x5370`) and isolation (`0x50c0 + port * 4`). Their P0
  targets are zero learning limits, CPU-only flood destination and isolated
  user-to-CPU paths. Obtain readbacks only through a method approved for this
  register transport; do not invent arbitrary raw MDIO commands.
- Unknown unicast, broadcast, multicast and reserved control-frame handling
  still need negative forwarding tests. Do not connect a physical loop before
  BPDU/RMA behavior has been verified on the isolated bench.

### Do not run these as P0 tests

- Do not create tagged VLANs or enable `vlan_filtering`; the candidate has no
  general DSA VLAN add/delete callbacks.
- Do not use the existing `tests/bridge-flags.sh` as a P0 acceptance test; it
  exercises bridge flags not implemented by this candidate.
- Do not test FDB/MDB or STP offload, LAG (#47), rate limiting (#49), EEE, or
  switch GPIO. They are out of scope and can produce misleading failures.
- Do not compare the candidate to the former 1.8–1.9 Gbit/s result as a pass
  criterion. That measurement belongs to the previous driver build.

## 7. Failure triage

| Symptom | First evidence to attach | Likely area to investigate |
| --- | --- | --- |
| No RTL8372N chip-ID log / probe fails | Full boot/dmesg log and any pstore output; MDIO node and address; chip-ID read result if available | MDIO transport, address 29, reset timing, chip-ID mask. |
| Probe succeeds but DSA ports are absent | Probe log, `ip -d link`, DT fragment and build config | DSA registration, CPU/user port topology, required tagger module. |
| CPU link reports up but router traffic fails | Both ends' link state, `ip -s link`, tagger/EDMA logs, all four DTS polarity flags | RTL8_4 path, SDS mode/polarity, CPU port 3 mapping, post-reset SerDes init. |
| One or more LAN ports have no carrier | Physical jack, expected DSA port, `ethtool`, PHY/MDIO errors | Port map, internal PHY power-up sequence, PHY access. |
| Link is up but LAN-to-LAN traffic fails | Host interface/IP setup, pair under test, ARP table, packet captures at both hosts, bridge and VLAN output | Isolation, VLAN 1 membership/PVID, dynamic learning, DSA forwarding. |
| Warm boot works but cold boot fails (or vice versa) | Separate logs for each cycle and reset/power sequence | Switch reset and power-on timing; reliance on bootloader state. |

Change one variable at a time. For a polarity experiment, first preserve the
known-good all-four-swaps DTS and a recovery image; record each exact change
and boot result. Do not edit the live device tree on the only reachable test
unit.

## 8. Maintainer report template

Copy this block into the issue/PR report and fill every field. Mark unrun steps
as `BLOCKED` or `NOT RUN`, not `PASS`.

```text
Board / hardware revision:
Switch package marking (if readable):
Test date and operator:
Source commit:
Kernel release:
Image SHA-256:
Feed revisions / attached p0-feeds.conf:
Build host / build result:
DT compatible / MDIO address:
CPU port / conduit:
SerDes polarity properties enabled:
Chip ID from boot log:
PHY IDs / bound drivers per port:
Supported / local / partner advertised link modes per port:
Physical switch and SoC PCS evidence:
Isolation / learning / flood readbacks and access method:
Standalone / bridge create-remove results:
Concurrent PHY access / port lifecycle results:
Reserved control-frame / negative forwarding results:
T0 build:
T1 probe:
T2 DSA topology:
T3 LAN jack-to-port and link-rate matrix:
T4 router reachability (sent / received / loss):
T5 LAN-pair matrix and iperf3 results:
T6 VLAN 1 output:
T7 warm/cold boot results:
T8 optional throughput:
Kernel warnings, timeouts, or errors:
Attached logs (with MAC/serial/calibration data redacted):
Overall result and first blocker:
```

## 9. Request for the driver maintainer

The first useful review from a maintainer with RTL8372N hardware is to check
whether the P0 sequence is sufficient after a software reset: chip detection,
SDS mode selection, both mode-specific polarity registers, internal PHY power
control, RTL8_4 CPU tagging, and VLAN 1 bootstrap. Please report the exact
silicon/board, source revision, boot log, per-port link states, and whether
untagged CPU↔LAN and LAN↔LAN traffic pass. If traffic fails, include the first
failing matrix row and the logs requested above; no vendor PHY patch dump is
needed for this P0 report.
