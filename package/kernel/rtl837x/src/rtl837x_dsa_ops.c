/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2025 StarField Xu <air_jinkela@163.com>
 */

#include <linux/bitops.h>
#include <linux/etherdevice.h>
#include <linux/if_bridge.h>
#include <linux/if_ether.h>
#include <linux/kernel.h>
#include <linux/phylink.h>
#include <linux/phy.h>
#include <linux/string.h>
#include <linux/dsa/8021q.h>
#include <net/dsa.h>
#include <net/devlink.h>
#include <net/switchdev.h>

#include "./rtl837x_common.h"
#include "./rtk-api/l2.h"
#include "./rtk-api/dal/rtl8373/dal_rtl8373_stp.h"

static int rtl837x_to_errno(int ret)
{
	/*
	 * Keep this mapping limited to SDK errors with a direct and stable
	 * Linux errno meaning.  Unknown and subsystem-specific errors stay
	 * -EIO rather than being guessed at here.
	 */
	switch (ret) {
	case RT_ERR_OK:
		return 0;
	case RT_ERR_INPUT:
	case RT_ERR_PORT_ID:
	case RT_ERR_PORT_MASK:
	case RT_ERR_NULL_POINTER:
	case RT_ERR_MAC:
	case RT_ERR_OUT_OF_RANGE:
	case RT_ERR_ENABLE:
	case RT_ERR_RANGE:
	case RT_ERR_VLAN_VID:
	case RT_ERR_L2_FID:
	case RT_ERR_L2_VID:
		return -EINVAL;
	case RT_ERR_BUSYWAIT_TIMEOUT:
		return -ETIMEDOUT;
	case RT_ERR_CHIP_NOT_SUPPORTED:
	case RT_ERR_DRIVER_NOT_FOUND:
		return -EOPNOTSUPP;
	case RT_ERR_L2_NO_EMPTY_ENTRY:
	case RT_ERR_L2_INDEXTBL_FULL:
		return -ENOSPC;
	default:
		return -EIO;
	}
}

static int rtl837x_devlink_info_get(struct dsa_switch *ds,
					struct devlink_info_req *req,
					struct netlink_ext_ack *extack)
{
	struct rtk_gsw *gsw = ds->priv;

	if (!gsw->chip_name)
		return -ENODEV;

	return devlink_info_version_fixed_put(
		req, DEVLINK_INFO_VERSION_GENERIC_ASIC_ID, gsw->chip_name);
}

static int rtl837x_mdio_setup(struct dsa_switch *ds);
static void rtl837x_mdio_teardown(struct dsa_switch *ds);

static DEFINE_MUTEX(rtl837x_phy_driver_lock);
static unsigned int rtl837x_phy_driver_users;

static bool rtl837x_valid_port(struct rtk_gsw *gsw, int port)
{
	return port >= 0 && port < RTK_MAX_NUM_OF_PORT &&
	       (gsw->valid_port_mask & BIT(port));
}

static bool rtl837x_user_port(struct rtk_gsw *gsw, int port)
{
	return rtl837x_valid_port(gsw, port) && port != gsw->cpu_port;
}

static u32 rtl837x_user_ports(struct rtk_gsw *gsw)
{
	return gsw->valid_port_mask & ~BIT(gsw->cpu_port);
}

static int rtl837x_set_learning(struct rtk_gsw *gsw, int port, bool enable)
{
	rtk_mac_cnt_t limit;
	rtk_api_ret_t ret;

	if (!rtl837x_user_port(gsw, port))
		return -EINVAL;

	limit = enable ? RTK_MAX_NUM_OF_LEARN_LIMIT : 0;
	ret = rtk_l2_limitLearningCnt_set(port, limit);

	return rtl837x_to_errno(ret);
}

static int rtl837x_set_hairpin(struct rtk_gsw *gsw, int port, bool enable)
{
	rtk_api_ret_t ret;

	if (!rtl837x_user_port(gsw, port))
		return -EINVAL;

	ret = rtk_l2_localPktPermit_set(port,
						enable ? ENABLED : DISABLED);

	return rtl837x_to_errno(ret);
}

#define RTL837X_SUPPORTED_BRIDGE_FLAGS \
	(BR_LEARNING | BR_HAIRPIN_MODE | BR_ISOLATED | BR_FLOOD | \
	 BR_MCAST_FLOOD | BR_BCAST_FLOOD)

struct rtl837x_flood_mask_update {
	rtk_l2_flood_type_t type;
	rtk_portmask_t old;
	rtk_portmask_t new;
	bool valid;
	bool attempted;
};

static int rtl837x_flood_mask_update_prepare(
		struct rtl837x_flood_mask_update *update, int port, bool enable)
{
	rtk_api_ret_t ret;

	ret = rtk_l2_floodPortMask_get(update->type, &update->old);
	if (ret != RT_ERR_OK)
		return rtl837x_to_errno(ret);

	update->new = update->old;
	if (enable)
		RTK_PORTMASK_PORT_SET(update->new, port);
	else
		RTK_PORTMASK_PORT_CLEAR(update->new, port);

	update->valid = true;
	return 0;
}

static void rtl837x_flood_mask_rollback(struct rtk_gsw *gsw,
					struct rtl837x_flood_mask_update *updates,
					unsigned int count)
{
	unsigned int i;
	rtk_api_ret_t ret;

	for (i = 0; i < count; i++) {
		if (!updates[i].valid || !updates[i].attempted)
			continue;

		ret = rtk_l2_floodPortMask_set(updates[i].type,
						       &updates[i].old);
		if (ret != RT_ERR_OK)
			dev_warn(gsw->dev,
				 "failed to rollback flood mask type %u: %d\n",
				 updates[i].type, rtl837x_to_errno(ret));
	}
}

static int rtl837x_set_bridge_flood_flags(struct rtk_gsw *gsw, int port,
						struct switchdev_brport_flags flags)
{
	struct rtl837x_flood_mask_update updates[] = {
		{ .type = FLOOD_UNKNOWNDA },
		{ .type = FLOOD_UNKNOWNL2MC },
		{ .type = FLOOD_UNKNOWNV4MC },
		{ .type = FLOOD_UNKNOWNV6MC },
		{ .type = FLOOD_BC },
	};
	unsigned int i;
	int ret;

	if (flags.mask & BR_FLOOD) {
		ret = rtl837x_flood_mask_update_prepare(&updates[0], port,
							flags.val & BR_FLOOD);
		if (ret)
			return ret;
	}

	if (flags.mask & BR_MCAST_FLOOD) {
		for (i = 1; i <= 3; i++) {
			ret = rtl837x_flood_mask_update_prepare(&updates[i], port,
								flags.val & BR_MCAST_FLOOD);
			if (ret)
				return ret;
		}
	}

	if (flags.mask & BR_BCAST_FLOOD) {
		ret = rtl837x_flood_mask_update_prepare(&updates[4], port,
							flags.val & BR_BCAST_FLOOD);
		if (ret)
			return ret;
	}

	for (i = 0; i < ARRAY_SIZE(updates); i++) {
		if (!updates[i].valid ||
		    updates[i].old.bits[0] == updates[i].new.bits[0])
			continue;

		updates[i].attempted = true;
		ret = rtl837x_to_errno(rtk_l2_floodPortMask_set(updates[i].type,
									&updates[i].new));
		if (ret) {
			rtl837x_flood_mask_rollback(gsw, updates,
							ARRAY_SIZE(updates));
			return ret;
		}
	}

	return 0;
}

static int rtl837x_apply_isolation(struct rtk_gsw *gsw,
					   u32 isolated_port_mask,
					   int changed_port,
					   struct net_device *changed_bridge_dev);

static int rtl837x_port_pre_bridge_flags(struct dsa_switch *ds, int port,
					 struct switchdev_brport_flags flags,
					 struct netlink_ext_ack *extack)
{
	struct rtk_gsw *gsw = ds->priv;

	if (!rtl837x_user_port(gsw, port))
		return -EINVAL;

	if (flags.mask & ~RTL837X_SUPPORTED_BRIDGE_FLAGS) {
		NL_SET_ERR_MSG_MOD(extack,
				   "unsupported bridge port flag");
		return -EOPNOTSUPP;
	}

	return 0;
}

static int rtl837x_port_bridge_flags(struct dsa_switch *ds, int port,
					 struct switchdev_brport_flags flags,
					 struct netlink_ext_ack *extack)
{
	struct rtk_gsw *gsw = ds->priv;
	int ret;

	ret = rtl837x_port_pre_bridge_flags(ds, port, flags, extack);
	if (ret)
		return ret;

	if (flags.mask & BR_LEARNING) {
		ret = rtl837x_set_learning(gsw, port,
					    flags.val & BR_LEARNING);
		if (ret)
			return ret;
	}

	if (flags.mask & BR_HAIRPIN_MODE) {
		ret = rtl837x_set_hairpin(gsw, port,
					   flags.val & BR_HAIRPIN_MODE);
		if (ret)
			return ret;
	}

	if (flags.mask & (BR_FLOOD | BR_MCAST_FLOOD | BR_BCAST_FLOOD)) {
		mutex_lock(&gsw->flood_lock);
		ret = rtl837x_set_bridge_flood_flags(gsw, port, flags);
		mutex_unlock(&gsw->flood_lock);
		if (ret)
			return ret;
	}

