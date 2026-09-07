#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later

set -eu

readonly RTL837X_FLAGS="learning flood mcast_flood bcast_flood hairpin isolated"

usage()
{
	cat <<'EOF'
Usage:
  bridge-flags.sh --manual BRIDGE PORT_A PORT_B NORMAL_PORT

The ports must already be members of BRIDGE. PORT_A and PORT_B must be
connected to separate external hosts. For the hairpin test, use a true
Ethernet hub/repeater, or a managed downstream switch configured to prevent
the two hosts from forwarding directly to each other. The preferred simple
topology is:

  host A ---\
             HUB ---- PORT_A on Flint
  host B ---/

Do not use an ordinary unmanaged switch: it may switch host A to host B
locally without sending the frame to PORT_A. Before the hairpin check, make
both hosts send traffic toward Flint and confirm in the RTL8373 FDB that both
destination MACs are learned on PORT_A. NORMAL_PORT is used by the isolated-
port test.

The script needs root, ip, bridge, awk, mktemp and a Linux 6.18 DSA driver
with the bridge flags under test. It changes only bridge-port flags. It
does not change bridge membership or VLAN configuration, but snapshots both
and verifies that they are unchanged during cleanup.

The flag readback checks are automatic. Forwarding checks are deliberately
interactive because they need physical peers and packet observation:

  - learning: a fresh source must not/become dynamically learned;
  - flood: an unknown unicast must not/may reach PORT_B;
  - bcast_flood: broadcast must not/may reach PORT_B;
  - mcast_flood: an unknown multicast must not/may reach PORT_B;
  - hairpin: after the destination is learned on PORT_A, ingress PORT_A ->
    egress PORT_A must be blocked when hairpin is off and allowed when it is
    on;
  - isolated: isolated PORT_A <-> PORT_B is blocked in both directions, CPU
    reachability stays, and isolated -> NORMAL_PORT remains allowed by the
    bridge topology.

Use tcpdump, ping, iperf3 or another suitable external traffic generator to
perform the packet checks, then enter pass, fail or skip at each prompt.
Skipped hardware checks make the script exit with status 77, not success.
EOF
}

die()
{
	echo "rtl837x bridge-flags test: $*" >&2
	exit 1
}

flag_state()
{
	dev="$1"
	flag="$2"

	bridge link show dev "$dev" | awk -v flag="$flag" '
		{
			for (i = 1; i < NF; i++)
				if ($i == flag) {
					print $(i + 1)
					exit
				}
		}'
}

set_flag()
{
	bridge link set dev "$1" "$2" "$3"
}

assert_flag()
{
	dev="$1"
	flag="$2"
	expected="$3"
	actual="$(flag_state "$dev" "$flag")" ||
		die "cannot read $flag state on $dev"

	[ "$actual" = "$expected" ] ||
		die "$dev $flag readback is '$actual', expected '$expected'"
}

original_state()
{
	dev="$1"
	flag="$2"

	awk -v dev="$dev" -v flag="$flag" \
		'$1 == dev && $2 == flag { print $3; exit }' "$state_snapshot"
}

record_state()
{
	dev="$1"
	flag="$2"
	state="$(flag_state "$dev" "$flag")" ||
		die "cannot read $flag state on $dev"

	case "$state" in
	on|off)
		printf '%s %s %s\n' "$dev" "$flag" "$state" >> "$state_snapshot"
		;;
	*)
		die "unexpected $flag state '$state' on $dev"
		;;
	esac
}

restore_flags()
{
	status=0

	while IFS=' ' read -r dev flag state; do
		if ! set_flag "$dev" "$flag" "$state"; then
			echo "failed to restore $dev $flag to $state" >&2
			status=1
		fi
	done < "$state_snapshot"

	return "$status"
}

snapshot_membership()
{
	output="$1"
	line="$(bridge link show master "$bridge_name")" || return 1
	printf '%s\n' "$line" | awk '
		$1 ~ /^[0-9]+:$/ {
			dev = $2
			sub(/:.*/, "", dev)
			sub(/@.*/, "", dev)
			print dev
		}' | sort -u > "$output"
}

snapshot_vlans()
{
	output="$1"
	: > "$output"

	for dev in "$port_a" "$port_b" "$port_normal"; do
		printf 'device %s\n' "$dev" >> "$output"
		bridge vlan show dev "$dev" >> "$output" || return 1
	done
}

