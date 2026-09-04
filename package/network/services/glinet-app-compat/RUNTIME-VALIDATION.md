# GL.iNet App compatibility runtime validation

This is the controlled test plan for a GL-BE9300/Flint 3 with
`glinet-app-compat` installed. Source-level checks and host harnesses do not
prove router, hardware, or official-App compatibility. Those columns remain
`NOT PERFORMED` until a real router and the official App are available.

## Safety and evidence collection

Use a dedicated LAN client and a reversible test configuration. Keep a second
management path available before changing the LAN address. Do not expose
`/rpc` on WAN, even when WAN happens to use an RFC1918 address.

Enable temporary diagnostic logging only for the test window:

    uci set glinet-app-compat.main.log_level='debug'
    uci commit glinet-app-compat
    /etc/init.d/uhttpd reload
    logread -f | grep glinet-app-compat

Run the read-only preflight before making changes:

    /usr/libexec/glinet-app-compat-check

After a failed run, collect the sanitized bundle locally. The default is
stdout; this example writes only below `/tmp` and performs no upload:

    /usr/libexec/glinet-app-compat-support /tmp/glinet-app-compat-support.txt

The bundle does not read `/etc/shadow` or session/challenge state. It redacts
passwords, hashes, nonces, SIDs, cookies, Admin-Token values, wireless keys
and private-key fields. Treat addresses, hostnames and logs as device data
nevertheless.

## RPC request format

The endpoint accepts HTTP `POST` only. The authenticated SDK4 call shape is:

    {"jsonrpc":"2.0","id":1,"method":"call","params":["<sid>","<module>","<method>",{}]}

The three-element form omitting the final empty object is also accepted. Keep
the SID in an unlogged shell variable or a protected client tool; never put a
real SID, password, nonce or cookie in a shared test transcript.

Expected error classes are:

| Condition | HTTP/JSON-RPC result |
| --- | --- |
| Successful method | `200` and a `result` object/value |
| Invalid JSON-RPC or method arguments | `-32600`/`-32602` |
| Malformed JSON | HTTP `400`, `-32700` |
| Unsupported method | `-32601` |
| Authentication failure | `-32000` |
| Backend/configuration failure | `-32001` |
| Non-LAN source | HTTP `403`, `-32003` |

## Expected diagnostic records

With debug logging, a normal authenticated call is represented by a
worker-local, non-secret request identifier:

    glinet-app-compat: req=17 rpc module=wifi method=get_status args=[object] source=<lan-ip> auth=session result=start
    glinet-app-compat: req=17 backend=network.wireless.status result=ok
    glinet-app-compat: req=17 rpc module=wifi method=get_status args=[object] source=<lan-ip> auth=session result=success duration_ms=<n>

The `req` value is not persisted and is not derived from authentication data.
Repeated messages still use the existing one-minute duplicate suppression, so
polling may not produce one complete triplet for every request. Backend or
rollback failures use the same prefix and include only method/key names and a
result class; argument values are never logged.

## One-shot authentication and lifecycle sequence

Record the time around the challenge and login requests without recording the
nonce or password. The current challenge lifetime is one second, and a
challenge is single-use.

| Step | Request | Expected observation | Log/result class |
| --- | --- | --- | --- |
| 1 | `call(<any placeholder>, ui, check_initialized, {})` | Truthful GL-BE9300/Flint 3 and initialization result; no SID required | `auth=preauth`, `success` |
| 2 | `challenge` with `{username:"root"}` | Challenge response contains SDK4 fields; keep nonce private | `auth`/`challenge`, `success` |
| 3 | `login` with `{username:"root", hash:"<challenge-derived>"}` | A valid SID and `Admin-Token` cookie are returned | `auth=authenticated`, `success` |
| 4 | top-level `alive` with `{sid:"<sid>"}` | JSON `result: null`; inactivity timeout is refreshed | `session`, `success` |
| 5 | top-level `logout` with `{sid:"<sid>"}` | JSON `result: null`; the owning session is removed | `session`, `success` |
| 6 | Repeat challenge/login | A new SID is issued; record whether the earlier SID remains valid, rather than assuming sessions are invalidated | `auth`, `success` |

