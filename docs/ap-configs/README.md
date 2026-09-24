# Reference AP configs: VLAN trunk over a WDS radio backhaul

Working configuration for two OpenWrt APs where one reaches the network only
over a wireless backhaul and has to carry **two VLANs** across it.

    [ router ] --- wired trunk ---> [ ap1 ]  ))) WDS 4-addr )))  [ risc-ap ]
                                                                   |  |
                                            vlan10 (mgmt/LAN) -----+  |
                                            vlan192 (guest/IoT) ------+

* `vlan10` — management/LAN, **untagged** (PVID) on the backhaul
* `vlan192` — guest/IoT, **tagged** on the backhaul

risc-ap serves a guest SSID and several of its wired ports are vlan192, so the
backhaul must be a **trunk**. Getting only vlan10 across is the failure mode
described below, and it is deceptively quiet.

## The problem these configs solve

netifd does not configure bridge VLANs on WDS/station ports, for two separate
reasons:

1. **AP side.** hostapd creates the 4-address AP_VLAN port dynamically when the
   peer associates (`wl0-ap2.sta1`, `phy0.1-ap2.sta1`). netifd never sees it, so
   it inherits none of the parent BSS's bridge VLANs and the bridge drops it into
   default PVID 1.
2. **Station side.** The port is named after the radio index, and that index is
   **not stable** — `service network restart` can rename `phy0-sta0` to
   `phy1-sta0`. A `bridge-vlan` list in `/etc/config/network` that names the old
   interface then matches nothing.

Either way the port loses vlan192 while vlan10 keeps working, so management
stays up and **hides the breakage**: the guest SSID is still broadcast, clients
still associate, and nothing behind the backhaul can reach its gateway.

`99-wds-vlan192` is a hotplug script that re-applies both VLANs whenever such a
port appears. It matches loosely on `*sta*` so it covers both the AP-side and
station-side naming, waits for netifd to finish enslaving the port, only touches
ports on `br-lan`, and is idempotent. Install it on **both** ends.

## Do not lose it on upgrade

List it in `/etc/sysupgrade.conf`:

    /etc/hotplug.d/net/99-wds-vlan192

This is not optional housekeeping. When an E8450 took over as ap1 on
2026-09-24, its `sysupgrade.conf` was empty, the script was not carried across,
and vlan192 died silently — the guest gateway was ARP-unreachable for a day
while vlan10 looked perfectly healthy.

## Verifying

Read the port's VLANs on each end — the backhaul port should look like a trunk:

    # bridge vlan show dev wl0-ap2.sta1     # AP side
    port              vlan-id
    wl0-ap2.sta1      10 PVID Egress Untagged
                      192

The `bridge` tool is not installed by default on every image (`ip-bridge`).
Failing that, test functionally from the station, which is the stronger check:

    ping -c2 -I br-lan.10  10.20.30.1      # management VLAN
    ping -c2 -I br-lan.192 192.168.2.1     # guest VLAN  <-- the one that breaks

## What has been removed from these files

* every PSK/passphrase/secret value replaced with `REDACTED`
* SSIDs replaced with placeholders (`LAN_SSID`, `GUEST_SSID`, `UPLINK_SSID_24`, …)
* BSSIDs replaced with `AA:BB:CC:DD:EE:xx`

Addresses are left as-is; they are all RFC1918. Adapt VLAN IDs, SSIDs and
addressing to your own network — the structure is the point, not the values.