cleanup()
{
	status=$?
	trap - EXIT HUP INT TERM

	if [ -f "$state_snapshot" ] && ! restore_flags; then
		status=1
	fi

	if [ -f "$membership_snapshot" ]; then
		if ! snapshot_membership "$membership_current"; then
			echo "failed to verify bridge membership during cleanup" >&2
			status=1
		elif ! cmp -s "$membership_snapshot" "$membership_current"; then
			echo "bridge membership changed during the test; refusing to claim a clean \
run" >&2
			status=1
		fi
	fi

	if [ -f "$vlan_snapshot" ]; then
		if ! snapshot_vlans "$vlan_current"; then
			echo "failed to verify VLAN state during cleanup" >&2
			status=1
		elif ! cmp -s "$vlan_snapshot" "$vlan_current"; then
			echo "VLAN state changed during the test; refusing to claim a clean run" >&2
			status=1
		fi
	fi

	rm -rf "$test_dir"
	exit "$status"
}

test_flag_readback()
{
	dev="$1"
	flag="$2"
	original="$(original_state "$dev" "$flag")"

	printf 'readback: %s %s\n' "$dev" "$flag"
	for state in off on; do
		set_flag "$dev" "$flag" "$state"
		assert_flag "$dev" "$flag" "$state"
	done

	set_flag "$dev" "$flag" "$original"
	assert_flag "$dev" "$flag" "$original"
}

restore_saved_flag()
{
	dev="$1"
	flag="$2"
	state="$(original_state "$dev" "$flag")"

	set_flag "$dev" "$flag" "$state"
	assert_flag "$dev" "$flag" "$state"
}

manual_check()
{
	title="$1"
	details="$2"
	expected="$3"

	printf '\n[%s]\n%s\nExpected: %s\n' "$title" "$details" "$expected"
	while :; do
		printf 'Result [pass/fail/skip]: '
		IFS= read -r answer || die "input ended before '$title' was answered"
		case "$answer" in
		pass)
			return 0
			;;
		fail)
			die "manual check failed: $title"
			;;
		skip)
			manual_incomplete=1
			echo "SKIPPED: $title" >&2
			return 0
			;;
		*)
			echo "enter pass, fail or skip" >&2
			;;
		esac
	done
}

if [ "${1:-}" = "--help" ] || [ "${1:-}" = "-h" ]; then
	usage
	exit 0
fi

[ "${1:-}" = "--manual" ] || {
	usage >&2
	exit 2
}
shift

[ "$#" -eq 4 ] || {
	usage >&2
	exit 2
}

[ "$(id -u)" -eq 0 ] || die "run as root"

command -v awk >/dev/null 2>&1 || die "awk is required"
command -v bridge >/dev/null 2>&1 || die "bridge is required"
command -v cmp >/dev/null 2>&1 || die "cmp is required"
command -v ip >/dev/null 2>&1 || die "ip is required"
command -v mktemp >/dev/null 2>&1 || die "mktemp is required"
command -v sort >/dev/null 2>&1 || die "sort is required"

bridge_name="$1"
port_a="$2"
port_b="$3"
port_normal="$4"

[ "$port_a" != "$port_b" ] || die "PORT_A and PORT_B must differ"
[ "$port_a" != "$port_normal" ] || die "PORT_A and NORMAL_PORT must differ"
[ "$port_b" != "$port_normal" ] || die "PORT_B and NORMAL_PORT must differ"

ip link show dev "$bridge_name" >/dev/null 2>&1 ||
	die "bridge device '$bridge_name' does not exist"

test_dir="$(mktemp -d /tmp/rtl837x-bridge-flags.XXXXXX)" ||
	die "cannot create temporary directory"
state_snapshot="$test_dir/flags"
membership_snapshot="$test_dir/membership"
membership_current="$test_dir/membership.current"
vlan_snapshot="$test_dir/vlans"
vlan_current="$test_dir/vlans.current"
manual_incomplete=0

trap cleanup EXIT HUP INT TERM

snapshot_membership "$membership_snapshot" ||
	die "cannot inspect bridge membership"
snapshot_vlans "$vlan_snapshot" ||
	die "cannot inspect VLAN state"

for dev in "$port_a" "$port_b" "$port_normal"; do
	line="$(bridge link show dev "$dev")" || die "cannot inspect $dev"
	master="$(printf '%s\n' "$line" | awk '
		{
			for (i = 1; i < NF; i++)
				if ($i == "master") {
					print $(i + 1)
					exit
				}
		}')"
	[ "$master" = "$bridge_name" ] ||
		die "$dev is not a member of $bridge_name"

	for flag in $RTL837X_FLAGS; do
		record_state "$dev" "$flag"
	done
done

