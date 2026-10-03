// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 StarField Xu <air_jinkela@163.com>
 */
#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/ethtool.h>
#include <linux/iopoll.h>
#include <linux/kernel.h>
#include <linux/of.h>
#include <linux/phy.h>
#include <linux/regmap.h>
#include <linux/of_net.h>

#include "rtl837x.h"

#define IS_SERDES_PORT(port) (((port) == 3) || ((port) == 8))

#define PORT_TO_SERDES_IDX(port) ((port) == 3 ? 0 : 1)

struct rtl8372n_pcs {
	struct phylink_pcs pcs;
	struct rtl837x_priv *priv;
	int index;
};

struct rtl8372n {
	struct rtl8372n_pcs pcs[RTL8372N_NUM_PORTS];
};

static int rtl8372n_detect(struct rtl837x_priv *priv)
{
	u32 chip_id;
	int ret;

	ret = rtl837x_reg_read(priv, RTL837X_REG_CHIP_ID, &chip_id);
	if (ret)
		return ret;

	dev_info(priv->dev, "chip ID: 0x%08x\n", chip_id);
	if ((chip_id >> 8) != 0x837270)
		return -ENODEV;

	priv->num_ports = RTL8372N_NUM_PORTS;
	return 0;
}

static int rtl8372n_soft_reset_chip(struct rtl837x_priv *priv)
{
	u32 tmp;
	int ret;

	ret = priv->write_reset(priv);
	if (ret)
		return ret;

	msleep(250);
	return regmap_read_poll_timeout(priv->map, RTL837X_REG_RESET, tmp,
					!(tmp & RTL837X_RESET_SOC),
					20000, 1000000);

}

static enum dsa_tag_protocol rtl8372n_get_tag_protocol(struct dsa_switch *ds,
							int port,
							enum dsa_tag_protocol mp)
{
	return DSA_TAG_PROTO_RTL8_4;
}

static int rtl8372n_mdio_phy_read_c22(struct mii_bus *bus, int addr, int regnum)
{
	struct rtl837x_priv *priv = bus->priv;
	u16 val;

	int ret = priv->ops->phy_read_c22(priv, addr, regnum, &val);

	if (ret)
		return ret;

	return val;
}

static int rtl8372n_mdio_phy_write_c22(struct mii_bus *bus, int addr, int regnum, u16 val)
{
	struct rtl837x_priv *priv = bus->priv;

	return priv->ops->phy_write_c22(priv, addr, regnum, val);
}

static int rtl8372n_mdio_phy_read_c45(struct mii_bus *bus, int port, int devad, int regnum)
{
	struct rtl837x_priv *priv = bus->priv;
	u16 val;
	int ret = priv->ops->phy_read_c45(priv, port, devad, regnum, &val);

	if (ret)
		return ret;

	return val;
}

static int rtl8372n_mdio_phy_write_c45(struct mii_bus *bus, int port, int devad, int regnum, u16 val)
{
	struct rtl837x_priv *priv = bus->priv;

	return priv->ops->phy_write_c45(priv, port, devad, regnum, val);
}

bool rtl8372n_phy_bus_match(struct mii_bus *bus)
{
	return bus && bus->read == rtl8372n_mdio_phy_read_c22;
}

