// SPDX-License-Identifier: GPL-2.0-only
/* RTL8372N private-bus PHY support.
 * Register meanings are documented in Linux realtek_main.c and mdio.h;
 * page/OCP mapping is documented in Linux r8169_main.c. See PROVENANCE.md.
 */
#include <linux/ethtool.h>
#include <linux/mdio.h>
#include <linux/phy.h>

#include "rtl837x.h"

static int rtl8372n_phy_match(struct phy_device *phydev,
			    const struct phy_driver *driver)
{
	return rtl8372n_phy_bus_match(phydev->mdio.bus) &&
	       phydev->mdio.addr >= 4 && phydev->mdio.addr <= 7;
}

/* Install before device_add(): a PHY ID match has no driver priority. */
int rtl8372n_phy_device_match(struct device *dev,
			      const struct device_driver *driver)
{
	return driver == &rtl8372n_phy_driver.mdiodrv.driver &&
	       rtl8372n_phy_match(to_phy_device(dev), &rtl8372n_phy_driver);
}

/* phylib already holds the child bus lock during page selection/restoration. */
static int rtl8372n_phy_read_page(struct phy_device *phydev)
{
	return __phy_read(phydev, 31);
}

static int rtl8372n_phy_write_page(struct phy_device *phydev, int page)
{
	return __phy_write(phydev, 31, page);
}

static int rtl8372n_phy_read_mmd(struct phy_device *phydev, int devad, u16 regnum)
{
	struct rtl837x_priv *priv = phydev->mdio.bus->priv;
	u16 value;
	int ret;

	ret = rtl837x_phy_read_c45(priv, phydev->mdio.addr, devad, regnum, &value);
	if (ret)
		dev_err_ratelimited(priv->dev,
				    "private PHY MMD read failed: port=%d devad=%d reg=0x%04x err=%d\n",
				    phydev->mdio.addr, devad, regnum, ret);
	return ret ? ret : value;
}

static int rtl8372n_phy_write_mmd(struct phy_device *phydev, int devad,
				u16 regnum, u16 value)
{
	return rtl837x_phy_write_c45(phydev->mdio.bus->priv, phydev->mdio.addr,
				     devad, regnum, value);
}

static int rtl8372n_phy_get_features(struct phy_device *phydev)
{
	int ret, value;

	phydev_info(phydev, "private PHY feature probe: id=0x%08x clause=%s\n",
		    phydev->phy_id, phydev->is_c45 ? "C45" : "C22");
	ret = genphy_read_abilities(phydev);
	if (ret) {
		phydev_err(phydev, "C22 ability read failed: %d\n", ret);
		return ret;
	}
	/* Same published capability predicate as Realtek Internal NBASE-T. */
	value = phy_read_paged(phydev, 0xa61, 0x13);
	if (value < 0) {
		phydev_err(phydev, "2.5G capability read failed: %d\n", value);
		return value;
	}
	phydev_info(phydev, "private PHY 2.5G capability: value=0x%04x\n", value);
	linkmode_mod_bit(ETHTOOL_LINK_MODE_2500baseT_Full_BIT,
			phydev->supported, value & MDIO_PMA_SPEED_2_5G);
	return 0;
}

static int rtl8372n_phy_config_aneg(struct phy_device *phydev)
{
	int changed;
	u16 advertise;

	/* BMCR cannot represent forced 2.5G; keep that mode unsupported in P0. */
	if (phydev->autoneg == AUTONEG_DISABLE) {
		if (phydev->speed == SPEED_2500)
			return -EOPNOTSUPP;
		return genphy_config_aneg(phydev);
	}
	advertise = linkmode_test_bit(ETHTOOL_LINK_MODE_2500baseT_Full_BIT,
				     phydev->advertising) ?
		    MDIO_AN_10GBT_CTRL_ADV2_5G : 0;
	changed = phy_modify_mmd_changed(phydev, MDIO_MMD_AN,
					MDIO_AN_10GBT_CTRL,
					MDIO_AN_10GBT_CTRL_ADV2_5G, advertise);
	if (changed < 0)
		return changed;
	return __genphy_config_aneg(phydev, changed > 0);
}

static int rtl8372n_phy_read_status(struct phy_device *phydev)
{
	int ret, value;

	ret = genphy_read_status(phydev);
	if (ret)
		goto failed;
	if (!phydev->link)
		return 0;
	if (phydev->autoneg == AUTONEG_ENABLE && phydev->autoneg_complete &&
	    linkmode_test_bit(ETHTOOL_LINK_MODE_2500baseT_Full_BIT,
			      phydev->supported)) {
		value = phy_read_mmd(phydev, MDIO_MMD_AN, MDIO_AN_10GBT_STAT);
		if (value < 0) {
			ret = value;
			goto failed;
		}
		linkmode_mod_bit(ETHTOOL_LINK_MODE_2500baseT_Full_BIT,
				phydev->lp_advertising,
				value & MDIO_AN_10GBT_STAT_LP2_5G);
	}
	value = phy_read_paged(phydev, 0xa43, 0x12);
	if (value < 0) {
		ret = value;
		goto failed;
	}
	/* Published Realtek PHYSR speed encoding; restrict to internal 2.5G PHYs. */
	switch (value & (GENMASK(10, 9) | GENMASK(5, 4))) {
	case 0:
		phydev->speed = SPEED_10;
		break;
	case 0x10:
		phydev->speed = SPEED_100;
		break;
	case 0x20:
		phydev->speed = SPEED_1000;
		break;
	case 0x210:
		phydev->speed = SPEED_2500;
		break;
	default:
		ret = -EIO;
		goto failed;
	}
	phydev->duplex = value & BIT(3) ? DUPLEX_FULL : DUPLEX_HALF;
	if (phydev->speed == SPEED_2500 && phydev->duplex != DUPLEX_FULL) {
		ret = -EIO;
		goto failed;
	}
	phydev->master_slave_state = phydev->speed >= SPEED_1000 ?
		(value & BIT(11) ? MASTER_SLAVE_STATE_MASTER :
		 MASTER_SLAVE_STATE_SLAVE) : MASTER_SLAVE_STATE_UNSUPPORTED;
	return 0;

failed:
	phydev->link = false;
	phydev->speed = SPEED_UNKNOWN;
	phydev->duplex = DUPLEX_UNKNOWN;
	phydev->master_slave_state = MASTER_SLAVE_STATE_UNKNOWN;
	phydev->pause = false;
	phydev->asym_pause = false;
	return ret;
}

struct phy_driver rtl8372n_phy_driver = {
	.name = "RTL8372N internal PHY (P0)",
	.flags = PHY_IS_INTERNAL,
	.match_phy_device = rtl8372n_phy_match,
	.read_page = rtl8372n_phy_read_page,
	.write_page = rtl8372n_phy_write_page,
	.get_features = rtl8372n_phy_get_features,
	.config_aneg = rtl8372n_phy_config_aneg,
	.read_status = rtl8372n_phy_read_status,
	.read_mmd = rtl8372n_phy_read_mmd,
	.write_mmd = rtl8372n_phy_write_mmd,
	.suspend = genphy_suspend,
	.resume = genphy_resume,
};
