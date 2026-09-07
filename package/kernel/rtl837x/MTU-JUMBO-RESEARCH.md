# RTL8373 MTU and jumbo-frame offload research

Status: research-only. This note deliberately does not add a DSA MTU callback
or change any RTL8373 register.

## Question

Can the RTL837x DSA driver implement `port_change_mtu` and
`port_max_mtu` with an exact Linux-to-hardware frame-length mapping for
ordinary and jumbo frames?

## Evidence in the current RTL8373 SDK

The chip-specific DAL provides `dal_rtl8373_portMaxLen_set()` and
`dal_rtl8373_portMaxLen_get()`. Their own comments describe these as *port RX
max length* operations. They select two speed buckets: FE/10M/100M and
1G/2.5G/5G/10G. The generated register definitions expose two 14-bit RX
fields in `MAC_L2_PORT_MAX_LEN_CTRL_ADDR(port)`, one for each speed bucket,
and a separate per-port TX max-length register, but no RTL8373 DAL set/get API
for the TX register was found.

The same DAL file has `portMaxLenIncTag_{set,get}()` for a generic tag-length
adjustment. The SDK does not establish whether that bit accounts for the
`tag_8021q` transport used by this DSA driver, which tags are included, or how
the adjustment interacts with Linux's MTU and FCS accounting.

The generic RTK API declares profile-style max-packet-length operations
(`rtk_switch_portMaxPktLen_{set,get}()` and
`rtk_switch_maxPktLenCfg_{set,get}()`), but the RTL8373 mapper leaves those
entries disabled and no RTL8373 profile implementation was found. Therefore
the generic API cannot currently be used as a proven DSA MTU abstraction.

## Other repository evidence

The public [RTLPlayground RTL8372/3 firmware](https://github.com/logicog/RTLPlayground)
reads and writes `0x1250 + (port << 8)`, masks the 14-bit RX value, accepts
64..16383, and presents example choices such as 1522, 1536, 1552, 9216, and
16383. This confirms that a per-port RX frame-length control is usable on
related RTL837x hardware. It does not document DSA MTU semantics, the TX
register, CPU-port propagation, `tag_8021q` overhead, or rollback behavior.

The Linux DSA Realtek RTL8365MB reference in OpenWrt programs a global RX
maximum and converts `new_mtu` to `new_mtu + VLAN_ETH_HLEN + ETH_FCS_LEN`.
That is useful evidence for the shape of a DSA implementation, but it cannot
be copied to RTL8373: the chips expose different RX/TX controls and the
RTL8373 driver uses a switch-internal `tag_8021q` path.

Because this driver does not currently provide `port_max_mtu`, the generic DSA
fallback advertises the ordinary `ETH_DATA_LEN` (1500-byte) user-port MTU.
That software limit is independent of the larger values visible in the
bootloader-configured RX register; documenting the register alone therefore
does not enable jumbo frames through DSA.

## Unresolved contract

Before implementing a callback, the following must be established by an
authoritative programming guide or hardware readback/traffic tests:

1. Whether Linux MTU controls the RX limit, TX limit, or both on RTL8373.
2. Whether the CPU port and every user port need the same value, and how DSA
   aggregates per-port MTUs.
3. The exact conversion for VLAN headers, FCS, CPU/tag_8021q metadata, and the
   `portMaxLenIncTag` bit.
4. Which speed bucket must be programmed for each port and what happens when a
   port changes speed.
5. The valid TX field range and whether the TX register is actually enforced
   for all RTL8373 MAC modes.
6. Reset/default values, readback behavior, and the required rollback if one
   of several port updates fails.

## Decision

Do not add `port_change_mtu` or `port_max_mtu` yet. A callback that writes only
the known RX field could accept a Linux jumbo MTU while TX or CPU-injected
frames still use a different limit, and could miscount the DSA transport tag.
That would make the advertised DSA MTU semantics unreliable.

The next safe step is a bench matrix that records RX and TX register readback
and actual pass/drop behavior for 1522, 1536, 1552, 9216, and the maximum
14-bit value on a user port and the CPU port, with `tag_8021q` enabled. Once
that establishes the conversion and ownership model, a separate implementation
can add the narrowest proven DSA callbacks with explicit bounds and rollback.

## References

- [RTL8373 port max-length DAL](https://github.com/perceival/openwrt-flint3/blob/flint3-be9300/package/kernel/rtl837x/src/rtk-api/dal/rtl8373/dal_rtl8373_port.c)
- [RTL8373 generated register definitions](https://github.com/perceival/openwrt-flint3/blob/flint3-be9300/package/kernel/rtl837x/src/rtk-api/dal/rtl8373/rtl8373_reg_definition.h)
- [Linux RTL8365MB DSA MTU reference](https://github.com/torvalds/linux/blob/master/drivers/net/dsa/realtek/rtl8365mb_main.c)
