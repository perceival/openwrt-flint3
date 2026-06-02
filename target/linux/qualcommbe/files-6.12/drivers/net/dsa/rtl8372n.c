// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Realtek RTL8372N / RTL8373 DSA driver — minimal first cut
 *
 * Drives a Realtek RTL8372N-family switch attached on a Clause-22 MDIO bus.
 * The switch occupies a single PHY address on the bus and exposes its
 * internal 16-bit register space through four host MDIO registers (21..24)
 * — the protocol details are documented in
 * docs/rtl8372n-register-map.md in this work tree.
 *
 * This first cut covers only probe + chip-ID readback so we can confirm
 * the DT binding and MDIO transport. DSA port + VLAN logic comes in
 * follow-up patches.
 *
 * Copyright (c) 2026 Kamil Bienkiewicz <perceivalpercy@gmail.com>
 */

#include <linux/bitfield.h>
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/etherdevice.h>
#include <linux/iopoll.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/phy.h>
#include <linux/if_bridge.h>
#include <linux/netlink.h>
#include <net/dsa.h>
#include <net/switchdev.h>

/* User-visible MDIO registers exposed by the switch within its PHY address.
 * (Internal-to-the-chip register access tunnels through these four regs.)
 */
#define RTL8372N_MDIO_CTRL		21
#define RTL8372N_MDIO_CTRL_BUSY		BIT(2)
#define RTL8372N_MDIO_ADDR		22
#define RTL8372N_MDIO_DATA_LO		23
#define RTL8372N_MDIO_DATA_HI		24

#define RTL8372N_MDIO_CMD_READ		0x1B
#define RTL8372N_MDIO_CMD_WRITE		0x19

/* Internal switch register: chip-ID. Upper 16 bits = chip model. */
#define RTL8372N_REG_CHIP_ID		0x0004
#define RTL8372N_CHIP_ID_8372		0x8372
#define RTL8372N_CHIP_ID_8373		0x8373

/* CPU-tag block — 0x8899 EtherType is the Realtek-tag default. */
#define RTL8372N_REG_CPU_TAG_TPID	0x6038	/* bits 15:0 = TPID, default 0x8899 */
#define RTL8372N_REG_CPU_TAG_CTRL	0x6720	/* enable bits + insert-mode */
#define RTL8372N_REG_EXT_CPU_CTRL	0x6724	/* bits 3:0 = external CPU port id */
#define RTL8372N_REG_CPU_TAG_AWARE	0x603C	/* bits 9:0 = tag-aware port mask */

/* MSTP per-port state. fid index 0 = the default STP instance.
 * Layout: 2 bits per port; bits[N*2+1:N*2] for port N. Values per
 * vendor enum RTL8373_MSTP_STATE: 0=DISABLE 1=BLOCKING 2=LEARNING 3=FORWARDING.
 */
#define RTL8372N_REG_MSPT_STATE(fid)	(0x5310 + ((fid) << 2))
#define RTL8372N_MSPT_FORWARDING	3
#define RTL8372N_MSPT_PORT_MASK(p)	(0x3u << ((p) << 1))
#define RTL8372N_MSPT_PORT_FWD(p)	(RTL8372N_MSPT_FORWARDING << ((p) << 1))

/* Per-port "port isolation" (allowed-egress) mask. */
#define RTL8372N_REG_PORT_ISO_PMSK(p)	(0x50C0 + ((p) << 2))

/* CPU_TAG_CTRL bit layout extracted from vendor SDK:
 *   bit  0      INT_CPUTAG_EN
 *   bit  1      EXT_CPUTAG_EN          ← external CPU port (port 3)
 *   bits 9:8    INT_CPUTAG_INSERTMOD
 *   bits 11:10  EXT_CPUTAG_INSERTMOD   ← 0=ALL 1=TRAPPING 2=NONE
 */
#define RTL8372N_CPU_TAG_CTRL_INT_EN	BIT(0)
#define RTL8372N_CPU_TAG_CTRL_EXT_EN	BIT(1)
#define RTL8372N_CPU_TAG_CTRL_INT_MODE	GENMASK(9, 8)
#define RTL8372N_CPU_TAG_CTRL_EXT_MODE	GENMASK(11, 10)
#define RTL8372N_CPU_TAG_INSERT_ALL	0
#define RTL8372N_CPU_TAG_INSERT_TRAP	1
#define RTL8372N_CPU_TAG_INSERT_NONE	2

#define RTL8372N_DEFAULT_TPID		0x8899
#define RTL8372N_TEST_TPID		0x4242
/* Flint 3: CPU traffic egresses via switch port 3 (10G SerDes back to PPE). */
#define RTL8372N_EXT_CPU_PORT		3

/* Per-port live status (read-only) — bits 9:0 = port 0..9 */
#define RTL8372N_REG_MAC_LINK_STS	0x63E8	/* bits 9:0 link, 25:16 mac-link */
#define RTL8372N_REG_MAC_LINK_DUP_STS	0x63F8	/* bits 9:0 = duplex (1=full) */
#define RTL8372N_REG_MAC_LINK_SPD_STS	0x63F0	/* 4-bit field per port, port 0..7 */

/* Per-port MAC force-mode. One 32-bit register per port at +4 stride.
 * Layout extracted from refs/rtl83xx dal_rtl8373_portFrcAbility_set:
 *   bit 0      MAC_FORCE_EN     (1=use forced cfg, 0=auto-neg)
 *   bit 1      FORCE_LINK_EN    (1=link up, 0=link down)
 *   bit 2      FORCE_DUPLEX     (1=full, 0=half)
 *   bits 6:3   FORCE_SPD_SEL    (4-bit, see rtk_port_speed_e below)
 *   bit 7      TX_PAUSE_EN
 *   bit 8      RX_PAUSE_EN
 *   bit 9      SMI_FORCE_FC
 *   bit 10     MEDIA            (0=UTP, 1=fibre)
 *
 * rtk_port_speed_e (from refs/rtl83xx/src/rtl8372/port.h):
 *   10M=0  100M=1  1000M=2  500M=3  10G=4  2500M=5  5G=6
 */
#define RTL8372N_REG_MAC_FORCE_CTRL0(p)	(0x6344 + (p) * 4)
#define RTL8372N_MAC_FORCE_EN		BIT(0)
#define RTL8372N_FORCE_LINK_EN		BIT(1)
#define RTL8372N_FORCE_DUPLEX_FULL	BIT(2)
#define RTL8372N_FORCE_SPD_SHIFT	3
#define RTL8372N_FORCE_SPD_MASK		(0xF << RTL8372N_FORCE_SPD_SHIFT)
#define RTL8372N_FORCE_TX_PAUSE		BIT(7)
#define RTL8372N_FORCE_RX_PAUSE		BIT(8)
#define RTL8372N_FORCE_SMI_FC		BIT(9)
#define RTL8372N_FORCE_MEDIA_FIBRE	BIT(10)

#define RTL8372N_SPD_10G		4
#define RTL8372N_SPD_2500M		5

/* SerDes mode select. Bits 4:0 = SDS0 mode, 9:5 = SDS1 mode,
 * 14:10 = SDS0 USX sub-mode, 20:16 = SDS1 USX sub-mode,
 * 21 = CFG_MAC3_8221B (port 3 talking to RTL8221B PHY),
 * 22 = CFG_MAC8_8221B.
 * For Flint 3 the SoC's PCS connects to the switch via port 3 SerDes
 * — we want SDS0 in 2500BASEX, not 8221B mode.
 */
#define RTL8372N_REG_SDS_MODE_SEL		0x7B20
#define RTL8372N_SDS_MODE_MASK			0x1F
#define RTL8372N_SDS_MODE_SHIFT_SDS0		0
#define RTL8372N_SDS_MODE_SHIFT_SDS1		5

/* SerDes mode-id encoding (5-bit field). Extracted from
 * refs/rtl83xx/src/rtl8372/port.h SERDES_* enum.
 */
#define RTL8372N_SDS_MODE_SG			0x02
#define RTL8372N_SDS_MODE_1000BASEX		0x04
#define RTL8372N_SDS_MODE_100FX			0x05
#define RTL8372N_SDS_MODE_10GUSXG		0x0D
#define RTL8372N_SDS_MODE_HSG			0x12
#define RTL8372N_SDS_MODE_2500BASEX		0x16
#define RTL8372N_SDS_MODE_10GR			0x1A
#define RTL8372N_SDS_MODE_OFF			0x1F

/* Indirect Table Access — used for the 4K CVLAN table.
 *   ITA_CTRL0 layout: bits 16-28 TBL_ADDR, 8-10 TLB_TYPE, bit 1 ACT,
 *                     bit 0 EXECUTE (write 1 to trigger, polls back to 0).
 * Constants from refs/rtl83xx/src/rtl8372/dal/rtl8373/.
 */
#define RTL8372N_REG_ITA_CTRL0		0x5CAC
#define RTL8372N_REG_ITA_WRITE_DATA0	0x5CB8
#define RTL8372N_ITA_TBL_ADDR_SHIFT	16
#define RTL8372N_ITA_TLB_TYPE_SHIFT	8
#define RTL8372N_ITA_TB_TARGET_CVLAN	3
#define RTL8372N_ITA_TB_OP_WRITE	BIT(1)
#define RTL8372N_ITA_TB_EXECUTE		BIT(0)

/* Per-port PVID register pair (2 ports per 32-bit reg, 12-bit fields). */
#define RTL8372N_REG_VLAN_PORT_PVID(p)	(0x4E1C + (((p) >> 1) << 2))

/* VLAN egress tag-mode (2 bits per port, all 10 ports in one 32-bit reg). */
#define RTL8372N_REG_VLAN_PORT_EGR_TAG	0x6738
#define RTL8372N_TAG_MODE_ORIGINAL	0

/* VLAN learn-disable table — vendor's init clears both entries. */
#define RTL8372N_REG_VLAN_L2_LRN_DIS(i)	(0x4E30 + ((i) << 2))

/* VLAN control register — TABLE_RST bit clears the 4K table. */
#define RTL8372N_REG_VLAN_CTRL		0x4E14
#define RTL8372N_VLAN_CTRL_TABLE_RST	BIT(3)
#define RTL8372N_VLAN_CTRL_CVLAN_FILTER	BIT(2)

/* Per-port ingress filter (1 bit per port, all 10 in one reg). */
#define RTL8372N_REG_VLAN_PORT_IGR_FLTR	0x4E18

/* Chip-init registers (vendor rtl8372n_init in dal_rtl8373_switch.c).
 * The Realtek Unmanaged Switch Programming Guide explicitly states
 * "Using any other API before using rtk_switch_init may cause
 * unpredicted error" — so these writes are mandatory before VLAN
 * and CPU-tag setup can be expected to behave.
 */
#define RTL8372N_REG_MAC_L2_GLOBAL_CTRL0	0x5FD4
#define RTL8372N_FWD_UNKN_OPCODE_EN		BIT(19)
#define RTL8372N_FWD_INVLD_MAC_CTRL_EN		BIT(20)

#define RTL8372N_REG_MAC_L2_PORT_CTRL(p)	(0x1238 + ((p) << 8))
#define RTL8372N_MAC_L2_RX_CHK_CRC_EN		BIT(4)
#define RTL8372N_MAC_L2_CLOCK_SWITCH		BIT(8)

#define RTL8372N_REG_RS_LAYER_CONFIG		0xB7C
#define RTL8372N_RS_LINK_FAULT_INDI_OFF		BIT(5)

#define RTL8372N_REG_FC_PORT_ACT_CTRL(p)	(0x7124 + ((p) << 2))
#define RTL8372N_FC_PORT_ACT_DEFAULT		0x1050

#define RTL8372N_REG_DW8051_CFG			0x6040
#define RTL8372N_DW8051_READY			BIT(0)

/* SerDes-internal indirect register access. SDS regs are organised as
 * (page, reg) tuples; we get at them by writing data → WD, then
 * stuffing INDEX/PAGE/REGAD/RWOP/CMD into the CMD register and polling
 * CMD back to 0.
 */
#define RTL8372N_REG_SDS_INDACS_CMD		0x3F8
#define RTL8372N_REG_SDS_INDACS_RD		0x3FC
#define RTL8372N_REG_SDS_INDACS_WD		0x400

#define RTL8372N_SDS_CMD_TRIGGER		BIT(15)
#define RTL8372N_SDS_CMD_RWOP_WRITE		BIT(14)	/* vendor naming: 1=write, 0=read */
#define RTL8372N_SDS_CMD_REGAD_MASK		GENMASK(11, 7)
#define RTL8372N_SDS_CMD_PAGE_MASK		GENMASK(6, 1)
#define RTL8372N_SDS_CMD_INDEX_MASK		BIT(0)