static int rtl8372n_setup_mdio(struct rtl837x_priv *priv)
{
	struct device_node *np = priv->dev->of_node;
	struct device_node *mnp;
	struct dsa_switch *ds = priv->ds;
	struct device *dev = priv->dev;
	struct mii_bus *bus;
	struct phy_device *phy;
	int port, ret = 0;

	mnp = of_get_child_by_name(np, "mdio");

	if (mnp && !of_device_is_available(mnp)) {
		ret = -ENODEV;
		goto out;
	}

	bus = devm_mdiobus_alloc(dev);
	if (!bus) {
		ret = -ENOMEM;
		goto out;
	}

	if (!mnp)
		ds->user_mii_bus = bus;

	bus->priv = priv;
	bus->name = KBUILD_MODNAME "-mii";
	snprintf(bus->id, MII_BUS_ID_SIZE, "%s", dev_name(dev));
	bus->read = rtl8372n_mdio_phy_read_c22;
	bus->write = rtl8372n_mdio_phy_write_c22;
	bus->read_c45 = rtl8372n_mdio_phy_read_c45;
	bus->write_c45 = rtl8372n_mdio_phy_write_c45;
	bus->parent = dev;
	bus->phy_mask = ~(dsa_user_ports(ds) & RTL8372N_PHY_PORT_MASK);

	ret = devm_of_mdiobus_register(dev, bus, mnp);
	if (!ret) {
		for (port = 4; port <= 7; port++) {
			if (!(dsa_user_ports(ds) & BIT(port)))
				continue;
			phy = mdiobus_get_phy(bus, port);
			if (!phy || phy->drv != &rtl8372n_phy_driver) {
				dev_err(dev, "port %d did not bind to the private PHY driver\n",
					port);
				ret = -ENODEV;
				break;
			}
		}
	}
	if (ret) {
		if (!mnp)
			ds->user_mii_bus = NULL;
		dev_err(dev, "failed to register MDIO bus: %d\n", ret);
	}

out:
	of_node_put(mnp);
	return ret;
}

static unsigned int rtl8372n_sds_pcs_inband_caps(struct phylink_pcs *pcs,
						 phy_interface_t interface)
{
	struct rtl8372n_pcs *p = container_of(pcs, struct rtl8372n_pcs, pcs);

	if ((p->index == 3 || p->index == 8) &&
	    interface == PHY_INTERFACE_MODE_10GBASER)
		return LINK_INBAND_DISABLE;

	return 0;
}

static void rtl8372n_sds_pcs_get_state(struct phylink_pcs *pcs,
				     unsigned int neg_mode,
				     struct phylink_link_state *state)
{
	struct rtl8372n_pcs *p = container_of(pcs, struct rtl8372n_pcs, pcs);
	struct rtl837x_priv *priv = p->priv;
	u32 link, duplex, rx_pause, tx_pause, speed;
	int port = p->index;

	/* Never expose a partially read snapshot after a transport failure. */
	state->link = false;
	state->an_complete = false;
	state->speed = SPEED_UNKNOWN;
	state->duplex = DUPLEX_UNKNOWN;
	state->pause &= ~(MLO_PAUSE_RX | MLO_PAUSE_TX);
	if (rtl837x_reg_bits_read(priv, RTL837X_MAC_LINK_STATUS,
				 BIT(port), &link) || !link)
		return;
	if (rtl837x_reg_bits_read(priv, RTL837X_MAC_DUPLEX_STATUS,
				 BIT(port), &duplex) ||
	    rtl837x_reg_bits_read(priv, RTL837X_MAC_RX_PAUSE_STATUS,
				 BIT(port), &rx_pause) ||
	    rtl837x_reg_bits_read(priv, RTL837X_MAC_TX_PAUSE_STATUS,
				 BIT(port), &tx_pause) ||
	    rtl837x_reg_bits_read(priv, RTL837X_MAC_SPEED_STATUS_REG(port),
				 RTL837X_MAC_SPEED_STATUS_MASK(port), &speed))
		return;

	/* P0 exposes this PCS only for a fixed 10GBASE-R CPU link. */
	if (speed != 4 || !duplex)
		return;
	state->speed = SPEED_10000;
	state->duplex = DUPLEX_FULL;
	if (rx_pause)
		state->pause |= MLO_PAUSE_RX;
	if (tx_pause)
		state->pause |= MLO_PAUSE_TX;
	state->an_complete = true;
	state->link = true;
}

