/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2025 StarField Xu <air_jinkela@163.com>
 */

#include <linux/bitops.h>
#include <linux/etherdevice.h>
#include <linux/if_bridge.h>
#include <linux/if_ether.h>
#include <linux/if_vlan.h>
#include <linux/kernel.h>
#include <linux/phylink.h>
#include <linux/phy.h>
#include <linux/string.h>
#include <linux/dsa/8021q.h>
#include <net/flow_offload.h>
#include <net/pkt_cls.h>
#include <net/dsa.h>
#include <net/devlink.h>
#include <net/switchdev.h>

#include "./rtl837x_common.h"
#include "./rtk-api/l2.h"
#include "./rtk-api/mirror.h"
#include "./rtk-api/rate.h"
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

static bool
rtl837x_rate_rule_is_port_wide(const struct flow_rule *rule)
{
	struct flow_match_basic basic;
	unsigned long used_keys;

	/* cls_flower always includes CONTROL and BASIC in used_keys.  Neither
	 * key represents a selective match unless its actual mask is non-zero.
	 */
	if (!rule->match.dissector)
		return true;

	used_keys = rule->match.dissector->used_keys;
	used_keys &= ~BIT(FLOW_DISSECTOR_KEY_CONTROL);
	used_keys &= ~BIT(FLOW_DISSECTOR_KEY_BASIC);
	if (used_keys)
		return false;

	if (!flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_BASIC))
		return true;

	flow_rule_match_basic(rule, &basic);
	return !basic.mask || (!basic.mask->n_proto && !basic.mask->ip_proto);
}

static const struct flow_action_entry *
rtl837x_rate_policy_extract(struct flow_cls_offload *cls)
{
	struct flow_rule *rule;

	/* Keep the offload contract narrow: one policing action and no
	 * statistics mode that this hardware cannot report.
	 */
	rule = flow_cls_offload_flow_rule(cls);
	if (!flow_action_basic_hw_stats_check(&cls->rule->action,
					     cls->common.extack))
		return NULL;

	if (!flow_offload_has_one_action(&rule->action))
		return NULL;

	return &rule->action.entries[0];
}

static bool
rtl837x_egress_rate_policy_validate(const struct flow_action_entry *act)
{
	if (!act || act->id != FLOW_ACTION_POLICE)
		return false;

	/* The RTL8373 egress port meter is a byte-rate policer with a byte
	 * bucket. Packet-rate, peak-rate, alternate actions, MTU limits below a
	 * VLAN Ethernet frame, and non-drop actions cannot be represented by the
	 * switch API.
	 */
	return act->police.rate_bytes_ps && !act->police.rate_pkt_ps &&
	       !act->police.peakrate_bytes_ps && !act->police.avrate &&
	       !act->police.overhead && !act->police.burst_pkt &&
	       act->police.mtu >= VLAN_ETH_FRAME_LEN && act->police.burst &&
	       act->police.exceed.act_id == FLOW_ACTION_DROP &&
	       act->police.notexceed.act_id == FLOW_ACTION_ACCEPT;
}

static int rtl837x_rate_to_kbps(const struct flow_action_entry *act,
				       rtk_rate_t *rate)
{
	u64 kbps;

	/* RTK's rate argument is expressed in kbit/s and is quantized in
	 * 16-kbit/s steps by the RTL8373 DAL.
	 */
	kbps = div_u64(act->police.rate_bytes_ps, 125);
	kbps &= ~0xfULL;
	if (!kbps || kbps > INBW_CTRL_RATE_MAX)
		return -ERANGE;

	*rate = (rtk_rate_t)kbps;
	return 0;
}

static int rtl837x_rate_burst_set(int port, u32 burst)
{
	int ret;

	ret = rtk_rate_egrBwCtrlBurst_set(port, burst);
	if (ret == RT_ERR_INPUT)
		return -ERANGE;

	return rtl837x_to_errno(ret);
}

static int rtl837x_rate_disable(int port)
{
	return rtl837x_to_errno(rtk_rate_egrBwCtrlPortEn_set(port, DISABLED));
}

static void rtl837x_rate_cleanup_on_error(int port, bool *cleanup_pending)
{
	if (rtl837x_rate_disable(port) && cleanup_pending)
		*cleanup_pending = true;
}

static int rtl837x_rate_program(int port, rtk_rate_t rate, u32 burst,
					bool disable_on_error, bool *cleanup_pending)
{
	int ret;

	/* Program only the port-specific rate and burst, preserving the
	 * chip-global IFG accounting setting.
	 */
	ret = rtl837x_to_errno(rtk_rate_egrBwCtrlRate_set(port, rate));
	if (ret) {
		if (disable_on_error)
			rtl837x_rate_cleanup_on_error(port, cleanup_pending);
		return ret;
	}

	ret = rtl837x_rate_burst_set(port, burst);
	if (ret) {
		if (disable_on_error)
			rtl837x_rate_cleanup_on_error(port, cleanup_pending);
		return ret;
	}

	ret = rtl837x_to_errno(rtk_rate_egrBwCtrlPortEn_set(port, ENABLED));
	if (ret && disable_on_error)
		rtl837x_rate_cleanup_on_error(port, cleanup_pending);

	return ret;
}

