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
| `cable.get_status` | Normalized `wan` interface/link state when present |
| `lan.get_config_list` | Read-only LAN address and DHCP configuration |
| `lan.get_static_bind_list` | Read-only UCI DHCP host reservations |
| `wifi.get_config` | Read-only radio/BSS configuration without returning keys |
| `wifi.get_status` | Read-only radio state, band and channel |
| `clients.get_list` | Wi-Fi associations plus optional dnsmasq/static-host records |
| `clients.get_status` | Wired/wireless counts, explicitly marking unavailable wired data |

The response uses the SDK4 JSON-RPC wrapper (`call`, `sid`, module, method,
and optional arguments) and returns normalized OpenWrt data. Both the common
three-element call form (with implicit empty arguments) and the four-element
form are accepted. The exact stock response fields are not part of the public
OpenWrt interface, so fields are only reported when their local backend
provides them.

## Deliberate non-goals

* No cloud, GoodCloud, account or telemetry integration.
* No guessed UDP/mDNS/GL-specific discovery protocol. The official local flow
  permits adding an initialized device by IP and password; discovery remains
  unsupported until its wire format is established.
* No setters, reboot, password, DNS, firewall, VPN, Multi-WAN or AdGuard
  methods in this core package. Unknown and not-yet-proven methods return a
  JSON-RPC `-32601` error and are logged for a later, separate PR.
* The stock `/ws` event stream remains unsupported until its local state and
  event contracts are runtime-verified.
* No stock OTA advertisement or firmware upgrade initiation. Firmware data is
  identified as the running OpenWrt build and the package never accepts an
  unverified stock image.

## Security properties

* `/rpc` accepts only HTTP POST and is restricted to loopback, the configured
  LAN IPv4 subnets, and IPv6 link-local peers.
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

## Local-management follow-up

This local-management follow-up adds only two authenticated methods
whose wire names and local behavior are source-proven by the stock SDK4
system RPC and standard OpenWrt configuration lifecycle:

| RPC | Result |
| --- | --- |
| `system.set_timezone_config` | Validates and commits `timezone`, `zonename` and optional `autotimezone` in the existing `system` section, then reloads system configuration |
| `system.reboot` | Requests the standard OpenWrt ubus `system.reboot` action |

The follow-up rejects unknown fields, invalid zone names and malformed boolean
values. It does not add a hostname setter because no exact SDK4 setter schema
was established in the source audit. Wi-Fi, LAN/DHCP, DNS and firewall setters
remain separate future work until their complete argument and rollback
semantics are source-proven.

## Diagnostics

The endpoint logs with the `glinet-app-compat:` prefix. To follow those logs:

    logread -f | grep glinet-app-compat

For temporary diagnostic verbosity, use UCI and reload uhttpd:

    uci set glinet-app-compat.main.log_level='debug'
    uci commit glinet-app-compat
    /etc/init.d/uhttpd reload