static int rtl8372n_sds_pcs_config(struct phylink_pcs *pcs,
				   unsigned int neg_mode,
				   phy_interface_t interface,
				   const unsigned long *advertising,
				   bool permit_pause_to_mac)
{
	struct rtl8372n_pcs *p = container_of(pcs, struct rtl8372n_pcs, pcs);
	struct rtl837x_priv *priv = p->priv;
	u32 mask;

	if (interface != PHY_INTERFACE_MODE_10GBASER)
		return -EOPNOTSUPP;
	if (p->index != 3 && p->index != 8)
		return -EINVAL;

	mask = p->index == 3 ? RTL837X_SDS0_MODE_MASK : RTL837X_SDS1_MODE_MASK;
	return rtl837x_reg_bits_write(priv, RTL837X_SDS_MODE_SELECT, mask,
				      RTL837X_SDS_MODE_10GBASE_R);
}

static void rtl8372n_sds_pcs_link_up(struct phylink_pcs *pcs, unsigned int neg_mode,
			    phy_interface_t interface, int speed, int duplex)
{
	struct rtl8372n_pcs *_pcs = container_of(pcs, struct rtl8372n_pcs, pcs);
	struct rtl837x_priv *priv = _pcs->priv;
	int port = _pcs->index;

	dev_dbg(priv->dev, "[%s]PCS link up serdes (%d) mode (%s)\n", __func__,
			  PORT_TO_SERDES_IDX(port),
			  phy_modes(interface));
}

static const struct phylink_pcs_ops rtl8372n_sds_pcs_ops = {
	.pcs_inband_caps = rtl8372n_sds_pcs_inband_caps,
	.pcs_get_state = rtl8372n_sds_pcs_get_state,
	.pcs_config = rtl8372n_sds_pcs_config,
	.pcs_link_up = rtl8372n_sds_pcs_link_up,
};

static void rtl8372n_phylink_get_caps(struct dsa_switch *ds, int port,
				       struct phylink_config *config)
{
	if (IS_SERDES_PORT(port)) {
		__set_bit(PHY_INTERFACE_MODE_10GBASER,
			  config->supported_interfaces);
		config->mac_capabilities = MAC_10000FD |
			MAC_SYM_PAUSE | MAC_ASYM_PAUSE;
	} else if (port >= 4 && port <= 7) {
		__set_bit(PHY_INTERFACE_MODE_INTERNAL,
			  config->supported_interfaces);
		config->mac_capabilities = MAC_2500FD | MAC_1000 | MAC_100 |
			MAC_10 | MAC_SYM_PAUSE | MAC_ASYM_PAUSE;
	}
}

static struct phylink_pcs *rtl8372n_phylink_mac_select_pcs(struct phylink_config *config,
						phy_interface_t interface)
{
	struct dsa_port *dp = dsa_phylink_to_port(config);
	struct rtl837x_priv *priv = dp->ds->priv;
	struct rtl8372n *chip_data = priv->chip_data;

	if (IS_SERDES_PORT(dp->index))
		return &(chip_data->pcs[dp->index].pcs);
	return NULL;
}

static void rtl8372n_phylink_mac_config(struct phylink_config *config, unsigned int mode,
			const struct phylink_link_state *state)
{
	struct dsa_port *dp = dsa_phylink_to_port(config);
	struct rtl837x_priv *priv = dp->ds->priv;

	if (!IS_SERDES_PORT(dp->index))
		return;
	dev_dbg(priv->dev, "[%s]: port:%d interface:%s speed:%s duplex:%s advertising:%*pb pause:0x%x\n", __func__,
			   dp->index, phy_modes(state->interface), phy_speed_to_str(state->speed),
			   phy_duplex_to_str(state->duplex), __ETHTOOL_LINK_MODE_MASK_NBITS, state->advertising, state->pause);
}

static void rtl8372n_phylink_mac_link_down(struct phylink_config *config, unsigned int mode,
				phy_interface_t interface)
{
	struct dsa_port *dp = dsa_phylink_to_port(config);
	struct rtl837x_priv *priv = dp->ds->priv;

	if (!IS_SERDES_PORT(dp->index))
		return;

	dev_dbg(priv->dev, "[%s]: port:%d interface:%s\n", __func__,
			   dp->index, phy_modes(interface));
}