echo "Running bridge flag configuration/readback checks..."
for dev in "$port_a" "$port_b" "$port_normal"; do
	for flag in $RTL837X_FLAGS; do
		test_flag_readback "$dev" "$flag"
	done
done

echo "Running interactive forwarding checks; use physical external peers."

set_flag "$port_a" learning off
manual_check "learning off" \
	"Send a frame with a fresh source MAC from the host connected to $port_a. \
	Inspect 'bridge fdb show br $bridge_name' (or the switch FDB) without \
	generating traffic that would make the same source known first." \
	"the fresh source is not dynamically learned"
set_flag "$port_a" learning on
manual_check "learning on" \
	"Repeat the same fresh-source test from the host connected to $port_a and \
	inspect the FDB after the frame arrives." \
	"the fresh source is dynamically learned"
restore_saved_flag "$port_a" learning

set_flag "$port_b" flood off
manual_check "unknown-unicast flood off" \
	"Send an Ethernet frame from the host on $port_a to a destination MAC that \
	is absent from the FDB. Observe the host on $port_b with tcpdump." \
	"the frame is not delivered to $port_b"
set_flag "$port_b" flood on
manual_check "unknown-unicast flood on" \
	"Repeat the same unknown-unicast test with the destination port flood flag enabled." \
	"delivery to $port_b is allowed by the bridge topology"
restore_saved_flag "$port_b" flood

set_flag "$port_b" bcast_flood off
manual_check "broadcast flood off" \
	"Send a broadcast frame from the host on $port_a and observe the host on $port_b." \
	"the broadcast is not delivered to $port_b"
set_flag "$port_b" bcast_flood on
manual_check "broadcast flood on" \
	"Repeat the broadcast test with broadcast flooding enabled." \
	"delivery to $port_b is allowed by the bridge topology"
restore_saved_flag "$port_b" bcast_flood

set_flag "$port_b" mcast_flood off
manual_check "unknown-multicast flood off" \
	"Send a raw L2 frame with destination MAC 01:00:5e:7f:01:23 and non-IP \
	EtherType 0x88b5 from the host on $port_a. Do not create an MDB entry, \
	send IGMP/MLD, or use an IP multicast tool. Observe the host on $port_b." \
	"the multicast is not delivered to $port_b"
set_flag "$port_b" mcast_flood on
manual_check "unknown-multicast flood on" \
	"Repeat the same controlled raw L2 multicast test with multicast flooding \
	enabled. Do not create an MDB entry or send IGMP/MLD; this validates only \
	BR_MCAST_FLOOD, not MDB or IGMP/MLD offload." \
	"delivery to $port_b is allowed by the bridge topology"
restore_saved_flag "$port_b" mcast_flood

set_flag "$port_a" hairpin off
manual_check "hairpin off" \
	"Using the required hub/repeater or isolated managed downstream switch, \
	first confirm both host MACs are learned on PORT_A in the RTL8373 FDB. Then \
	send a unicast frame from one host to the other and observe PORT_A for \
	reflected traffic." \
	"same-port reflection is blocked"
set_flag "$port_a" hairpin on
manual_check "hairpin on" \
	"Repeat the same learned-destination unicast test with hairpin enabled on \
	$port_a; verify the downstream topology cannot deliver the frame locally." \
	"same-port reflection is allowed"
restore_saved_flag "$port_a" hairpin

set_flag "$port_a" isolated on
set_flag "$port_b" isolated on
set_flag "$port_normal" isolated off
manual_check "isolated ports do not forward to each other" \
	"Send traffic from the host on $port_a to the host on $port_b while both ports \
	are isolated." \
	"$port_a to $port_b is blocked"
manual_check "isolated ports block the reverse direction" \
	"Send traffic from the host on $port_b to the host on $port_a while both ports \
	are isolated." \
	"$port_b to $port_a is blocked"
manual_check "isolated ports keep CPU reachability" \
	"From hosts on $port_a and $port_b, reach the router/CPU through \
	$bridge_name (for example with ping) while both ports remain isolated." \
	"CPU/router reachability remains possible"
manual_check "isolated to normal forwarding" \
	"Send traffic from the host on $port_a to the host on $port_normal with \
	the normal port's bridge rules otherwise permitting forwarding." \
	"$port_a to $port_normal remains allowed"
restore_saved_flag "$port_a" isolated
restore_saved_flag "$port_b" isolated
restore_saved_flag "$port_normal" isolated

if [ "$manual_incomplete" -ne 0 ]; then
	echo "Forwarding checks were incomplete; returning status 77." >&2
	exit 77
fi

echo "All bridge flag readback and interactive forwarding checks passed."