#define RTL8372N_SDS_CMD_POLL_US		20
#define RTL8372N_SDS_CMD_TIMEOUT_US		5000

#define RTL8372N_BUSY_TIMEOUT_US	5000
#define RTL8372N_BUSY_POLL_US		10

/* Total number of switch ports the chip family exposes (0..9 in vendor
 * SDK port-id terms). Port 3 is the CPU port on Flint 3 wiring; ports
 * 5..8 are the four LAN jacks; the rest are unused but must still be
 * declared so DSA sets up unused-port suppression correctly.
 */
#define RTL8372N_NUM_PORTS		10

struct rtl8372n_priv {
	struct mii_bus *bus;
	int phy_addr;
	struct mutex lock;  /* serialises multi-MDIO indirect transactions */
	struct dsa_switch *ds;
	/* Periodic LUT diagnostic — re-read our static CPU-MAC entry every
	 * few seconds for the first minute to see whether the chip
	 * preserves it under traffic (testing the nosalearn=1 / age=6
	 * static-entry assumption).
	 */
	struct delayed_work fdb_diag_work;
	u8 fdb_diag_mac[ETH_ALEN];
	unsigned int fdb_diag_iter;

	/* Per-VID port membership state. Two bitmasks (one bit per chip
	 * port, 10 ports) tracked across the full 4K VID space so that
	 * `bridge vlan add/del` calls accumulate into the chip's 4K VLAN
	 * entry correctly. vid_mbr[v] is the union of all DSA ports that
	 * have been told to forward VID v; vid_untag[v] is the subset that
	 * should send v out as an untagged frame.
	 */
	u16 vid_mbr[4096];
	u16 vid_untag[4096];
};

static int rtl8372n_wait_not_busy(struct rtl8372n_priv *p)
{
	int val;
	int ret;

	ret = read_poll_timeout(mdiobus_read, val,
				val < 0 || !(val & RTL8372N_MDIO_CTRL_BUSY),
				RTL8372N_BUSY_POLL_US, RTL8372N_BUSY_TIMEOUT_US,
				false,
				p->bus, p->phy_addr, RTL8372N_MDIO_CTRL);
	if (ret)
		return ret;
	if (val < 0)
		return val;
	return 0;
}

/* Read a 32-bit internal switch register via the MDIO indirect protocol. */
static int rtl8372n_reg_read(struct rtl8372n_priv *p, u16 addr, u32 *out)
{
	int lo, hi, ret;

	mutex_lock(&p->lock);

	ret = rtl8372n_wait_not_busy(p);
	if (ret)
		goto out;

	ret = mdiobus_write(p->bus, p->phy_addr, RTL8372N_MDIO_ADDR, addr);
	if (ret)
		goto out;

	ret = mdiobus_write(p->bus, p->phy_addr, RTL8372N_MDIO_CTRL,
			    RTL8372N_MDIO_CMD_READ);
	if (ret)
		goto out;

	ret = rtl8372n_wait_not_busy(p);
	if (ret)
		goto out;

	lo = mdiobus_read(p->bus, p->phy_addr, RTL8372N_MDIO_DATA_LO);
	if (lo < 0) {
		ret = lo;
		goto out;
	}
	hi = mdiobus_read(p->bus, p->phy_addr, RTL8372N_MDIO_DATA_HI);
	if (hi < 0) {
		ret = hi;
		goto out;
	}

	*out = ((u32)hi << 16) | (u32)lo;
out:
	mutex_unlock(&p->lock);
	return ret;
}

/* Write a 32-bit value to an internal switch register. */
static int rtl8372n_reg_write(struct rtl8372n_priv *p,
			      u16 addr, u32 val)
{
	int ret;

	mutex_lock(&p->lock);

	ret = rtl8372n_wait_not_busy(p);
	if (ret)
		goto out;

	ret = mdiobus_write(p->bus, p->phy_addr, RTL8372N_MDIO_ADDR, addr);
	if (ret)
		goto out;
	ret = mdiobus_write(p->bus, p->phy_addr, RTL8372N_MDIO_DATA_LO,
			    val & 0xFFFF);
	if (ret)
		goto out;
	ret = mdiobus_write(p->bus, p->phy_addr, RTL8372N_MDIO_DATA_HI,
			    (val >> 16) & 0xFFFF);
	if (ret)
		goto out;
	ret = mdiobus_write(p->bus, p->phy_addr, RTL8372N_MDIO_CTRL,
			    RTL8372N_MDIO_CMD_WRITE);
	if (ret)
		goto out;

	ret = rtl8372n_wait_not_busy(p);
out:
	mutex_unlock(&p->lock);
	return ret;
}

/* Read-only baseline dump — proves reg access across the address space, and
 * gives us per-port link / duplex / speed as the chip sees them right now.
 * Pure diagnostic; runs once at probe.
 */
static void rtl8372n_dump_baseline(struct rtl8372n_priv *p, struct device *dev)
{
	u32 tpid, ctrl, link, dup, spd_lo, spd_hi;
	int ret;

	ret = rtl8372n_reg_read(p, RTL8372N_REG_CPU_TAG_TPID, &tpid);
	if (!ret)
		dev_info(dev, "CPU_TAG_TPID = 0x%08x (expect 0x00008899)\n", tpid);
	ret = rtl8372n_reg_read(p, RTL8372N_REG_CPU_TAG_CTRL, &ctrl);
	if (!ret)
		dev_info(dev, "CPU_TAG_CTRL = 0x%08x\n", ctrl);

	ret = rtl8372n_reg_read(p, RTL8372N_REG_MAC_LINK_STS, &link);
	if (ret) {
		dev_warn(dev, "MAC_LINK_STS read failed: %d\n", ret);
		return;
	}
	(void)rtl8372n_reg_read(p, RTL8372N_REG_MAC_LINK_DUP_STS, &dup);
	(void)rtl8372n_reg_read(p, RTL8372N_REG_MAC_LINK_SPD_STS,     &spd_lo);
	(void)rtl8372n_reg_read(p, RTL8372N_REG_MAC_LINK_SPD_STS + 4, &spd_hi);

	dev_info(dev,
		 "ports: link=0x%03x mac-link=0x%03x dup=0x%03x spd[0-7]=0x%08x spd[8-9]=0x%02x\n",
		 link & 0x3FF, (link >> 16) & 0x3FF, dup & 0x3FF,
		 spd_lo, spd_hi & 0xFF);

	{
		u32 sds;
		if (!rtl8372n_reg_read(p, RTL8372N_REG_SDS_MODE_SEL, &sds))
			dev_info(dev,
				 "SDS_MODE_SEL = 0x%08x — SDS0=%u SDS1=%u MAC3_8221B=%u MAC8_8221B=%u\n",
				 sds,
				 sds & 0x1F,
				 (sds >> 5) & 0x1F,
				 (sds >> 21) & 1,
				 (sds >> 22) & 1);
	}
}

/* Round-trip CPU_TAG_TPID through a marker value to prove the write path
 * is wired up. TPID is only consulted when the chip is actively injecting
 * a CPU tag (gated by the EN bits in CPU_TAG_CTRL, which we leave clear),
 * so it's safe to toggle during boot — the change doesn't disturb the
 * default U-Boot forwarding state.
 */
static int rtl8372n_write_selftest(struct rtl8372n_priv *p, struct device *dev)
{
	u32 saved, probed;
	int ret;

	ret = rtl8372n_reg_read(p, RTL8372N_REG_CPU_TAG_TPID, &saved);
	if (ret) {
		dev_warn(dev, "write-selftest: pre-read failed: %d\n", ret);
		return ret;
	}

	ret = rtl8372n_reg_write(p, RTL8372N_REG_CPU_TAG_TPID,
				 RTL8372N_TEST_TPID);
	if (ret) {
		dev_warn(dev, "write-selftest: write failed: %d\n", ret);
		return ret;
	}

	ret = rtl8372n_reg_read(p, RTL8372N_REG_CPU_TAG_TPID, &probed);
	if (ret) {
		dev_warn(dev, "write-selftest: post-read failed: %d\n", ret);
		goto restore;
	}

	if ((probed & 0xFFFF) != RTL8372N_TEST_TPID) {
		dev_err(dev,
			"write-selftest: mismatch — wrote 0x%04x, read 0x%08x\n",
			RTL8372N_TEST_TPID, probed);
		ret = -EIO;
		goto restore;
	}

	dev_info(dev, "write-selftest: OK (round-tripped TPID)\n");

restore:
	{
		int rret = rtl8372n_reg_write(p, RTL8372N_REG_CPU_TAG_TPID,
					      saved & 0xFFFF);
		if (rret)
			dev_warn(dev,
				 "write-selftest: TPID restore failed: %d (was 0x%04x)\n",
				 rret, saved & 0xFFFF);
	}
	return ret;
}

/* Poll the SDS_INDACS CMD bit until it self-clears, signalling the chip
 * has finished the previous indirect transaction.
 */
static int rtl8372n_sds_wait_cmd(struct rtl8372n_priv *p)
{
	u32 val;
	int ret, polls;

	for (polls = 0;
	     polls * RTL8372N_SDS_CMD_POLL_US < RTL8372N_SDS_CMD_TIMEOUT_US;
	     polls++) {
		ret = rtl8372n_reg_read(p, RTL8372N_REG_SDS_INDACS_CMD, &val);
		if (ret)
			return ret;
		if (!(val & RTL8372N_SDS_CMD_TRIGGER))
			return 0;
		usleep_range(RTL8372N_SDS_CMD_POLL_US,
			     RTL8372N_SDS_CMD_POLL_US + 5);
	}
	return -ETIMEDOUT;
}

/* Write a 16-bit value to an SDS-internal (page, reg) register on sdsid. */
static int rtl8372n_sds_reg_write(struct rtl8372n_priv *p,
				  u8 sdsid, u8 page, u8 regad, u16 data)
{
	u32 cmd;
	int ret;

	ret = rtl8372n_sds_wait_cmd(p);
	if (ret)
		return ret;

	ret = rtl8372n_reg_write(p, RTL8372N_REG_SDS_INDACS_WD, data);
	if (ret)
		return ret;

	cmd = RTL8372N_SDS_CMD_TRIGGER | RTL8372N_SDS_CMD_RWOP_WRITE;
	cmd |= FIELD_PREP(RTL8372N_SDS_CMD_REGAD_MASK, regad & 0x1F);
	cmd |= FIELD_PREP(RTL8372N_SDS_CMD_PAGE_MASK, page & 0x3F);
	cmd |= FIELD_PREP(RTL8372N_SDS_CMD_INDEX_MASK, sdsid & 0x1);

	ret = rtl8372n_reg_write(p, RTL8372N_REG_SDS_INDACS_CMD, cmd);
	if (ret)
		return ret;

	return rtl8372n_sds_wait_cmd(p);
}

static int rtl8372n_sds_reg_read(struct rtl8372n_priv *p,
				 u8 sdsid, u8 page, u8 regad, u16 *out)
{
	u32 cmd, val;
	int ret;

	ret = rtl8372n_sds_wait_cmd(p);
	if (ret)
		return ret;

	cmd = RTL8372N_SDS_CMD_TRIGGER;	/* RWOP cleared = read */
	cmd |= FIELD_PREP(RTL8372N_SDS_CMD_REGAD_MASK, regad & 0x1F);
	cmd |= FIELD_PREP(RTL8372N_SDS_CMD_PAGE_MASK, page & 0x3F);
	cmd |= FIELD_PREP(RTL8372N_SDS_CMD_INDEX_MASK, sdsid & 0x1);

	ret = rtl8372n_reg_write(p, RTL8372N_REG_SDS_INDACS_CMD, cmd);
	if (ret)
		return ret;

	ret = rtl8372n_sds_wait_cmd(p);
	if (ret)
		return ret;

	ret = rtl8372n_reg_read(p, RTL8372N_REG_SDS_INDACS_RD, &val);
	if (ret)
		return ret;

	*out = val & 0xFFFF;
	return 0;
}

/* Read-modify-write at the bit-field level. mask gives the target bits in
 * place (e.g. 0x3 << 8 for bits 9:8); value is pre-shifted to the LSB of the
 * field (e.g. 0x3 for both bits set). Matches vendor SDK's
 * dal_rtl8373_sds_regbits_write semantics exactly so the extracted init
 * sequences below can be transcribed without bit-twiddling translation.
 */