static int rtl837x_rate_disable_all(struct rtk_gsw *gsw)
{
	int first_ret = 0;
	int port, ret;

	for (port = 0; port < RTK_MAX_NUM_OF_PORT; port++) {
		if (!rtl837x_valid_port(gsw, port))
			continue;

		ret = rtl837x_rate_disable(port);
		if (ret) {
			dev_warn(gsw->dev,
				 "failed to disable egress rate limiter on port %d: %d\n",
				 port, ret);
			if (!first_ret)
				first_ret = ret;
		}
	}

	return first_ret;
}

static void rtl837x_rate_clear_state(struct rtk_gsw *gsw, int port)
{
	clear_bit(port, &gsw->rate_egress_mask);
	gsw->rate_egress_cookie[port] = 0;
	gsw->rate_egress_rate[port] = 0;
	gsw->rate_egress_burst[port] = 0;
	gsw->rate_egress_cleanup_pending[port] = false;
}

static int rtl837x_cls_flower_add(struct dsa_switch *ds, int port,
					  struct flow_cls_offload *cls,
					  bool ingress)
{
	struct rtk_gsw *gsw = ds->priv;
	const struct flow_action_entry *act;
	unsigned long *mask;
	rtk_rate_t rate, old_rate;
	u32 old_burst;
	bool replacing;
	int rollback_ret;
	int ret;

	if (!rtl837x_user_port(gsw, port))
		return -EINVAL;

	if (ingress) {
		NL_SET_ERR_MSG_MOD(cls->common.extack,
				   "RTL837x police offload supports egress only");
		return -EOPNOTSUPP;
	}

	if (cls->common.chain_index) {
		NL_SET_ERR_MSG_MOD(cls->common.extack,
				   "RTL837x port rate limiter supports chain 0 only");
		return -EOPNOTSUPP;
	}

	/* The RTK rate meters are attached to a physical port, not to a
	 * classifier entry. Never turn a selective flower rule into a
	 * port-wide limiter.
	 */
	if (!rtl837x_rate_rule_is_port_wide(cls->rule)) {
		NL_SET_ERR_MSG_MOD(cls->common.extack,
				   "RTL837x rate limiter rejects selective/non-empty matches");
		return -EOPNOTSUPP;
	}

	act = rtl837x_rate_policy_extract(cls);
	if (!rtl837x_egress_rate_policy_validate(act)) {
		NL_SET_ERR_MSG_MOD(cls->common.extack,
				   "RTL837x egress police requires a byte rate, non-zero byte burst, and drop/accept actions");
		return -EOPNOTSUPP;
	}

	ret = rtl837x_rate_to_kbps(act, &rate);
	if (ret) {
		NL_SET_ERR_MSG_MOD(cls->common.extack,
				   "RTL837x egress police rate is outside the supported hardware range");
		return ret;
	}

	mask = &gsw->rate_egress_mask;
	replacing = test_bit(port, mask);
	if (gsw->rate_egress_cleanup_pending[port] &&
	    (!replacing || gsw->rate_egress_cookie[port] != cls->cookie)) {
		/* A previous operation could not disable the meter. Reclaim it
		 * before programming a new owner or a new rule.
		 */
		ret = rtl837x_rate_disable(port);
		if (ret) {
			NL_SET_ERR_MSG_MOD(cls->common.extack,
					   "RTL837x egress rate limiter cleanup is still pending");
			return ret;
		}

		rtl837x_rate_clear_state(gsw, port);
		replacing = false;
	}

	if (replacing && gsw->rate_egress_cookie[port] != cls->cookie) {
		NL_SET_ERR_MSG_MOD(cls->common.extack,
				   "RTL837x egress rate limiter is already owned by another filter");
		return -EOPNOTSUPP;
	}

	if (!replacing) {
		ret = rtl837x_rate_program(port, rate, act->police.burst, true,
					   &gsw->rate_egress_cleanup_pending[port]);
		if (ret) {
			if (ret == -ERANGE)
				NL_SET_ERR_MSG_MOD(cls->common.extack,
						   "RTL837x egress police burst is outside the supported hardware range");
			return ret;
		}

		set_bit(port, mask);
		gsw->rate_egress_cookie[port] = cls->cookie;
		gsw->rate_egress_rate[port] = rate;
		gsw->rate_egress_burst[port] = act->police.burst;
		gsw->rate_egress_cleanup_pending[port] = false;
		return 0;
	}

	old_rate = gsw->rate_egress_rate[port];
	old_burst = gsw->rate_egress_burst[port];
	ret = rtl837x_rate_program(port, rate, act->police.burst, false, NULL);
	if (ret) {
		rollback_ret = rtl837x_rate_program(port, old_rate, old_burst, false,
						     NULL);
		if (rollback_ret) {
			dev_err(gsw->dev,
				 "failed to restore egress rate limiter on port %d after replace failure: %d\n",
				 port, rollback_ret);
			if (rtl837x_rate_disable(port))
				dev_err(gsw->dev,
					 "failed to disable egress rate limiter on port %d after replace rollback failure\n",
					 port);
		}
		if (ret == -ERANGE)
			NL_SET_ERR_MSG_MOD(cls->common.extack,
					   "RTL837x egress police rate or burst is outside the supported hardware range");
		return ret;
	}

	gsw->rate_egress_rate[port] = rate;
	gsw->rate_egress_burst[port] = act->police.burst;
	gsw->rate_egress_cleanup_pending[port] = false;

	return 0;
}