	if (flags.mask & BR_ISOLATED) {
		u32 isolated_port_mask;

		mutex_lock(&gsw->isolation_lock);
		isolated_port_mask = gsw->isolated_port_mask;
		if (flags.val & BR_ISOLATED)
			isolated_port_mask |= BIT(port);
		else
			isolated_port_mask &= ~BIT(port);

		ret = rtl837x_apply_isolation(gsw, isolated_port_mask, -1, NULL);
		if (!ret)
			gsw->isolated_port_mask = isolated_port_mask;
		mutex_unlock(&gsw->isolation_lock);
		return ret;
	}

	return 0;
}

static bool rtl837x_support_eee(struct dsa_switch *ds, int port)
{
	struct rtk_gsw *gsw = ds->priv;

	return rtl837x_user_port(gsw, port);
}

static int rtl837x_eee_set_force_speed(int port, rtk_enable_t enable)
{
	rtk_eee_speedInMacForceMode_t speed;
	int ret;

	for (speed = EEE_MAC_FORCE_SPEED_100M;
	     speed < EEE_MAC_FORCE_SPEED_END; speed++) {
		ret = rtk_eee_macForceSpeedEn_set(port, speed, enable);
		if (ret)
			return rtl837x_to_errno(ret);
	}

	return 0;
}

static int rtl837x_set_mac_eee(struct dsa_switch *ds, int port,
				       struct ethtool_keee *eee)
{
	struct rtk_gsw *gsw = ds->priv;
	int ret;

	if (!eee || !rtl837x_user_port(gsw, port))
		return -EINVAL;

	if (!eee->eee_enabled) {
		/* Disable the capability first, so a partial speed-mask write
		 * cannot leave an operational EEE path behind.
		 */
		ret = rtl837x_to_errno(rtk_eee_portTxRxEn_set(port,
								 DISABLED, DISABLED));
		if (ret)
			return ret;

		return rtl837x_eee_set_force_speed(port, DISABLED);
	}

	/* The RTL8373 API keeps the per-speed MAC force bits separate from
	 * the port TX/RX capability bits.  Program both, but keep EEE off if
	 * either half cannot be written.
	 */
	ret = rtl837x_eee_set_force_speed(port, ENABLED);
	if (ret) {
		rtl837x_eee_set_force_speed(port, DISABLED);
		return ret;
	}

	ret = rtl837x_to_errno(rtk_eee_portTxRxEn_set(port, ENABLED, ENABLED));
	if (ret) {
		rtl837x_eee_set_force_speed(port, DISABLED);
		return ret;
	}

	return 0;
}

/* With tag_8021q, forwarding and isolation are governed entirely by VLAN
 * membership (standalone per-port VIDs isolate; shared bridge VIDs bridge).
 * Keep the hardware port-isolation matrix fully permissive so VLAN egress
 * filtering is the sole gate.
 */
static int rtl837x_open_isolation(struct rtk_gsw *gsw)
{
	int port, ret;

	for (port = 0; port < RTK_MAX_NUM_OF_PORT; port++) {
		if (!rtl837x_valid_port(gsw, port))
			continue;

		ret = rtk_port_isolation_set(port, gsw->valid_port_mask);
		if (ret)
			return rtl837x_to_errno(ret);
	}

	return 0;
}

struct rtl837x_isolation_update {
	u32 old_mask;
	u32 new_mask;
	bool attempted;
};

static struct net_device *
rtl837x_bridge_dev_for_port(struct rtk_gsw *gsw, int port, int changed_port,
				     struct net_device *changed_bridge_dev)
{
	if (port == changed_port)
		return changed_bridge_dev;

	return gsw->bridge_dev[port];
}

static u32 rtl837x_isolation_mask_for_port(struct rtk_gsw *gsw, int port,
					   u32 isolated_port_mask,
					   int changed_port,
					   struct net_device *changed_bridge_dev)
{
	struct net_device *bridge_dev;
	u32 mask = gsw->valid_port_mask;
	int other;

	if (!(isolated_port_mask & BIT(port)))
		return mask;

	bridge_dev = rtl837x_bridge_dev_for_port(gsw, port, changed_port,
						 changed_bridge_dev);
	if (!bridge_dev)
		return mask;

	for (other = 0; other < RTK_MAX_NUM_OF_PORT; other++) {
		struct net_device *other_bridge_dev;

		if (other == port || !rtl837x_valid_port(gsw, other))
			continue;

		if (!(isolated_port_mask & BIT(other)))
			continue;

		other_bridge_dev = rtl837x_bridge_dev_for_port(gsw, other,
								changed_port,
								changed_bridge_dev);
		if (other_bridge_dev == bridge_dev)
			mask &= ~BIT(other);
	}

	return mask;
}

/* Rebuild the global isolation matrix from the driver's bridge membership
 * and isolated-port state.  The temporary changed-port arguments let join
 * and leave stage their new state without exposing it as committed state.
 */
static int rtl837x_apply_isolation(struct rtk_gsw *gsw,
					   u32 isolated_port_mask,
					   int changed_port,
					   struct net_device *changed_bridge_dev)
{
	struct rtl837x_isolation_update updates[RTK_MAX_NUM_OF_PORT] = {};
	int port, rollback_port;
	rtk_api_ret_t ret;

	for (port = 0; port < RTK_MAX_NUM_OF_PORT; port++) {
		if (!rtl837x_valid_port(gsw, port))
			continue;

		ret = rtk_port_isolation_get(port, &updates[port].old_mask);
		if (ret)
			return rtl837x_to_errno(ret);

		updates[port].new_mask = rtl837x_isolation_mask_for_port(
			gsw, port, isolated_port_mask, changed_port,
			changed_bridge_dev);
	}

	for (port = 0; port < RTK_MAX_NUM_OF_PORT; port++) {
		if (!rtl837x_valid_port(gsw, port) ||
		    updates[port].old_mask == updates[port].new_mask)
			continue;

		updates[port].attempted = true;
		ret = rtk_port_isolation_set(port, updates[port].new_mask);
		if (ret)
			goto rollback;
	}

	return 0;

rollback:
	for (rollback_port = 0; rollback_port < RTK_MAX_NUM_OF_PORT;
	     rollback_port++) {
		if (!updates[rollback_port].attempted)
			continue;

		if (rtk_port_isolation_set(rollback_port,
					   updates[rollback_port].old_mask))
			dev_warn(gsw->dev,
				 "failed to roll back isolation mask for port %d\n",
				 rollback_port);
	}

	return rtl837x_to_errno(ret);
}

/* DSA starts bridge hairpin mode disabled.  Set that state explicitly for
 * user ports because the SDK does not document the reset value of the
 * source-port permit register.  Leave the CPU port unchanged.
 */
static int rtl837x_disable_hairpin(struct rtk_gsw *gsw)
{
	struct {
		rtk_enable_t old;
		bool attempted;
	} updates[RTK_MAX_NUM_OF_PORT] = {};
	int port, rollback_port;
	rtk_api_ret_t ret;

	for (port = 0; port < RTK_MAX_NUM_OF_PORT; port++) {
		if (!rtl837x_user_port(gsw, port))
			continue;

		ret = rtk_l2_localPktPermit_get(port, &updates[port].old);
		if (ret)
			return rtl837x_to_errno(ret);
	}

	for (port = 0; port < RTK_MAX_NUM_OF_PORT; port++) {
		if (!rtl837x_user_port(gsw, port) ||
		    updates[port].old == DISABLED)
			continue;

		updates[port].attempted = true;
		ret = rtk_l2_localPktPermit_set(port, DISABLED);
		if (ret)
			goto rollback;
	}

	return 0;

rollback:
	for (rollback_port = 0; rollback_port < RTK_MAX_NUM_OF_PORT;
	     rollback_port++) {
		if (!updates[rollback_port].attempted)
			continue;

		if (rtk_l2_localPktPermit_set(rollback_port,
					       updates[rollback_port].old))
			dev_warn(gsw->dev,
				 "failed to roll back hairpin state for port %d\n",
				 rollback_port);
	}

	return rtl837x_to_errno(ret);
}

static int rtl837x_commit_pvid_for_mode(struct rtk_gsw *gsw, int port,
					bool vlan_filtering)
{
	bool valid = gsw->tag8021q_pvid_valid[port];
	u16 vid = gsw->tag8021q_pvid[port];
	int ret;

	if (vlan_filtering) {
		vid = gsw->bridge_pvid[port];
		valid = gsw->bridge_pvid_valid[port];
	}

	ret = rtk_vlan_portPvid_set(port, valid ? vid : 0);
	if (ret)
		return rtl837x_to_errno(ret);

	gsw->port_pvid[port] = valid ? vid : 0;
	return 0;
}

static int rtl837x_commit_pvid(struct rtk_gsw *gsw, int port)
{
	struct dsa_port *dp = dsa_to_port(&gsw->ds, port);

	return rtl837x_commit_pvid_for_mode(gsw, port,
					    dsa_port_is_vlan_filtering(dp));
}

