/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * RTL8372N register addresses compared with these public sources:
 * - RTLPlayground, MIT, commit f0aea3dcac056e3274fd39e1c76a7117471c37da
 * - linux-mainline-zte-zxslc-sr1010, GPL-2.0, commit
 *   07f8687248578d4be6931c665ff5d08bb6cc3d9d
 *
 * SDS indirect-command fields and polarity operations are compared with the
 * public ZTE driver and RTLPlayground's MIT register-operation traces.
 * RTLPlayground attributes the companion TX bits to OEM firmware, so that is
 * behavioral corroboration, not independent provenance. Airjinkela's driver
 * also has disclosed SDK-derived lineage. Keep the map limited to definitions
 * used here; these comparisons do not establish independent origin.
 * Do not add unverified SerDes pages or vendor PHY patch data.
 */
#ifndef __RTL837X_REGMAP_H__
#define __RTL837X_REGMAP_H__

#include <linux/bits.h>

#define RTL837X_REG_CHIP_ID                         0x0004
#define RTL837X_REG_RESET                           0x0024
#define RTL837X_RESET_SOC                           BIT(0)

#define RTL837X_SMI_PHY_PORT_SELECT                 0x6438
#define RTL837X_SMI_PHY_CTRL                        0x643c
#define RTL837X_SMI_PHY_READ_DATA                   0x6440
#define RTL837X_SMI_PHY_WRITE_DATA                  0x6444
#define RTL837X_SMI_PHY_BUSY                        BIT(0)
#define RTL837X_SMI_PHY_STATUS                      GENMASK(26, 24)
#define RTL837X_SMI_PHY_READ_CMD                    0x3
#define RTL837X_SMI_PHY_WRITE_CMD                   0x7

#define RTL837X_SMI_MAC_TYPE                        0x6330
#define RTL837X_SMI_MAC_PORT3_TYPE                  GENMASK(7, 6)
#define RTL837X_SMI_MAC_PORT8_TYPE                  GENMASK(17, 16)
#define RTL837X_SMI_PORT_POLLING                    0x6334
#define RTL837X_SMI_PORT_POLLING_4_7                GENMASK(7, 4)
#define RTL837X_SMI_GLOBAL_CTRL                     0x632c
#define RTL837X_SMI_POLLING_MASK                    GENMASK(20, 12)
#define RTL837X_SMI_CTRL                            0x6454
#define RTL837X_SMI_MDC_ENABLE                      GENMASK(14, 12)

#define RTL837X_CPU_TAG_AWARE                       0x603c
#define RTL837X_CPU_TAG_CTRL                        0x6720
#define RTL837X_CPU_TAG_ENABLE                      BIT(1)
#define RTL837X_CPU_TAG_INSERT_MODE                 GENMASK(11, 10)
#define RTL837X_EXTERNAL_CPU_PORT                   0x6724
#define RTL837X_EXTERNAL_CPU_PORT_MASK              GENMASK(3, 0)

#define RTL837X_SDS_MODE_SELECT                     0x7b20
#define RTL837X_SDS0_MODE_MASK                      GENMASK(4, 0)
#define RTL837X_SDS1_MODE_MASK                      GENMASK(9, 5)
#define RTL837X_SDS_MODE_10GBASE_R                  0x1a
#define RTL837X_SDS_INDACS_COMMAND                  0x03f8
#define RTL837X_SDS_INDACS_READ_DATA                0x03fc
#define RTL837X_SDS_INDACS_WRITE_DATA               0x0400
#define RTL837X_SDS_INDACS_EXECUTE                  BIT(15)
#define RTL837X_SDS_INDACS_WRITE                    BIT(14)
#define RTL837X_SDS_INDACS_REG                      GENMASK(11, 7)
#define RTL837X_SDS_INDACS_PAGE                     GENMASK(6, 1)
#define RTL837X_SDS_INDACS_INDEX                    BIT(0)
#define RTL837X_SDS_PAGE_XSG                        0x00
#define RTL837X_SDS_PAGE_10GBASE_R                  0x06
#define RTL837X_SDS_XSG_REG_POLARITY                0x00
#define RTL837X_SDS_10G_REG_POLARITY                0x02
#define RTL837X_SDS_XSG_TX_POLARITY_SWAP            BIT(8)
#define RTL837X_SDS_XSG_RX_POLARITY_SWAP            BIT(9)
#define RTL837X_SDS_10G_TX_POLARITY_SWAP            BIT(14)
#define RTL837X_SDS_10G_RX_POLARITY_SWAP            BIT(13)

