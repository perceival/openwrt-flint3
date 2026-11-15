// SPDX-License-Identifier: GPL-2.0-or-later
/* Copyright (C) 2025 StarField Xu <air_jinkela@163.com> */
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/bitops.h>
#include <linux/errno.h>
#include <linux/gpio/consumer.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_net.h>
#include <linux/overflow.h>
#include <linux/regmap.h>
#include <linux/slab.h>

#include "rtl837x.h"

static int rtl837x_mdio_wait_idle(struct mii_bus *bus, int addr)
{
	int value, i;

	for (i = 0; i < 1000; i++) {
		value = bus->read(bus, addr, MDC_MDIO_CTRL_REG);
		if (value < 0)
			return value;
		if (!(value & RTL837X_SMI_MDIO_BUSY))
			return 0;

		usleep_range(10, 20);
	}

	return -ETIMEDOUT;
}

static int rtl837x_mdio_write_command(struct rtl837x_priv *priv, u32 reg,
				      u32 val, bool wait)
{
	struct mii_bus *bus = priv->bus;
	int ret;

	mutex_lock_nested(&bus->mdio_lock, MDIO_MUTEX_NESTED);
	ret = rtl837x_mdio_wait_idle(bus, priv->mdio_addr);
	if (ret)
		goto out;
	ret = bus->write(bus, priv->mdio_addr, MDC_MDIO_ADDR_REG, reg);
	if (ret < 0)
		goto out;
	ret = bus->write(bus, priv->mdio_addr, MDC_MDIO_DATA_LOW, val & 0xffff);
	if (ret < 0)
		goto out;
	ret = bus->write(bus, priv->mdio_addr, MDC_MDIO_DATA_HIGH,
			 (val >> 16) & 0xffff);
	if (ret < 0)
		goto out;
	ret = bus->write(bus, priv->mdio_addr, MDC_MDIO_CTRL_REG,
			 MDC_MDIO_WRITE_CMD);
	if (ret < 0)
		goto out;
	if (wait)
		ret = rtl837x_mdio_wait_idle(bus, priv->mdio_addr);
out:
	mutex_unlock(&bus->mdio_lock);
	return ret;
}

static int rtl837x_mdio_write(void *ctx, u32 reg, u32 val)
{
	return rtl837x_mdio_write_command(ctx, reg, val, true);
}

static int rtl837x_mdio_write_reset(struct rtl837x_priv *priv)
{
	int ret;

	/* A reset may stop the register engine before its completion poll.
	 * Serialize with regmap users, issue the command, then let chip setup
	 * wait for reset readiness. MDIO staging errors are still returned.
	 */
	mutex_lock(&priv->map_lock);
	ret = rtl837x_mdio_write_command(priv, RTL837X_REG_RESET,
					RTL837X_RESET_SOC, false);
	mutex_unlock(&priv->map_lock);
	return ret;
}

static int rtl837x_mdio_read(void *ctx, u32 reg, u32 *val)
{
	struct rtl837x_priv *priv = ctx;
	struct mii_bus *bus = priv->bus;
	int low, high, ret;

	mutex_lock_nested(&bus->mdio_lock, MDIO_MUTEX_NESTED);
	ret = rtl837x_mdio_wait_idle(bus, priv->mdio_addr);
	if (ret)
		goto out;
	ret = bus->write(bus, priv->mdio_addr, MDC_MDIO_ADDR_REG, reg);
	if (ret < 0)
		goto out;
	ret = bus->write(bus, priv->mdio_addr, MDC_MDIO_CTRL_REG,
			 MDC_MDIO_READ_CMD);
	if (ret < 0)
		goto out;
	ret = rtl837x_mdio_wait_idle(bus, priv->mdio_addr);
	if (ret)
		goto out;

	low = bus->read(bus, priv->mdio_addr, MDC_MDIO_DATA_LOW);
	if (low < 0) {
		ret = low;
		goto out;
	}
	high = bus->read(bus, priv->mdio_addr, MDC_MDIO_DATA_HIGH);
	if (high < 0) {
		ret = high;
		goto out;
	}

	*val = (low & 0xffff) | ((u32)(high & 0xffff) << 16);
	ret = 0;
out:
	mutex_unlock(&bus->mdio_lock);
	return ret;
}