static int rtl837x_port_bridge_join(struct dsa_switch *ds, int port,
					    struct dsa_bridge bridge,
					    bool *tx_fwd_offload,
					    struct netlink_ext_ack *extack)
{
	struct rtk_gsw *gsw = ds->priv;
	u32 isolated_port_mask;
	int ret;

	if (!rtl837x_user_port(gsw, port))
		return -EINVAL;

	/* A newly created bridge port starts with Linux's hairpin default off. */
	ret = rtl837x_set_hairpin(gsw, port, false);
	if (ret)
		return ret;

	ret = dsa_tag_8021q_bridge_join(ds, port, bridge, tx_fwd_offload,
					 extack);
	if (ret)
		return ret;

	mutex_lock(&gsw->isolation_lock);
	isolated_port_mask = gsw->isolated_port_mask & ~BIT(port);
	ret = rtl837x_apply_isolation(gsw, isolated_port_mask, port, bridge.dev);
	if (!ret) {
		gsw->bridge_dev[port] = bridge.dev;
		gsw->isolated_port_mask = isolated_port_mask;
	}
	mutex_unlock(&gsw->isolation_lock);
	if (ret) {
		/* The tag join may already have changed VLAN state.  The DSA core
		 * does not own partial state from a failed driver callback, so
		 * explicitly undo it while preserving the isolation error.
		 * dsa_tag_8021q_bridge_leave() has no return value.
		 */
		dsa_tag_8021q_bridge_leave(ds, port, bridge);
		dev_err(gsw->dev,
			"failed to apply isolation for port %d: %d; tag_8021q join rollback requested\n",
			port, ret);
	}

	return ret;
}

static void rtl837x_port_bridge_leave(struct dsa_switch *ds, int port,
					      struct dsa_bridge bridge)
{
	struct rtk_gsw *gsw = ds->priv;
	u32 isolated_port_mask;
	int ret;

	if (!rtl837x_user_port(gsw, port))
		return;

	/* DSA has already removed dp->bridge by the time this callback runs.
	 * Stage the departing port as standalone explicitly for the matrix.
	 */
	mutex_lock(&gsw->isolation_lock);
	isolated_port_mask = gsw->isolated_port_mask & ~BIT(port);
	ret = rtl837x_apply_isolation(gsw, isolated_port_mask, port, NULL);
	if (ret) {
		dev_err(gsw->dev, "failed to restore isolation for port %d: %d\n",
			port, ret);
		/* The callback cannot report an error.  Drop the membership
		 * pointer anyway so it cannot outlive the bridge device; keep
		 * isolated_port_mask unchanged because the hardware rolled back.
		 */
		gsw->bridge_dev[port] = NULL;
	} else {
		gsw->bridge_dev[port] = NULL;
		gsw->isolated_port_mask = isolated_port_mask;
	}
	mutex_unlock(&gsw->isolation_lock);

	/* Do not carry hairpin state into the next bridge-port instance. */
	ret = rtl837x_set_hairpin(gsw, port, false);
	if (ret)
		dev_err(gsw->dev, "failed to reset hairpin state for port %d: %d\n",
			port, ret);

	dsa_tag_8021q_bridge_leave(ds, port, bridge);
}

static int rtl837x_set_stp_state(struct rtk_gsw *gsw, int port, u8 state)
{
	u32 mstp_state;
	int ret;

	if (!rtl837x_valid_port(gsw, port))
		return -EINVAL;

	switch (state) {
	case BR_STATE_DISABLED:
		mstp_state = MSTP_DISABLE;
		break;
	case BR_STATE_BLOCKING:
	case BR_STATE_LISTENING:
		mstp_state = MSTP_BLOCKING;
		break;
	case BR_STATE_LEARNING:
		mstp_state = MSTP_LEARNING;
		break;
	case BR_STATE_FORWARDING:
		mstp_state = MSTP_FORWARDING;
		break;
	default:
		return -EINVAL;
	}

	ret = dal_rtl8373_asicMstpPortStatus_set(0, port, mstp_state);
	return rtl837x_to_errno(ret);
}

static int rtl837x_read_ethtool_stat(int port, rtk_stat_port_type_t counter,
					     u64 *value)
{
	rtk_stat_counter_t counter_value = 0;
	int ret;

	ret = rtk_stat_port_get(port, counter, &counter_value);
	if (ret)
		return rtl837x_to_errno(ret);

	*value = counter_value;
	return 0;
}

static u64 rtl837x_read_stat(int port, u32 counter)
{
	u64 value;

	if (rtl837x_read_ethtool_stat(port, counter, &value))
		return 0;

	return value;
}

static int rtl837x_read_stats_snapshot(int port,
					       struct rtl837x_mib_snapshot *snapshot)
{
	u64 value;
	int ret;

	ret = rtl837x_read_ethtool_stat(port, ifInOctets_H, &value);
	if (ret)
		return ret;
	snapshot->rx_octets = value;

	ret = rtl837x_read_ethtool_stat(port, ifOutOctets_H, &value);
	if (ret)
		return ret;
	snapshot->tx_octets = value;

	ret = rtl837x_read_ethtool_stat(port, ifInUcastPkts_H, &value);
	if (ret)
		return ret;
	snapshot->rx_ucast_pkts = value;

	ret = rtl837x_read_ethtool_stat(port, ifInMulticastPkts_H, &value);
	if (ret)
		return ret;
	snapshot->rx_mcast_pkts = value;

	ret = rtl837x_read_ethtool_stat(port, ifInBroadcastPkts_H, &value);
	if (ret)
		return ret;
	snapshot->rx_bcast_pkts = value;

	ret = rtl837x_read_ethtool_stat(port, ifOutUcastPkts_H, &value);
	if (ret)
		return ret;
	snapshot->tx_ucast_pkts = value;

	ret = rtl837x_read_ethtool_stat(port, ifOutMulticastPkts_H, &value);
	if (ret)
		return ret;
	snapshot->tx_mcast_pkts = value;

	ret = rtl837x_read_ethtool_stat(port, ifOutBroadcastPkts_H, &value);
	if (ret)
		return ret;
	snapshot->tx_bcast_pkts = value;

	ret = rtl837x_read_ethtool_stat(port, ifOutDiscards, &value);
	if (ret)
		return ret;
	snapshot->tx_discards = value;

	ret = rtl837x_read_ethtool_stat(port, tx_etherStatsCollisions, &value);
	if (ret)
		return ret;
	snapshot->collisions = value;

	return 0;
}

static bool
rtl837x_stats_64bit_reset_detected(const struct rtl837x_mib_snapshot *old,
						   const struct rtl837x_mib_snapshot *latest)
{
	return latest->rx_octets < old->rx_octets ||
	       latest->tx_octets < old->tx_octets ||
	       latest->rx_ucast_pkts < old->rx_ucast_pkts ||
	       latest->rx_mcast_pkts < old->rx_mcast_pkts ||
	       latest->rx_bcast_pkts < old->rx_bcast_pkts ||
	       latest->tx_ucast_pkts < old->tx_ucast_pkts ||
	       latest->tx_mcast_pkts < old->tx_mcast_pkts ||
	       latest->tx_bcast_pkts < old->tx_bcast_pkts;
}

static void rtl837x_update_port_stats(struct rtk_gsw *gsw, int port,
				       const struct rtl837x_mib_snapshot *snapshot)
{
	struct rtl837x_port_stats *port_stats = &gsw->port_stats[port];
	const struct rtl837x_mib_snapshot *old = &port_stats->snapshot;
	u32 tx_discards_delta;
	u32 collisions_delta;

	spin_lock_bh(&port_stats->lock);

	if (!port_stats->snapshot_valid ||
	    rtl837x_stats_64bit_reset_detected(old, snapshot)) {
		/* A backwards 64-bit counter means the hardware MIB was reset.
		 * Keep Linux counters monotonic and use this sample only as the
		 * new hardware baseline.
		 */
		port_stats->snapshot = *snapshot;
		port_stats->snapshot_valid = true;
		spin_unlock_bh(&port_stats->lock);
		return;
	}

	/* These hardware counters are 32-bit.  Unsigned subtraction is
	 * intentionally modulo-2^32, so an ordinary wrap is accumulated as
	 * its real delta instead of being mistaken for a reset.
	 */
	tx_discards_delta = snapshot->tx_discards - old->tx_discards;
	collisions_delta = snapshot->collisions - old->collisions;

	port_stats->stats.rx_bytes += snapshot->rx_octets - old->rx_octets;
	port_stats->stats.tx_bytes += snapshot->tx_octets - old->tx_octets;
	port_stats->stats.rx_packets +=
		snapshot->rx_ucast_pkts - old->rx_ucast_pkts +
		snapshot->rx_mcast_pkts - old->rx_mcast_pkts +
		snapshot->rx_bcast_pkts - old->rx_bcast_pkts;
	port_stats->stats.tx_packets +=
		snapshot->tx_ucast_pkts - old->tx_ucast_pkts +
		snapshot->tx_mcast_pkts - old->tx_mcast_pkts +
		snapshot->tx_bcast_pkts - old->tx_bcast_pkts;
	port_stats->stats.tx_dropped += tx_discards_delta;
	port_stats->stats.multicast +=
		snapshot->rx_mcast_pkts - old->rx_mcast_pkts;
	port_stats->stats.collisions += collisions_delta;

	port_stats->snapshot = *snapshot;
	spin_unlock_bh(&port_stats->lock);
}