static int rtl8372n_sds_regbits_write(struct rtl8372n_priv *p, u8 sdsid,
				      u8 page, u8 regad, u16 mask, u16 value)
{
	u16 cur, shift;
	int ret;

	if (!mask)
		return -EINVAL;
	shift = __ffs(mask);

	ret = rtl8372n_sds_reg_read(p, sdsid, page, regad, &cur);
	if (ret)
		return ret;
	cur = (cur & ~mask) | ((value << shift) & mask);
	return rtl8372n_sds_reg_write(p, sdsid, page, regad, cur);
}

/* SerDes 2500BASEX init sequence. The (page, reg, value) triples below are
 * direct hardware facts extracted from refs/rtl83xx an_3p125g_chipb and
 * dig_patch_mac tables (chip-Ver8372N path, MAC-side SerDes — which is
 * exactly the Flint 3 wiring: switch port 3 talking to the SoC's PCS).
 * The post-patch poke loop on (page 0x20, reg 0) mirrors the
 * SDS_MODE_SET_SW PLL/reset cycle. Per-write delays match vendor.
 */
struct rtl8372n_sds_patch { u8 page, reg; u16 val; };

static const struct rtl8372n_sds_patch rtl8372n_an_3p125g_chipb[] = {
	{ 0x21, 0x10, 0x6480 }, { 0x21, 0x13, 0x0400 }, { 0x21, 0x18, 0x6d02 },
	{ 0x21, 0x1b, 0x424e }, { 0x21, 0x1d, 0x0002 }, { 0x36, 0x1c, 0x1390 },
	{ 0x36, 0x14, 0x003F }, { 0x36, 0x10, 0x0200 }, { 0x28, 0x04, 0x0080 },
	{ 0x28, 0x07, 0x1201 }, { 0x28, 0x09, 0x0601 }, { 0x28, 0x0b, 0x232c },
	{ 0x28, 0x0c, 0x9217 }, { 0x28, 0x0f, 0x5b50 }, { 0x28, 0x15, 0xe7f1 },
	{ 0x28, 0x16, 0x0443 }, { 0x28, 0x1d, 0xabb0 },
};

static const struct rtl8372n_sds_patch rtl8372n_dig_patch_mac[] = {
	{  6, 18, 0x5078 }, {  7,  6, 0x9401 }, {  7,  8, 0x9401 },
	{  7, 10, 0x9401 }, {  7, 12, 0x9401 }, { 31, 11, 0x0003 },
	{  6,  3, 0xc45c }, {  6, 31, 0x2100 },
};

static int rtl8372n_apply_patch_list(struct rtl8372n_priv *p, u8 sdsid,
				     const struct rtl8372n_sds_patch *list,
				     size_t n)
{
	int ret;
	size_t i;

	for (i = 0; i < n; i++) {
		ret = rtl8372n_sds_reg_write(p, sdsid, list[i].page,
					     list[i].reg, list[i].val);
		if (ret)
			return ret;
	}
	return 0;
}

/* Big PLL/reset poke loop in SDS_MODE_SET_SW for non-USXG modes.
 * The exact sequence (bits, values, ordering, delays) comes from vendor
 * SDK; treat it as opaque bring-up choreography for the SerDes block.
 */
static int rtl8372n_sds_pll_kick(struct rtl8372n_priv *p, u8 sdsid)
{
	static const struct {
		u16 mask;	/* in-place mask */
		u16 val;	/* LSB-shifted value */
		u32 delay_us;
	} seq[] = {
		{ 0x3 << 4,  0x3, 1000 },
		{ 0x3 << 4,  0x1, 100  },
		{ 0x3 << 6,  0x1, 1000 },
		{ 0x3 << 6,  0x3, 100  },
		{ 0x3 << 10, 0x3, 1000 },
		{ 0x3 << 10, 0x1, 1000 },
		{ 0x3 << 10, 0x1, 1000 },
		{ 0x3 << 10, 0x3, 100  },
		{ 0x3 << 10, 0x0, 1000 },
		{ 0x3 << 6,  0x3, 1000 },
		{ 0x3 << 6,  0x1, 100  },
		{ 0x3 << 6,  0x0, 1000 },
		{ 0x3 << 4,  0x1, 1000 },
		{ 0x3 << 4,  0x3, 100  },
		{ 0x3 << 4,  0x0, 100  },
	};
	int ret;
	size_t i;

	for (i = 0; i < ARRAY_SIZE(seq); i++) {
		ret = rtl8372n_sds_regbits_write(p, sdsid, 0x20, 0x00,
						 seq[i].mask, seq[i].val);
		if (ret)
			return ret;
		usleep_range(seq[i].delay_us, seq[i].delay_us + 100);
	}

	/* Page 0x1F register 0x00: write 0xB then 0x0 as a final trigger. */
	ret = rtl8372n_sds_reg_write(p, sdsid, 0x1F, 0x00, 0xB);
	if (ret)
		return ret;
	usleep_range(100, 200);
	ret = rtl8372n_sds_reg_write(p, sdsid, 0x1F, 0x00, 0x0);
	if (ret)
		return ret;
	usleep_range(100, 200);
	return 0;
}

/* Full SDS bring-up for 2500BASEX on the given SerDes index. Kept here
 * for the eventual case where we want to drive the SerDes at 2.5G (e.g.
 * a different board with a 2.5G inter-chip link) but unused on Flint 3
 * — vendor wires the SoC↔switch SerDes at 10G (mode 10GR) and we
 * match that instead of overriding to 2500BASEX.
 */
static int __maybe_unused
rtl8372n_sds_setup_2500basex(struct rtl8372n_priv *p, u8 sdsid,
			     struct device *dev)
{
	int ret;

	dev_info(dev, "SDS%u: applying 2500BASEX serdes patch + PLL kick\n",
		 sdsid);

	ret = rtl8372n_apply_patch_list(p, sdsid, rtl8372n_an_3p125g_chipb,
					ARRAY_SIZE(rtl8372n_an_3p125g_chipb));
	if (ret) {
		dev_warn(dev, "SDS%u: an_patch failed: %d\n", sdsid, ret);
		return ret;
	}

	ret = rtl8372n_apply_patch_list(p, sdsid, rtl8372n_dig_patch_mac,
					ARRAY_SIZE(rtl8372n_dig_patch_mac));
	if (ret) {
		dev_warn(dev, "SDS%u: dig_patch failed: %d\n", sdsid, ret);
		return ret;
	}

	/* fiber_fc_en for 2500BASEX (with fc_en=1, vendor default):
	 *   page 31 reg 5 bit 2 = 1, bit 3 = 0
	 *   page 2  reg 4 bits 8:7 = 0x3
	 */
	rtl8372n_sds_regbits_write(p, sdsid, 31, 5, 0x1 << 2, 0x1);
	rtl8372n_sds_regbits_write(p, sdsid, 31, 5, 0x1 << 3, 0x0);
	rtl8372n_sds_regbits_write(p, sdsid,  2, 4, 0x3 << 7, 0x3);

	/* sds_nway_set(2500BASEX, an_en=1):
	 *   page 0 reg 2 bits 9:8 = 0x3
	 *   page 0 reg 4 bit 2    = 0x1
	 * Vendor does this BEFORE the PLL kick, not after.
	 */
	rtl8372n_sds_regbits_write(p, sdsid, 0, 2, 0x3 << 8, 0x3);
	rtl8372n_sds_regbits_write(p, sdsid, 0, 4, 0x1 << 2, 0x1);

	usleep_range(1000, 2000);
	ret = rtl8372n_sds_pll_kick(p, sdsid);
	if (ret) {
		dev_warn(dev, "SDS%u: PLL kick failed: %d\n", sdsid, ret);
		return ret;
	}

	dev_info(dev, "SDS%u: 2500BASEX setup complete\n", sdsid);
	return 0;
}

/* Unused on Flint 3 (vendor leaves SDS0 in 10GR which is what we want);
 * kept here as a worked example of the basic SDS_MODE_SEL bit-field
 * rewrite + OFF dip + read-back pattern for future board ports that
 * really do want a different SerDes mode.
 */
__maybe_unused
/* Force SDS0 into 2500BASEX so the SerDes line code matches what the SoC's
 * PCS expects on port 1 (phy-mode = "2500base-x"). Vendor U-Boot leaves SDS0
 * in 10GR (0x1A); without this fix the SoC's PCS can't recover an RX clock
 * from the line and the NSSCC uniphy0_nss_rx_clk CBCR refuses to leave
 * CLK_OFF, blocking the entire port1 RCG mux update. See
 * memory/project_flint3_pcs_rx_asymmetric.md for the diagnosis.
 *
 * Note: this just writes the 5-bit SDS_MODE_SEL field. The reference SDK
 * additionally pokes SerDes-internal registers via a page+reg indirection;
 * we may need to extend this if the mode change alone doesn't bring the
 * link up at the right line code.
 */
static int rtl8372n_force_sds0_2500basex(struct rtl8372n_priv *p,
					 struct device *dev)
{
	u32 v, off, want;
	int ret;

	ret = rtl8372n_reg_read(p, RTL8372N_REG_SDS_MODE_SEL, &v);
	if (ret)
		return ret;

	want = (v & ~RTL8372N_SDS_MODE_MASK) |
	       (RTL8372N_SDS_MODE_2500BASEX << RTL8372N_SDS_MODE_SHIFT_SDS0);
	off  = (v & ~RTL8372N_SDS_MODE_MASK) |
	       (RTL8372N_SDS_MODE_OFF       << RTL8372N_SDS_MODE_SHIFT_SDS0);

	if (want == v) {
		dev_info(dev, "SDS0 already 2500BASEX (0x%08x)\n", v);
		return 0;
	}

	dev_info(dev, "SDS0 reset+switch: 0x%08x -> OFF(0x%08x) -> 0x%08x\n",
		 v, off, want);

	/* OFF→target writes mimic the vendor SDK's reset-around-mode-change
	 * pattern — without the OFF dip, writing a new SDS_MODE_SEL value
	 * doesn't appear to actually reconfigure the SerDes block.
	 */
	ret = rtl8372n_reg_write(p, RTL8372N_REG_SDS_MODE_SEL, off);
	if (ret) {
		dev_warn(dev, "SDS_MODE_SEL OFF write failed: %d\n", ret);
		return ret;
	}
	usleep_range(10000, 11000);

	ret = rtl8372n_reg_write(p, RTL8372N_REG_SDS_MODE_SEL, want);
	if (ret) {
		dev_warn(dev, "SDS_MODE_SEL write failed: %d\n", ret);
		return ret;
	}
	usleep_range(10000, 11000);

	(void)rtl8372n_reg_read(p, RTL8372N_REG_SDS_MODE_SEL, &v);
	dev_info(dev, "SDS_MODE_SEL post-write = 0x%08x\n", v);
	return 0;
}

/* Pre-stage the registers the CPU-tag insertion path needs, but do NOT
 * flip the EN bits in CPU_TAG_CTRL yet — enabling tag injection without a
 * matching DSA tag protocol handler in the kernel would break forwarding.
 * This just makes the eventual "set EN" step a one-bit RMW.
 */
static int rtl8372n_stage_cpu_tag(struct rtl8372n_priv *p, struct device *dev)
{
	u32 v;
	int ret;

	ret = rtl8372n_reg_write(p, RTL8372N_REG_EXT_CPU_CTRL,
				 RTL8372N_EXT_CPU_PORT);
	if (ret) {
		dev_warn(dev, "stage: EXT_CPU_CTRL write failed: %d\n", ret);
		return ret;
	}

	ret = rtl8372n_reg_write(p, RTL8372N_REG_CPU_TAG_AWARE,
				 BIT(RTL8372N_EXT_CPU_PORT));
	if (ret) {
		dev_warn(dev, "stage: CPU_TAG_AWARE write failed: %d\n", ret);
		return ret;
	}

	(void)rtl8372n_reg_read(p, RTL8372N_REG_EXT_CPU_CTRL, &v);
	dev_info(dev, "stage: EXT_CPU_CTRL = 0x%08x (port %u)\n",
		 v, v & 0xF);
	(void)rtl8372n_reg_read(p, RTL8372N_REG_CPU_TAG_AWARE, &v);
	dev_info(dev, "stage: CPU_TAG_AWARE = 0x%08x (mask)\n", v);
	return 0;
}

/* Subset of vendor's rtl8372n_init() chip-bring-up. The vendor SDK
 * also reconfigures SMI / PHY power-down / PHY power-up; we skip
 * those because U-Boot has already brought the chip up enough for
 * MDIO indirect access to work. What's left — the bits that govern
 * how the L2 forwarding engine treats frames — is what we add here.
 *
 * Per the Realtek Unmanaged Switch Programming Guide v1.3.11: this
 * MUST run before vlan-init / cpu-tag-stage / any other API, or
 * "unpredicted error" results. (We see exactly that: external→CPU
 * frames silently dropped.)
 */
