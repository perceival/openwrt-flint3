/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2025 StarField Xu <air_jinkela@163.com> */
#ifndef __RTL837X_H__
#define __RTL837X_H__

#include <linux/gpio/consumer.h>
#include <linux/mutex.h>
#include <linux/of_mdio.h>
#include <linux/regmap.h>
#include <net/dsa.h>

#include "rtl837x_regmap.h"

#define MDC_MDIO_CTRL_REG   RTL837X_SMI_MDIO_CTRL_REG
#define MDC_MDIO_ADDR_REG   RTL837X_SMI_MDIO_ADDR_REG
#define MDC_MDIO_DATA_LOW   RTL837X_SMI_MDIO_DATA_LOW
#define MDC_MDIO_DATA_HIGH  RTL837X_SMI_MDIO_DATA_HIGH
#define MDC_MDIO_READ_CMD   RTL837X_SMI_MDIO_READ_CMD
#define MDC_MDIO_WRITE_CMD  RTL837X_SMI_MDIO_WRITE_CMD

#define RTL8372N_NUM_PORTS 9
#define RTL8372N_PHY_PORT_MASK GENMASK(7, 4)

struct rtl837x_ops;
struct rtl837x_priv {
	struct device *dev;
	struct gpio_desc *reset;
	struct mii_bus *bus;
	struct regmap *map;
	struct mutex map_lock;
	struct mutex sds_lock;
	/* Child MII bus -> PHY/SDS engine -> regmap -> parent MII bus. */
	struct mutex phy_lock;
	u16 phy_page[RTL8372N_NUM_PORTS];
	int mdio_addr;
	unsigned int num_ports;
	struct dsa_switch *ds;
	const struct rtl837x_ops *ops;
	int (*write_reset)(struct rtl837x_priv *priv);
	void *chip_data;
};

struct rtl837x_variant {
	const struct dsa_switch_ops *ds_ops_mdio;
	const struct rtl837x_ops *ops;
	const struct phylink_mac_ops *pl_mac_ops;
	size_t chip_data_sz;
};

struct rtl837x_ops {
	int (*detect)(struct rtl837x_priv *priv);
	int (*phy_read_c22)(struct rtl837x_priv *priv, u16 phy, int regnum,
			    u16 *pval);
	int (*phy_write_c22)(struct rtl837x_priv *priv, u16 phy, int regnum,
			     u16 val);
	int (*phy_read_c45)(struct rtl837x_priv *priv, int phy, int devad,
			    int regnum, u16 *pval);
	int (*phy_write_c45)(struct rtl837x_priv *priv, int phy, int devad,
			     int regnum, u16 val);
};

#define rtl837x_reg_read(priv, reg, pval) regmap_read((priv)->map, (reg), (pval))
#define rtl837x_reg_write(priv, reg, val) regmap_write((priv)->map, (reg), (val))

int rtl837x_reg_bits_read(struct rtl837x_priv *priv, u32 reg, u32 mask,
			  u32 *pval);
int rtl837x_reg_bits_write(struct rtl837x_priv *priv, u32 reg, u32 mask,
			   u32 val);
int rtl837x_sds_reg_bits_write(struct rtl837x_priv *priv, u8 sds, u8 page,
			       u8 reg, u16 mask, bool set);
int rtl837x_phy_read_ocp(struct rtl837x_priv *priv, u16 phy, int regnum,
			 u16 *pval);
int rtl837x_phy_write_ocp(struct rtl837x_priv *priv, u16 phy, int regnum,
			  u16 val);
int rtl837x_phy_read_c22(struct rtl837x_priv *priv, u16 phy, int regnum,
			 u16 *pval);
int rtl837x_phy_write_c22(struct rtl837x_priv *priv, u16 phy, int regnum,
			  u16 val);
int rtl837x_phy_read_c45(struct rtl837x_priv *priv, int phy, int devad,
			 int regnum, u16 *pval);
int rtl837x_phy_write_c45(struct rtl837x_priv *priv, int phy, int devad,
			  int regnum, u16 val);
int rtl837x_phys_write_c45(struct rtl837x_priv *priv, u16 phy_mask,
			   int devad, int regnum, u16 val);

bool rtl8372n_phy_bus_match(struct mii_bus *bus);
int rtl8372n_phy_device_match(struct device *dev,
			      const struct device_driver *driver);
extern struct phy_driver rtl8372n_phy_driver;
extern const struct rtl837x_variant rtl8372n_variant;

#endif /* __RTL837X_H__ */