static int rtl837x_cls_flower_del(struct dsa_switch *ds, int port,
					  struct flow_cls_offload *cls,
					  bool ingress)
{
	struct rtk_gsw *gsw = ds->priv;
	unsigned long *mask;
	int ret;

	if (!rtl837x_user_port(gsw, port))
		return -EINVAL;

	if (ingress)
		return 0;

	mask = &gsw->rate_egress_mask;
	if (!test_bit(port, mask) ||
	    gsw->rate_egress_cookie[port] != cls->cookie)
		return 0;

	ret = rtl837x_rate_disable(port);
	if (!ret)
		rtl837x_rate_clear_state(gsw, port);
	else
		gsw->rate_egress_cleanup_pending[port] = true;

	return ret;
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
	} else if (!valid) {
		/* Without tag_8021q there is no per-port PVID to carry: the
		 * port classifies into the seeded base VLAN. Programming 0
		 * here put every untagged frame into a VLAN with no members --
		 * learned on ingress, then dropped before reaching the CPU --
		 * which is why the LAN was dead under the native rtl8_4 tag.
		 */
		vid = 1;
		valid = true;
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

	/* The tag_8021q join moves the port from its standalone VID onto
	 * a bridge VID and makes that VID the port's PVID; the CPU port is
	 * a tagged member of it. That is what tag_8021q needs, and its
	 * receiver strips the VID again. Under the native rtl8_4 tag the
	 * CPU tag itself carries the source port, the port must stay in
	 * the seeded VLAN 1 layout, and nothing strips a bridge VID: the
	 * frames reached the CPU tagged with a VLAN the bridge is not a
	 * member of and were all dropped -- a switch that learned every
	 * client in hardware while the bridge never saw a single frame.
	 * tag_rtl8_4 has no bridge TX forwarding offload either, so
	 * *tx_fwd_offload stays false there.
	 */
	if (gsw->tag_proto == DSA_TAG_PROTO_VSC73XX_8021Q) {
		ret = dsa_tag_8021q_bridge_join(ds, port, bridge,
						tx_fwd_offload, extack);
		if (ret)
			return ret;
	}

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
		if (gsw->tag_proto == DSA_TAG_PROTO_VSC73XX_8021Q)
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

	/* Only undo what the join did; see rtl837x_port_bridge_join(). */
	if (gsw->tag_proto == DSA_TAG_PROTO_VSC73XX_8021Q)
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

static bool rtl837x_mirror_active(const struct rtk_gsw *gsw)
{
	int port;

	for (port = 0; port < RTK_MAX_NUM_OF_PORT; port++)
		if (gsw->mirror_rx_refcnt[port] ||
		    gsw->mirror_tx_refcnt[port])
			return true;

	return false;
}

static void rtl837x_mirror_masks_from_refcnt(const struct rtk_gsw *gsw,
						     u32 *rx_mask, u32 *tx_mask)
{
	int port;

	*rx_mask = 0;
	*tx_mask = 0;

	for (port = 0; port < RTK_MAX_NUM_OF_PORT; port++) {
		if (gsw->mirror_rx_refcnt[port])
			*rx_mask |= BIT(port);
		if (gsw->mirror_tx_refcnt[port])
			*tx_mask |= BIT(port);
	}
}

static int rtl837x_mirror_set_config(int mirror_port, u32 rx_mask,
					     u32 tx_mask, bool ingress)
{
	rtk_port_mir_set_t mir = {
		.mtp_port = mirror_port,
		.rx_tx_sel = ingress ? RX_DIR : TX_DIR,
	};

	mir.rx_pmsk.bits[0] = rx_mask;
	mir.tx_pmsk.bits[0] = tx_mask;

	return rtl837x_to_errno(rtk_mirror_portBased_set(&mir));
}

static void rtl837x_mirror_disable_and_clear(struct rtk_gsw *gsw,
					    int mirror_port, bool ingress,
					    const char *context)
{
	int ret;

	ret = rtl837x_to_errno(rtk_mirror_set_en(DISABLED));
	if (ret)
		dev_warn(gsw->dev,
			 "failed to disable RTL837x mirror during %s: %d\n",
			 context, ret);

	/* Clear the source masks even if disabling the block failed. */
	ret = rtl837x_mirror_set_config(mirror_port, 0, 0, ingress);
	if (ret)
		dev_warn(gsw->dev,
			 "failed to clear RTL837x mirror masks during %s: %d\n",
			 context, ret);
}

static void rtl837x_mirror_clear_shadow(struct rtk_gsw *gsw)
{
	gsw->mirror_port = -1;
	gsw->mirror_rx_mask = 0;
	gsw->mirror_tx_mask = 0;
	memset(gsw->mirror_rx_refcnt, 0, sizeof(gsw->mirror_rx_refcnt));
	memset(gsw->mirror_tx_refcnt, 0, sizeof(gsw->mirror_tx_refcnt));
	gsw->mirror_direction_valid = false;
	gsw->mirror_ingress = false;
}

static int rtl837x_port_mirror_add(struct dsa_switch *ds, int port,
					   struct dsa_mall_mirror_tc_entry *mirror,
					   bool ingress,
					   struct netlink_ext_ack *extack)
{
	struct rtk_gsw *gsw = ds->priv;
	u32 rx_mask, tx_mask;
	bool active;
	int rollback_ret;
	int ret;

	if (!rtl837x_valid_port(gsw, port) ||
	    !rtl837x_valid_port(gsw, mirror->to_local_port))
		return -EINVAL;

	if (port == mirror->to_local_port) {
		NL_SET_ERR_MSG_MOD(extack,
				   "Mirror source and destination must differ");
		return -EINVAL;
	}

	if (mirror->to_local_port == gsw->cpu_port) {
		NL_SET_ERR_MSG_MOD(extack,
				   "RTL837x cannot mirror traffic to the CPU port");
		return -EOPNOTSUPP;
	}

	active = rtl837x_mirror_active(gsw);
	rtl837x_mirror_masks_from_refcnt(gsw, &rx_mask, &tx_mask);

	/* The RTL837x mirror block has one global monitor port. */
	if (active && gsw->mirror_port != mirror->to_local_port) {
		NL_SET_ERR_MSG_MOD(extack,
				   "RTL837x supports one mirror destination port");
		return -EBUSY;
	}

	/* rx_tx_sel is a single global hardware bit, so the chip cannot
	 * represent ingress and egress mirror rules at the same time.
	 */
	if (active && gsw->mirror_direction_valid &&
	    gsw->mirror_ingress != ingress) {
		NL_SET_ERR_MSG_MOD(extack,
				   "RTL837x supports one mirror direction at a time");
		return -EOPNOTSUPP;
	}

	if (ingress)
		rx_mask |= BIT(port);
	else
		tx_mask |= BIT(port);

	ret = rtl837x_mirror_set_config(mirror->to_local_port, rx_mask,
					       tx_mask, ingress);
	if (ret) {
		rollback_ret = active ?
			rtl837x_mirror_set_config(gsw->mirror_port,
						  gsw->mirror_rx_mask,
						  gsw->mirror_tx_mask,
						  gsw->mirror_ingress) :
			rtl837x_mirror_set_config(mirror->to_local_port, 0, 0,
						  ingress);
		if (rollback_ret)
			dev_err(ds->dev,
				"failed to restore RTL837x mirror after update failure: %d\n",
				rollback_ret);
		NL_SET_ERR_MSG_MOD(extack, "Failed to program RTL837x mirror");
		return ret;
	}

	if (!active) {
		ret = rtl837x_to_errno(rtk_mirror_set_en(ENABLED));
		if (ret) {
			int disable_ret;

			/* Best effort: do not leave a partially configured mirror
			 * enabled if the second hardware operation fails.
			 */
			rollback_ret = rtl837x_mirror_set_config(
					mirror->to_local_port, 0, 0, ingress);
			if (rollback_ret)
				dev_err(ds->dev,
					"failed to clear RTL837x mirror after enable failure: %d\n",
					rollback_ret);
			disable_ret = rtl837x_to_errno(rtk_mirror_set_en(DISABLED));
			if (disable_ret)
				dev_err(ds->dev,
					"failed to roll back RTL837x mirror: %d\n",
					disable_ret);
			NL_SET_ERR_MSG_MOD(extack,
					   "Failed to enable RTL837x mirror");
			return ret;
		}
	}

	if (ingress)
		gsw->mirror_rx_refcnt[port]++;
	else
		gsw->mirror_tx_refcnt[port]++;

	gsw->mirror_port = mirror->to_local_port;
	gsw->mirror_rx_mask = rx_mask;
	gsw->mirror_tx_mask = tx_mask;
	gsw->mirror_direction_valid = true;
	gsw->mirror_ingress = ingress;

	return 0;
}

static void rtl837x_port_mirror_del(struct dsa_switch *ds, int port,
	struct dsa_mall_mirror_tc_entry *mirror)
{
	struct rtk_gsw *gsw = ds->priv;
	u32 rx_mask, tx_mask;
	int ret;

	if (!rtl837x_valid_port(gsw, port) ||
	    !rtl837x_valid_port(gsw, mirror->to_local_port))
		return;

	if (!rtl837x_mirror_active(gsw) ||
	    gsw->mirror_port != mirror->to_local_port ||
	    (gsw->mirror_direction_valid &&
	     gsw->mirror_ingress != mirror->ingress) ||
	    (mirror->ingress ? !gsw->mirror_rx_refcnt[port] :
					 !gsw->mirror_tx_refcnt[port]))
		return;

	/* Multiple filters for one source share one hardware mask bit.  Removing
	 * one of them only changes the reference count until the last filter is
	 * gone; no hardware transaction is needed in that case.
	 */
	if (mirror->ingress) {
		if (gsw->mirror_rx_refcnt[port] > 1) {
			gsw->mirror_rx_refcnt[port]--;
			return;
		}
	} else if (gsw->mirror_tx_refcnt[port] > 1) {
		gsw->mirror_tx_refcnt[port]--;
		return;
	}

	rx_mask = gsw->mirror_rx_mask;
	tx_mask = gsw->mirror_tx_mask;
	if (mirror->ingress)
		rx_mask &= ~BIT(port);
	else
		tx_mask &= ~BIT(port);

	if (!rx_mask && !tx_mask) {
		rtl837x_mirror_disable_and_clear(gsw, gsw->mirror_port,
						 gsw->mirror_ingress,
						 "rule deletion");
		rtl837x_mirror_clear_shadow(gsw);
		return;
	}

	ret = rtl837x_mirror_set_config(gsw->mirror_port, rx_mask, tx_mask,
					gsw->mirror_ingress);
	if (ret) {
		dev_err(ds->dev,
			"failed to update RTL837x mirror during rule deletion: %d\n",
			ret);
		/* DSA has already removed this rule and cannot report cleanup
		 * failure. Fail closed and forget the old state so a later add can
		 * reprogram the mirror block.
		 */
		rtl837x_mirror_disable_and_clear(gsw, gsw->mirror_port,
						 gsw->mirror_ingress,
						 "rule deletion recovery");
		rtl837x_mirror_clear_shadow(gsw);
		return;
	}

	if (mirror->ingress)
		gsw->mirror_rx_refcnt[port]--;
	else
		gsw->mirror_tx_refcnt[port]--;
	gsw->mirror_rx_mask = rx_mask;
	gsw->mirror_tx_mask = tx_mask;
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

/* Apply a port's ingress policy for the given VLAN-filtering mode.
 *
 * Always write the RESTRICTIVE setting first, in both directions, so no
 * transition ever passes through "ingress filtering off AND every frame type
 * accepted" -- the exact combination that lets whatever is plugged into a user
 * port choose its own VLAN. The two SDK calls are separate register writes and
 * are never atomic with each other, and the off direction is routine, not just
 * an admin toggle: DSA calls us with vlan_filtering=false from
 * dsa_port_reset_vlan_filtering() whenever the last VLAN-aware bridge on the
 * switch goes away.
 *
 * KNOWN GAP, deliberately not addressed here: a VLAN-AWARE bridge port stays at
 * ACCEPT_FRAME_TYPE_ALL, and dsa_tag_8021q_bridge_join() has made it a hardware
 * member of the shared bridge VID. Ingress filtering therefore does NOT stop it
 * injecting a frame tagged with that VID -- the port genuinely is a member --
 * which floods within the bridge VID and also reaches the CPU, where the VBID
 * path attributes it to some port of that bridge. Closing that needs the user
 * port's bridge-VID membership dropped while vlan_filtering is on (the tagger
 * does not use that VID in VLAN-aware mode), which is a separate change.
 */
static int rtl837x_set_ingress_policy(struct rtk_gsw *gsw, int port,
				      bool vlan_filtering)
{
	int ret;

	if (vlan_filtering) {
		/* Start checking membership before admitting tagged frames. */
		ret = rtk_vlan_portIgrFilterEnable_set(port, ENABLED);
		if (ret)
			return rtl837x_to_errno(ret);

		ret = rtk_vlan_portAcceptFrameType_set(port, ACCEPT_FRAME_TYPE_ALL);
		if (ret)
			return rtl837x_to_errno(ret);

		return 0;
	}

	/* Stop admitting tagged frames before membership checking goes away. */
	ret = rtk_vlan_portAcceptFrameType_set(port, ACCEPT_FRAME_TYPE_UNTAG_ONLY);
	if (ret)
		return rtl837x_to_errno(ret);

	ret = rtk_vlan_portIgrFilterEnable_set(port, DISABLED);
	if (ret)
		return rtl837x_to_errno(ret);

	return 0;
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

		/* With ingress filtering off, the switch would honour whatever
		 * VLAN tag a frame arrives with, so anything plugged into a
		 * user port could pick its own VLAN: the tag_8021q bridge VID
		 * (reaching the CPU, where DSA decodes it as a bridge port, so
		 * the frame is firewalled as if it came from that bridge),
		 * another port's standalone VID, or VLAN 1. Admit only
		 * untagged and priority-tagged frames on user ports, so
		 * classification is always the port's own PVID -- which is
		 * what tag_8021q relies on to identify the source port.
		 *
		 * Nothing that works today is lost: an unknown VID has no
		 * members in the table, so such frames are already dropped by
		 * egress filtering. ACCEPT_FRAME_TYPE_UNTAG_ONLY still admits
		 * priority-tagged (VID 0) frames, so 802.1p clients are
		 * unaffected. The CPU port must keep accepting tagged frames:
		 * that is how the tagger addresses a port.
		 */
		if (rtl837x_user_port(gsw, port)) {
			ret = rtl837x_set_ingress_policy(gsw, port, false);
			if (ret)
				return ret;
		} else {
			ret = rtk_vlan_portIgrFilterEnable_set(port, DISABLED);
			if (ret)
				return rtl837x_to_errno(ret);

			ret = rtk_vlan_portAcceptFrameType_set(port, ACCEPT_FRAME_TYPE_ALL);
			if (ret)
				return rtl837x_to_errno(ret);
		}

	}

	return 0;
}

/* Turn the conduit's checksum offloads on or off.
 *
 * edma_tx.c sets the descriptor's IP_CSUM/L4_CSUM bits whenever an skb arrives
 * as CHECKSUM_PARTIAL and never passes skb->csum_start -- it leaves hardware to
 * locate L3/L4 by parsing the frame. An 8-byte 0x8899 CPU tag sits between the
 * source MAC and the ethertype and moves those headers, so the offload
 * checksums the wrong range. Measured on the bench unit: with rtl8_4 every TCP
 * connection through the CPU failed while ICMP -- which the stack checksums in
 * software -- worked perfectly, including 1400-byte payloads.
 *
 * So checksum offload, and the segmentation offloads that depend on it, have to
 * go while a header-tag protocol is in use. That is the real, measured price of
 * precise per-port identity.
 *
 * Caller holds rtnl.
 */
static void rtl837x_conduit_csum_offload(struct dsa_switch *ds, bool enable)
{
	netdev_features_t csum = NETIF_F_IP_CSUM | NETIF_F_IPV6_CSUM |
				  NETIF_F_HW_CSUM | NETIF_F_RXCSUM |
				  NETIF_F_TSO | NETIF_F_TSO6;
	struct rtk_gsw *gsw = ds->priv;
	struct net_device *conduit;
	struct dsa_port *cpu_dp;

	cpu_dp = dsa_to_port(ds, gsw->cpu_port);
	if (!cpu_dp)
		return;

	conduit = cpu_dp->conduit;
	if (!conduit)
		return;

	if (enable)
		conduit->wanted_features |= csum;
	else
		conduit->wanted_features &= ~csum;

	netdev_update_features(conduit);

	dev_info(ds->dev, "%s checksum offload on conduit %s for tagger change\n",
		 enable ? "enabled" : "disabled", netdev_name(conduit));
}

/* Apply the chip-side configuration a tagger needs. Caller holds rtnl. */
static int rtl837x_tag_protocol_apply(struct dsa_switch *ds,
				       enum dsa_tag_protocol proto)
{
	struct rtk_gsw *gsw = ds->priv;
	int ret;

	switch (proto) {
	case DSA_TAG_PROTO_VSC73XX_8021Q:
		/* Port identity rides a standard 802.1Q tag, so the
		 * proprietary 0x8899 CPU tag must stay off. This is the
		 * fallback tagger: the PPE parser handles a plain 802.1Q tag
		 * natively, whereas RTL8_4 relies on the conduit driver's
		 * parser alias. The cost is that identity is lost whenever a
		 * port is bridged -- the bridge takes the VLAN field the port
		 * number is encoded in.
		 */
		ret = rtk_cpuTag_enable_set(EXTERNAL_CPU, DISABLED);
		if (ret)
			return rtl837x_to_errno(ret);

		/* tag_8021q reads the port off the VLAN tag the switch itself
		 * puts on CPU-port egress, so that tagging must be on.
		 */
		ret = rtk_vlan_tagMode_set(gsw->cpu_port,
					   VLAN_EGRESS_TAG_MODE_ORIGINAL);
		if (ret)
			return rtl837x_to_errno(ret);

		ret = dsa_tag_8021q_register(ds, htons(ETH_P_8021Q));
		if (ret)
			return ret;

		/* A standard 802.1Q tag is something the PPE parser handles,
		 * so hardware checksumming is safe again.
		 */
		rtl837x_conduit_csum_offload(ds, true);
		break;
	case DSA_TAG_PROTO_RTL8_4:
		/* The chip's own CPU tag. The source port is an explicit field
		 * rather than an overloaded VLAN ID, so identity stays precise
		 * under a bridge in either VLAN mode.
		 *
		 * cpuTag_enable_set(EXTERNAL_CPU, ENABLED) also adds ONLY the
		 * external CPU port to CPU_TAG_AWARE_CTRL's port mask, so an
		 * 0x8899 frame forged from a user port is parsed as data, not
		 * as a CPU tag.
		 */
		ret = rtk_cpuTag_insertMode_set(EXTERNAL_CPU, CPU_INSERT_TO_ALL);
		if (ret)
			return rtl837x_to_errno(ret);

		ret = rtk_cpuTag_enable_set(EXTERNAL_CPU, ENABLED);
		if (ret)
			return rtl837x_to_errno(ret);

		/* The head tag carries the port, so the VLAN-1 tag the CPU
		 * port would otherwise add on egress (DSA keeps the CPU port a
		 * tagged member of every bridge VLAN) is only a third tag in
		 * front of L3 -- one more than the PPE parser can walk over
		 * even with the tag aliased as QinQ. Egress on the CPU port in
		 * the ingress format instead: untagged stays untagged, and a
		 * VLAN-aware bridge's own tags pass through unchanged.
		 */
		ret = rtk_vlan_tagMode_set(gsw->cpu_port,
					   VLAN_EGRESS_TAG_MODE_KEEP_FORMAT);
		if (ret)
			return rtl837x_to_errno(ret);

		/* Must come after the tag is actually being inserted, so the
		 * conduit never advertises an offload that is already wrong.
		 */
		rtl837x_conduit_csum_offload(ds, false);
		break;
	default:
		return -EPROTONOSUPPORT;
	}

	gsw->tag_proto = proto;

	return 0;
}

/* Undo whatever rtl837x_tag_protocol_apply() set up. Caller holds rtnl. */
static void rtl837x_tag_protocol_unapply(struct dsa_switch *ds,
					  enum dsa_tag_protocol proto)
{
	struct rtk_gsw *gsw = ds->priv;
	int ret;

	if (proto == DSA_TAG_PROTO_VSC73XX_8021Q) {
		int port;

		dsa_tag_8021q_unregister(ds);

		/* Unregistering drops tag_8021q's VLANs, but the ports are
		 * still classifying by the PVIDs it installed. Forget that
		 * bookkeeping and put the switch back on its base VLAN layout
		 * (VLAN 1, every port a member, PVID 1), which is what a tagger
		 * carrying identity in its own header needs. Skipping this
		 * leaves the switch applying tag_8021q VLANs to frames that no
		 * longer carry tag_8021q -- measured as a completely dead LAN.
		 */
		for (port = 0; port < RTK_MAX_NUM_OF_PORT; port++)
			gsw->tag8021q_pvid_valid[port] = false;

		ret = rtl837x_seed_vlan_table(gsw);
		if (ret)
			dev_err(ds->dev,
				"failed to restore base VLAN table: %d\n", ret);
		return;
	}

	ret = rtk_cpuTag_enable_set(EXTERNAL_CPU, DISABLED);
	if (ret)
		dev_err(ds->dev, "failed to disable CPU tag: %d\n", ret);

	ret = rtk_vlan_tagMode_set(gsw->cpu_port, VLAN_EGRESS_TAG_MODE_ORIGINAL);
	if (ret)
		dev_err(ds->dev, "failed to restore CPU port tag mode: %d\n", ret);
}

static enum dsa_tag_protocol
rtl837x_get_tag_protocol(struct dsa_switch *ds, int port,
			 enum dsa_tag_protocol mprot)
{
	struct rtk_gsw *gsw = ds->priv;

	/* Called before .setup, so seed the default here rather than depend on
	 * an init ordering. RTL8_4 is the default: the chip's own head tag gives
	 * precise per-port identity under a bridge in either VLAN mode, and the
	 * PPE parser is taught to walk over it (edma_port.c, the S+C alias), so
	 * hardware flow offload is the same as with the 802.1Q-based tagger.
	 * VSC73XX_8021Q stays selectable at runtime through
	 * .change_tag_protocol (/sys/class/net/<conduit>/dsa/tagging; DSA
	 * insists on the conduit and every user port being down, which is also
	 * what lets the conduit's own open/close re-evaluate the parser alias)
	 * as the fallback and for A/B measurements.
	 */
	if (gsw->tag_proto == DSA_TAG_PROTO_NONE)
		gsw->tag_proto = DSA_TAG_PROTO_RTL8_4;

	return gsw->tag_proto;
}

static int rtl837x_change_tag_protocol(struct dsa_switch *ds,
					enum dsa_tag_protocol proto)
{
	struct rtk_gsw *gsw = ds->priv;
	enum dsa_tag_protocol old = gsw->tag_proto;
	int ret;

	if (proto == old)
		return 0;

	gsw->tag_proto_changing = true;
	rtl837x_tag_protocol_unapply(ds, old);
	gsw->tag_proto_changing = false;

	ret = rtl837x_tag_protocol_apply(ds, proto);
	if (ret) {
		/* Leave the switch on a working tagger rather than none. */
		if (rtl837x_tag_protocol_apply(ds, old))
			dev_err(ds->dev,
				"failed to restore tagger %d after switch to %d failed\n",
				old, proto);
		return ret;
	}

	return 0;
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
	struct dsa_port *dp = dsa_to_port(ds, port);
	struct rtk_gsw *gsw = ds->priv;
	typeof(gsw->vlan_table[0]) old_vlan;
	int ret;

	if (!rtl837x_valid_port(gsw, port) || !vid || vid > RTK_VID_MAX)
		return -EINVAL;

	/* Joining a bridge drops the port's standalone VLAN in favour of
	 * the bridge's. Keep it in hardware anyway: the tagger still
	 * addresses this port by its standalone VID for link-local frames
	 * (STP, LLDP, PTP), which have to reach one specific link. Nothing
	 * else uses the VID -- the port's PVID is the bridge VLAN, so
	 * ingress and isolation are unaffected.
	 */
	if (!gsw->tag_proto_changing && dsa_port_bridge_dev_get(dp) &&
	    vid == dsa_tag_8021q_standalone_vid(dp))
		return 0;

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

	ret = rtl837x_rate_disable_all(gsw);
	if (ret)
		dev_warn(gsw->dev,
			 "failed to reset optional egress rate limiters; continuing: %d\n",
			 ret);

	/* Mirror enable and source masks may survive bootloader or module state. */
	rtl837x_mirror_disable_and_clear(gsw, gsw->cpu_port, true, "setup");

	memset(gsw->bridge_dev, 0, sizeof(gsw->bridge_dev));
	gsw->isolated_port_mask = 0;
	memset(gsw->tag8021q_pvid, 0, sizeof(gsw->tag8021q_pvid));
	memset(gsw->tag8021q_pvid_valid, 0, sizeof(gsw->tag8021q_pvid_valid));
	memset(gsw->bridge_pvid, 0, sizeof(gsw->bridge_pvid));
	memset(gsw->bridge_pvid_valid, 0, sizeof(gsw->bridge_pvid_valid));
	rtl837x_mirror_clear_shadow(gsw);
	gsw->rate_egress_mask = 0;
	memset(gsw->rate_egress_cookie, 0, sizeof(gsw->rate_egress_cookie));
	memset(gsw->rate_egress_rate, 0, sizeof(gsw->rate_egress_rate));
	memset(gsw->rate_egress_burst, 0, sizeof(gsw->rate_egress_burst));
	memset(gsw->rate_egress_cleanup_pending, 0,
	       sizeof(gsw->rate_egress_cleanup_pending));

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
	ret = rtl837x_tag_protocol_apply(ds, gsw->tag_proto);
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
	rtl837x_tag_protocol_unapply(ds, gsw->tag_proto);
	rtnl_unlock();

	ret = rtl837x_rate_disable_all(gsw);
	if (ret)
		dev_warn(gsw->dev,
			 "failed to clear egress rate limiter state on teardown: %d\n",
			 ret);
	gsw->rate_egress_mask = 0;
	memset(gsw->rate_egress_cookie, 0, sizeof(gsw->rate_egress_cookie));
	memset(gsw->rate_egress_rate, 0, sizeof(gsw->rate_egress_rate));
	memset(gsw->rate_egress_burst, 0, sizeof(gsw->rate_egress_burst));
	memset(gsw->rate_egress_cleanup_pending, 0,
	       sizeof(gsw->rate_egress_cleanup_pending));
	rtl837x_mirror_disable_and_clear(gsw,
		gsw->mirror_port >= 0 ? gsw->mirror_port : gsw->cpu_port,
		gsw->mirror_ingress, "teardown");

	rtl837x_mdio_teardown(ds);
	rtl837x_mirror_clear_shadow(gsw);
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
	u32 dt_user_ports = 0;
	int port, ret;

	/* Scan only the user ports the device tree declares. A port the chip
	 * has but the board leaves unwired (port 8 on the GL-BE9300) has no
	 * PHY behind it, and every C45 read of the bus scan there times out
	 * after ~250 ms: some 60 of them held DSA setup up for ~15 s.
	 */
	for (port = 0; port < ds->num_ports; port++)
		if (dsa_is_user_port(ds, port))
			dt_user_ports |= BIT(port);
	ds->phys_mii_mask &= dt_user_ports;

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

/* The shared tag_8021q bridge VID is only meaningful while the bridge is
 * VLAN-UNAWARE: there the tagger classifies by it and the switch picks the
 * egress port from its own FDB. Once the bridge becomes VLAN-aware the tagger
 * stops using it entirely (tag_vsc73xx_8021q returns the skb untouched when
 * br_vlan_enabled()), but dsa_tag_8021q_bridge_join() has already made the
 * user port a hardware member of it -- and a member is exactly what ingress
 * filtering lets through. A client on such a port could therefore inject a
 * frame carrying that internal VID and have it flooded inside the bridge VID
 * and delivered to the CPU, where the VBID path attributes it to some port of
 * that bridge. Drop the membership while filtering is on, restore it when it
 * goes off. The CPU port keeps its membership either way -- the tagger needs
 * it for the VLAN-unaware direction.
 */
static int rtl837x_bridge_vid_member(struct rtk_gsw *gsw, int port, bool member)
{
	struct dsa_port *dp = dsa_to_port(&gsw->ds, port);
	typeof(gsw->vlan_table[0]) old_vlan;
	unsigned int bridge_num;
	u16 vid;
	int ret;

	bridge_num = dsa_port_bridge_num_get(dp);
	if (!bridge_num)
		return 0;

	vid = dsa_tag_8021q_bridge_vid(bridge_num);
	if (!vid || vid > RTK_VID_MAX)
		return 0;

	old_vlan = gsw->vlan_table[vid];

	if (member) {
		gsw->vlan_table[vid].valid = 1;
		gsw->vlan_table[vid].vid = vid;
		gsw->vlan_table[vid].mbr |= BIT(port);
		gsw->vlan_table[vid].untag |= BIT(port);
	} else {
		gsw->vlan_table[vid].mbr &= ~BIT(port);
		gsw->vlan_table[vid].untag &= ~BIT(port);
	}

	ret = rtl837x_write_vlan(gsw, vid);
	if (ret)
		gsw->vlan_table[vid] = old_vlan;

	return ret;
}

/* Put a port into the hardware state for one VLAN-filtering mode.
 *
 * The step order is the whole point and differs by direction: each direction
 * gives up a privilege before taking the matching protection away, so no
 * intermediate state is more permissive than both the start and the end state.
 * Rolling back is just applying the other mode, so the ordering is expressed
 * once rather than being re-derived in every error path.
 */
static int rtl837x_apply_vlan_mode(struct rtk_gsw *gsw, int port,
				   bool vlan_filtering)
{
	int ret;

	if (vlan_filtering) {
		/* Stop classifying into the bridge VID, leave it, and only
		 * then start admitting tagged frames.
		 */
		ret = rtl837x_commit_pvid_for_mode(gsw, port, true);
		if (ret)
			return ret;

		ret = rtl837x_bridge_vid_member(gsw, port, false);
		if (ret)
			return ret;

		return rtl837x_set_ingress_policy(gsw, port, true);
	}

	/* Stop admitting tagged frames first, then rejoin the bridge VID and
	 * classify into it again.
	 */
	ret = rtl837x_set_ingress_policy(gsw, port, false);
	if (ret)
		return ret;

	ret = rtl837x_bridge_vid_member(gsw, port, true);
	if (ret)
		return ret;

	return rtl837x_commit_pvid_for_mode(gsw, port, false);
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

	ret = rtl837x_apply_vlan_mode(gsw, port, vlan_filtering);
	if (ret) {
		rollback_ret = rtl837x_apply_vlan_mode(gsw, port, old_vlan_filtering);
		if (rollback_ret)
			dev_err(gsw->dev,
				"failed to restore VLAN mode on port %d: %d\n",
				port, rollback_ret);
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
	.change_tag_protocol = rtl837x_change_tag_protocol,
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
	.cls_flower_add = rtl837x_cls_flower_add,
	.cls_flower_del = rtl837x_cls_flower_del,
	.port_vlan_filtering = rtl837x_port_vlan_filtering,
	.port_vlan_add = rtl837x_port_vlan_add,
	.port_vlan_del = rtl837x_port_vlan_del,
	.port_fdb_add = rtl837x_port_fdb_add,
	.port_fdb_del = rtl837x_port_fdb_del,
	.port_fdb_dump = rtl837x_port_fdb_dump,
	.port_mirror_add = rtl837x_port_mirror_add,
	.port_mirror_del = rtl837x_port_mirror_del,
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
