# glinet-app-compat

This package adds a local JSON-RPC endpoint at `POST /rpc` for the GL.iNet
App's established SDK4 transport. It is intentionally a compatibility layer,
not the stock GL.iNet firmware stack.

## Implemented in this core package

| RPC | Result |
| --- | --- |
| `challenge` / `login` | Local `root` challenge-response using the existing `/etc/shadow` entry |
| `alive` / `logout` | Source-bound session refresh and idempotent session termination |
| `ui.check_initialized` | Pre-login initialized/model check with truthful GL-BE9300 identity |
| `system.get_info` | OpenWrt board, release, time, MAC and hardware-info data when available |
| `system.get_status` | Uptime/load/memory plus normalized LAN/WAN/Wi-Fi status |
| `system.get_load` | Standard OpenWrt load, uptime and memory fields when available |
| `system.set_timezone_config` | Authenticated timezone and zonename update through standard system UCI |
| `system.reboot` | Authenticated standard OpenWrt ubus reboot request |
| `cable.get_status` | Normalized `wan` interface/link state when present |
| `dns.get_config` | Current automatic/manual IPv4 DNS mode and runtime automatic servers |
| `dns.set_config` | Authenticated automatic or manual IPv4 DNS through `network.wan` |
| `lan.get_config_list` | Read-only LAN address and DHCP configuration |
| `lan.set_config` | Authenticated LAN IPv4 and DHCP pool/lease configuration for the `lan` interface |
| `lan.get_static_bind_list` | Read-only UCI DHCP host reservations |
| `wifi.get_config` | Read-only radio/BSS configuration without returning keys |
| `wifi.set_config` | Authenticated UCI-backed BSS and radio configuration for an existing `iface_name` |
| `wifi.get_status` | Read-only radio state, band and channel |
| `clients.get_list` | Wi-Fi associations plus optional dnsmasq/static-host records |
| `clients.get_status` | Wired/wireless counts, explicitly marking unavailable wired data |
| `firewall.get_port_forward_list` | Compatibility-owned IPv4 WAN-to-LAN redirects |
| `firewall.add_port_forward` | Authenticated firewall4/UCI IPv4 redirect creation |
| `firewall.set_port_forward` | Full replacement of an existing compatibility-owned redirect |
| `firewall.remove_port_forward` | Removal of an existing compatibility-owned redirect |

The response uses the SDK4 JSON-RPC wrapper (`call`, `sid`, module, method,
and optional arguments) and returns normalized OpenWrt data. Both the common
three-element call form (with implicit empty arguments) and the four-element
form are accepted. The exact stock response fields are not part of the public
OpenWrt interface, so fields are only reported when their local backend
provides them.

The two setters are deliberately narrower than the stock SDK4 schema. `wifi.set_config`
accepts an existing BSS selected by its unique `iface_name` and supports that BSS's
`enabled`, `ssid`, `encryption`, `key` and `hidden` options, plus the owning radio's
`channel`, `hwmode` and `htmode`. The optional `device` is checked against the selected
BSS rather than used to select an arbitrary section. The implementation resolves both
configured and runtime-generated interface names, while preserving unrelated BSSes and
radios; radio-level fields are applied only to the selected BSS's owning radio. Radio-wide
enable/disable is not exposed because the SDK4 write target for that operation is not
established; callers must use the selected BSS's `enabled` field. `hidden` is written using
OpenWrt's canonical `ignore_broadcast_ssid` option, and `hwmode`/`htmode` are restricted to
the values accepted by this tree's wireless schema. The encryption allowlist includes the
source-supported `psk3`/`psk3-mixed`/`sae-compat` aliases as well as the stock-compatible
PSK/SAE modes. A 64-character key is accepted only when it is hexadecimal, matching
OpenWrt's raw-PSK handling; shorter keys use the normal 8--63 character passphrase range.
When available, `iwinfo.freqlist` is also consulted for the selected radio's source-derived
channel list; regulatory availability remains a runtime property when that list is
unavailable. The stock bytecode also mentions a bare `ccmp` value, but this tree has no
source-backed local authentication mode for that value; it is rejected rather than being
treated as an open or downgraded network. Stock-specific portal, MAC, random-BSSID, 6 GHz
PSC, use-mode, txpower, environment and MLO fields are rejected until their OpenWrt
semantics are established. `init` is also rejected because its SDK4 lifecycle meaning is
not established here.