/*
 * dal_rtl8373_portMib_read() exposes the ifIn/ifOut octet and packet
 * counters as 64-bit values.  The discard and collision counters below are
 * 32-bit, so one second leaves ample margin before they can wrap even at the
 * RTL8373 line rate.
 */
#define RTL837X_STATS_POLL_INTERVAL (HZ)

static void rtl837x_stats_work_func(struct work_struct *work)
{
	struct rtk_gsw *gsw = container_of(to_delayed_work(work),
					 struct rtk_gsw, stats_work);
	struct rtl837x_mib_snapshot snapshot;
	unsigned long user_ports = rtl837x_user_ports(gsw);
	int port, ret;

	for_each_set_bit(port, &user_ports, RTK_MAX_NUM_OF_PORT) {
		if (READ_ONCE(gsw->stats_work_stopping))
			return;

		ret = rtl837x_read_stats_snapshot(port, &snapshot);
		if (ret) {
			dev_warn_ratelimited(gsw->dev,
					     "failed to read statistics for port %d: %d\n",
					     port, ret);
			continue;
		}

		rtl837x_update_port_stats(gsw, port, &snapshot);
	}

	if (!READ_ONCE(gsw->stats_work_stopping))
		queue_delayed_work(system_wq, &gsw->stats_work,
				   RTL837X_STATS_POLL_INTERVAL);
}

static void rtl837x_stats_init(struct rtk_gsw *gsw)
{
	int port;

	for (port = 0; port < RTK_MAX_NUM_OF_PORT; port++)
		spin_lock_init(&gsw->port_stats[port].lock);

	INIT_DELAYED_WORK(&gsw->stats_work, rtl837x_stats_work_func);
	WRITE_ONCE(gsw->stats_work_stopping, true);
}

static void rtl837x_stats_start(struct rtk_gsw *gsw)
{
	struct rtl837x_port_stats *port_stats;
	int port;

	for (port = 0; port < RTK_MAX_NUM_OF_PORT; port++) {
		port_stats = &gsw->port_stats[port];

		spin_lock_bh(&port_stats->lock);
		/* Establish a post-registration baseline; do not count earlier traffic. */
		memset(&port_stats->stats, 0, sizeof(port_stats->stats));
		memset(&port_stats->snapshot, 0, sizeof(port_stats->snapshot));
		port_stats->snapshot_valid = false;
		spin_unlock_bh(&port_stats->lock);
	}

	WRITE_ONCE(gsw->stats_work_stopping, false);
	queue_delayed_work(system_wq, &gsw->stats_work,
			   RTL837X_STATS_POLL_INTERVAL);
}

static void rtl837x_stats_stop(struct rtk_gsw *gsw)
{
	WRITE_ONCE(gsw->stats_work_stopping, true);
	cancel_delayed_work_sync(&gsw->stats_work);
}

static int rtl837x_write_vlan(struct rtk_gsw *gsw, u16 vid)
{
	rtk_vlan_entry_t vlan = { 0 };
	int ret;

	vlan.mbr.bits[0] = gsw->vlan_table[vid].mbr;
	vlan.untag.bits[0] = gsw->vlan_table[vid].untag;
	vlan.fid_msti = 0;
	vlan.svlan_chk_ivl_svl = 0;
	vlan.ivl_svl = 1;

	ret = rtk_vlan_set(vid, &vlan);
	return rtl837x_to_errno(ret);
}

static bool rtl837x_vlan_has_user(struct rtk_gsw *gsw, u16 vid)
{
	return gsw->vlan_table[vid].mbr & rtl837x_user_ports(gsw);
}

static int rtl837x_seed_vlan_table(struct rtk_gsw *gsw)
{
	int port, ret;

	memset(gsw->vlan_table, 0, sizeof(gsw->vlan_table));

	gsw->vlan_table[1].valid = 1;
	gsw->vlan_table[1].vid = 1;
	gsw->vlan_table[1].mbr = gsw->valid_port_mask;
	gsw->vlan_table[1].untag = gsw->valid_port_mask;

	ret = rtl837x_write_vlan(gsw, 1);
	if (ret)
		return ret;

	for (port = 0; port < RTK_MAX_NUM_OF_PORT; port++) {
		if (!rtl837x_valid_port(gsw, port))
			continue;

		gsw->port_pvid[port] = 1;

		ret = rtk_vlan_portPvid_set(port, 1);
		if (ret)
			return rtl837x_to_errno(ret);

		ret = rtk_vlan_portIgrFilterEnable_set(port, DISABLED);
		if (ret)
			return rtl837x_to_errno(ret);

		ret = rtk_vlan_portAcceptFrameType_set(port, ACCEPT_FRAME_TYPE_ALL);
		if (ret)
			return rtl837x_to_errno(ret);
	}

	return 0;
}

static enum dsa_tag_protocol
rtl837x_get_tag_protocol(struct dsa_switch *ds, int port,
			 enum dsa_tag_protocol mprot)
{
	/* VSC73XX_8021Q is a switch-agnostic tag_8021q tagger, reused here so
	 * CPU traffic uses a standard 802.1Q header the IPQ5332 EDMA checksum
	 * parser can see past (the proprietary 0x8899 tag defeats it).
	 */
	return DSA_TAG_PROTO_VSC73XX_8021Q;
}

static int rtl837x_tag_8021q_vlan_add(struct dsa_switch *ds, int port, u16 vid,
				      u16 flags)
{
	struct rtk_gsw *gsw = ds->priv;
	bool untagged = flags & BRIDGE_VLAN_INFO_UNTAGGED;
	bool pvid = flags & BRIDGE_VLAN_INFO_PVID;
	typeof(gsw->vlan_table[0]) old_vlan;
	int ret;

	if (!rtl837x_valid_port(gsw, port) || !vid || vid > RTK_VID_MAX)
		return -EINVAL;

	old_vlan = gsw->vlan_table[vid];

	gsw->vlan_table[vid].valid = 1;
	gsw->vlan_table[vid].vid = vid;
	gsw->vlan_table[vid].mbr |= BIT(port);

	if (untagged)
		gsw->vlan_table[vid].untag |= BIT(port);
	else
		gsw->vlan_table[vid].untag &= ~BIT(port);

	ret = rtl837x_write_vlan(gsw, vid);
	if (ret) {
		gsw->vlan_table[vid] = old_vlan;
		return ret;
	}

	if (pvid) {
		u16 old_pvid = gsw->tag8021q_pvid[port];
		bool old_pvid_valid = gsw->tag8021q_pvid_valid[port];

		gsw->tag8021q_pvid[port] = vid;
		gsw->tag8021q_pvid_valid[port] = true;
		ret = rtl837x_commit_pvid(gsw, port);
		if (ret) {
			gsw->tag8021q_pvid[port] = old_pvid;
			gsw->tag8021q_pvid_valid[port] = old_pvid_valid;
		}
		return ret;
	}

	return 0;
}

static int rtl837x_tag_8021q_vlan_del(struct dsa_switch *ds, int port, u16 vid)
{
	struct rtk_gsw *gsw = ds->priv;
	typeof(gsw->vlan_table[0]) old_vlan;
	int ret;

	if (!rtl837x_valid_port(gsw, port) || !vid || vid > RTK_VID_MAX)
		return -EINVAL;

	if (!gsw->vlan_table[vid].valid)
		return 0;

	old_vlan = gsw->vlan_table[vid];

	gsw->vlan_table[vid].mbr &= ~BIT(port);
	gsw->vlan_table[vid].untag &= ~BIT(port);

	if (!gsw->vlan_table[vid].mbr)
		gsw->vlan_table[vid].valid = 0;

	ret = rtl837x_write_vlan(gsw, vid);
	if (ret) {
		gsw->vlan_table[vid] = old_vlan;
		return ret;
	}

	if (gsw->tag8021q_pvid_valid[port] && gsw->tag8021q_pvid[port] == vid) {
		u16 old_pvid = gsw->tag8021q_pvid[port];
		bool old_pvid_valid = gsw->tag8021q_pvid_valid[port];

		gsw->tag8021q_pvid_valid[port] = false;
		ret = rtl837x_commit_pvid(gsw, port);
		if (ret) {
			gsw->tag8021q_pvid[port] = old_pvid;
			gsw->tag8021q_pvid_valid[port] = old_pvid_valid;
		}
		return ret;
	}

	return 0;
}