static void rtl837x_mdio_lock(void *ctx)
{
	struct rtl837x_priv *priv = ctx;

	mutex_lock(&priv->map_lock);
}

static void rtl837x_mdio_unlock(void *ctx)
{
	struct rtl837x_priv *priv = ctx;

	mutex_unlock(&priv->map_lock);
}

static void rtl837x_assert_reset(void *data)
{
	struct rtl837x_priv *priv = data;

	gpiod_set_value_cansleep(priv->reset, 1);
}

/* Reject unsupported wiring before asserting a board reset GPIO. */
static int rtl8372n_check_dt(struct device *dev)
{
	struct device_node *ports, *port, *fixed;
	phy_interface_t interface;
	u32 index, speed;
	unsigned int cpus = 0, users = 0;
	u16 seen = 0;
	int ret = -EOPNOTSUPP;

	ports = of_get_child_by_name(dev->of_node, "ports");
	if (!ports)
		return -EINVAL;
	for_each_available_child_of_node(ports, port) {
		if (of_property_read_u32(port, "reg", &index) ||
		    index >= RTL8372N_NUM_PORTS || (seen & BIT(index)) ||
		    of_get_phy_mode(port, &interface) ||
		    of_find_property(port, "link", NULL))
			goto out_port;
		if (of_find_property(port, "ethernet", NULL)) {
			if ((index != 3 && index != 8) ||
			    interface != PHY_INTERFACE_MODE_10GBASER)
				goto out_port;
			fixed = of_get_child_by_name(port, "fixed-link");
			if (!fixed)
				goto out_port;
			ret = of_property_read_u32(fixed, "speed", &speed);
			if (!ret && (speed != SPEED_10000 ||
				     !of_property_read_bool(fixed, "full-duplex")))
				ret = -EOPNOTSUPP;
			of_node_put(fixed);
			if (ret)
				goto out_port;
			cpus++;
		} else {
			if (index < 4 || index > 7 ||
			    interface != PHY_INTERFACE_MODE_INTERNAL)
				goto out_port;
			users++;
		}
		seen |= BIT(index);
		ret = -EOPNOTSUPP;
	}
	ret = cpus == 1 && users ? 0 : -EOPNOTSUPP;
	goto out;
out_port:
	of_node_put(port);
out:
	of_node_put(ports);
	return ret;
}