Negative checks: delay login beyond one second, reuse one challenge, submit a
wrong hash, omit `sid`, and send a SID from a different source. Each must fail
without logging a password, hash, nonce, cookie or SID. A foreign-source
logout must not remove the owning client's active session.

## Read-only compatibility sequence

Run these calls after the second login. A missing backend must produce a
truthful backend error or an explicitly incomplete result; it must not be
reported as a fabricated connected/online state.

| RPC call | Expected response/state |
| --- | --- |
| `system.get_info` | OpenWrt/board identity and available hardware information; no claim of stock firmware |
| `system.get_status` | Normalized system, LAN, WAN and Wi-Fi status; `internet` follows route/link state |
| `system.get_load` | Values returned by `system.info` when available |
| `cable.get_status` | Normalized WAN status; down/absent/pending cases remain distinguishable |
| `lan.get_config_list` | Current LAN IPv4/prefix and DHCP range from UCI/netifd |
| `wifi.get_config` | All configured radios/BSSes; no WPA key values |
| `wifi.get_status` | Source-derived radio state/channel/band, including absent radios safely |
| `clients.get_list` | Merged association/lease data; static-only reservations are not active clients |
| `clients.get_status` | Wireless count and explicit wired-data availability |
| `dns.get_config` | `mode`, GL-shaped `manual_list`, and runtime `server_auto` |
| `firewall.get_port_forward_list` | Only compatibility-owned IPv4 WAN-to-LAN rules, using stable UCI IDs |

For every call, compare the response with `logread` and the corresponding
`ubus`/UCI state. Do not treat the presence of an interface object alone as a
WAN connection.

## Reversible Wi-Fi checks

Save the original values from `wifi.get_config` for one existing BSS. Change
one BSS at a time, preserving all other radios and BSSes:

    wifi.set_config({iface_name:"<existing-bss>",ssid:"<temporary-ssid>"})

Expected response is a JSON-RPC success with an empty transaction result.
Verify the selected UCI section, `network.wireless` status, radio/BSS state,
hostapd association and a client reconnect. Restore the original SSID and
verify again. For a password test, use a protected client variable; never use
shell tracing or put the password in a log command. Verify that
`wifi.get_config` continues to return `key: null`/no key value.

Repeat with `hidden`, a supported encryption mode, and a source-derived
channel only when the original state can be restored. A regulatory fallback or
an unavailable channel must be recorded as a runtime result, not silently
called success. Radio-wide enable/disable and unsupported stock fields must
return `-32602`, without changing UCI.

Rollback check: induce a failed `network.reload` in a test image or use the
host harness. The original wireless options must be restored before the error
response; a rollback failure must be visible as `rollback` diagnostics.

## LAN and DHCP checks

First change only the DHCP pool, then restore it:

    lan.set_config({interface:"lan",start:"<new-host>",end:"<new-host>",enable:true,leasetime:"12h"})

Use a pool inside the resulting subnet, excluding network, broadcast and the
router address. Verify `network.lan`/`dhcp` UCI, `network.interface.lan`, a
renewed client lease and the getter's absolute `start`/`end` values. Restore
the original range and lease time.

Test invalid pools (outside subnet, network/broadcast/router address, reversed
range, malformed address) and confirm `-32602` with no UCI delta.

Test a LAN address/netmask change last. Verify the response reaches the client
before reload, whether the App reconnects, and whether manual reconnect using
the new address works. Do not change behavior based on expectation; record the
actual result and restore the original LAN address afterward.

## DNS checks

Capture the original `network.wan.peerdns`, `network.wan.dns` and getter result.
Then run:

| Request | Expected UCI/runtime result | Restore |
| --- | --- | --- |
| `dns.set_config({mode:"manual",manual_list:["<dns1>"]})` | `peerdns=0`, one owned `dns` list, network reload accepted; getter returns `manual` | automatic or original state |
| `dns.set_config({mode:"manual",manual_list:["<dns1>","<dns2>"]})` | Two distinct IPv4 servers; runtime WAN status exposes resolver state when available | automatic or original state |
| `dns.set_config({mode:"auto"})` | `peerdns=1`; only a DNS list marked by this layer is removed; ISP/runtime DNS resumes | original state |

Verify the resulting resolver state and make an actual DNS query after the
reload has completed. A successful ubus acceptance is not proof that the
asynchronous network transition has completed. Do not edit `/etc/resolv.conf`.

Reject and verify no change for duplicate servers, an empty manual list,
malformed/multicast/broadcast/unspecified addresses, more than five servers,
IPv6 servers when IPv6 semantics have not been established, and stock fields
such as `rebind_protection`, `force_dns`, `override_vpn` or secure/proxy DNS.
The existing dnsmasq rebind setting must remain unchanged.

## Port-forwarding checks

Use an online or controlled LAN host address inside the current LAN subnet.
The initial TCP rule is:

    firewall.add_port_forward({name:"<test-name>",src:"wan",dest:"lan",proto:"tcp",src_dport:"<external-port>",dest_ip:"<lan-host>",dest_port:"<internal-port>",enabled:true})

Expected response is a stable UCI section ID. Verify all of the following:

1. `uci show firewall` contains an IPv4 `DNAT` redirect with
   `glinet_app_compat='1'`.
2. The firewall service reload has completed and the relevant `nft` DNAT rule
   exists in the runtime ruleset.
3. A real WAN-side TCP connection reaches the intended LAN host.
4. `firewall.get_port_forward_list` returns the rule with `src_port`.
5. `firewall.set_port_forward` replaces the complete rule and the list reflects
   the new values.
6. Disable and re-enable with `set_port_forward`, checking both UCI and runtime
   rules.
7. `firewall.remove_port_forward` removes the section and leaves no runtime or
   UCI residue.

Repeat with UDP where practical. The source-proven protocol values are only
`tcp`, `udp` and `tcp udp`; ports may be a single value or a range shorter than
100 ports with matching source/destination lengths. Verify invalid protocol,
port 0, port 65536, malformed/reversed/oversized ranges, invalid rule ID,
unknown ID, duplicate owned rule, LAN network/broadcast/router destination and
outside-subnet destination. Existing unmarked redirects must remain absent
from the compatibility list and unchanged.

Rollback checks must cover a failed reload after add, update and delete. The
created section must disappear, a deleted section must be recreated in its
original order/options, and an updated section must be restored exactly. A
rollback failure must be reported; the RPC must not report success.

## LAN-source security checks

Run from controlled source addresses and record HTTP status plus JSON error:

| Source | Expected |
| --- | --- |
| `127.0.0.1` and configured LAN IPv4 subnet | Allowed (subject to authentication for calls) |
| A second IPv4 subnet assigned to the LAN interface | Allowed if reported by `network.interface.lan` |
| WAN IPv4, including an RFC1918 WAN address | Rejected; private address class alone is not sufficient |
| IPv6 link-local or global address | Rejected because uhttpd does not provide reliable ingress-interface provenance |
| `::1` local loopback | Local-only exception; still requires authentication for protected calls |
| Malformed/missing source metadata | Rejected |

Use two clients to verify a valid SID is bound to the source address: the
second client cannot read, refresh or log out the first client's session.

## Official-App-first path

Run the same sequence through the official App after preflight. Initially add
the router manually by LAN IP so discovery is not part of the test. Record:

* App version and phone OS/version;
* router IP and whether manual-IP add was used;
* first screen/action that fails;
* exact RPC module/method and argument key names from debug logs;
* HTTP status, JSON-RPC error code/message and `req` identifier;
* whether retrying or reconnecting changes the result.

Do not start with packet capture. The endpoint diagnostics should first expose
unsupported calls as:

    glinet-app-compat: ... unsupported method: <module>.<method>