static void rtl8372n_phylink_mac_link_up(struct phylink_config *config,
			struct phy_device *phy, unsigned int mode,
			phy_interface_t interface, int speed, int duplex,
			bool tx_pause, bool rx_pause)
{
	struct dsa_port *dp = dsa_phylink_to_port(config);
	struct rtl837x_priv *priv = dp->ds->priv;

	if (!IS_SERDES_PORT(dp->index))
		return;
	dev_dbg(priv->dev,
			   "[%s]: port:%d mode:%s speed:%s duplex:%s tx_pause:%d rx_pause:%d\n", __func__,
			   dp->index, phy_modes(interface), phy_speed_to_str(speed),
			   phy_duplex_to_str(duplex), tx_pause, rx_pause
			);
}

static const struct phylink_mac_ops rtl8372n_phylink_mac_ops = {
	.mac_select_pcs	= rtl8372n_phylink_mac_select_pcs,
	.mac_config	= rtl8372n_phylink_mac_config,
	.mac_link_down	= rtl8372n_phylink_mac_link_down,
	.mac_link_up	= rtl8372n_phylink_mac_link_up,
};

static int rtl8372n_set_tag_rtl(struct dsa_switch *ds)
{
	int ret;
	struct rtl837x_priv *priv = ds->priv;
	struct dsa_port *dp, *cpu_dp = NULL;

	/* This P0 implementation supports exactly one CPU port. */
	dsa_switch_for_each_cpu_port(dp, ds) {
		if (cpu_dp)
			return -EOPNOTSUPP;
		cpu_dp = dp;
	}

	if (!cpu_dp)
		return -ENODEV;

	/* Set external CPU DSA tag insert mode. */
	/*
	 *	CPU_INSERT_TO_ALL = 0,
	 *	CPU_INSERT_TO_TRAPPING,
	 *	CPU_INSERT_TO_NONE,
	 *	CPU_INSERT_END
	 */
	ret = rtl837x_reg_bits_write(priv, RTL837X_CPU_TAG_CTRL,
			  RTL837X_CPU_TAG_INSERT_MODE, 0
			);
	if (ret)
		return ret;

	/* Set external CPU port. */
	ret = rtl837x_reg_bits_write(priv, RTL837X_EXTERNAL_CPU_PORT,
			  RTL837X_EXTERNAL_CPU_PORT_MASK, cpu_dp->index
			);
	if (ret)
		return ret;

	/* Enable CPU tag. */
	ret = rtl837x_reg_bits_write(priv, RTL837X_CPU_TAG_CTRL,
			  RTL837X_CPU_TAG_ENABLE, 1
			);
	if (ret)
		return ret;

	/* Add the CPU port to the RTL8_4 tag-aware port mask. */
	ret = rtl837x_reg_bits_write(priv, RTL837X_CPU_TAG_AWARE, BIT(cpu_dp->index), 1);
	if (ret)
		return ret;

	return 0;
}

static int rtl8372n_port_set_isolation(struct rtl837x_priv *priv, int port,
					u32 mask)
{
	return rtl837x_reg_write(priv, RTL837X_PORT_ISOLATION_REG(port), mask);
}

static int rtl8372n_port_enable(struct dsa_switch *ds, int port,
			       struct phy_device *phy)
{
	struct rtl837x_priv *priv = ds->priv;
	int ret;

	if (IS_SERDES_PORT(port))
		return 0;

	ret = priv->ops->phy_write_c45(priv, port, 31, 0xa610, 0x2058);
	if (ret)
		return ret;

	return 0;
}

static void rtl8372n_port_disable(struct dsa_switch *ds, int port)
{
	struct rtl837x_priv *priv = ds->priv;
	int ret;

	if (IS_SERDES_PORT(port))
		return;

	ret = priv->ops->phy_write_c45(priv, port, 31, 0xa610, 0x2858);
	if (ret)
		dev_warn(priv->dev, "failed to power down PHY on port %d: %d\n",
			 port, ret);
}