static int rtl8372n_chip_init(struct rtl8372n_priv *p, struct device *dev)
{
	u32 val;
	int ret;
	unsigned int port;

	/* MAC L2 global: enable forwarding of invalid MAC-CTRL and unknown
	 * opcode frames. Without these, the switch may silently drop
	 * frames the kernel needs to see (e.g. control frames).
	 */
	ret = rtl8372n_reg_read(p, RTL8372N_REG_MAC_L2_GLOBAL_CTRL0, &val);
	if (ret) {
		dev_warn(dev, "chip-init: MAC_L2_GLOBAL_CTRL0 read failed: %d\n",
			 ret);
		return ret;
	}
	val |= RTL8372N_FWD_INVLD_MAC_CTRL_EN | RTL8372N_FWD_UNKN_OPCODE_EN;
	ret = rtl8372n_reg_write(p, RTL8372N_REG_MAC_L2_GLOBAL_CTRL0, val);
	if (ret) {
		dev_warn(dev, "chip-init: MAC_L2_GLOBAL_CTRL0 write failed: %d\n",
			 ret);
		return ret;
	}

	/* Per-port MAC L2 control: enable CRC check + clock-switch on the
	 * ports we use (CPU port 3 + LAN ports 4..8). Vendor's loop runs
	 * 3..8 inclusive; we follow.
	 */
	for (port = 3; port <= 8; port++) {
		ret = rtl8372n_reg_read(p, RTL8372N_REG_MAC_L2_PORT_CTRL(port),
					&val);
		if (ret)
			return ret;
		val |= RTL8372N_MAC_L2_RX_CHK_CRC_EN |
		       RTL8372N_MAC_L2_CLOCK_SWITCH;
		ret = rtl8372n_reg_write(p, RTL8372N_REG_MAC_L2_PORT_CTRL(port),
					 val);
		if (ret) {
			dev_warn(dev, "chip-init: MAC_L2_PORT_CTRL port %u failed: %d\n",
				 port, ret);
			return ret;
		}
	}

	/* RS layer: disable link-fault indication (vendor comment:
	 * "resolve port4-port7 linkdown dsc expand issue").
	 */
	ret = rtl8372n_reg_read(p, RTL8372N_REG_RS_LAYER_CONFIG, &val);
	if (ret)
		return ret;
	val |= RTL8372N_RS_LINK_FAULT_INDI_OFF;
	(void)rtl8372n_reg_write(p, RTL8372N_REG_RS_LAYER_CONFIG, val);

	/* Flow-control port action: vendor writes 0x1050 to all 10 ports. */
	for (port = 0; port < 10; port++) {
		ret = rtl8372n_reg_write(p, RTL8372N_REG_FC_PORT_ACT_CTRL(port),
					 RTL8372N_FC_PORT_ACT_DEFAULT);
		if (ret) {
			dev_warn(dev, "chip-init: FC_PORT_ACT port %u failed: %d\n",
				 port, ret);
			return ret;
		}
	}

	/* Mark the on-die DW8051 microcontroller READY — vendor's final
	 * step in rtl8372n_init. Even though we don't load µC firmware,
	 * downstream subsystems poll this bit before producing traffic.
	 */
	ret = rtl8372n_reg_read(p, RTL8372N_REG_DW8051_CFG, &val);
	if (ret)
		return ret;
	val |= RTL8372N_DW8051_READY;
	(void)rtl8372n_reg_write(p, RTL8372N_REG_DW8051_CFG, val);

	(void)rtl8372n_reg_read(p, RTL8372N_REG_MAC_L2_GLOBAL_CTRL0, &val);
	dev_info(dev, "chip-init: MAC_L2_GLOBAL_CTRL0 = 0x%08x\n", val);
	(void)rtl8372n_reg_read(p, RTL8372N_REG_DW8051_CFG, &val);
	dev_info(dev, "chip-init: DW8051_CFG = 0x%08x\n", val);

	return 0;
}

/* Wide register-state dump, called as the final step of probe. Used to
 * diagnose the DSA TX black-hole (memory/project_flint3_dsa_tx_broken.md):
 * device→laptop frames egress eth0 with correct rtl8_4 tag but never
 * appear at the laptop. Dumps the registers that would gate L2 forwarding
 * of CPU-tagged frames, plus per-port STP and isolation state.
 *
 * Read-only — does NOT modify chip state.
 */
static void rtl8372n_dump_diag(struct rtl8372n_priv *p, struct device *dev)
{
	u32 v;
	unsigned int port;

	if (!rtl8372n_reg_read(p, RTL8372N_REG_MSPT_STATE(0), &v))
		dev_info(dev,
			 "diag: MSPT_STATE[0] = 0x%08x (port3=%u port5=%u port6=%u port7=%u port8=%u; 0=DIS 1=BLK 2=LRN 3=FWD)\n",
			 v,
			 (v >> 6) & 0x3,
			 (v >> 10) & 0x3,
			 (v >> 12) & 0x3,
			 (v >> 14) & 0x3,
			 (v >> 16) & 0x3);

	if (!rtl8372n_reg_read(p, RTL8372N_REG_CPU_TAG_CTRL, &v))
		dev_info(dev,
			 "diag: CPU_TAG_CTRL = 0x%08x (INT_EN=%u EXT_EN=%u INT_MODE=%u EXT_MODE=%u)\n",
			 v, v & 1, (v >> 1) & 1, (v >> 8) & 3, (v >> 10) & 3);

	if (!rtl8372n_reg_read(p, RTL8372N_REG_EXT_CPU_CTRL, &v))
		dev_info(dev, "diag: EXT_CPU_CTRL = 0x%08x (port=%u, want 3)\n",
			 v, v & 0xF);

	if (!rtl8372n_reg_read(p, RTL8372N_REG_CPU_TAG_AWARE, &v))
		dev_info(dev, "diag: CPU_TAG_AWARE = 0x%08x (mask, want BIT(3)=0x008)\n", v);

	if (!rtl8372n_reg_read(p, RTL8372N_REG_VLAN_CTRL, &v))
		dev_info(dev, "diag: VLAN_CTRL = 0x%08x\n", v);

	if (!rtl8372n_reg_read(p, RTL8372N_REG_VLAN_PORT_EGR_TAG, &v))
		dev_info(dev, "diag: VLAN_PORT_EGR_TAG = 0x%08x (2 bits/port, 0=ORIGINAL)\n", v);

	if (!rtl8372n_reg_read(p, RTL8372N_REG_VLAN_PORT_IGR_FLTR, &v))
		dev_info(dev, "diag: VLAN_PORT_IGR_FLTR = 0x%08x (per-port ingress filter)\n", v);

	for (port = 3; port <= 8; port++) {
		if (!(port == 3 || port == 5 || port == 6 || port == 7 || port == 8))
			continue;
		if (!rtl8372n_reg_read(p, RTL8372N_REG_PORT_ISO_PMSK(port), &v))
			dev_info(dev, "diag: PORT_ISO[%u] = 0x%08x (allowed-egress mask)\n",
				 port, v);
	}

	for (port = 3; port <= 8; port++) {
		if (!(port == 3 || port == 5 || port == 6 || port == 7 || port == 8))
			continue;
		if (!rtl8372n_reg_read(p, RTL8372N_REG_MAC_FORCE_CTRL0(port), &v))
			dev_info(dev,
				 "diag: MAC_FORCE_CTRL0[%u] = 0x%08x (FORCE_EN=%u LINK_EN=%u DUP=%u SPD=%u TXP=%u RXP=%u)\n",
				 port, v,
				 v & 1, (v >> 1) & 1, (v >> 2) & 1,
				 (v >> 3) & 0xF, (v >> 7) & 1, (v >> 8) & 1);
	}

	if (!rtl8372n_reg_read(p, RTL8372N_REG_MAC_LINK_STS, &v))
		dev_info(dev, "diag: MAC_LINK_STS = 0x%08x (live link bits 9:0; mac-link 25:16)\n", v);

	if (!rtl8372n_reg_read(p, 0x5350, &v))
		dev_info(dev, "diag: L2_CTRL @ 0x5350 = 0x%08x\n", v);
	if (!rtl8372n_reg_read(p, 0x535C, &v))
		dev_info(dev, "diag: L2_SA_MOVE_FORBID @ 0x535C = 0x%08x (bit p = port p forbids SA-move)\n", v);
	if (!rtl8372n_reg_read(p, 0x5390, &v))
		dev_info(dev, "diag: L2_LIMIT_LRN_CNT_PORT3 @ 0x5390 = 0x%08x\n", v);

	/* Storm-control on RX (per-port enable in bit N; vendor SDK rtl8372/storm.c).
	 * If bit 3 is set on any of these, the chip rate-limits packets *ingressing*
	 * the CPU port — i.e. our DSA-injected TX. Vendor init never touches them,
	 * so chip-default state is the open question.
	 */
	if (!rtl8372n_reg_read(p, 0x54E4, &v))
		dev_info(dev, "diag: STORM_BCAST_CTRL @ 0x54E4 = 0x%08x (port3 bit3 = %u)\n",
			 v, (v >> 3) & 1);
	if (!rtl8372n_reg_read(p, 0x54E8, &v))
		dev_info(dev, "diag: STORM_MCAST_CTRL @ 0x54E8 = 0x%08x (port3 bit3 = %u)\n",
			 v, (v >> 3) & 1);
	if (!rtl8372n_reg_read(p, 0x54EC, &v))
		dev_info(dev, "diag: STORM_UNUCAST_CTRL @ 0x54EC = 0x%08x (port3 bit3 = %u)\n",
			 v, (v >> 3) & 1);
	if (!rtl8372n_reg_read(p, 0x54F0, &v))
		dev_info(dev, "diag: STORM_UNMCAST_CTRL @ 0x54F0 = 0x%08x (port3 bit3 = %u)\n",
			 v, (v >> 3) & 1);
	if (!rtl8372n_reg_read(p, 0x54F4, &v))
		dev_info(dev, "diag: STORM_BCAST_METER @ 0x54F4 = 0x%08x (meter_idx bits 23:18 = %u)\n",
			 v, (v >> 18) & 0x3F);
	if (!rtl8372n_reg_read(p, 0x54FC, &v))
		dev_info(dev, "diag: STORM_MCAST_METER @ 0x54FC = 0x%08x (meter_idx bits 23:18 = %u)\n",
			 v, (v >> 18) & 0x3F);
	if (!rtl8372n_reg_read(p, 0x5504, &v))
		dev_info(dev, "diag: STORM_UNUCAST_METER @ 0x5504 = 0x%08x (meter_idx bits 23:18 = %u)\n",
			 v, (v >> 18) & 0x3F);
	if (!rtl8372n_reg_read(p, 0x550C, &v))
		dev_info(dev, "diag: STORM_UNMCAST_METER @ 0x550C = 0x%08x (meter_idx bits 23:18 = %u)\n",
			 v, (v >> 18) & 0x3F);
}

/* Program the default VLAN-1 entry (members=ALL, untag=ALL) and set
 * PVID=1 on every port. Without this the chip rejects all ingress
 * from external ports to the CPU port — the DSA tagger sees only the
 * occasional rogue frame and front-panel LAN ports look dead at L2.
 *
 * 4K CVLAN entry encoding (from _dal_rtl8373_Vlan4kStUser2Smi):
 *   bits  0-9  mbr   (port member mask, 10 ports)
 *   bits 10-19 untag (untag-on-egress mask)
 *   bits 20-23 fid_msti
 *   bit  24    svlan_chk_ivl_svl
 *   bit  25    ivl_svl
 */
static int rtl8372n_program_default_vlan(struct rtl8372n_priv *p,
					 struct device *dev)
{
	const u32 all_ports = 0x3FF;
	u32 entry, ctrl, pvid_pair;
	int ret, retries;
	unsigned int port;

	/* mbr (9:0) + untag (19:10) + fid_msti=1 (23:20) + ivl_svl=1 (bit 25).
	 *
	 * Setting IVL with fid_msti=1 makes the chip's runtime L2 lookup
	 * use FID=1 for frames in VLAN-1. Our pinned static LUT entry was
	 * inserted with ivl_svl=1 / vid_fid=1, so this is what the lookup
	 * needs to find it. Without these bits the chip is in SVL mode
	 * (FID=0) and our FID-1 entry is invisible to the forwarding path
	 * — symptom: laptop's ARP replies to our MAC are not trapped to
	 * CPU even with the static entry pinned. See
	 * memory/project_flint3_fdb_aging_ruled_out.md for the full chase.
	 */
	entry = (all_ports & 0x3FF)
	      | ((all_ports & 0x3FF) << 10)
	      | (1u << 20)   /* fid_msti = 1 */
	      | (1u << 25);  /* ivl_svl   = 1 (IVL) */