`lan.set_config` accepts only `interface: "lan"`, `ip`, `netmask`, `start`, `end`, `enable`
and `leasetime`. The LAN address and netmask must be supplied together when changed. `start`
and `end` are absolute IPv4 addresses; they are validated against the resulting subnet,
must identify at least two usable addresses (matching the stock validator), and are
converted to the standard OpenWrt dnsmasq `start`/`limit` offsets. The getter converts those
offsets back to absolute `start`/`end` addresses and uses the same unique LAN DHCP section
discovery as the setter. Static IPv4 subnets and dynamically configured active interfaces
are checked for overlap before a change is accepted; an unavailable or malformed active
interface status fails closed. Other SDK4 LAN fields, including DNS, guest/isolation and
transfer settings, are rejected rather than ignored. Existing UCI values and the target
sections are validated before either setter commits. A root-owned runtime lock serializes
setters, and changes to `network`/`dhcp` or `wireless` are committed as one transaction with
restoration attempted if commit or the standard ubus `network.reload` operation fails. In
this OpenWrt tree `/sbin/wifi reload` delegates to the same `ubus call network reload` path;
the response confirms reload acceptance, not completion of asynchronous hostapd/netifd
setup. A successful LAN address change can disconnect the caller; the client must reconnect
using the new address.

## DNS compatibility

The stock SDK4 bytecode exposes `dns.get_config` and `dns.set_config`. The local
Android client disassembly confirms the SDK4 field names, but this package
intentionally implements only the two modes that have an unambiguous standard
OpenWrt mapping:

* `mode: "auto"` sets `network.wan.peerdns` to `1` and removes the
  `network.wan.dns` list only when it was previously written by this layer;
* `mode: "manual"` accepts `manual_list`, a non-empty list of at most five
  valid, distinct IPv4 unicast addresses, sets `network.wan.peerdns` to `0`
  and stores the list in `network.wan.dns`. A private UCI marker records that
  ownership so automatic mode does not delete an unrelated pre-existing DNS
  list.

The getter reports `mode`, the manual list and the source-derived runtime
`server_auto` value. It does not edit `/etc/resolv.conf` or any generated
resolver file. The stock setter also accepts `rebind_protection`, `force_dns`,
`override_vpn` and secure/proxy fields, but those writes are deliberately
rejected here: `rebind_protection` is a security control and the other fields
have no equivalent in this standard OpenWrt mapping. The existing dnsmasq
rebind setting is left unchanged. A failed UCI commit or `network.reload`
returns an error and attempts to restore the original options.

## Port-forward compatibility

The stock firewall bytecode exposes `get_port_forward_list`,
`add_port_forward`, `set_port_forward`, `remove_port_forward` and
`order_port_forward`. Its recovered validator confirms the request fields
`name`, `src`, `dest`, `proto`, `src_dport`, `dest_ip`, `dest_port` and
`enabled`; the accepted protocols are exactly `tcp`, `udp` and `tcp udp`, and
ports may be a single value or one range of fewer than 100 ports. The stock
getter calls the source-port result field `src_port`, while the setter
validator calls its request field `src_dport`; this compatibility layer keeps
that distinction explicit.

The stock firmware currently applies these rules through its separate
`port_forward` configuration/module path. The captured Android v4 firewall
body proves the WAN-access methods but does not contain a direct port-forward
call site. This package therefore implements only the server-schema subset
that can be represented safely by this tree's standard firewall4 UCI backend;
runtime App behavior remains to be verified.

This package implements the list/add/set/remove subset using standard
firewall4 `firewall` UCI `redirect` sections. Only IPv4 `DNAT` redirects from
zone `wan` to zone `lan` are accepted. Destination addresses must be usable
hosts inside the configured LAN subnet and cannot be the router address.
Mapped fields are `family`, `target`, `src`, `dest`, `proto`, `src_dport`,
`dest_ip`, `dest_port` and `enabled`.

Created sections carry the `glinet_app_compat '1'` marker. Listing, updating
and deleting are limited to marked sections, so an unrelated administrator-
created redirect is neither exposed nor modified. Exact duplicates among
compatibility-owned rules are rejected; other existing redirects remain under
their original UCI ownership and ordering. UCI section names are returned as
stable rule IDs; list positions are not used as IDs. The stock `all` delete
and `order_port_forward` methods, descriptions and other fields not required
for this narrow mapping remain unsupported.

Firewall changes use `uci.cursor()` and the standard ubus `service.reload`
operation for `firewall`. Section-aware snapshots restore the complete
original section, including its UCI order, when commit or reload fails. A
created section is removed on rollback, a deleted section is recreated and an
updated section is restored before a compensating firewall reload. No raw
nftables, iptables or stock `port_forward` kernel-module commands are used.

## Deliberate non-goals

* No cloud, GoodCloud, account or telemetry integration.
* No guessed UDP/mDNS/GL-specific discovery protocol. The official local flow
  permits adding an initialized device by IP and password; discovery remains
  unsupported until its wire format is established.