#define RTL837X_MAC_LINK_STATUS                     0x63e8
#define RTL837X_MAC_SPEED_STATUS_0                  0x63f0
#define RTL837X_MAC_SPEED_STATUS_1                  0x63f4
#define RTL837X_MAC_DUPLEX_STATUS                   0x63f8
#define RTL837X_MAC_TX_PAUSE_STATUS                 0x63fc
#define RTL837X_MAC_RX_PAUSE_STATUS                 0x6400
#define RTL837X_MAC_SPEED_STATUS_REG(port) \
	((port) < 8 ? RTL837X_MAC_SPEED_STATUS_0 : RTL837X_MAC_SPEED_STATUS_1)
#define RTL837X_MAC_SPEED_STATUS_MASK(port) \
	(GENMASK(3, 0) << (((port) % 8) * 4))

#define RTL837X_PORT_ISOLATION_BASE                 0x50c0
#define RTL837X_PORT_ISOLATION_REG(port) \
	(RTL837X_PORT_ISOLATION_BASE + ((port) * 4))

/* Public ZTE DSA map: per-port learning limit and global flood masks. */
#define RTL837X_L2_LEARN_LIMIT_REG(port) (0x5384 + (port) * 4)
#define RTL837X_L2_LEARN_LIMIT_MASK GENMASK(12, 0)
#define RTL837X_L2_UNKNOWN_UC_FLOOD 0x5360
#define RTL837X_L2_UNKNOWN_MC_FLOOD 0x5364
#define RTL837X_IPV4_UNKNOWN_MC_FLOOD 0x5368
#define RTL837X_IPV6_UNKNOWN_MC_FLOOD 0x536c
#define RTL837X_L2_BROADCAST_FLOOD 0x5370
#define RTL837X_L2_FLOOD_MASK GENMASK(9, 0)

#define RTL837X_SMI_MDIO_CTRL_REG                   21
#define RTL837X_SMI_MDIO_BUSY                       BIT(2)
#define RTL837X_SMI_MDIO_ADDR_REG                   22
#define RTL837X_SMI_MDIO_DATA_LOW                   23
#define RTL837X_SMI_MDIO_DATA_HIGH                  24
#define RTL837X_SMI_MDIO_READ_CMD                   0x1b
#define RTL837X_SMI_MDIO_WRITE_CMD                  0x19

#define RTL837X_TABLE_CTRL                          0x5cac
#define RTL837X_TABLE_STATUS_METHOD                 0x5cb0
#define RTL837X_TABLE_WRITE_DATA0                   0x5cb8
#define RTL837X_TABLE_READ_DATA0                    0x5ccc
#define RTL837X_TABLE_VLAN                          0x03
#define RTL837X_TABLE_WRITE                         BIT(1)
#define RTL837X_TABLE_EXECUTE                       BIT(0)
#define RTL837X_TABLE_ADDRESS                       GENMASK(31, 16)
#define RTL837X_VLAN_FIELD25                        BIT(25)
#define RTL837X_VLAN_MEMBER_MASK                    GENMASK(9, 0)
#define RTL837X_VLAN_UNTAG_MASK                     GENMASK(19, 10)

#define RTL837X_VLAN_CTRL                           0x4e14
#define RTL837X_VLAN_CTRL_FILTER                    BIT(2)
#define RTL837X_VLAN_INGRESS_CTRL                   0x4e10
#define RTL837X_VLAN_INGRESS_FILTER                 0x4e18
#define RTL837X_VLAN_EGRESS_TAG                     0x6738
#define RTL837X_PORT_PVID_BASE                      0x4e1c
#define RTL837X_PORT_PVID_REG(port) \
	(RTL837X_PORT_PVID_BASE + (((port) >> 1) * 4))
#define RTL837X_PORT_PVID_MASK(port) \
	(GENMASK(11, 0) << (((port) & 1) * 12))


#endif /* __RTL837X_REGMAP_H__ */