static int rtl8372n_setup_default_vlan(struct rtl837x_priv *priv,
				       u16 members, u16 untagged)
{
	u32 vlan_word, command;
	int ret;

	vlan_word = RTL837X_VLAN_DATA_VALID |
		    FIELD_PREP(RTL837X_VLAN_MEMBER_MASK, members) |
		    FIELD_PREP(RTL837X_VLAN_UNTAG_MASK, untagged);
	ret = rtl837x_reg_write(priv, RTL837X_TABLE_WRITE_DATA0, vlan_word);
	if (ret)
		return ret;

	command = FIELD_PREP(RTL837X_TABLE_ADDRESS, 1) |
		  (RTL837X_TABLE_VLAN << 8) |
		  RTL837X_TABLE_WRITE | RTL837X_TABLE_EXECUTE;
	ret = rtl837x_reg_write(priv, RTL837X_TABLE_CTRL, command);
	if (ret)
		return ret;

	return regmap_read_poll_timeout(priv->map, RTL837X_TABLE_CTRL, command,
					!(command & RTL837X_TABLE_EXECUTE),
						10, 1000);
}

static int rtl8372n_configure_sds_polarity(struct rtl837x_priv *priv)
{
	static const char * const rx_swap[] = {
		"sds0-rx-swap", "sds1-rx-swap",
	};
	static const char * const tx_swap[] = {
		"sds0-tx-swap", "sds1-tx-swap",
	};
	struct device_node *np = priv->dev->of_node;
	int sds, ret;

	for (sds = 0; sds < ARRAY_SIZE(rx_swap); sds++) {
		if (of_property_read_bool(np, rx_swap[sds])) {
			ret = rtl837x_sds_reg_bits_write(priv, sds,
						 RTL837X_SDS_PAGE_XSG,
						 RTL837X_SDS_XSG_REG_POLARITY,
						 RTL837X_SDS_XSG_RX_POLARITY_SWAP,
						 true);
			if (ret)
				return ret;

			ret = rtl837x_sds_reg_bits_write(priv, sds,
						 RTL837X_SDS_PAGE_10GBASE_R,
						 RTL837X_SDS_10G_REG_POLARITY,
						 RTL837X_SDS_10G_RX_POLARITY_SWAP,
						 true);
			if (ret)
				return ret;
		}

		if (of_property_read_bool(np, tx_swap[sds])) {
			ret = rtl837x_sds_reg_bits_write(priv, sds,
						 RTL837X_SDS_PAGE_XSG,
						 RTL837X_SDS_XSG_REG_POLARITY,
						 RTL837X_SDS_XSG_TX_POLARITY_SWAP,
						 true);
			if (ret)
				return ret;

			ret = rtl837x_sds_reg_bits_write(priv, sds,
						 RTL837X_SDS_PAGE_10GBASE_R,
						 RTL837X_SDS_10G_REG_POLARITY,
						 RTL837X_SDS_10G_TX_POLARITY_SWAP,
						 true);
			if (ret)
				return ret;
		}
	}

	return 0;
}

static int rtl8372n_validate_topology(struct dsa_switch *ds, u16 *members)
{
	struct rtl837x_priv *priv = ds->priv;
	struct dsa_port *dp;
	phy_interface_t interface;
	unsigned int cpu_count = 0;
	int ret;

	*members = 0;
	dsa_switch_for_each_available_port(dp, ds) {
		if (dsa_port_is_dsa(dp))
			return -EOPNOTSUPP;
		ret = of_get_phy_mode(dp->dn, &interface);
		if (ret)
			return dev_err_probe(priv->dev, ret,
					     "port %d needs phy-mode\n", dp->index);
		if (dsa_port_is_cpu(dp)) {
			if (!IS_SERDES_PORT(dp->index) ||
			    interface != PHY_INTERFACE_MODE_10GBASER ||
			    !of_phy_is_fixed_link(dp->dn))
				return -EOPNOTSUPP;
			cpu_count++;
		} else if (dsa_port_is_user(dp)) {
			if (dp->index < 4 || dp->index > 7 ||
			    interface != PHY_INTERFACE_MODE_INTERNAL)
				return -EOPNOTSUPP;
		} else {
			return -EOPNOTSUPP;
		}
		*members |= BIT(dp->index);
	}
	return cpu_count == 1 && dsa_user_ports(ds) ? 0 : -EOPNOTSUPP;
}

