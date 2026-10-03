// SPDX-License-Identifier: GPL-2.0-or-later
/* RTL8372N register and internal-PHY accessors. */
#include <linux/bitfield.h>
#include <linux/errno.h>
#include <linux/bitops.h>
#include <linux/iopoll.h>
#include <linux/regmap.h>

#include "rtl837x.h"

/* Field helpers accept one contiguous mask and an unshifted value. */
static bool rtl837x_field_mask_valid(u32 mask)
{
	u32 field;

	if (!mask)
		return false;
	field = mask >> __ffs(mask);
	return !(field & (field + 1));
}

int rtl837x_reg_bits_read(struct rtl837x_priv *priv, u32 reg, u32 mask,
			  u32 *pval)
{
	u32 val;
	int ret;

	if (!rtl837x_field_mask_valid(mask) || !pval)
		return -EINVAL;

	ret = rtl837x_reg_read(priv, reg, &val);
	if (ret)
		return ret;

	*pval = (val & mask) >> __ffs(mask);
	return 0;
}

int rtl837x_reg_bits_write(struct rtl837x_priv *priv, u32 reg, u32 mask,
			   u32 val)
{
	if (!rtl837x_field_mask_valid(mask))
		return -EINVAL;
	if (val > (mask >> __ffs(mask)))
		return -ERANGE;

	return regmap_update_bits(priv->map, reg, mask, val << __ffs(mask));
}

static int rtl837x_sds_wait_idle(struct rtl837x_priv *priv, u32 *command)
{
	return regmap_read_poll_timeout(priv->map, RTL837X_SDS_INDACS_COMMAND,
					*command,
					!(*command & RTL837X_SDS_INDACS_EXECUTE),
					10, 1000);
}

static int rtl837x_sds_read_locked(struct rtl837x_priv *priv, u8 sds,
				   u8 page, u8 reg, u16 *value)
{
	u32 command, data;
	int ret;

	ret = rtl837x_sds_wait_idle(priv, &command);
	if (ret)
		return ret;

	command = FIELD_PREP(RTL837X_SDS_INDACS_INDEX, sds) |
		  FIELD_PREP(RTL837X_SDS_INDACS_PAGE, page) |
		  FIELD_PREP(RTL837X_SDS_INDACS_REG, reg) |
		  RTL837X_SDS_INDACS_EXECUTE;
	ret = rtl837x_reg_write(priv, RTL837X_SDS_INDACS_COMMAND, command);
	if (ret)
		return ret;

	ret = rtl837x_sds_wait_idle(priv, &command);
	if (ret)
		return ret;

	ret = rtl837x_reg_read(priv, RTL837X_SDS_INDACS_READ_DATA, &data);
	if (ret)
		return ret;

	*value = data & 0xffff;
	return 0;
}

static int rtl837x_sds_write_locked(struct rtl837x_priv *priv, u8 sds,
				    u8 page, u8 reg, u16 value)
{
	u32 command;
	int ret;

	ret = rtl837x_sds_wait_idle(priv, &command);
	if (ret)
		return ret;

	ret = rtl837x_reg_write(priv, RTL837X_SDS_INDACS_WRITE_DATA, value);
	if (ret)
		return ret;

	command = FIELD_PREP(RTL837X_SDS_INDACS_INDEX, sds) |
		  FIELD_PREP(RTL837X_SDS_INDACS_PAGE, page) |
		  FIELD_PREP(RTL837X_SDS_INDACS_REG, reg) |
		  RTL837X_SDS_INDACS_WRITE | RTL837X_SDS_INDACS_EXECUTE;
	ret = rtl837x_reg_write(priv, RTL837X_SDS_INDACS_COMMAND, command);
	if (ret)
		return ret;

	return rtl837x_sds_wait_idle(priv, &command);
}

int rtl837x_sds_reg_bits_write(struct rtl837x_priv *priv, u8 sds, u8 page,
			       u8 reg, u16 mask, bool set)
{
	u16 value;
	int ret;

	if (sds > 1 || page > 0x3f || reg > 0x1f || !mask)
		return -EINVAL;

	mutex_lock(&priv->sds_lock);
	ret = rtl837x_sds_read_locked(priv, sds, page, reg, &value);
	if (!ret) {
		if (set)
			value |= mask;
		else
			value &= ~mask;
		ret = rtl837x_sds_write_locked(priv, sds, page, reg, value);
	}
	mutex_unlock(&priv->sds_lock);

	return ret;
}

static bool rtl837x_phy_valid(int phy)
{
	return phy >= 4 && phy <= 7;
}

static int rtl837x_phy_wait_idle(struct rtl837x_priv *priv, u32 *ctrl)
{
	return regmap_read_poll_timeout(priv->map, RTL837X_SMI_PHY_CTRL, *ctrl,
					!(*ctrl & RTL837X_SMI_PHY_BUSY),
					10, 1000);
}

static int rtl837x_phy_wait_ready(struct rtl837x_priv *priv, u32 *ctrl)
{
	int ret;

	ret = rtl837x_phy_wait_idle(priv, ctrl);
	if (ret)
		return ret;

	return (*ctrl & RTL837X_SMI_PHY_STATUS) ? -EIO : 0;
}

