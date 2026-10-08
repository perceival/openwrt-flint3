# RTL8372N internal-PHY binding failure and correction

Updated: 2026-10-08 after the Release-8 bench report. The RTL8224 binding
blocker is resolved on hardware. Release 7 passes T0/T1/T2/T3/T4/T5/T7; the
spurious PHY power-down warnings on ports 0–2 are absent. Perceival's
[Release-7 follow-up](https://github.com/perceival/openwrt-flint3/pull/104#issuecomment-6044579250)
confirms the full DSA topology, 100/100 T4 pings, and physical down/up events
on all four jacks. T5 covers all six software-bridge pairs. Release 8 on exact
source commit `7e51247b` adds exact-revision T1 and partial T6 evidence: 33/33 setup
readbacks match expected values, and one detached port did not forward the
tested traffic to one bridge observer. The reverse-direction check is
counter-only without endpoint capture. General VLAN/bridge offload and
reserved-control-frame behavior remain unverified; P0 is not fully qualified.

## Earlier diagnostic hardware failure (release 4)

Perceival's [diagnostic-revision report](https://github.com/perceival/openwrt-flint3/pull/104#issuecomment-6006419562) tests `1b7a32bef2`
on GL-BE9300 with the reference AP configuration, `kmod-qca-ssdk` and
`kmod-qca-nss-ppe` removed, and `kmod-phy-realtek` retained for WAN.
T0 passed: both candidate/tagger modules are in the image and SHA-256 was
verified. The report does not publish the new image checksum itself.

The switch ID is `0x83727000`. All four internal PHYs read PHYSID1 `0x001c`
and PHYSID2 `0xcad0` with `err=0`. Port 4's binding summary is:

```text
id=0x001ccad0 clause=C22 bound=1 driver=RTL8224 2.5Gbps PHY private_phy=0
```

The standard Realtek driver has successfully bound where the private driver
was required. Setup correctly rejects it with `-ENODEV`.
**T1 FAIL; T2–T8 BLOCKED.** PHY-ID discovery now has positive evidence;
private feature probing, link negotiation, CPU traffic and forwarding do not.

The operator reported no further release-4 serial output for minutes and
reproduced the stop after another power cycle. That log ends after
`10GBASE-R link not up before USXG_EN`, after switch registration had already
failed on PHY binding. The complete release-6 log contains the same PCS message
during `wan` setup, then continues through service startup and later link
events; the operator also reports usable SSH and five successful boots. This
makes the PCS line insufficient to explain the old stop. The release-4 log has
no panic, hung-task trace, or other evidence that identifies why output stopped,
so the reported earlier hang remains unexplained.

## Release-6 hardware report and remaining warnings

Perceival's [release-6 bench report](https://github.com/perceival/openwrt-flint3/pull/104#issuecomment-6014637917)
and [redacted serial logs](https://gist.github.com/perceival/f7abebb5395db63b97d3775ebd2c544a) test source `32d958fe17441f849aad085c6d4c26c0a5275b87`, package release 6, kernel 6.18.39, image revision `r35533+282-3b2bc55dcb`.

| Test | Result | Evidence / limit |
| --- | --- | --- |
| T0 | PASS | Exact-source image built with the documented AP config adjustments; both candidate modules are present. The report does not include the image SHA-256. |
| T1 | PASS | Chip ID `0x83727000`; PHY ID `0x001ccad0` and private-driver binding on ports 4–7. |
| T2 | PASS | Four DSA interfaces exist under `br-lan`, conduit `lan`, both modules loaded. |
| T3 | PARTIAL | lan1 2.5G, lan2 1G, lan3 2.5G; lan4 had no peer. lan2/lan3 recovered after peer-interface down/up; no physical unplug/replug was run. |
| T4 | PASS | 100/100 pings to the router from one 2.5G client; this is router reachability, not LAN-pair forwarding. |
| T5 | PARTIAL (3/6 pairs) | Bidirectional ping and 8–10 s iperf3 TCP runs passed for LAN1–LAN2, LAN1–LAN3 and LAN2–LAN3 on the CPU/software-bridge path. LAN4 pairs were unavailable; procedure's 30 s duration was not met. |
| T6 | NOT VERIFIED IN HARDWARE | VLAN 1/PVID output reflects software configuration; there was no switch-register readback. |
| T7 | PASS | Three warm reboots and one cold power cycle; binding, link rates and pings repeated, without the earlier stop. |
| T8 | RECORDED ONLY | iperf3 used the router as endpoint and was CPU-bound; it is not a switch-forwarding result or acceptance threshold. |

## Release-7 hardware report (2026-10-07)

Perceival's [release-7 report](https://github.com/perceival/openwrt-flint3/pull/104#issuecomment-6037094988)
identifies source `79afa2c51a3c2396c33ed511ed092d799c52e1bf`, image revision
`r35533+286-3b2bc55dcb`, and flashed-image SHA-256
`a219c20026965027b425b80181abb362377df5450298f86932246033ed480ac9`.
The [raw logs](https://gist.github.com/perceival/145d80ee322c48e88870ec011ae1200d)
record all four private PHY bindings and expected LAN rates. T7 passed three
warm reboots and one cold cycle, with all four bindings and 20/20 router pings
after each boot.

T3 is PASS after physically unplugging/reconnecting each jack. The initial
five-second SSH sampler ran through LAN1, so removing that cable interrupted
the sampler and left a 20-second hole. Perceival's follow-up provides the
kernel link-event excerpt: every jack has a down/up event, and each returns at
the same rate. LAN1/3 return at 2.5G, LAN2/4 at 1G; LAN2 flapped a second time
during reinsertion before settling at 1G. This resolves the earlier capture gap.

T5 passed all six directly connected LAN pairs, in both directions, with
30-second TCP runs and 20/20 pings. Rates ranged from 927 Mbit/s to 1.56
Gbit/s on the CPU/software-bridge path; no P0 throughput threshold is defined.
The LAN3 RX drop counter increased from 0 to 77 across 9,271,560 received
packets, while LAN1 TX drops stayed at 1 and LAN3 TX drops stayed at 2. Several
iperf3 directions reported TCP retransmissions, but all transfers completed
and all pings had zero loss. The `lan` conduit `tx_errors` value was
`2^64-1` both before and after the matrix; the underlying PPE MIB operands were
not included, so this counter does not establish packet loss. The aggregate
CPU snapshot omitted softirq time and cannot estimate forwarding CPU load.

At the time of the Release-7 run, T6 was unverified because no switch
VLAN/isolation register readback was provided. Release 8 supplies tested-revision
readback and limited negative-forwarding evidence below. The release-7
follow-up supplies T2 output: four `lanN@lan` netdevs,
the `lan` DSA conduit, all four ports attached to `br-lan` in forwarding state,
and both required modules loaded. It also supplies the release-7 T4 run: 100
packets transmitted and received, 0% loss, RTT min/avg/max/mdev of
0.173/0.265/0.407/0.064 ms. The ordinary conduit error/drop counters are
reported as zero; its separate `tx_errors=2^64-1` value remains uninterpretable
without the raw PPE MIB operands.

## Release-8 P0 readback and isolation follow-up (2026-10-08)

Perceival's [Release-8 report](https://github.com/perceival/openwrt-flint3/pull/104#issuecomment-6048213461)
tests exact source commit `7e51247b3882567ce891481395b34e2a2c25f116`, kernel
6.18.39, image revision `r35533+292-3b2bc55dcb`. The report lists the image
and module SHA-256 values and links the [raw serial/test logs](https://gist.github.com/perceival/3172215cb83c6ea3866fd55bee0808be).
The boot log shows all four private PHY bindings, no port 0–2 power-down
warnings, and LAN link rates of 2.5G/1G/2.5G/1G.

The T6 setup readback file contains **33/33 PASS** entries with matching actual
and expected values for the VLAN 1 table word, isolation masks, learning
limits, CPU flood destinations, available PVID fields, and VLAN filter/tag
controls. This verifies that these registers read back as programmed expected;
it does not establish general VLAN offload semantics by itself.

For the negative-forwarding test, E8450-to-Cudy traffic was visible at the Cudy
bridge port with `lan2` in the bridge (control); after `lan2 nomaster`, the
bench CPU saw E8450 frames but the Cudy observer saw zero E8450-sourced frames
for the tested ARP broadcast and unicast pings. Restoring `lan2` restored 5/5
ping. This supports isolation for one detached port, one observer and the
measured direction. The reverse Cudy-to-E8450 result is reported using an
interface RX-counter delta equal to an idle-window delta, without endpoint
packet capture, so it is weaker evidence. The raw v3 test file does not include
that reverse run.

Record T6 as **PARTIAL**: setup readbacks passed and a single CPU-only
standalone-port scenario is supported. VLAN-tagged traffic, VLAN-aware bridge
configuration, all-port combinations, multicast/BPDU/reserved-control frames,
and general bridge/VLAN offload remain untested. Do not treat this as an STP or
P1-B result.

### Warning diagnosis

- **PHY power-down `-22` on ports 0–2:** Release 6's `port_disable` treated every non-SerDes port as an internal PHY. The PHY accessor explicitly accepts only ports 4–7, so it returned `-EINVAL` before issuing an MDIO/PHY command. Package release 7 checks the supported PHY-port mask in both `port_enable` and `port_disable`; its module and full-image CI passed. The release-7 boot log confirms the port 0–2 power-down warnings are gone.
- **Conduit `tx_errors`:** Release 6 reported `18446744073709551614` (`2^64−2`); release 7 reported `2^64−1` both before and after T5. Both values are consistent with unsigned underflow in the existing Qualcomm PPE calculation in `target/linux/qualcommbe/patches-6.18/0342-net-qualcomm-Update-IPQ9574-PPE-driver.patch`, which computes `tx_packets - tx_frames_g`. The raw PPE operands were not included, so this explains the representation but does not establish the hardware-counter semantics or packet loss. This is outside the RTL8372N driver and needs separate raw-MIB validation.
- **`10GBASE-R link not up before USXG_EN`:** Release 7 still logs this after `qcom_ppe ... wan: configuring for inband/usxgmii`. The boot continues, the DSA CPU link comes up at 10 Gbit/s, and the hardware tests pass. This WAN PCS event is not evidence that the switch-to-SoC CPU link failed and does not explain the earlier release-4 log ending.

Remaining hardware evidence: Release 8 resolves the safe read-only readback question for the listed setup registers and partially tests one-port isolation. Extend T6 with direct reverse-direction capture, other feasible port/observer combinations, and reserved control-frame behavior. Recovery/concurrency checks are also open. T2/T3/T4 and T5 are documented for Release 7; T5 covers all six pairs on the CPU/software-bridge path.

## Why matching the same ID is insufficient

Linux 6.18.39's [driver core](https://github.com/gregkh/linux/blob/v6.18.39/drivers/base/dd.c)
iterates matching drivers and stops after successful binding. A private
`match_phy_device` predicate grants no priority over an already registered
Realtek driver. Module autoload priorities 17/18 do not establish runtime order.
Adding another matching PHY ID does not resolve that ordering issue.

The [MDIO bus matcher](https://github.com/gregkh/linux/blob/v6.18.39/drivers/net/phy/mdio_bus.c)
uses the PHY device's `mdio.bus_match` callback after OF matching. This
per-device callback can be set before registration, preventing the standard
ID matcher from selecting RTL8224 on these private-bus devices. A DT change
is unnecessary for the reported BE9300 path, which has no child MDIO node.
No arbitrary vendor-specific PHY compatible override is promised.

## Registration correction (package release 6)

1. Register the managed internal MDIO bus with all automatic scanning masked.
   This prevents a PHY from being created and bound before its matcher is set.
2. Discover each enabled internal user PHY, addresses 4–7, with
   `get_phy_device()`. Retain the actual hardware IDs and existing C22/C45
   discovery semantics; do not fabricate IDs to avoid `realtek.ko`.
3. Set `mdio.bus_match` to select the private driver on this bus/address range,
   then call `phy_device_register()`. Linux checks OF matches before this
   callback, so the override applies to the BE9300 path with no child MDIO PHY
   node and ordinary PHY-ID matching. A vendor-specific child compatible needs
   separate review; the callback cannot override an earlier OF match.
4. Preserve explicit child-PHY association and bus reset delays. Require one
   unique child address per enabled internal user port. This PHY driver uses C22
   page/ability operations, so reject C45 declarations and C45-only discoveries
   before registration. Reject unsupported PHY package nodes explicitly rather
   than reporting a misleading invalid address.
5. Match upstream OF-MDIO's 10 us default reset delay and parse optional bus
   reset delays.
6. Keep the strict completed-binding gate and feature/MMD/read diagnostics.
   A private probe failure still aborts setup; it cannot become a false pass.
7. Free an unregistered PHY after registration failure. Registered PHYs belong
   to managed bus teardown, including partial registration and setup failure.

The standard Realtek driver remains available for WAN and all other buses.
Reset and underlying PHY/SerDes register values are unchanged; release 7 only
limits PHY enable/disable callback access to ports 4–7. No restricted header or
vendor patch data is introduced.

## Revision-specific build evidence

| Revision | Mainline ARM64 module | Full BE9300 image | Hardware |
| --- | --- | --- | --- |
| Diagnostic `1b7a32bef2`, release 4 | [PASS](https://github.com/MNeroba/openwrt-flint3/actions/runs/37273928197) | [PASS](https://github.com/MNeroba/openwrt-flint3/actions/runs/37273960255) | Maintainer T0 PASS; T1 FAIL; T2–T8 BLOCKED |
| Registration correction, release 5 (`614188cff5`) | [FAIL](https://github.com/MNeroba/openwrt-flint3/actions/runs/37395423769): private kernel macro not visible | Cancelled after release-5 module CI failed | Not run |
| Corrected registration, release 6 (`32d958fe17`) | [PASS](https://github.com/MNeroba/openwrt-flint3/actions/runs/37397943227) | [PASS](https://github.com/MNeroba/openwrt-flint3/actions/runs/37397974073) | T0/T1/T2/T4/T7 pass; T3 partial; T5 partial (3/6 pairs, short traffic runs); T6 software output only; T8 recorded |
| PHY callback guard, release 7 | [PASS](https://github.com/MNeroba/openwrt-flint3/actions/runs/37454484695) | [PASS](https://github.com/MNeroba/openwrt-flint3/actions/runs/37454704524) | T0/T1/T7 pass; T5 passes 6/6 pairs; T3 reported pass with LAN1 capture gap; T6 was not instrumented |
| P0 readback follow-up, source commit `7e51247b` | [PASS](https://github.com/MNeroba/openwrt-flint3/actions/runs/37676942253) | Maintainer-built Release-8 image booted | T1 pass; T6 partial: 33/33 readbacks and one standalone-port negative-forwarding scenario; reverse evidence counter-only |

Release 5 failed compilation because `DEFAULT_GPIO_RESET_DELAY` is private to
kernel `of_mdio.c`. Release 6 uses a named local 10 us constant matching
`__of_mdiobus_register()` and explicitly reports unsupported PHY package nodes.
Release 6 [ARM64 module CI](https://github.com/MNeroba/openwrt-flint3/actions/runs/37397943227) passed with all four objects, `W=1`,
modpost and link success; candidate compilation has no warnings. Module
SHA-256: `9ccd428ae58f7650d8f7e47455c24250349e840758208e800146663a44037263`. [Full-image CI](https://github.com/MNeroba/openwrt-flint3/actions/runs/37397974073) also passed. Release 7 passed both module and full-image CI, and its hardware report confirms that the port 0–2 power-down warnings are absent.

## Remaining hardware work

1. Clarify the LAN1 physical cycle reported for T3; the attached raw link sample
   does not show the expected down/up event. Repeat T2/T4 on release 7 if exact-
   revision qualification is required; release 6's T2/T4 results remain valid
   for that earlier revision.
2. T6 setup readbacks passed 33/33 on Release 8. Keep broader isolation,
   reverse-direction behavior and reserved control-frame checks open; software
   `bridge vlan show` output alone is not a switch-register result.
3. Test reserved control-frame behavior and complete the remaining recovery and
   PHY concurrency checks in the first-hardware procedure.
4. Keep throughput figures labeled as CPU/software-bridge traffic. To qualify
   switching performance, use two hosts with traffic that does not terminate
   on the router and capture both switch-port and CPU/conduit counters.

Successful T1 and one router ping do not establish complete P0 qualification
or feature parity. P1-A remains separate until the P0 acceptance matrix and
provenance gates are resolved.