static void rtl8372n_quiesce(struct dsa_switch *ds)
{
	struct rtl837x_priv *priv = ds->priv;
	int port, ret;

	/* Best effort on setup failure/teardown; never hide the first error. */
	for (port = 0; port < RTL8372N_NUM_PORTS; port++) {
		ret = rtl8372n_port_set_isolation(priv, port, 0);
		if (ret)
			dev_warn(priv->dev, "failed to isolate port %d: %d\n", port, ret);
	}
	ret = rtl837x_phys_write_c45(priv, RTL8372N_PHY_PORT_MASK,
				    31, 0xa610, 0x2858);
	if (ret)
		dev_warn(priv->dev, "failed to power down PHYs: %d\n", ret);
}

static int rtl8372n_setup_cpu_forwarding(struct dsa_switch *ds)
{
	static const u32 flood_regs[] = {
		RTL837X_L2_UNKNOWN_UC_FLOOD, RTL837X_L2_UNKNOWN_MC_FLOOD,
		RTL837X_IPV4_UNKNOWN_MC_FLOOD, RTL837X_IPV6_UNKNOWN_MC_FLOOD,
		RTL837X_L2_BROADCAST_FLOOD,
	};
	struct rtl837x_priv *priv = ds->priv;
	unsigned int cpu_mask = dsa_cpu_ports(ds);
	unsigned int user_mask = dsa_user_ports(ds);
	int port, i, ret;

	/* Standalone and software bridge traffic must always traverse the CPU.
	 * No bridge join callback opens a user-to-user hardware path.
	 */
	for (port = 0; port < RTL8372N_NUM_PORTS; port++) {
		u32 mask = 0;

		if (user_mask & BIT(port))
			mask = cpu_mask;
		else if (cpu_mask & BIT(port))
			mask = user_mask;
		ret = rtl8372n_port_set_isolation(priv, port, mask);
		if (ret)
			return ret;
		ret = rtl837x_reg_bits_write(priv, RTL837X_L2_LEARN_LIMIT_REG(port),
					     RTL837X_L2_LEARN_LIMIT_MASK, 0);
		if (ret)
			return ret;
	}
	for (i = 0; i < ARRAY_SIZE(flood_regs); i++) {
		ret = regmap_update_bits(priv->map, flood_regs[i],
					 RTL837X_L2_FLOOD_MASK, cpu_mask);
		if (ret)
			return ret;
	}
	return 0;
}