	ret = rtl8372n_reg_write(p, RTL8372N_REG_ITA_WRITE_DATA0, entry);
	if (ret) {
		dev_warn(dev, "vlan-init: WRITE_DATA0 failed: %d\n", ret);
		return ret;
	}

	ctrl = (1u << RTL8372N_ITA_TBL_ADDR_SHIFT)
	     | (RTL8372N_ITA_TB_TARGET_CVLAN << RTL8372N_ITA_TLB_TYPE_SHIFT)
	     | RTL8372N_ITA_TB_OP_WRITE
	     | RTL8372N_ITA_TB_EXECUTE;
	ret = rtl8372n_reg_write(p, RTL8372N_REG_ITA_CTRL0, ctrl);
	if (ret) {
		dev_warn(dev, "vlan-init: CTRL0 trigger failed: %d\n", ret);
		return ret;
	}

	for (retries = 0; retries < 200; retries++) {
		ret = rtl8372n_reg_read(p, RTL8372N_REG_ITA_CTRL0, &ctrl);
		if (ret)
			return ret;
		if (!(ctrl & RTL8372N_ITA_TB_EXECUTE))
			break;
	}
	if (ctrl & RTL8372N_ITA_TB_EXECUTE) {
		dev_warn(dev, "vlan-init: CVLAN write never completed (ctrl=0x%x)\n",
			 ctrl);
		return -ETIMEDOUT;
	}
	dev_info(dev, "vlan-init: VLAN-1 entry written (mbr=untag=0x%x)\n",
		 all_ports);

	pvid_pair = 1u | (1u << 12);
	for (port = 0; port < 10; port += 2) {
		ret = rtl8372n_reg_write(p, RTL8372N_REG_VLAN_PORT_PVID(port),
					 pvid_pair);
		if (ret) {
			dev_warn(dev, "vlan-init: PVID port %u failed: %d\n",
				 port, ret);
			return ret;
		}
	}
	(void)rtl8372n_reg_read(p, RTL8372N_REG_VLAN_PORT_PVID(0), &ctrl);
	dev_info(dev, "vlan-init: PVID=1 set on ports 0..9; PVID[0/1]=0x%08x\n",
		 ctrl);

	/* Egress tag mode = ORIGINAL on all ports (2 bits per port, write 0
	 * to clear bits 0..19). Without this, the chip may default to a
	 * mode that adds an 802.1Q tag on egress — which the host sees as
	 * a malformed frame.
	 */
	ret = rtl8372n_reg_write(p, RTL8372N_REG_VLAN_PORT_EGR_TAG, 0);
	if (ret)
		dev_warn(dev, "vlan-init: EGR_TAG write failed: %d\n", ret);

	/* Clear L2-learning-disable table. Vendor's init does this; without
	 * it the chip may refuse to learn MAC addresses for our VLAN.
	 */
	(void)rtl8372n_reg_write(p, RTL8372N_REG_VLAN_L2_LRN_DIS(0), 0);
	(void)rtl8372n_reg_write(p, RTL8372N_REG_VLAN_L2_LRN_DIS(1), 0);

	/* Readback for sanity. */
	(void)rtl8372n_reg_read(p, RTL8372N_REG_VLAN_PORT_EGR_TAG, &ctrl);
	dev_info(dev, "vlan-init: EGR_TAG = 0x%08x (want 0 = ORIGINAL)\n", ctrl);

	/* Ingress + egress VLAN filtering deliberately NOT enabled. With
	 * filtering ON the chip drops 100% of test ARP/ICMP traffic —
	 * presumably because our minimum VLAN setup doesn't program every
	 * port-membership detail the filter checks against. Leave them
	 * disabled until we know the full table is correct.
	 */
	(void)rtl8372n_reg_read(p, RTL8372N_REG_VLAN_CTRL, &ctrl);
	dev_info(dev, "vlan-init: VLAN_CTRL = 0x%08x\n", ctrl);

	return 0;
}

/* Experimental: flip EXT_CPUTAG_EN on so that frames forwarded from any
 * switch port to port 3 (the CPU port) are PREPENDED with the Realtek
 * proprietary 0x8899-EtherType tag. This lets us capture raw frames at
 * eth0 with an AF_PACKET socket and inspect the actual tag bytes the
 * chip emits — needed before we commit to a DSA tag protocol
 * (DSA_TAG_PROTO_RTL8_4 vs a new one specific to RTL8372N).
 *
 * EXT_INSERTMOD is set to CPU_INSERT_TO_ALL (0) so every CPU-bound frame
 * carries the tag — not just trap frames.
 *
 * Traffic CPU→LAN won't work in this state because the kernel can't
 * prepend the matching tag yet (no DSA tagger wired in). That's fine —
 * we're only trying to observe the chip's tag emission shape here.
 *
 * After analysis we'll either: (a) confirm tag_rtl8_4 matches and reuse
 * it, or (b) write a fresh tag_rtl8372n.c with the right protocol byte
 * and field offsets.
 */
static int __maybe_unused
rtl8372n_capture_enable(struct rtl8372n_priv *p, struct device *dev)
{
	u32 pre, post;
	int ret;

	ret = rtl8372n_reg_read(p, RTL8372N_REG_CPU_TAG_CTRL, &pre);
	if (ret) {
		dev_warn(dev, "capture-enable: CPU_TAG_CTRL read failed: %d\n", ret);
		return ret;
	}

	post = pre;
	post &= ~RTL8372N_CPU_TAG_CTRL_EXT_MODE;
	post |= FIELD_PREP(RTL8372N_CPU_TAG_CTRL_EXT_MODE,
			   RTL8372N_CPU_TAG_INSERT_ALL);
	post |= RTL8372N_CPU_TAG_CTRL_EXT_EN;

	ret = rtl8372n_reg_write(p, RTL8372N_REG_CPU_TAG_CTRL, post);
	if (ret) {
		dev_warn(dev, "capture-enable: CPU_TAG_CTRL write failed: %d\n", ret);
		return ret;
	}
	(void)rtl8372n_reg_read(p, RTL8372N_REG_CPU_TAG_CTRL, &post);
	dev_info(dev, "capture-enable: CPU_TAG_CTRL 0x%08x -> 0x%08x (EXT_EN+MODE=ALL)\n",
		 pre, post);
	return 0;
}

/* Force a port's MAC into a fixed link state with explicit speed/duplex.
 * Used for port 3 (the CPU port towards the SoC PCS) on Flint 3: vendor
 * SDK configures the switch's MAC for port 3 at 10G/full, link-forced-up.
 * Without this, port 3 leaves U-Boot's defaults and the SerDes never
 * actually transmits framing back to the SoC — which leaves the SoC's
 * PCS unable to recover an RX clock (see
 * memory/project_flint3_pcs_rx_asymmetric.md).
 *
 * The "force-down before force-up" dance mirrors what vendor's
 * dal_rtl8373_portFrcAbility_set does when the port is already linked.
 * We always do it here on probe to get into a known state, since the
 * pre-state is whatever U-Boot left behind.
 */
static int __maybe_unused
rtl8372n_force_port_mac(struct rtl8372n_priv *p, struct device *dev,
			u8 port, u8 speed_sel, bool full_duplex)
{
	u32 pre, val, post;
	int ret;

	ret = rtl8372n_reg_read(p, RTL8372N_REG_MAC_FORCE_CTRL0(port), &pre);
	if (ret) {
		dev_warn(dev, "MAC_FORCE_CTRL0[%u] pre-read failed: %d\n",
			 port, ret);
		return ret;
	}

	/* Step 1: force-down (FORCE_EN=1, FORCE_LINK=0) so any later cfg
	 * write doesn't get rejected mid-link. Per vendor SDK pattern.
	 */
	ret = rtl8372n_reg_write(p, RTL8372N_REG_MAC_FORCE_CTRL0(port),
				 RTL8372N_MAC_FORCE_EN);
	if (ret) {
		dev_warn(dev, "MAC_FORCE_CTRL0[%u] force-down failed: %d\n",
			 port, ret);
		return ret;
	}
	usleep_range(1000, 2000);

	/* Step 2: write target state — link up, speed, duplex, pause both. */
	val = RTL8372N_MAC_FORCE_EN |
	      RTL8372N_FORCE_LINK_EN |
	      (full_duplex ? RTL8372N_FORCE_DUPLEX_FULL : 0) |
	      ((speed_sel << RTL8372N_FORCE_SPD_SHIFT) &
	       RTL8372N_FORCE_SPD_MASK) |
	      RTL8372N_FORCE_TX_PAUSE |
	      RTL8372N_FORCE_RX_PAUSE;
	ret = rtl8372n_reg_write(p, RTL8372N_REG_MAC_FORCE_CTRL0(port), val);
	if (ret) {
		dev_warn(dev, "MAC_FORCE_CTRL0[%u] force-up failed: %d\n",
			 port, ret);
		return ret;
	}
	usleep_range(1000, 2000);

	(void)rtl8372n_reg_read(p, RTL8372N_REG_MAC_FORCE_CTRL0(port), &post);
	dev_info(dev,
		 "port %u MAC force-link: 0x%08x -> 0x%08x (wanted 0x%08x, speed=%u, fdx=%u)\n",
		 port, pre, post, val, speed_sel, full_duplex);

	{
		u32 link, dup;
		(void)rtl8372n_reg_read(p, RTL8372N_REG_MAC_LINK_STS, &link);
		(void)rtl8372n_reg_read(p, RTL8372N_REG_MAC_LINK_DUP_STS, &dup);
		dev_info(dev,
			 "port %u after force: MAC_LINK_STS link=%u dup=%u (port-bit %u)\n",
			 port,
			 (link >> port) & 1,
			 (dup >> port) & 1,
			 port);
	}
	return 0;
}

static int rtl8372n_detect(struct rtl8372n_priv *p, u16 *model_out)
{
	u32 v;
	int ret;

	ret = rtl8372n_reg_read(p, RTL8372N_REG_CHIP_ID, &v);
	if (ret)
		return ret;

	*model_out = (v >> 16) & 0xFFFF;
	switch (*model_out) {
	case RTL8372N_CHIP_ID_8372:
	case RTL8372N_CHIP_ID_8373:
		return 0;
	default:
		return -ENODEV;
	}
}

/* Indirect MIB-counter access. To read counter mibid N for port P, write
 * CTRL = (mibid/2 << 5) | (port << 1) | 1, poll bit 0 until clear,
 * then read CNT_L (mibid+0 32-bit counter) and CNT_H (mibid+1 counter).
 * See vendor SDK dal_rtl8373_portMib_read.
 */
#define RTL8372N_REG_MIB_CTRL		0x0F60
#define RTL8372N_REG_MIB_CNT_L		0x0F64
#define RTL8372N_REG_MIB_CNT_H		0x0F68
#define RTL8372N_MIB_CTRL_TRIG		BIT(0)

/* MIB index constants from rtk_stat_port_type_e in vendor mib.h.
 * Pairs share an indirect slot (mibid/2).
 */
#define MIB_IF_IN_OCTETS_H		0
#define MIB_IF_IN_UCAST_PKTS_H		4
#define MIB_IF_OUT_UCAST_PKTS_H		10
#define MIB_IF_OUT_DISCARDS		16
#define MIB_DOT1D_TP_PORT_IN_DISCARDS	17
#define MIB_RX_MAC_DISCARDS		72

/* RTL8373 Indirect Table Access (ITA) registers used to read/write the
 * L2 LUT entries. Layout cross-referenced with vendor SDK
 * dal_rtl8373_lut.c / rtl8373_reg_definition.h:
 *   ITA_CTRL0     0x5CAC  command/status (bit 0 = busy/exec, bit 1 = act,
 *                          bits 10:8 = tbl type)
 *   ITA_L2_CTRL   0x5CB0  L2-specific control (bit 12 = ACT_STS hit/ok)
 *   ITA_WRITE_DATA(i)  0x5CB8 + i*4 (i=0..4) — 3 used for L2 entry
 */