* No secure/proxy DNS, DNS filtering, firewall ACLs, IPv6 pinholes, DMZ, UPnP,
  port triggering, source-IP restrictions, reflection customization or arbitrary
  nftables rules. Unknown and not-yet-proven methods return a JSON-RPC `-32601`
  error and are logged for a later, separate PR. Static DHCP bindings,
  guest-network configuration, Wi-Fi txpower/MLO/environment configuration,
  and radio-wide Wi-Fi enable/disable remain outside this focused setter
  follow-up.
* The stock `/ws` event stream remains unsupported until its local state and
  event contract are runtime-verified.
* No stock OTA advertisement or firmware upgrade initiation. Firmware data is
  identified as the running OpenWrt build and the package never accepts an
  unverified stock image.

## Security properties

* `/rpc` accepts only HTTP POST and is restricted to loopback and the configured
  LAN IPv4 subnets. IPv6 network peers, including link-local peers, are rejected
  because uhttpd does not provide reliable ingress-interface provenance.
* All state and status calls require a short-lived session, except the
  read-only `ui.check_initialized` pre-login check. Challenge data is one-use,
  source-address bound and expires after one second. Sessions expire after
  five minutes of inactivity and are source-address bound. `alive` refreshes
  only the requesting source's session; `logout` cannot terminate another
  source's session and is idempotent for an unknown or already expired SID.
* The App may include the entered password in the challenge request for SDK4
  wire compatibility. The handler never stores or logs that field; the final
  response is checked against the existing root `/etc/shadow` entry.
* Session/challenge state is kept below a root-owned `0700` runtime directory
  with `0600` files. Failed logins are rate-limited, and expired sessions,
  challenges, failure records and log-suppression records are cleaned up
  opportunistically.
* Diagnostic messages always carry the `glinet-app-compat:` prefix and never
  include passwords, password hashes, nonces, session IDs or cookie values.
  Repeated unsupported/backend and request-validation events are suppressed
  for one minute. With `log_level='debug'`, a rate-limited summary also records
  the method, argument keys or shape, authentication state, result class and
  elapsed time without recording argument values.

Package removal removes this package's `/rpc` ucode prefix from the uhttpd
configuration. Other uhttpd prefixes are left unchanged.

The endpoint is not a replacement for HTTPS. A deployment that exposes the
router beyond its trusted LAN must put it behind an authenticated TLS
terminator and keep the LAN guard enabled.

The stock `gl-session` implementation is not part of the public OpenWrt or
GL.iNet source reviewed for this package. The authentication path therefore
matches the documented SDK4 wire shape and the independently observed client
calculation, but has not been runtime-tested against the stock App. The
extracted GL-BE9300 stock gateway does establish the top-level lifecycle
shape: `alive` passes its `sid` object to `gl-session.touch`, while `logout`
passes it to `gl-session.logout` and returns a JSON `null` result. The local
implementation keeps the stronger source-bound ownership checks used by this
compatibility layer.

## Local-management layers

The local-management stack provides authenticated, validated setters for:

* system timezone and reboot (`system.set_timezone_config`,
  `system.reboot`);
* Wi-Fi BSS configuration (`wifi.set_config`);
* LAN/DHCP configuration (`lan.set_config`);
* WAN DNS mode and manual IPv4 resolvers (`dns.set_config`);
* owned IPv4 WAN-to-LAN port forwards (`firewall.add_port_forward`,
  `firewall.set_port_forward`, and `firewall.remove_port_forward`).

The corresponding getters expose the resulting OpenWrt configuration. The
system setter rejects unknown fields, invalid zone names and malformed boolean
values; no hostname setter is included because no exact SDK4 setter schema was
established in the source audit. The package does not claim full GL.iNet App
support; VPN, cloud, discovery, firmware-update and other unsupported methods
remain outside this stack.

## Diagnostics

The endpoint logs with the `glinet-app-compat:` prefix. To follow those logs:

    logread -f | grep glinet-app-compat

For temporary diagnostic verbosity, use UCI and reload uhttpd:

    uci set glinet-app-compat.main.log_level='debug'
    uci commit glinet-app-compat
    /etc/init.d/uhttpd reload

Debug records include a non-secret worker-local `req=<number>` correlation ID,
module/method, argument key names or shape, source/authentication class, backend
result class and duration. Repeated events remain suppressed for one minute.
Rollback success/failure is logged without configuration values.

For a read-only runtime preflight and a sanitized post-test bundle:

    /usr/libexec/glinet-app-compat-check
    /usr/libexec/glinet-app-compat-support /tmp/glinet-app-compat-support.txt

The complete manual sequence, expected UCI/ubus/runtime observations, source
guard checks and official-App evidence checklist is in
`RUNTIME-VALIDATION.md`. The support script never uploads data and never reads
`/etc/shadow` or session/challenge state.