static int rtl8372n_setup(struct dsa_switch *ds)
{
	struct rtl837x_priv *priv = ds->priv;
	struct rtl8372n *chip_data = priv->chip_data;
	struct dsa_port *dp;
	u16 members;
	int port, ret;

	ret = rtl8372n_validate_topology(ds, &members);
	if (ret)
		return dev_err_probe(priv->dev, ret, "unsupported P0 topology\n");
	ret = rtl8372n_soft_reset_chip(priv);
	if (ret)
		return dev_err_probe(priv->dev, ret, "failed to reset switch\n");

	for (port = 3; port <= 8; port += 5) {
		chip_data->pcs[port].pcs.ops = &rtl8372n_sds_pcs_ops;
		__set_bit(PHY_INTERFACE_MODE_10GBASER,
			  chip_data->pcs[port].pcs.supported_interfaces);
		chip_data->pcs[port].priv = priv;
		chip_data->pcs[port].index = port;
	}

	ret = rtl837x_reg_bits_write(priv, RTL837X_SMI_MAC_TYPE,
				     RTL837X_SMI_MAC_PORT3_TYPE, 0);
	if (ret)
		goto fail;
	ret = rtl837x_reg_bits_write(priv, RTL837X_SMI_MAC_TYPE,
				     RTL837X_SMI_MAC_PORT8_TYPE, 0);
	if (ret)
		goto fail;
	ret = rtl837x_reg_bits_write(priv, RTL837X_SMI_PORT_POLLING,
				     RTL837X_SMI_PORT_POLLING_4_7, 0xf);
	if (ret)
		goto fail;
	ret = rtl837x_reg_bits_write(priv, RTL837X_SMI_CTRL,
				     RTL837X_SMI_MDC_ENABLE, 0x7);
	if (ret)
		goto fail;
	ret = rtl837x_reg_bits_write(priv, RTL837X_SMI_GLOBAL_CTRL,
				     RTL837X_SMI_POLLING_MASK, 0x1f8);
	if (ret)
		goto fail;
	msleep(20);

	ret = rtl8372n_configure_sds_polarity(priv);
	if (ret)
		goto fail;
	ret = rtl837x_phys_write_c45(priv, RTL8372N_PHY_PORT_MASK,
				    31, 0xa610, 0x2858);
	if (ret)
		goto fail;
	ret = rtl8372n_setup_default_vlan(priv, members, members);
	if (ret)
		goto fail;
	dsa_switch_for_each_available_port(dp, ds) {
		ret = rtl837x_reg_bits_write(priv, RTL837X_PORT_PVID_REG(dp->index),
					     RTL837X_PORT_PVID_MASK(dp->index), 1);
		if (ret)
			goto fail;
	}
	ret = rtl8372n_setup_cpu_forwarding(ds);
	if (ret)
		goto fail;
	ret = rtl837x_reg_write(priv, RTL837X_VLAN_INGRESS_CTRL, 0);
	if (ret)
		goto fail;
	ret = regmap_update_bits(priv->map, RTL837X_VLAN_INGRESS_FILTER,
				 GENMASK(RTL8372N_NUM_PORTS - 1, 0), members);
	if (ret)
		goto fail;
	ret = rtl837x_reg_write(priv, RTL837X_VLAN_EGRESS_TAG, 0);
	if (ret)
		goto fail;
	ret = rtl837x_reg_bits_write(priv, RTL837X_VLAN_CTRL,
				     RTL837X_VLAN_CTRL_FILTER, 1);
	if (ret)
		goto fail;
	ret = rtl8372n_set_tag_rtl(ds);
	if (ret)
		goto fail;
	/* Register PHYs only after forwarding and the PHY power policy are set. */
	ret = rtl8372n_setup_mdio(priv);
	if (ret)
		goto fail;
	dev_info(priv->dev, "P0 CPU-only forwarding; bridge/VLAN offload unavailable\n");
	return 0;

fail:
	rtl8372n_quiesce(ds);
	return dev_err_probe(priv->dev, ret, "switch setup failed\n");
}

static const struct dsa_switch_ops rtl8372n_switch_ops_mdio = {
	.get_tag_protocol = rtl8372n_get_tag_protocol,
	.setup = rtl8372n_setup,
	.teardown = rtl8372n_quiesce,
	.phylink_get_caps = rtl8372n_phylink_get_caps,
	.port_enable = rtl8372n_port_enable,
	.port_disable = rtl8372n_port_disable,
};

static const struct rtl837x_ops rtl8372n_ops = {
	.detect		= rtl8372n_detect,

	.phy_read_c22   = rtl837x_phy_read_c22,
	.phy_write_c22  = rtl837x_phy_write_c22,
	.phy_read_c45   = rtl837x_phy_read_c45,
	.phy_write_c45  = rtl837x_phy_write_c45,
};

const struct rtl837x_variant rtl8372n_variant = {
	.ds_ops_mdio = &rtl8372n_switch_ops_mdio,
	.ops = &rtl8372n_ops,
	.pl_mac_ops = &rtl8372n_phylink_mac_ops,
	.chip_data_sz = sizeof(struct rtl8372n),
};