static int rtl837x_setup(struct dsa_switch *ds)
{
	struct rtk_gsw *gsw = ds->priv;
	int port, ret;

	ret = rtk_cpu_externalCpuPort_set(gsw->cpu_port);
	if (ret)
		return rtl837x_to_errno(ret);

	/* tag_8021q carries port identity in a standard 802.1Q tag, so the
	 * proprietary 0x8899 CPU tag must stay off.
	 */
	ret = rtk_cpuTag_enable_set(EXTERNAL_CPU, DISABLED);
	if (ret)
		return rtl837x_to_errno(ret);

	ret = rtk_l2_init();
	if (ret)
		return rtl837x_to_errno(ret);

	for (port = 0; port < RTK_MAX_NUM_OF_PORT; port++) {
		if (!rtl837x_user_port(gsw, port))
			continue;

		ret = rtl837x_set_learning(gsw, port, false);
		if (ret)
			return ret;
	}

	/* RTL8373 exposes link-down FDB age-out as a chip-global setting. */
	ret = rtk_l2_flushLinkDownPortAddrEnable_set(ENABLED);
	if (ret)
		return rtl837x_to_errno(ret);

	ret = rtk_l2_table_clear();
	if (ret)
		return rtl837x_to_errno(ret);

	ret = rtk_l2_aging_set(300);
	if (ret)
		return rtl837x_to_errno(ret);

	ret = rtl837x_disable_hairpin(gsw);
	if (ret)
		return ret;

	ret = rtk_stat_global_reset();
	if (ret)
		return rtl837x_to_errno(ret);

	ret = rtk_vlan_reset();
	if (ret)
		return rtl837x_to_errno(ret);

	ret = rtk_vlan_init();
	if (ret)
		return rtl837x_to_errno(ret);

	ret = rtk_vlan_egrFilterEnable_set(ENABLED);
	if (ret)
		return rtl837x_to_errno(ret);

	ret = rtl837x_seed_vlan_table(gsw);
	if (ret)
		return ret;

	memset(gsw->bridge_dev, 0, sizeof(gsw->bridge_dev));
	gsw->isolated_port_mask = 0;
	memset(gsw->tag8021q_pvid, 0, sizeof(gsw->tag8021q_pvid));
	memset(gsw->tag8021q_pvid_valid, 0, sizeof(gsw->tag8021q_pvid_valid));
	memset(gsw->bridge_pvid, 0, sizeof(gsw->bridge_pvid));
	memset(gsw->bridge_pvid_valid, 0, sizeof(gsw->bridge_pvid_valid));

	for (port = 0; port < RTK_MAX_NUM_OF_PORT; port++) {
		if (!rtl837x_valid_port(gsw, port))
			continue;

		ret = rtk_stat_port_reset(port);
		if (ret)
			return rtl837x_to_errno(ret);

		ret = rtl837x_set_stp_state(gsw, port,
					    port == gsw->cpu_port ?
					    BR_STATE_FORWARDING :
					    BR_STATE_DISABLED);
		if (ret)
			return ret;
	}

	ret = rtl837x_open_isolation(gsw);
	if (ret)
		return ret;

	ret = rtl837x_mdio_setup(ds);
	if (ret)
		return ret;

	rtnl_lock();
	ret = dsa_tag_8021q_register(ds, htons(ETH_P_8021Q));
	rtnl_unlock();
	if (ret) {
		rtl837x_mdio_teardown(ds);
		return ret;
	}

	rtl837x_stats_start(gsw);

	return 0;
}

static void rtl837x_teardown(struct dsa_switch *ds)
{
	struct rtk_gsw *gsw = ds->priv;
	int ret;

	rtl837x_stats_stop(gsw);

	rtnl_lock();
	dsa_tag_8021q_unregister(ds);
	rtnl_unlock();

	rtl837x_mdio_teardown(ds);
	ret = rtk_cpuTag_enable_set(EXTERNAL_CPU, DISABLED);
	if (ret)
		dev_err(ds->dev, "failed to disable CPU tag during teardown: %d\n", ret);
}

static int rtl837x_mdio_read_c45(struct mii_bus *bus, int port, int devad,
				 int regnum)
{
	struct rtk_gsw *gsw = bus->priv;
	u32 value = 0;
	int ret;

	if (!rtl837x_user_port(gsw, port))
		return -EOPNOTSUPP;

	ret = rtk_port_phyReg_get(port, devad, regnum, &value);
	/* MDIO bus probing treats negative reads as fatal bus errors. */
	if (ret)
		return 0xffff;

	return value & 0xffff;
}

static int rtl837x_mdio_write_c45(struct mii_bus *bus, int port, int devad,
				  int regnum, u16 val)
{
	struct rtk_gsw *gsw = bus->priv;
	int ret;

	if (!rtl837x_user_port(gsw, port))
		return -EOPNOTSUPP;

	ret = rtk_port_phyReg_set(BIT(port), devad, regnum, val);
	return rtl837x_to_errno(ret);
}

static int rtl837x_phy_speed_to_ethtool(u32 speed)
{
	switch (speed) {
	case PORT_SPEED_10M:
		return SPEED_10;
	case PORT_SPEED_100M:
		return SPEED_100;
	case PORT_SPEED_1000M:
		return SPEED_1000;
	case PORT_SPEED_10G:
		return SPEED_10000;
	case PORT_SPEED_2500M:
		return SPEED_2500;
	case PORT_SPEED_5G:
		return SPEED_5000;
	default:
		return SPEED_UNKNOWN;
	}
}

static int rtl837x_phy_match(struct phy_device *phydev,
			     const struct phy_driver *phydrv)
{
	struct mii_bus *bus = phydev->mdio.bus;
	struct rtk_gsw *gsw;

	if (!bus || bus->read_c45 != rtl837x_mdio_read_c45)
		return 0;

	gsw = bus->priv;
	return gsw && rtl837x_user_port(gsw, phydev->mdio.addr);
}

static int rtl837x_phy_probe(struct phy_device *phydev)
{
	phydev->is_internal = true;
	phydev->port = PORT_TP;

	return 0;
}

static int rtl837x_phy_get_features(struct phy_device *phydev)
{
	linkmode_zero(phydev->supported);

	linkmode_set_bit(ETHTOOL_LINK_MODE_TP_BIT, phydev->supported);
	linkmode_set_bit(ETHTOOL_LINK_MODE_MII_BIT, phydev->supported);
	linkmode_set_bit(ETHTOOL_LINK_MODE_Autoneg_BIT, phydev->supported);
	linkmode_set_bit(ETHTOOL_LINK_MODE_10baseT_Half_BIT, phydev->supported);
	linkmode_set_bit(ETHTOOL_LINK_MODE_10baseT_Full_BIT, phydev->supported);
	linkmode_set_bit(ETHTOOL_LINK_MODE_100baseT_Half_BIT, phydev->supported);
	linkmode_set_bit(ETHTOOL_LINK_MODE_100baseT_Full_BIT, phydev->supported);
	linkmode_set_bit(ETHTOOL_LINK_MODE_1000baseT_Full_BIT, phydev->supported);
	linkmode_set_bit(ETHTOOL_LINK_MODE_2500baseT_Full_BIT, phydev->supported);
	linkmode_set_bit(ETHTOOL_LINK_MODE_Pause_BIT, phydev->supported);
	linkmode_set_bit(ETHTOOL_LINK_MODE_Asym_Pause_BIT, phydev->supported);

	return 0;
}

static int rtl837x_phy_read_status(struct phy_device *phydev)
{
	rtk_port_status_t status = { 0 };
	int ret;

	ret = rtk_port_macStatus_get(phydev->mdio.addr, &status);
	if (ret)
		return rtl837x_to_errno(ret);

	phydev->link = !!status.link;
	phydev->autoneg_complete = phydev->link;

	if (!phydev->link) {
		phydev->speed = SPEED_UNKNOWN;
		phydev->duplex = DUPLEX_UNKNOWN;
		return 0;
	}

	phydev->speed = rtl837x_phy_speed_to_ethtool(status.speed);
	phydev->duplex = status.duplex ? DUPLEX_FULL : DUPLEX_HALF;

	linkmode_mod_bit(ETHTOOL_LINK_MODE_Pause_BIT, phydev->lp_advertising,
			 status.rxpause && status.txpause);
	linkmode_mod_bit(ETHTOOL_LINK_MODE_Asym_Pause_BIT,
			 phydev->lp_advertising,
			 status.rxpause != status.txpause);

	return 0;
}

static struct phy_driver rtl837x_phy_driver = {
	.name = "RTL837x internal PHY",
	.match_phy_device = rtl837x_phy_match,
	.probe = rtl837x_phy_probe,
	.get_features = rtl837x_phy_get_features,
	.read_status = rtl837x_phy_read_status,
};

static int rtl837x_phy_driver_get(void)
{
	int ret = 0;

	mutex_lock(&rtl837x_phy_driver_lock);

	if (!rtl837x_phy_driver_users) {
		ret = phy_drivers_register(&rtl837x_phy_driver, 1, THIS_MODULE);
		if (ret)
			goto out;
	}

	rtl837x_phy_driver_users++;

out:
	mutex_unlock(&rtl837x_phy_driver_lock);
	return ret;
}

static void rtl837x_phy_driver_put(void)
{
	mutex_lock(&rtl837x_phy_driver_lock);

	if (rtl837x_phy_driver_users && !--rtl837x_phy_driver_users)
		phy_drivers_unregister(&rtl837x_phy_driver, 1);

	mutex_unlock(&rtl837x_phy_driver_lock);
}