Build a unique list without exposing values:

    logread | grep 'unsupported method' | sed -E 's/.*unsupported method: //' | sort -u

For each new call, preserve the exact method and argument-key shape in a
separate task record. Do not add aliases or accept ignored security fields
until stock bytecode/App evidence establishes their semantics.

For DNS, record whether the App sends unsupported secure/proxy, rebind or VPN
override fields. For port forwarding, record whether it actually invokes
`firewall.get_port_forward_list`, `add_port_forward`, `set_port_forward` or
`remove_port_forward`, and preserve the observed fields before changing code.

## Challenge and LAN-IP timing observations

The one-second challenge window must be tested against the real App. Record
challenge and login timestamps, but not the nonce/hash/password. If a valid
App flow exceeds one second, compare with stock behavior before changing the
TTL.

For a LAN-IP change, record whether the JSON-RPC response arrives before the
reload, whether the App reconnects automatically, and whether manual
reconnection succeeds. This remains an observation gate, not an assumed
compatibility guarantee.

## Runtime validation matrix

`Host tests` refers to source/fixture checks; `OpenWrt build` refers to an
actual package/image build. Runtime and official-App columns are intentionally
not marked PASS here.

| Feature | Host tests | OpenWrt build | Router runtime | Official App | Hardware-dependent |
| --- | --- | --- | --- | --- | --- |
| challenge/login | NOT RUN (no protocol/configuration fixture harness in tree) | package required | NOT PERFORMED | NOT PERFORMED | No |
| alive/logout | NOT RUN (no protocol/configuration fixture harness in tree) | package required | NOT PERFORMED | NOT PERFORMED | No |
| system info/status/load | source/fixture checks | package required | NOT PERFORMED | NOT PERFORMED | Yes |
| WAN status | fixture checks | package required | NOT PERFORMED | NOT PERFORMED | Yes |
| LAN getter | fixture checks | package required | NOT PERFORMED | NOT PERFORMED | Yes |
| LAN setter/DHCP | NOT RUN (no protocol/configuration fixture harness in tree) | package required | NOT PERFORMED | NOT PERFORMED | Yes |
| Wi-Fi getter | fixture checks | package required | NOT PERFORMED | NOT PERFORMED | Yes |
| Wi-Fi setter | NOT RUN (no protocol/configuration fixture harness in tree) | package required | NOT PERFORMED | NOT PERFORMED | Yes |
| clients | fixture checks | package required | NOT PERFORMED | NOT PERFORMED | Yes |
| timezone | source check only | package required | NOT PERFORMED | NOT PERFORMED | No |
| reboot | source check only | package required | NOT PERFORMED | NOT PERFORMED | No |
| DNS automatic | NOT RUN (no protocol/configuration fixture harness in tree) | package required | NOT PERFORMED | NOT PERFORMED | Yes |
| DNS manual | NOT RUN (no protocol/configuration fixture harness in tree) | package required | NOT PERFORMED | NOT PERFORMED | Yes |
| port-forward list | NOT RUN (no protocol/configuration fixture harness in tree) | package required | NOT PERFORMED | NOT PERFORMED | Yes |
| port-forward add/update | NOT RUN (no protocol/configuration fixture harness in tree) | package required | NOT PERFORMED | NOT PERFORMED | Yes |
| port-forward delete | NOT RUN (no protocol/configuration fixture harness in tree) | package required | NOT PERFORMED | NOT PERFORMED | Yes |

## Build authority and stopping point

Run the repository's Linux CI for the package/profile checks. Queued jobs are
not PASS, and cancelled superseded jobs are not failures. The local macOS
environment previously stopped before OpenWrt compilation because Apple GNU
make is unsupported; do not alter that prerequisite or claim a package build
until compilation is reached.

This task deliberately stops before VPN, Multi-WAN, AdGuard, secure DNS,
firmware update, discovery, WebSocket, cloud or remote-management work. The
next implementation task should be driven by captured router/App evidence,
not by guessing from an unsupported method name.