#define RTL8372N_REG_ITA_CTRL0		0x5CAC
#define RTL8372N_REG_ITA_L2_CTRL	0x5CB0
#define RTL8372N_REG_ITA_WRITE_DATA(i)	(0x5CB8 + ((i) << 2))
#define RTL8372N_REG_ITA_READ_DATA(i)	(0x5CCC + ((i) << 2))
#define RTL8372N_ITA_TBL_L2		4   /* L2 LUT */
#define RTL8372N_ITA_CMD_WRITE		(BIT(1) | (RTL8372N_ITA_TBL_L2 << 8) | BIT(0))
#define RTL8372N_ITA_CMD_READ		((RTL8372N_ITA_TBL_L2 << 8) | BIT(0))
#define RTL8372N_ITA_BUSY		BIT(0)
#define RTL8372N_ITA_L2_ACT_STS		BIT(12)
#define RTL8372N_ITA_L2_READ_MTHD_MASK	GENMASK(17, 14)
#define RTL8372N_ITA_L2_READ_MTHD_MAC	0   /* LUTREADMETHOD_MAC */

/* Install a static unicast FDB entry mapping @mac on VLAN @vid to @port.
 * Used to pin the CPU's MAC -> port 3 so unicasts to the host stack are
 * always switch-trapped (with cpu-tag insertion), independent of the
 * auto-learning FDB aging that we've observed dropping ARP/ICMP replies
 * intermittently.
 *
 * On-wire encoding from vendor SDK _rtl8373_fdbStUser2Smi (unicast slot):
 *   smi[0]  = octet[5] | (octet[4]<<8) | (octet[3]<<16) | (octet[2]<<24)
 *   smi[1]  = octet[1] | (octet[0]<<8)
 *             | ((vid & 0xFFF) << 16)
 *             | (l3lookup << 28)        // 0
 *             | (ivl_svl << 29)         // 1 = independent-VLAN learning
 *             | ((spa & 0x3) << 30)     // low 2 bits of source port
 *   smi[2]  = (spa >> 2) & 0x3          // high 2 bits of source port
 *             | (age << 2)              // 6 = max age, ~static
 *             | (auth << 5)             // 0
 *             | (nosalearn << 16)       // 1 = static, don't auto-relearn
 */
static int rtl8372n_l2_static_fdb_add(struct rtl8372n_priv *p,
				      const u8 *mac, u8 port, u16 vid)
{
	const u32 spa = port & 0xF;
	const u32 age = 6;
	const u32 ivl_svl = 1;
	const u32 nosalearn = 1;
	u32 smi[3];
	u32 v;
	int ret, busy;

	if (!mac || (mac[0] & 0x01))
		return -EINVAL;

	smi[0] = (u32)mac[5]        |
		 ((u32)mac[4] << 8)  |
		 ((u32)mac[3] << 16) |
		 ((u32)mac[2] << 24);
	smi[1] = (u32)mac[1]        |
		 ((u32)mac[0] << 8)  |
		 (((u32)vid & 0xFFF) << 16) |
		 (ivl_svl << 29)     |
		 ((spa & 0x3) << 30);
	smi[2] = ((spa >> 2) & 0x3) |
		 (age << 2)         |
		 (nosalearn << 16);

	/* Wait for previous command to drain. */
	busy = 200;
	while (busy--) {
		ret = rtl8372n_reg_read(p, RTL8372N_REG_ITA_CTRL0, &v);
		if (ret)
			return ret;
		if (!(v & RTL8372N_ITA_BUSY))
			break;
	}
	if (busy <= 0)
		return -EBUSY;

	ret = rtl8372n_reg_write(p, RTL8372N_REG_ITA_WRITE_DATA(0), smi[0]);
	if (ret)
		return ret;
	ret = rtl8372n_reg_write(p, RTL8372N_REG_ITA_WRITE_DATA(1), smi[1]);
	if (ret)
		return ret;
	ret = rtl8372n_reg_write(p, RTL8372N_REG_ITA_WRITE_DATA(2), smi[2]);
	if (ret)
		return ret;

	ret = rtl8372n_reg_write(p, RTL8372N_REG_ITA_CTRL0,
				 RTL8372N_ITA_CMD_WRITE);
	if (ret)
		return ret;

	busy = 200;
	while (busy--) {
		ret = rtl8372n_reg_read(p, RTL8372N_REG_ITA_CTRL0, &v);
		if (ret)
			return ret;
		if (!(v & RTL8372N_ITA_BUSY))
			break;
	}
	if (busy <= 0)
		return -EBUSY;

	ret = rtl8372n_reg_read(p, RTL8372N_REG_ITA_L2_CTRL, &v);
	if (ret)
		return ret;
	if (!(v & RTL8372N_ITA_L2_ACT_STS))
		return -EIO;

	return 0;
}

/* Read the L2 LUT entry that matches @mac on VLAN @vid. Returns 0 on hit
 * and fills *out_smi with the 3 chip-format words (use the same encoding
 * as _rtl8373_fdbStSmi2User to decode). Returns -ENOENT if not found.
 */
static int rtl8372n_l2_static_fdb_get(struct rtl8372n_priv *p,
				      const u8 *mac, u16 vid,
				      u32 out_smi[3])
{
	const u32 ivl_svl = 1;
	u32 smi[3] = {0};
	u32 v;
	int ret, busy;

	if (!mac || (mac[0] & 0x01))
		return -EINVAL;

	/* Build the lookup key — same encoding as write, but only the
	 * MAC + VID + ivl_svl bits matter for the hash lookup.
	 */
	smi[0] = (u32)mac[5]        |
		 ((u32)mac[4] << 8)  |
		 ((u32)mac[3] << 16) |
		 ((u32)mac[2] << 24);
	smi[1] = (u32)mac[1]        |
		 ((u32)mac[0] << 8)  |
		 (((u32)vid & 0xFFF) << 16) |
		 (ivl_svl << 29);
	smi[2] = 0;

	busy = 200;
	while (busy--) {
		ret = rtl8372n_reg_read(p, RTL8372N_REG_ITA_CTRL0, &v);
		if (ret)
			return ret;
		if (!(v & RTL8372N_ITA_BUSY))
			break;
	}
	if (busy <= 0)
		return -EBUSY;

	/* Set read method = MAC (value 0, already masked). */
	ret = rtl8372n_reg_read(p, RTL8372N_REG_ITA_L2_CTRL, &v);
	if (ret)
		return ret;
	v &= ~RTL8372N_ITA_L2_READ_MTHD_MASK;
	v |= FIELD_PREP(RTL8372N_ITA_L2_READ_MTHD_MASK,
			RTL8372N_ITA_L2_READ_MTHD_MAC);
	ret = rtl8372n_reg_write(p, RTL8372N_REG_ITA_L2_CTRL, v);
	if (ret)
		return ret;

	ret = rtl8372n_reg_write(p, RTL8372N_REG_ITA_WRITE_DATA(0), smi[0]);
	if (ret)
		return ret;
	ret = rtl8372n_reg_write(p, RTL8372N_REG_ITA_WRITE_DATA(1), smi[1]);
	if (ret)
		return ret;
	ret = rtl8372n_reg_write(p, RTL8372N_REG_ITA_WRITE_DATA(2), smi[2]);
	if (ret)
		return ret;

	ret = rtl8372n_reg_write(p, RTL8372N_REG_ITA_CTRL0,
				 RTL8372N_ITA_CMD_READ);
	if (ret)
		return ret;

	busy = 200;
	while (busy--) {
		ret = rtl8372n_reg_read(p, RTL8372N_REG_ITA_CTRL0, &v);
		if (ret)
			return ret;
		if (!(v & RTL8372N_ITA_BUSY))
			break;
	}
	if (busy <= 0)
		return -EBUSY;

	ret = rtl8372n_reg_read(p, RTL8372N_REG_ITA_L2_CTRL, &v);
	if (ret)
		return ret;
	if (!(v & RTL8372N_ITA_L2_ACT_STS))
		return -ENOENT;

	ret = rtl8372n_reg_read(p, RTL8372N_REG_ITA_READ_DATA(0), &out_smi[0]);
	if (ret)
		return ret;
	ret = rtl8372n_reg_read(p, RTL8372N_REG_ITA_READ_DATA(1), &out_smi[1]);
	if (ret)
		return ret;
	ret = rtl8372n_reg_read(p, RTL8372N_REG_ITA_READ_DATA(2), &out_smi[2]);
	if (ret)
		return ret;

	return 0;
}

static void rtl8372n_fdb_diag_work(struct work_struct *work);

/* Read one pair of MIB counters for a port. @mibid must be even (the
 * "_H" index of a pair). Returns counter values for mibid in *cnt_lo
 * and mibid+1 in *cnt_hi.
 */
static int rtl8372n_mib_read_pair(struct rtl8372n_priv *p, u8 port,
				  u8 mibid, u32 *cnt_lo, u32 *cnt_hi)
{
	u32 v;
	int ret, busy;

	if (mibid & 1)
		return -EINVAL;

	v = ((u32)(mibid / 2) << 5) | ((u32)(port & 0xF) << 1) | BIT(0);
	ret = rtl8372n_reg_write(p, RTL8372N_REG_MIB_CTRL, v);
	if (ret)
		return ret;

	busy = 200;
	while (busy--) {
		ret = rtl8372n_reg_read(p, RTL8372N_REG_MIB_CTRL, &v);
		if (ret)
			return ret;
		if (!(v & RTL8372N_MIB_CTRL_TRIG))
			break;
	}
	if (busy <= 0)
		return -EBUSY;

	ret = rtl8372n_reg_read(p, RTL8372N_REG_MIB_CNT_L, cnt_lo);
	if (ret)
		return ret;
	return rtl8372n_reg_read(p, RTL8372N_REG_MIB_CNT_H, cnt_hi);
}


/* Decode and log a LUT entry — names match vendor _rtl8373_fdbStSmi2User. */
static void rtl8372n_l2_log_entry(struct device *dev, const char *tag,
				  const u8 *mac, const u32 smi[3])
{
	u32 cvid_fid    = (smi[1] >> 16) & 0xFFF;
	u32 l3lookup    = (smi[1] >> 28) & 1;
	u32 ivl_svl     = (smi[1] >> 29) & 1;
	u32 spa         = ((smi[2] & 0x3) << 2) | ((smi[1] >> 30) & 3);
	u32 age         = (smi[2] >> 2) & 0x7;
	u32 auth        = (smi[2] >> 5) & 0x1;
	u32 nosalearn   = (smi[2] >> 16) & 0x1;

	dev_info(dev,
		 "LUT %s: mac=%pM smi=%08x:%08x:%08x port=%u vid=%u ivl=%u l3=%u age=%u auth=%u static(nosalearn)=%u\n",
		 tag, mac, smi[0], smi[1], smi[2],
		 spa, cvid_fid, ivl_svl, l3lookup, age, auth, nosalearn);
}

/* Periodic per-port MIB-counter dump. Fires at 30s, 60s, 120s after
 * setup. Logs key in/out unicast/discard counters for CPU port 3 and
 * user port 7 (lan1) so we can see where unicast frames addressed to
 * our pinned MAC are being dropped. The LUT entry has already been
 * confirmed stable in earlier sessions (see memory/
 * project_flint3_fdb_aging_ruled_out.md), so no need to re-read it.
 */