static int rtl837x_mdio_setup(struct dsa_switch *ds)
{
	struct rtk_gsw *gsw = ds->priv;
	struct mii_bus *bus;
	int ret;

	ret = rtl837x_phy_driver_get();
	if (ret)
		return ret;

	bus = mdiobus_alloc();
	if (!bus) {
		rtl837x_phy_driver_put();
		return -ENOMEM;
	}

	ds->user_mii_bus = bus;
	bus->priv = gsw;
	bus->name = "rtl837x slave mii";
	snprintf(bus->id, MII_BUS_ID_SIZE, "%s-mii", dev_name(gsw->dev));
	bus->read_c45 = rtl837x_mdio_read_c45;
	bus->write_c45 = rtl837x_mdio_write_c45;
	bus->parent = gsw->dev;
	bus->phy_mask = ~ds->phys_mii_mask;

	ret = mdiobus_register(bus);
	if (ret) {
		dev_err(gsw->dev, "failed to register slave MDIO bus: %d\n", ret);
		mdiobus_free(bus);
		ds->user_mii_bus = NULL;
		rtl837x_phy_driver_put();
		return ret;
	}

	return 0;
}

static void rtl837x_mdio_teardown(struct dsa_switch *ds)
{
	if (!ds->user_mii_bus)
		return;

	mdiobus_unregister(ds->user_mii_bus);
	mdiobus_free(ds->user_mii_bus);
	ds->user_mii_bus = NULL;
	rtl837x_phy_driver_put();
}

static void rtl837x_phylink_get_caps(struct dsa_switch *ds, int port,
				     struct phylink_config *config)
{
	struct rtk_gsw *gsw = ds->priv;

	if (!rtl837x_valid_port(gsw, port))
		return;

	config->mac_capabilities = MAC_ASYM_PAUSE | MAC_SYM_PAUSE |
				   MAC_10 | MAC_100 | MAC_1000FD |
				   MAC_2500FD | MAC_5000FD | MAC_10000FD;

	__set_bit(PHY_INTERFACE_MODE_INTERNAL, config->supported_interfaces);
	__set_bit(PHY_INTERFACE_MODE_GMII, config->supported_interfaces);
	__set_bit(PHY_INTERFACE_MODE_SGMII, config->supported_interfaces);
	__set_bit(PHY_INTERFACE_MODE_100BASEX, config->supported_interfaces);
	__set_bit(PHY_INTERFACE_MODE_1000BASEX, config->supported_interfaces);
	__set_bit(PHY_INTERFACE_MODE_2500BASEX, config->supported_interfaces);
	__set_bit(PHY_INTERFACE_MODE_5GBASER, config->supported_interfaces);
	__set_bit(PHY_INTERFACE_MODE_10GBASER, config->supported_interfaces);
	__set_bit(PHY_INTERFACE_MODE_10GKR, config->supported_interfaces);
	__set_bit(PHY_INTERFACE_MODE_USXGMII, config->supported_interfaces);
}


static void rtl837x_get_strings(struct dsa_switch *ds, int port,
				u32 stringset, uint8_t *data)
{
	struct rtk_gsw *gsw = ds->priv;
	int i;

	if (stringset != ETH_SS_STATS || !rtl837x_valid_port(gsw, port))
		return;

	for (i = 0; i < gsw->num_mib_counters; i++)
		strscpy(data + i * ETH_GSTRING_LEN,
			gsw->mib_counters[i].name, ETH_GSTRING_LEN);
}

static void rtl837x_get_ethtool_stats(struct dsa_switch *ds, int port,
				      uint64_t *data)
{
	struct rtk_gsw *gsw = ds->priv;
	int i;

	if (!rtl837x_valid_port(gsw, port))
		return;

	for (i = 0; i < gsw->num_mib_counters; i++)
		data[i] = rtl837x_read_stat(port, gsw->mib_counters[i].base);
}

static int rtl837x_get_sset_count(struct dsa_switch *ds, int port, int sset)
{
	struct rtk_gsw *gsw = ds->priv;

	if (sset != ETH_SS_STATS)
		return 0;

	if (!rtl837x_valid_port(gsw, port))
		return -EINVAL;

	return gsw->num_mib_counters;
}

static void rtl837x_get_pause_stats(struct dsa_switch *ds, int port,
				    struct ethtool_pause_stats *pause_stats)
{
	struct rtk_gsw *gsw = ds->priv;

	if (!rtl837x_valid_port(gsw, port))
		return;

	pause_stats->rx_pause_frames = rtl837x_read_stat(port, dot3InPauseFrames);
	pause_stats->tx_pause_frames = rtl837x_read_stat(port, dot3OutPauseFrames);
}

static void rtl837x_get_eth_phy_stats(struct dsa_switch *ds, int port,
				      struct ethtool_eth_phy_stats *phy_stats)
{
	struct rtk_gsw *gsw = ds->priv;
	u64 value;

	if (!rtl837x_valid_port(gsw, port))
		return;

	if (!rtl837x_read_ethtool_stat(port, dot3StatsSymbolErrors, &value))
		phy_stats->SymbolErrorDuringCarrier = value;
}

/*
 * RTL8373 exposes the standard packet categories as full-width counters.
 * Read the category counters together so a failed read cannot produce a
 * partial aggregate.
 */
static void rtl837x_get_eth_mac_stats(struct dsa_switch *ds, int port,
					      struct ethtool_eth_mac_stats *mac_stats)
{
	struct rtk_gsw *gsw = ds->priv;
	u64 rx_ucast, rx_mcast, rx_bcast;
	u64 tx_ucast, tx_mcast, tx_bcast;
	u64 value;

	if (!rtl837x_valid_port(gsw, port))
		return;

	if (rtl837x_read_ethtool_stat(port, ifInUcastPkts_H, &rx_ucast) ||
	    rtl837x_read_ethtool_stat(port, ifInMulticastPkts_H, &rx_mcast) ||
	    rtl837x_read_ethtool_stat(port, ifInBroadcastPkts_H, &rx_bcast) ||
	    rtl837x_read_ethtool_stat(port, ifOutUcastPkts_H, &tx_ucast) ||
	    rtl837x_read_ethtool_stat(port, ifOutMulticastPkts_H, &tx_mcast) ||
	    rtl837x_read_ethtool_stat(port, ifOutBroadcastPkts_H, &tx_bcast))
		return;

	mac_stats->FramesReceivedOK = rx_ucast + rx_mcast + rx_bcast;
	mac_stats->FramesTransmittedOK = tx_ucast + tx_mcast + tx_bcast;
	mac_stats->MulticastFramesReceivedOK = rx_mcast;
	mac_stats->BroadcastFramesReceivedOK = rx_bcast;
	mac_stats->MulticastFramesXmittedOK = tx_mcast;
	mac_stats->BroadcastFramesXmittedOK = tx_bcast;

	if (!rtl837x_read_ethtool_stat(port, ifInOctets_H, &value))
		mac_stats->OctetsReceivedOK = value;
	if (!rtl837x_read_ethtool_stat(port, ifOutOctets_H, &value))
		mac_stats->OctetsTransmittedOK = value;
	if (!rtl837x_read_ethtool_stat(port, dot3StatsSingleCollisionFrames,
					     &value))
		mac_stats->SingleCollisionFrames = value;
	if (!rtl837x_read_ethtool_stat(port, dot3StatMultipleCollisionFrames,
					     &value))
		mac_stats->MultipleCollisionFrames = value;
	if (!rtl837x_read_ethtool_stat(port, dot3sDeferredTransmissions, &value))
		mac_stats->FramesWithDeferredXmissions = value;
	if (!rtl837x_read_ethtool_stat(port, dot3StatsLateCollisions, &value))
		mac_stats->LateCollisions = value;
	if (!rtl837x_read_ethtool_stat(port, dot3StatsExcessiveCollisions,
					     &value))
		mac_stats->FramesAbortedDueToXSColls = value;

	/*
	 * rx_etherStatsCRCAlignErrors combines FCS and alignment errors, while
	 * the standard interface exposes them separately.  Leave both fields
	 * unset instead of reporting the same combined counter twice.
	 */
}

static void rtl837x_get_eth_ctrl_stats(struct dsa_switch *ds, int port,
					      struct ethtool_eth_ctrl_stats *ctrl_stats)
{
	struct rtk_gsw *gsw = ds->priv;
	u64 value;

	if (!rtl837x_valid_port(gsw, port))
		return;

	if (!rtl837x_read_ethtool_stat(port, dot3ControlInUnknownOpcodes, &value))
		ctrl_stats->UnsupportedOpcodesReceived = value;

	/* Pause frames are already exposed through get_pause_stats(). */
}

static const struct ethtool_rmon_hist_range rtl837x_rmon_ranges[] = {
	{ 0, 64 },
	{ 65, 127 },
	{ 128, 255 },
	{ 256, 511 },
	{ 512, 1023 },
	{ 1024, 1518 },
	{}
};