static int rtl837x_mdio_probe(struct mdio_device *mdiodev)
{
	struct device *dev = &mdiodev->dev;
	const struct rtl837x_variant *variant;
	struct rtl837x_priv *priv;
	const struct regmap_config regmap_template = {
		.reg_bits = 16,
		.val_bits = 32,
		.reg_stride = 4,
		.max_register = 0xffff,
		.reg_format_endian = REGMAP_ENDIAN_BIG,
		.reg_read = rtl837x_mdio_read,
		.reg_write = rtl837x_mdio_write,
		.cache_type = REGCACHE_NONE,
		.lock = rtl837x_mdio_lock,
		.unlock = rtl837x_mdio_unlock,
	};
	struct regmap_config regmap_config = regmap_template;
	int ret;

	variant = of_device_get_match_data(dev);
	if (!variant)
		return -EINVAL;
	ret = rtl8372n_check_dt(dev);
	if (ret)
		return dev_err_probe(dev, ret, "unsupported P0 device tree\n");

	priv = devm_kzalloc(dev, size_add(sizeof(*priv), variant->chip_data_sz),
			    GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	regmap_config.lock_arg = priv;
	mutex_init(&priv->map_lock);
	mutex_init(&priv->sds_lock);
	mutex_init(&priv->phy_lock);
	priv->map = devm_regmap_init(dev, NULL, priv, &regmap_config);
	if (IS_ERR(priv->map)) {
		ret = PTR_ERR(priv->map);
		dev_err(dev, "regmap init failed: %d\n", ret);
		return ret;
	}

	priv->mdio_addr = mdiodev->addr;
	priv->bus = mdiodev->bus;
	priv->dev = dev;
	priv->chip_data = (void *)(priv + 1);
	priv->ops = variant->ops;
	priv->write_reset = rtl837x_mdio_write_reset;

	/* Logical active means asserted, including active-low GPIOs. */
	priv->reset = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(priv->reset)) {
		ret = PTR_ERR(priv->reset);
		dev_err(dev, "failed to get reset GPIO: %d\n", ret);
		return ret;
	}
	if (priv->reset) {
		ret = devm_add_action_or_reset(dev, rtl837x_assert_reset, priv);
		if (ret)
			return ret;
		usleep_range(50000, 51100);
		gpiod_set_value_cansleep(priv->reset, 0);
		usleep_range(50000, 51100);
	}

	ret = priv->ops->detect(priv);
	if (ret) {
		dev_err(dev, "unable to detect RTL8372N: %d\n", ret);
		return ret;
	}

	priv->ds = devm_kzalloc(dev, sizeof(*priv->ds), GFP_KERNEL);
	if (!priv->ds)
		return -ENOMEM;

	priv->ds->dev = dev;
	priv->ds->num_ports = priv->num_ports;
	priv->ds->priv = priv;
	priv->ds->ops = variant->ds_ops_mdio;
	priv->ds->phylink_mac_ops = variant->pl_mac_ops;

	ret = dsa_register_switch(priv->ds);
	if (ret) {
		if (priv->reset)
			gpiod_set_value_cansleep(priv->reset, 1);
		return dev_err_probe(dev, ret, "unable to register switch\n");
	}

	dev_set_drvdata(dev, priv);
	return 0;
}

static void rtl837x_mdio_remove(struct mdio_device *mdiodev)
{
	struct rtl837x_priv *priv = dev_get_drvdata(&mdiodev->dev);

	if (!priv)
		return;

	dsa_unregister_switch(priv->ds);
	if (priv->reset)
		gpiod_set_value_cansleep(priv->reset, 1);
}

static void rtl837x_mdio_shutdown(struct mdio_device *mdiodev)
{
	struct rtl837x_priv *priv = dev_get_drvdata(&mdiodev->dev);

	if (priv) {
		dsa_switch_shutdown(priv->ds);
		dev_set_drvdata(&mdiodev->dev, NULL);
	}
}

static const struct of_device_id rtl837x_mdio_match[] = {
	{ .compatible = "realtek,rtl8372n", .data = &rtl8372n_variant },
	{ }
};
MODULE_DEVICE_TABLE(of, rtl837x_mdio_match);

static struct mdio_driver rtl837x_mdio_driver = {
	.mdiodrv.driver = {
		.name = "rtl8372n-mdio",
		.of_match_table = rtl837x_mdio_match,
	},
	.probe = rtl837x_mdio_probe,
	.remove = rtl837x_mdio_remove,
	.shutdown = rtl837x_mdio_shutdown,
};
static int __init rtl8372n_init(void)
{
	int ret;

	/* Register the private PHY driver before creating its child bus. */
	ret = phy_drivers_register(&rtl8372n_phy_driver, 1, THIS_MODULE);
	if (ret)
		return ret;
	ret = mdio_driver_register(&rtl837x_mdio_driver);
	if (ret)
		phy_drivers_unregister(&rtl8372n_phy_driver, 1);
	return ret;
}
module_init(rtl8372n_init);

static void __exit rtl8372n_exit(void)
{
	mdio_driver_unregister(&rtl837x_mdio_driver);
	phy_drivers_unregister(&rtl8372n_phy_driver, 1);
}
module_exit(rtl8372n_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("StarField Xu <air_jinkela@163.com>");
MODULE_DESCRIPTION("RTL8372N DSA switch driver");