static void rtl8372n_fdb_diag_work(struct work_struct *work)
{
	struct rtl8372n_priv *p = container_of(work, struct rtl8372n_priv,
					       fdb_diag_work.work);
	struct device *dev = p->ds ? p->ds->dev : NULL;
	static const unsigned int delays_ms[] = {
		30000, 30000, 60000, 0
	};
	static const u8 ports[] = { 3, 7 };
	int i, ret;

	if (!dev)
		return;

	for (i = 0; i < ARRAY_SIZE(ports); i++) {
		u8 port = ports[i];
		u32 in_uc_hi = 0, in_uc_lo = 0;
		u32 out_uc_hi = 0, out_uc_lo = 0;
		u32 out_discards = 0, in_discards = 0;
		u32 rx_mac_discards = 0, dummy = 0;

		/* ifInUcastPkts pair (H, L) */
		ret = rtl8372n_mib_read_pair(p, port, MIB_IF_IN_UCAST_PKTS_H,
					     &in_uc_hi, &in_uc_lo);
		if (ret)
			dev_warn(dev, "MIB read ifInUcastPkts port %u failed: %d\n",
				 port, ret);

		/* ifOutUcastPkts pair (H, L) */
		ret = rtl8372n_mib_read_pair(p, port, MIB_IF_OUT_UCAST_PKTS_H,
					     &out_uc_hi, &out_uc_lo);
		if (ret)
			dev_warn(dev, "MIB read ifOutUcastPkts port %u failed: %d\n",
				 port, ret);

		/* ifOutDiscards (mibid=16) + dot1dTpPortInDiscards (mibid=17) */
		ret = rtl8372n_mib_read_pair(p, port, MIB_IF_OUT_DISCARDS,
					     &out_discards, &in_discards);
		if (ret)
			dev_warn(dev, "MIB read OutDiscards port %u failed: %d\n",
				 port, ret);

		/* rxMacDiscards (mibid=72) + rxMacIPGShortDropRT (mibid=73) */
		ret = rtl8372n_mib_read_pair(p, port, MIB_RX_MAC_DISCARDS,
					     &rx_mac_discards, &dummy);
		if (ret)
			dev_warn(dev, "MIB read rxMacDiscards port %u failed: %d\n",
				 port, ret);

		dev_info(dev,
			 "MIB t+%us port=%u: inUcast=(H=%u L=%u) outUcast=(H=%u L=%u) outDisc=%u inDisc=%u rxMacDisc=%u\n",
			 (p->fdb_diag_iter * 30 + 30),
			 port,
			 in_uc_hi, in_uc_lo,
			 out_uc_hi, out_uc_lo,
			 out_discards, in_discards, rx_mac_discards);
	}

	if (p->fdb_diag_iter < ARRAY_SIZE(delays_ms) &&
	    delays_ms[p->fdb_diag_iter] > 0) {
		schedule_delayed_work(&p->fdb_diag_work,
				      msecs_to_jiffies(delays_ms[p->fdb_diag_iter]));
		p->fdb_diag_iter++;
	}
}

/* DSA switch ops — minimum viable: tag protocol + setup. */

static enum dsa_tag_protocol
rtl8372n_get_tag_protocol(struct dsa_switch *ds, int port,
			  enum dsa_tag_protocol mp)
{
	/* Wire-format capture (see memory/project_flint3_tag_protocol_confirmed.md)
	 * confirmed the chip emits the rtl8_4 form: 8-byte tag with
	 * protocol byte 0x04, positioned between MAC SA and the original
	 * EtherType. The mainline tagger at net/dsa/tag_rtl8_4.c handles
	 * both directions.
	 */
	return DSA_TAG_PROTO_RTL8_4;
}

/* Low-level 4K VLAN-table write — shared by the boot-time default-VLAN
 * setup and the runtime port_vlan_add/del callbacks.
 *
 * mbr   : 10-bit port membership mask (which chip ports forward this VID)
 * untag : 10-bit untag-on-egress mask (subset of mbr to strip the tag)
 *
 * Always sets ivl_svl=1 + fid_msti=1 so the chip uses FID=1 for runtime
 * L2 lookups in this VID — matching the static FDB entries we install
 * (see rtl8372n_program_default_vlan() for the long form of the why).
 */
static int rtl8372n_write_vlan4k(struct rtl8372n_priv *p, u16 vid,
				 u16 mbr, u16 untag)
{
	u32 entry, ctrl;
	int ret, retries;

	entry = (mbr & 0x3FF)
	      | ((untag & 0x3FF) << 10)
	      | (1u << 20)   /* fid_msti = 1 */
	      | (1u << 25);  /* ivl_svl   = 1 (IVL) */

	ret = rtl8372n_reg_write(p, RTL8372N_REG_ITA_WRITE_DATA0, entry);
	if (ret)
		return ret;

	ctrl = ((u32)vid << RTL8372N_ITA_TBL_ADDR_SHIFT)
	     | (RTL8372N_ITA_TB_TARGET_CVLAN << RTL8372N_ITA_TLB_TYPE_SHIFT)
	     | RTL8372N_ITA_TB_OP_WRITE
	     | RTL8372N_ITA_TB_EXECUTE;
	ret = rtl8372n_reg_write(p, RTL8372N_REG_ITA_CTRL0, ctrl);
	if (ret)
		return ret;

	for (retries = 0; retries < 200; retries++) {
		ret = rtl8372n_reg_read(p, RTL8372N_REG_ITA_CTRL0, &ctrl);
		if (ret)
			return ret;
		if (!(ctrl & RTL8372N_ITA_TB_EXECUTE))
			return 0;
	}
	return -ETIMEDOUT;
}

/* DSA: enable/disable 802.1Q VLAN filtering on a port. The chip is
 * always running its VLAN engine (it has to be — we already use VID 1
 * for the default LAN bridge), so this is a no-op acknowledgement.
 * A real implementation would gate per-port "accept tagged frames"
 * bits in the chip's PVR register, but the bridge layer doesn't rely
 * on those for forwarding decisions; the 4K table membership in
 * port_vlan_add/del is sufficient.
 */
static int rtl8372n_port_vlan_filtering(struct dsa_switch *ds, int port,
					bool vlan_filtering,
					struct netlink_ext_ack *extack)
{
	dev_dbg(ds->dev, "port %d vlan_filtering=%d (noop)\n",
		port, vlan_filtering);
	return 0;
}

/* DSA: add a VID to a port's membership set. Accumulates state in
 * priv->vid_mbr / vid_untag and rewrites the 4K table entry.
 */
static int rtl8372n_port_vlan_add(struct dsa_switch *ds, int port,
				  const struct switchdev_obj_port_vlan *vlan,
				  struct netlink_ext_ack *extack)
{
	struct rtl8372n_priv *p = ds->priv;
	u16 vid = vlan->vid;
	u16 mbr, untag;
	int ret;

	if (vid == 0 || vid >= 4096)
		return -EINVAL;
	if (port < 0 || port >= RTL8372N_NUM_PORTS)
		return -EINVAL;

	/* DSA calls us under rtnl_lock; reg_read/write serialise their
	 * own MDIO access via p->lock internally. No outer mutex here —
	 * grabbing it would deadlock (the reg helpers re-acquire it).
	 */

	mbr = p->vid_mbr[vid] | BIT(port);
	if (vlan->flags & BRIDGE_VLAN_INFO_UNTAGGED)
		untag = p->vid_untag[vid] | BIT(port);
	else
		untag = p->vid_untag[vid] & ~BIT(port);

	ret = rtl8372n_write_vlan4k(p, vid, mbr, untag);
	if (ret) {
		dev_warn(ds->dev,
			 "vlan_add: vid=%u port=%d write failed (ret=%d)\n",
			 vid, port, ret);
		return ret;
	}

	p->vid_mbr[vid] = mbr;
	p->vid_untag[vid] = untag;

	/* Set port PVID for ingress untagged frames in this VID. */
	if (vlan->flags & BRIDGE_VLAN_INFO_PVID) {
		u32 cur, pair_reg = RTL8372N_REG_VLAN_PORT_PVID(port);
		u32 shift = (port & 1) ? 12 : 0;
		u32 mask = 0xFFF << shift;

		ret = rtl8372n_reg_read(p, pair_reg, &cur);
		if (!ret) {
			cur = (cur & ~mask) | ((vid & 0xFFF) << shift);
			ret = rtl8372n_reg_write(p, pair_reg, cur);
		}
		if (ret)
			dev_warn(ds->dev,
				 "vlan_add: PVID program port=%d vid=%u failed (ret=%d)\n",
				 port, vid, ret);
	}

	dev_dbg(ds->dev,
		"vlan_add: vid=%u port=%d flags=0x%x -> mbr=0x%03x untag=0x%03x\n",
		vid, port, vlan->flags, mbr, untag);
	return ret;
}

/* DSA: remove a VID from a port's membership set. */
static int rtl8372n_port_vlan_del(struct dsa_switch *ds, int port,
				  const struct switchdev_obj_port_vlan *vlan)
{
	struct rtl8372n_priv *p = ds->priv;
	u16 vid = vlan->vid;
	u16 mbr, untag;
	int ret;

	if (vid == 0 || vid >= 4096)
		return -EINVAL;
	if (port < 0 || port >= RTL8372N_NUM_PORTS)
		return -EINVAL;

	mbr = p->vid_mbr[vid] & ~BIT(port);
	untag = p->vid_untag[vid] & ~BIT(port);

	ret = rtl8372n_write_vlan4k(p, vid, mbr, untag);
	if (ret) {
		dev_warn(ds->dev,
			 "vlan_del: vid=%u port=%d write failed (ret=%d)\n",
			 vid, port, ret);
		return ret;
	}

	p->vid_mbr[vid] = mbr;
	p->vid_untag[vid] = untag;

	dev_dbg(ds->dev,
		"vlan_del: vid=%u port=%d -> mbr=0x%03x untag=0x%03x\n",
		vid, port, mbr, untag);
	return ret;
}

static int rtl8372n_dsa_setup(struct dsa_switch *ds)
{
	struct rtl8372n_priv *p = ds->priv;
	struct dsa_port *cpu_dp;
	int ret;

	/* The chip-side staging + EXT_CPUTAG-enable were already done in
	 * the mdio probe path; nothing else to program here for the
	 * first-cut DSA bring-up. Leave room for VLAN/learning/STP
	 * configuration when bridge support is added.
	 */
	dev_info(ds->dev, "DSA setup complete (cpu-tag insertion already enabled)\n");

	/* Pin the conduit (master eth0) MAC to the CPU port in the L2
	 * LUT as a static entry. Without this, unicast frames addressed
	 * to the host kernel network stack get treated as unknown-unicast
	 * by the switch and either flooded (without CPU-tag insertion) or
	 * dropped — which we observed as intermittent ARP/ICMP-reply RX.
	 * See memory/project_flint3_intermittent_unicast_rx.md.
	 */
	cpu_dp = dsa_to_port(ds, RTL8372N_EXT_CPU_PORT);
	if (cpu_dp && cpu_dp->conduit) {
		const u8 *mac = cpu_dp->conduit->dev_addr;
		u32 smi[3];

		ret = rtl8372n_l2_static_fdb_add(p, mac,
						RTL8372N_EXT_CPU_PORT, 1);
		if (ret) {
			dev_warn(ds->dev,
				 "static FDB add for conduit MAC %pM -> port %u failed: %d\n",
				 mac, RTL8372N_EXT_CPU_PORT, ret);
		} else {
			dev_info(ds->dev,
				 "pinned conduit MAC %pM to CPU port %u in L2 LUT (static)\n",
				 mac, RTL8372N_EXT_CPU_PORT);

			/* Read it back immediately so we can see how the
			 * chip actually stored it — verifies nosalearn /
			 * age / port encoding.
			 */
			ret = rtl8372n_l2_static_fdb_get(p, mac, 1, smi);
			if (ret)
				dev_warn(ds->dev,
					 "LUT readback immediately after add failed: %d\n",
					 ret);
			else
				rtl8372n_l2_log_entry(ds->dev, "post-add",
						      mac, smi);

			/* Re-enabled — now reads MIB counters instead of
			 * LUT, so we can see where unicast frames are
			 * dropped between the chip's L2 lookup and the
			 * CPU port wire.
			 */
			ether_addr_copy(p->fdb_diag_mac, mac);
			p->fdb_diag_iter = 0;
			schedule_delayed_work(&p->fdb_diag_work,
					      msecs_to_jiffies(30000));
		}

		/* One-shot diagnostic dump of CPU port (3) chip-side state.
		 * Used to look for clues about why post-IVL/FID-fix unicast
		 * trap throughput is still low (~9% of expected).
		 */
		{
			static const struct {
				u16 addr;
				const char *name;
			} regs[] = {
				/* per-port egress bandwidth */
				{ 0x1C34 + (3 << 10), "EGBW_PORT_CTRL[3]" },
				/* per-port egress bandwidth global */
				{ 0x447C, "EGBW_CTRL (RATE_MODE_CPU + INC_IFG)" },
				/* per-port leaky-bucket token reset */
				{ 0x4484, "EGBW_PORT_LB_RST (bit 3 = port 3)" },
				/* per-port queue scheduler config */
				{ 0x1C30 + (3 << 10), "PORT_SCH_CFG[3]" },
				/* L2 control */
				{ 0x5350, "L2_CTRL" },
				/* learning-count limit for port 3 */
				{ 0x5390, "L2_LIMIT_LRN_CNT_PORT3" },
				/* per-port ingress packet count */
				{ 0x1C10 + (3 << 10), "PKT_CNT_H[3]" },
				{ 0x1C14 + (3 << 10), "PKT_CNT_L[3]" },
				/* MAC L2 global */
				{ 0x4448, "MAC_L2_GLOBAL_CTRL0" },
				{ 0, NULL }
			};
			int i;
			u32 v;

			for (i = 0; regs[i].name; i++) {
				if (rtl8372n_reg_read(p, regs[i].addr, &v) == 0)
					dev_info(ds->dev,
						 "regdump: %s @0x%04x = 0x%08x\n",
						 regs[i].name, regs[i].addr, v);
				else
					dev_warn(ds->dev,
						 "regdump: read of %s @0x%04x failed\n",
						 regs[i].name, regs[i].addr);
			}
		}
	} else {
		dev_warn(ds->dev,
			 "no conduit at CPU port %u — skipping static FDB pin\n",
			 RTL8372N_EXT_CPU_PORT);
	}

	return 0;
}