static void rtl837x_get_rmon_stats(struct dsa_switch *ds, int port,
					      struct ethtool_rmon_stats *rmon_stats,
					      const struct ethtool_rmon_hist_range **ranges)
{
	struct rtk_gsw *gsw = ds->priv;
	u64 value;

	*ranges = rtl837x_rmon_ranges;

	if (!rtl837x_valid_port(gsw, port))
		return;

	if (!rtl837x_read_ethtool_stat(port, rx_etherStatsUndersizePkts, &value))
		rmon_stats->undersize_pkts = value;
	if (!rtl837x_read_ethtool_stat(port, rx_etherStatsOversizePkts, &value))
		rmon_stats->oversize_pkts = value;
	if (!rtl837x_read_ethtool_stat(port, rx_etherStatsFragments, &value))
		rmon_stats->fragments = value;
	if (!rtl837x_read_ethtool_stat(port, rx_etherStatsJabbers, &value))
		rmon_stats->jabbers = value;

	if (!rtl837x_read_ethtool_stat(port, rx_etherStatsPkts64Octets, &value))
		rmon_stats->hist[0] = value;
	if (!rtl837x_read_ethtool_stat(port, rx_etherStatsPkts65to127Octets,
					    &value))
		rmon_stats->hist[1] = value;
	if (!rtl837x_read_ethtool_stat(port, rx_etherStatsPkts128to255Octets,
					    &value))
		rmon_stats->hist[2] = value;
	if (!rtl837x_read_ethtool_stat(port, rx_etherStatsPkts256to511Octets,
					    &value))
		rmon_stats->hist[3] = value;
	if (!rtl837x_read_ethtool_stat(port, rx_etherStatsPkts512to1023Octets,
					    &value))
		rmon_stats->hist[4] = value;
	if (!rtl837x_read_ethtool_stat(port, rx_etherStatsPkts1024to1518Octets,
					    &value))
		rmon_stats->hist[5] = value;

	if (!rtl837x_read_ethtool_stat(port, tx_etherStatsPkts64Octets, &value))
		rmon_stats->hist_tx[0] = value;
	if (!rtl837x_read_ethtool_stat(port, tx_etherStatsPkts65to127Octets,
					    &value))
		rmon_stats->hist_tx[1] = value;
	if (!rtl837x_read_ethtool_stat(port, tx_etherStatsPkts128to255Octets,
					    &value))
		rmon_stats->hist_tx[2] = value;
	if (!rtl837x_read_ethtool_stat(port, tx_etherStatsPkts256to511Octets,
					    &value))
		rmon_stats->hist_tx[3] = value;
	if (!rtl837x_read_ethtool_stat(port, tx_etherStatsPkts512to1023Octets,
					    &value))
		rmon_stats->hist_tx[4] = value;
	if (!rtl837x_read_ethtool_stat(port, tx_etherStatsPkts1024to1518Octets,
					    &value))
		rmon_stats->hist_tx[5] = value;

	/*
	 * The SDK has 1519-to-max counters too, but does not expose the
	 * RTL8373 maximum frame size needed to describe that range.  Keep the
	 * seventh bucket unset rather than publishing an invented upper bound.
	 */
}

static void rtl837x_get_stats64(struct dsa_switch *ds, int port,
				struct rtnl_link_stats64 *stats)
{
	struct rtk_gsw *gsw = ds->priv;
	struct rtl837x_port_stats *port_stats;

	if (!rtl837x_user_port(gsw, port))
		return;

	port_stats = &gsw->port_stats[port];
	spin_lock_bh(&port_stats->lock);
	*stats = port_stats->stats;
	spin_unlock_bh(&port_stats->lock);
}

static int rtl837x_set_ageing_time(struct dsa_switch *ds, unsigned int msecs)
{
	unsigned int secs = DIV_ROUND_UP(msecs, 1000);

	secs = clamp_t(unsigned int, secs, 14, 800);
	return rtl837x_to_errno(rtk_l2_aging_set(secs));
}

static void rtl837x_port_stp_state_set(struct dsa_switch *ds, int port,
				       u8 state)
{
	struct rtk_gsw *gsw = ds->priv;
	int ret;

	ret = rtl837x_set_stp_state(gsw, port, state);
	if (ret)
		dev_err(gsw->dev, "failed to set STP state %u on port %d: %d\n",
			state, port, ret);
}

static void rtl837x_port_fast_age(struct dsa_switch *ds, int port)
{
	struct rtk_gsw *gsw = ds->priv;
	rtk_l2_flushCfg_t cfg = { 0 };
	rtk_api_ret_t ret;

	if (!rtl837x_user_port(gsw, port))
		return;

	cfg.flushByPort = ENABLED;
	cfg.portmask = BIT(port);
	cfg.flushStaticAddr = DISABLED;
	cfg.flushAddrOnAllPorts = DISABLED;

	ret = rtk_l2_ucastAddr_flush(&cfg);
	if (ret)
		dev_err(gsw->dev, "failed to flush FDB for port %d: %d\n", port,
			ret);
}