/* The caller holds phy_lock across staging, command and readback. */
static int rtl837x_phy_read_locked(struct rtl837x_priv *priv, int phy,
				   int devad, int regnum, u16 *pval)
{
	u32 ctrl, data;
	int ret;

	ret = rtl837x_phy_wait_idle(priv, &ctrl);
	if (ret)
		return ret;
	ret = rtl837x_reg_write(priv, RTL837X_SMI_PHY_WRITE_DATA, phy);
	if (ret)
		return ret;
	ctrl = ((u32)devad << 19) | ((u32)regnum << 3) |
	       RTL837X_SMI_PHY_READ_CMD;
	ret = rtl837x_reg_write(priv, RTL837X_SMI_PHY_CTRL, ctrl);
	if (ret)
		return ret;
	ret = rtl837x_phy_wait_ready(priv, &ctrl);
	if (ret)
		return ret;
	ret = rtl837x_reg_read(priv, RTL837X_SMI_PHY_READ_DATA, &data);
	if (ret)
		return ret;

	*pval = data & 0xffff;
	return 0;
}

static int rtl837x_phys_write_locked(struct rtl837x_priv *priv, u16 phy_mask,
				     int devad, int regnum, u16 val)
{
	u32 ctrl;
	int ret;

	ret = rtl837x_phy_wait_idle(priv, &ctrl);
	if (ret)
		return ret;
	ret = rtl837x_reg_write(priv, RTL837X_SMI_PHY_PORT_SELECT, phy_mask);
	if (ret)
		return ret;
	ret = rtl837x_reg_write(priv, RTL837X_SMI_PHY_WRITE_DATA, val);
	if (ret)
		return ret;
	ctrl = ((u32)devad << 19) | ((u32)regnum << 3) |
	       RTL837X_SMI_PHY_WRITE_CMD;
	ret = rtl837x_reg_write(priv, RTL837X_SMI_PHY_CTRL, ctrl);
	if (ret)
		return ret;

	return rtl837x_phy_wait_ready(priv, &ctrl);
}

int rtl837x_phy_read_c45(struct rtl837x_priv *priv, int phy, int devad,
			 int regnum, u16 *pval)
{
	int ret;

	if (!pval || !rtl837x_phy_valid(phy) || devad < 0 || devad > 31 ||
	    regnum < 0 || regnum > 0xffff)
		return -EINVAL;

	mutex_lock(&priv->phy_lock);
	ret = rtl837x_phy_read_locked(priv, phy, devad, regnum, pval);
	mutex_unlock(&priv->phy_lock);
	return ret;
}

int rtl837x_phys_write_c45(struct rtl837x_priv *priv, u16 phy_mask,
			   int devad, int regnum, u16 val)
{
	int ret;

	if (!phy_mask || (phy_mask & ~RTL8372N_PHY_PORT_MASK) ||
	    devad < 0 || devad > 31 || regnum < 0 || regnum > 0xffff)
		return -EINVAL;

	mutex_lock(&priv->phy_lock);
	ret = rtl837x_phys_write_locked(priv, phy_mask, devad, regnum, val);
	mutex_unlock(&priv->phy_lock);
	return ret;
}

int rtl837x_phy_write_c45(struct rtl837x_priv *priv, int phy, int devad,
			  int regnum, u16 val)
{
	if (!rtl837x_phy_valid(phy))
		return -EINVAL;

	return rtl837x_phys_write_c45(priv, BIT(phy), devad, regnum, val);
}

int rtl837x_phy_read_ocp(struct rtl837x_priv *priv, u16 phy, int regnum,
			 u16 *pval)
{
	return rtl837x_phy_read_c45(priv, phy, 31, regnum, pval);
}

int rtl837x_phy_write_ocp(struct rtl837x_priv *priv, u16 phy, int regnum,
			  u16 val)
{
	return rtl837x_phy_write_c45(priv, phy, 31, regnum, val);
}

/* Realtek pages expose OCP words at C22 registers 16-30. Page zero
 * aliases the standard MII register block at 0xa400. Register 31 selects
 * a software page per PHY, independently of native C45/OCP operations.
 */
static int rtl837x_c22_ocp_address(u16 page, int regnum)
{
	if (!page)
		return 0xa400 + regnum * 2;
	if (regnum < 16)
		return -EINVAL;
	return (page << 4) + (regnum - 16) * 2;
}

int rtl837x_phy_read_c22(struct rtl837x_priv *priv, u16 phy, int regnum,
			 u16 *pval)
{
	int address, ret = 0;

	if (!pval || !rtl837x_phy_valid(phy) || regnum < 0 || regnum > 31)
		return -EINVAL;

	mutex_lock(&priv->phy_lock);
	if (regnum == 31) {
		*pval = priv->phy_page[phy];
	} else {
		address = rtl837x_c22_ocp_address(priv->phy_page[phy], regnum);
		if (address < 0 || address > 0xffff)
			ret = -EINVAL;
		else
			ret = rtl837x_phy_read_locked(priv, phy, 31, address, pval);
	}
	mutex_unlock(&priv->phy_lock);
	return ret;
}

int rtl837x_phy_write_c22(struct rtl837x_priv *priv, u16 phy, int regnum,
			  u16 val)
{
	int address, ret = 0;

	if (!rtl837x_phy_valid(phy) || regnum < 0 || regnum > 31 ||
	    (regnum == 31 && val > 0xfff))
		return -EINVAL;

	mutex_lock(&priv->phy_lock);
	if (regnum == 31) {
		priv->phy_page[phy] = val == 0xa40 ? 0 : val;
	} else {
		address = rtl837x_c22_ocp_address(priv->phy_page[phy], regnum);
		if (address < 0 || address > 0xffff)
			ret = -EINVAL;
		else
			ret = rtl837x_phys_write_locked(priv, BIT(phy), 31,
						       address, val);
	}
	mutex_unlock(&priv->phy_lock);
	return ret;
}