static void rtl8372n_phylink_get_caps(struct dsa_switch *ds, int port,
				      struct phylink_config *config)
{
	/* Permissive caps for the first cut — let phylink try anything
	 * the DT advertises. Will tighten later once we know which port
	 * actually carries which mode (port 3 = 10GBASE-R to SoC; port
	 * 5..8 = internal copper PHYs at 2.5G).
	 */
	__set_bit(PHY_INTERFACE_MODE_INTERNAL, config->supported_interfaces);
	__set_bit(PHY_INTERFACE_MODE_10GBASER, config->supported_interfaces);
	__set_bit(PHY_INTERFACE_MODE_2500BASEX, config->supported_interfaces);
	__set_bit(PHY_INTERFACE_MODE_USXGMII, config->supported_interfaces);
	config->mac_capabilities = MAC_10 | MAC_100 | MAC_1000FD |
				   MAC_2500FD | MAC_5000FD | MAC_10000FD;
}

/* Translate phylink speed value to the chip's FORCE_SPD_SEL nibble. */
static u8 rtl8372n_phylink_speed_to_spd(int speed)
{
	switch (speed) {
	case SPEED_10:		return 0;
	case SPEED_100:		return 1;
	case SPEED_1000:	return 2;
	case 500:		return 3;
	case SPEED_10000:	return RTL8372N_SPD_10G;
	case SPEED_2500:	return RTL8372N_SPD_2500M;
	case 5000:		return 6;
	default:		return RTL8372N_SPD_10G;
	}
}

/* Called by phylink when a port's link comes up. Used for the CPU port (3)
 * specifically: program the chip's MAC for the negotiated 10GBASE-R link so
 * frames forwarded out port 3 are actually clocked at 10G to match the
 * SoC's PCS / SerDes.
 *
 * Previously (memory/project_flint3_port3_already_up.md) we observed that
 * forcing port 3 to 2.5G broke a working U-Boot 10G link. The chip MAC was
 * apparently picking up the 10G state via some default. But MIB counters
 * now show frames egress port 3 yet never reach SoC eth0 — which suggests
 * the MAC state was *not* truly 10G end-to-end and we just hadn't noticed.
 * Try forcing 10G/full/fibre+pause here and see if that closes the gap.
 *
 * User ports (5, 6, 7) drive copper PHYs and get their MAC state via the
 * PHY's auto-negotiation; we leave their MAC_FORCE_CTRL0 alone.
 */
static void rtl8372n_phylink_mac_link_up(struct dsa_switch *ds, int port,
					 unsigned int mode,
					 phy_interface_t interface,
					 struct phy_device *phydev,
					 int speed, int duplex,
					 bool tx_pause, bool rx_pause)
{
	struct rtl8372n_priv *p = ds->priv;
	struct device *dev = ds->dev;
	u8 spd_sel;
	u32 val, post;
	int ret;

	if (port != RTL8372N_EXT_CPU_PORT)
		return;

	spd_sel = rtl8372n_phylink_speed_to_spd(speed);

	/* Force-down first so we don't poke a live link. */
	ret = rtl8372n_reg_write(p, RTL8372N_REG_MAC_FORCE_CTRL0(port),
				 RTL8372N_MAC_FORCE_EN);
	if (ret) {
		dev_warn(dev, "port %d MAC link-up force-down failed: %d\n",
			 port, ret);
		return;
	}
	usleep_range(1000, 2000);

	val = RTL8372N_MAC_FORCE_EN |
	      RTL8372N_FORCE_LINK_EN |
	      (duplex == DUPLEX_FULL ? RTL8372N_FORCE_DUPLEX_FULL : 0) |
	      ((spd_sel << RTL8372N_FORCE_SPD_SHIFT) &
	       RTL8372N_FORCE_SPD_MASK) |
	      (tx_pause ? RTL8372N_FORCE_TX_PAUSE : 0) |
	      (rx_pause ? RTL8372N_FORCE_RX_PAUSE : 0) |
	      RTL8372N_FORCE_MEDIA_FIBRE;

	ret = rtl8372n_reg_write(p, RTL8372N_REG_MAC_FORCE_CTRL0(port), val);
	if (ret) {
		dev_warn(dev, "port %d MAC link-up force-up failed: %d\n",
			 port, ret);
		return;
	}
	usleep_range(1000, 2000);

	(void)rtl8372n_reg_read(p, RTL8372N_REG_MAC_FORCE_CTRL0(port), &post);
	dev_info(dev,
		 "phylink_mac_link_up: port %d speed=%d duplex=%d txp=%d rxp=%d iface=%d -> MAC_FORCE_CTRL0=0x%08x (wanted 0x%08x, spd_sel=%u)\n",
		 port, speed, duplex, tx_pause, rx_pause, interface,
		 post, val, spd_sel);
}

static void rtl8372n_phylink_mac_link_down(struct dsa_switch *ds, int port,
					   unsigned int mode,
					   phy_interface_t interface)
{
	struct rtl8372n_priv *p = ds->priv;
	struct device *dev = ds->dev;
	int ret;

	if (port != RTL8372N_EXT_CPU_PORT)
		return;

	/* Force the MAC down so phylink can re-establish on the next up.
	 * Leave FORCE_EN=1 so it doesn't drift into auto-neg state.
	 */
	ret = rtl8372n_reg_write(p, RTL8372N_REG_MAC_FORCE_CTRL0(port),
				 RTL8372N_MAC_FORCE_EN);
	if (ret)
		dev_warn(dev, "port %d MAC link-down failed: %d\n", port, ret);
}

static const struct dsa_switch_ops rtl8372n_switch_ops = {
	.get_tag_protocol	= rtl8372n_get_tag_protocol,
	.setup			= rtl8372n_dsa_setup,
	.phylink_get_caps	= rtl8372n_phylink_get_caps,
	.phylink_mac_link_up	= rtl8372n_phylink_mac_link_up,
	.phylink_mac_link_down	= rtl8372n_phylink_mac_link_down,
	.port_vlan_filtering	= rtl8372n_port_vlan_filtering,
	.port_vlan_add		= rtl8372n_port_vlan_add,
	.port_vlan_del		= rtl8372n_port_vlan_del,
};

static int rtl8372n_mdio_probe(struct mdio_device *mdiodev)
{
	struct device *dev = &mdiodev->dev;
	struct rtl8372n_priv *p;
	u16 model;
	int ret;

	p = devm_kzalloc(dev, sizeof(*p), GFP_KERNEL);
	if (!p)
		return -ENOMEM;

	p->bus = mdiodev->bus;
	p->phy_addr = mdiodev->addr;
	mutex_init(&p->lock);
	INIT_DELAYED_WORK(&p->fdb_diag_work, rtl8372n_fdb_diag_work);

	dev_info(dev, "probing RTL8372N at MDIO addr 0x%02x\n", p->phy_addr);

	ret = rtl8372n_detect(p, &model);
	if (ret) {
		dev_err(dev, "chip-ID readout failed: %d (model=0x%04x)\n",
			ret, model);
		return ret;
	}

	dev_info(dev, "RTL8372N family chip detected, model=0x%04x\n", model);

	rtl8372n_dump_baseline(p, dev);
	rtl8372n_write_selftest(p, dev);
	rtl8372n_chip_init(p, dev);
	rtl8372n_stage_cpu_tag(p, dev);
	rtl8372n_program_default_vlan(p, dev);

	/* Seed the priv state to match what program_default_vlan() just
	 * wrote, so a subsequent bridge vlan add on VID 1 won't clobber
	 * the default membership.
	 */
	p->vid_mbr[1] = 0x3FF;
	p->vid_untag[1] = 0x3FF;

	/* Re-enabled 2026-05-22 (commit 7c8544a73d): the eth0 RX path is
	 * now alive after the index-based EDMA opaque refactor, so frames
	 * forwarded with the Realtek 0x8899 tag should finally reach
	 * AF_PACKET via pktdump. This lets us confirm the wire-format
	 * before committing to DSA_TAG_PROTO_RTL8_4 vs. a new tagger.
	 */
	rtl8372n_capture_enable(p, dev);

	/* Dump the current chip-side state we just programmed (plus a few
	 * read-only sanity bits) — used to diagnose the DSA TX black-hole
	 * before deciding which register to poke. See
	 * memory/project_flint3_dsa_tx_broken.md.
	 */
	rtl8372n_dump_diag(p, dev);

	/* Vendor U-Boot already leaves SDS0 in 10GR (line code matches
	 * what the SoC PCS does in 10gbase-r/usxgmii mode). Don't override
	 * with 2500BASEX — that's the wrong line rate and breaks RX CDR. */

	/* DO NOT call rtl8372n_force_port_mac(EXT_CPU_PORT) here. Tried it
	 * in commit 308e3ad3e8 — broke a working link. Pre-state at probe
	 * shows port 3 ALREADY linked at 10G (MAC_LINK_STS bit 3 = 1,
	 * MAC_LINK_SPD_STS field 3 = 4 = PORT_SPEED_10G) under auto-mode
	 * with MAC_FORCE_EN=0. Forcing the MAC explicitly transitioned the
	 * port to link=0 and the kernel UART hung shortly after — likely
	 * a SerDes lock loss the chip didn't recover from in the short
	 * usleep window the helper uses. The helper itself stays in the
	 * source for future port bring-up on ports that genuinely need it.
	 *
	 * Implication for the original hypothesis (CBCR=0x80000010): port 3
	 * is fine and the SoC's eth0 reports "Link is Up - 10Gbps/Full",
	 * yet nss_cc_uniphy_port1_rx_clk CBCR stays CLK_OFF=1 — confirming
	 * the 2026-05-20 forum-draft conclusion that the CBCR enable bit
	 * is cosmetic on the IPQ5332 NSSCC port branches. The real LAN
	 * data-path blocker is the missing DSA wiring + CPU-tag enable,
	 * not the clock framework state.
	 */

	dev_set_drvdata(dev, p);

	/* Register as a DSA switch so the tag_rtl8_4 protocol handler
	 * wires up to the master (eth0 / qcom_ppe), DSA slave devices
	 * (lan1..lan4) get created from the DT ports{} list, and bridge
	 * + VLAN logic flows through. Tag-format confirmed via on-wire
	 * capture; see memory/project_flint3_tag_protocol_confirmed.md.
	 */
	p->ds = devm_kzalloc(dev, sizeof(*p->ds), GFP_KERNEL);
	if (!p->ds)
		return -ENOMEM;

	p->ds->dev = dev;
	p->ds->num_ports = RTL8372N_NUM_PORTS;
	p->ds->priv = p;
	p->ds->ops = &rtl8372n_switch_ops;

	ret = dsa_register_switch(p->ds);
	if (ret) {
		dev_err(dev, "dsa_register_switch failed: %d\n", ret);
		return ret;
	}

	dev_info(dev, "DSA switch registered, %d ports\n", RTL8372N_NUM_PORTS);
	return 0;
}

static void rtl8372n_mdio_remove(struct mdio_device *mdiodev)
{
	struct rtl8372n_priv *p = dev_get_drvdata(&mdiodev->dev);

	if (!p)
		return;

	cancel_delayed_work_sync(&p->fdb_diag_work);

	if (p->ds)
		dsa_unregister_switch(p->ds);

	mutex_destroy(&p->lock);
}

static const struct of_device_id rtl8372n_of_match[] = {
	{ .compatible = "realtek,rtl8372n" },
	{ .compatible = "realtek,rtl8373" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, rtl8372n_of_match);

static struct mdio_driver rtl8372n_mdio_driver = {
	.probe = rtl8372n_mdio_probe,
	.remove = rtl8372n_mdio_remove,
	.mdiodrv.driver = {
		.name = "rtl8372n",
		.of_match_table = rtl8372n_of_match,
	},
};
mdio_module_driver(rtl8372n_mdio_driver);

MODULE_AUTHOR("Kamil Bienkiewicz <perceivalpercy@gmail.com>");
MODULE_DESCRIPTION("Realtek RTL8372N / RTL8373 switch driver");
MODULE_LICENSE("GPL");