static int rtl837x_fdb_vid(u16 vid, struct dsa_db db, u16 *fdb_vid)
{
	if (vid) {
		*fdb_vid = vid;
		return 0;
	}

	switch (db.type) {
	case DSA_DB_PORT:
		*fdb_vid = dsa_tag_8021q_standalone_vid(db.dp);
		return 0;
	case DSA_DB_BRIDGE:
		*fdb_vid = dsa_tag_8021q_bridge_vid(db.bridge.num);
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

static int rtl837x_port_vlan_fast_age(struct dsa_switch *ds, int port, u16 vid)
{
	struct rtk_gsw *gsw = ds->priv;
	rtk_l2_flushCfg_t cfg = { 0 };
	rtk_api_ret_t ret;

	if (!rtl837x_user_port(gsw, port) || !vid || vid > RTK_VID_MAX)
		return -EINVAL;

	cfg.flushByVid = ENABLED;
	cfg.vid = vid;
	/* The RTL8373 VID mode uses FLUSH_PMSK as the port restriction. */
	cfg.portmask = BIT(port);
	cfg.flushStaticAddr = DISABLED;
	cfg.flushAddrOnAllPorts = DISABLED;

	ret = rtk_l2_ucastAddr_flush(&cfg);
	return rtl837x_to_errno(ret);
}

static int rtl837x_port_vlan_filtering(struct dsa_switch *ds, int port,
				       bool vlan_filtering,
				       struct netlink_ext_ack *extack)
{
	struct rtk_gsw *gsw = ds->priv;
	struct dsa_port *dp;
	bool old_vlan_filtering;
	int ret, rollback_ret;

	if (!rtl837x_user_port(gsw, port))
		return 0;

	dp = dsa_to_port(ds, port);
	old_vlan_filtering = dsa_port_is_vlan_filtering(dp);

	ret = rtk_vlan_portIgrFilterEnable_set(port,
					       vlan_filtering ? ENABLED : DISABLED);
	if (ret)
		return rtl837x_to_errno(ret);

	ret = rtl837x_commit_pvid_for_mode(gsw, port, vlan_filtering);
	if (ret) {
		rollback_ret = rtk_vlan_portIgrFilterEnable_set(
				port, old_vlan_filtering ? ENABLED : DISABLED);
		if (rollback_ret)
			dev_err(gsw->dev,
				"failed to restore VLAN ingress filtering on port %d: %d\n",
				port, rtl837x_to_errno(rollback_ret));
	}

	return ret;
}

static int rtl837x_port_vlan_add(struct dsa_switch *ds, int port,
				 const struct switchdev_obj_port_vlan *vlan,
				 struct netlink_ext_ack *extack)
{
	struct rtk_gsw *gsw = ds->priv;
	bool untagged = vlan->flags & BRIDGE_VLAN_INFO_UNTAGGED;
	bool pvid = vlan->flags & BRIDGE_VLAN_INFO_PVID;
	u16 vid = vlan->vid;
	typeof(gsw->vlan_table[0]) old_vlan;
	int ret;

	if (!rtl837x_valid_port(gsw, port))
		return -EINVAL;

	if (!vid)
		return 0;

	if (vid > RTK_VID_MAX) {
		NL_SET_ERR_MSG_MOD(extack, "VLAN ID out of range");
		return -EINVAL;
	}

	old_vlan = gsw->vlan_table[vid];

	gsw->vlan_table[vid].valid = 1;
	gsw->vlan_table[vid].vid = vid;
	gsw->vlan_table[vid].mbr |= BIT(port);
	gsw->vlan_table[vid].untag &= ~BIT(port);

	if (untagged)
		gsw->vlan_table[vid].untag |= BIT(port);

	if (port != gsw->cpu_port) {
		gsw->vlan_table[vid].mbr |= BIT(gsw->cpu_port);
		gsw->vlan_table[vid].untag &= ~BIT(gsw->cpu_port);
	}

	ret = rtl837x_write_vlan(gsw, vid);
	if (ret) {
		gsw->vlan_table[vid] = old_vlan;
		NL_SET_ERR_MSG_MOD(extack, "failed to program VLAN");
		return ret;
	}

	if (pvid && port != gsw->cpu_port) {
		u16 old_pvid = gsw->bridge_pvid[port];
		bool old_pvid_valid = gsw->bridge_pvid_valid[port];

		gsw->bridge_pvid[port] = vid;
		gsw->bridge_pvid_valid[port] = true;
		ret = rtl837x_commit_pvid(gsw, port);
		if (ret) {
			gsw->bridge_pvid[port] = old_pvid;
			gsw->bridge_pvid_valid[port] = old_pvid_valid;
		}
		return ret;
	}

	return 0;
}

static int rtl837x_port_vlan_del(struct dsa_switch *ds, int port,
				 const struct switchdev_obj_port_vlan *vlan)
{
	struct rtk_gsw *gsw = ds->priv;
	u16 vid = vlan->vid;
	typeof(gsw->vlan_table[0]) old_vlan;
	int ret;

	if (!rtl837x_valid_port(gsw, port))
		return -EINVAL;

	if (!vid || vid > RTK_VID_MAX || !gsw->vlan_table[vid].valid)
		return 0;

	old_vlan = gsw->vlan_table[vid];

	gsw->vlan_table[vid].mbr &= ~BIT(port);
	gsw->vlan_table[vid].untag &= ~BIT(port);

	if (!rtl837x_vlan_has_user(gsw, vid)) {
		gsw->vlan_table[vid].mbr &= ~BIT(gsw->cpu_port);
		gsw->vlan_table[vid].untag &= ~BIT(gsw->cpu_port);
	}

	if (!gsw->vlan_table[vid].mbr)
		gsw->vlan_table[vid].valid = 0;

	ret = rtl837x_write_vlan(gsw, vid);
	if (ret) {
		gsw->vlan_table[vid] = old_vlan;
		return ret;
	}

	if (port != gsw->cpu_port && gsw->bridge_pvid_valid[port] &&
	    gsw->bridge_pvid[port] == vid) {
		u16 old_pvid = gsw->bridge_pvid[port];
		bool old_pvid_valid = gsw->bridge_pvid_valid[port];

		gsw->bridge_pvid_valid[port] = false;
		ret = rtl837x_commit_pvid(gsw, port);
		if (ret) {
			gsw->bridge_pvid[port] = old_pvid;
			gsw->bridge_pvid_valid[port] = old_pvid_valid;
		}
		return ret;
	}

	return 0;
}

static int rtl837x_port_fdb_add(struct dsa_switch *ds, int port,
				const unsigned char *addr, u16 vid,
				struct dsa_db db)
{
	struct rtk_gsw *gsw = ds->priv;
	rtk_l2_ucastAddr_t l2 = { 0 };
	rtk_mac_t mac = { 0 };
	u16 fdb_vid;
	int ret;

	if (!rtl837x_valid_port(gsw, port))
		return -EINVAL;

	/* Host-bound unknown unicast is already flooded to the CPU port. */
	if (gsw->chip_id == CHIP_RTL8372N && port == gsw->cpu_port)
		return 0;

	ret = rtl837x_fdb_vid(vid, db, &fdb_vid);
	if (ret)
		return ret;

	memcpy(mac.octet, addr, ETH_ALEN);
	memcpy(l2.mac.octet, addr, ETH_ALEN);
	l2.ivl = fdb_vid ? 1 : 0;
	l2.vid_fid = fdb_vid;
	l2.port = port;
	l2.auth = 1;
	l2.is_static = 1;

	ret = rtk_l2_addr_add(&mac, &l2);
	return rtl837x_to_errno(ret);
}

static int rtl837x_port_fdb_del(struct dsa_switch *ds, int port,
				const unsigned char *addr, u16 vid,
				struct dsa_db db)
{
	struct rtk_gsw *gsw = ds->priv;
	rtk_l2_ucastAddr_t l2 = { 0 };
	rtk_mac_t mac = { 0 };
	u16 fdb_vid;
	int ret;

	if (!rtl837x_valid_port(gsw, port))
		return -EINVAL;

	if (gsw->chip_id == CHIP_RTL8372N && port == gsw->cpu_port)
		return 0;

	ret = rtl837x_fdb_vid(vid, db, &fdb_vid);
	if (ret)
		return ret;

	memcpy(mac.octet, addr, ETH_ALEN);
	memcpy(l2.mac.octet, addr, ETH_ALEN);
	l2.ivl = fdb_vid ? 1 : 0;
	l2.vid_fid = fdb_vid;
	l2.port = port;
	l2.is_static = 1;

	ret = rtk_l2_addr_del(&mac, &l2);
	if (ret == RT_ERR_L2_ENTRY_NOTFOUND)
		return 0;

	return rtl837x_to_errno(ret);
}

static int rtl837x_port_fdb_dump(struct dsa_switch *ds, int port,
				 dsa_fdb_dump_cb_t *cb, void *data)
{
	struct rtk_gsw *gsw = ds->priv;
	u32 max = RTK_MAX_LUT_ADDRESS;
	u32 address = 0;
	int ret;

	if (!rtl837x_valid_port(gsw, port))
		return -EINVAL;

	while (address < max) {
		rtk_l2_ucastAddr_t l2 = { 0 };
		u16 vid;

		ret = rtk_l2_addr_next_get(READMETHOD_NEXT_L2UCSPA, port,
					   &address, &l2);
		if (ret == RT_ERR_L2_ENTRY_NOTFOUND)
			break;
		if (ret != RT_ERR_OK)
			return rtl837x_to_errno(ret);
		if (l2.port != port || is_multicast_ether_addr(l2.mac.octet)) {
			address++;
			continue;
		}

		/* tag_8021q VIDs are this driver's own transport, not
		 * something the bridge configured, so report them as 0 the
		 * way an unaware entry is reported.
		 */
		vid = l2.ivl ? l2.vid_fid : 0;
		if (vid_is_dsa_8021q(vid))
			vid = 0;

		ret = cb(l2.mac.octet, vid, l2.is_static, data);
		if (ret)
			return ret;

		address++;
	}

	return 0;
}

static const struct dsa_switch_ops rtl837x_dsa_ops = {
	.get_tag_protocol = rtl837x_get_tag_protocol,
	.devlink_info_get = rtl837x_devlink_info_get,
	.setup = rtl837x_setup,
	.teardown = rtl837x_teardown,
	.phylink_get_caps = rtl837x_phylink_get_caps,
	.get_strings = rtl837x_get_strings,
	.get_ethtool_stats = rtl837x_get_ethtool_stats,
	.get_sset_count = rtl837x_get_sset_count,
	.get_stats64 = rtl837x_get_stats64,
	.get_pause_stats = rtl837x_get_pause_stats,
	.get_eth_phy_stats = rtl837x_get_eth_phy_stats,
	.get_eth_mac_stats = rtl837x_get_eth_mac_stats,
	.get_eth_ctrl_stats = rtl837x_get_eth_ctrl_stats,
	.get_rmon_stats = rtl837x_get_rmon_stats,
	.set_ageing_time = rtl837x_set_ageing_time,
	.port_pre_bridge_flags = rtl837x_port_pre_bridge_flags,
	.port_bridge_flags = rtl837x_port_bridge_flags,
	.support_eee = rtl837x_support_eee,
	.set_mac_eee = rtl837x_set_mac_eee,
	.port_bridge_join = rtl837x_port_bridge_join,
	.port_bridge_leave = rtl837x_port_bridge_leave,
	.port_stp_state_set = rtl837x_port_stp_state_set,
	.port_fast_age = rtl837x_port_fast_age,
	.port_vlan_fast_age = rtl837x_port_vlan_fast_age,
	.port_vlan_filtering = rtl837x_port_vlan_filtering,
	.port_vlan_add = rtl837x_port_vlan_add,
	.port_vlan_del = rtl837x_port_vlan_del,
	.port_fdb_add = rtl837x_port_fdb_add,
	.port_fdb_del = rtl837x_port_fdb_del,
	.port_fdb_dump = rtl837x_port_fdb_dump,
	.tag_8021q_vlan_add = rtl837x_tag_8021q_vlan_add,
	.tag_8021q_vlan_del = rtl837x_tag_8021q_vlan_del,
};

int rtl837x_dsa_register(struct rtk_gsw *gsw)
{
	struct dsa_switch *ds = &gsw->ds;
	int ret;

	rtl837x_stats_init(gsw);
	mutex_init(&gsw->isolation_lock);

	ds->dev = gsw->dev;
	ds->priv = gsw;
	ds->ops = &rtl837x_dsa_ops;
	ds->num_ports = gsw->dsa_num_ports;
	ds->phys_mii_mask = rtl837x_user_ports(gsw);
	ds->configure_vlan_while_not_filtering = true;
	ds->untag_bridge_pvid = true;
	ds->fdb_isolation = true;
	ds->max_num_bridges = DSA_TAG_8021Q_MAX_NUM_BRIDGES;
	ds->ageing_time_min = 14000;
	ds->ageing_time_max = 800000;

	ret = dsa_register_switch(ds);
	if (ret)
		return ret;

	gsw->dsa_registered = true;
	return 0;
}

void rtl837x_dsa_unregister(struct rtk_gsw *gsw)
{
	if (!gsw->dsa_registered)
		return;

	dsa_unregister_switch(&gsw->ds);
	gsw->dsa_registered = false;
}

void rtl837x_dsa_shutdown(struct rtk_gsw *gsw)
{
	if (!gsw->dsa_registered)
		return;

	rtl837x_stats_stop(gsw);
	dsa_switch_shutdown(&gsw->ds);
	gsw->dsa_registered = false;
}
