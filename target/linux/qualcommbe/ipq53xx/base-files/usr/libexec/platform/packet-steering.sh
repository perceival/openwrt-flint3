#!/bin/sh
# Packet steering for the Qualcomm IPQ53xx PPE ports (qcom_ppe / EDMA).
#
# Run the generic steering logic first, so Wi-Fi and everything else is
# configured as usual, then turn RPS back off on the PPE ports.
#
# The PPE hashes received flows over the four EDMA Rx rings and the driver
# pins each ring's interrupt to its own CPU, so those rings already arrive
# spread over the four cores. The generic logic cannot see that: with no
# threaded NAPI on these devices it takes the single-device path and writes
# one single-CPU mask across every rx-* queue of both lan and wan, which
# would funnel the whole datapath back onto one core through the RPS
# backlog. Other devices are left as the generic logic configured them.

steering_flows="$(uci -q get network.@globals[0].steering_flows)"
[ "${steering_flows:-0}" -gt 0 ] && opts="-l $steering_flows"

/usr/libexec/network/packet-steering.uc $opts "$1"

for dev in /sys/class/net/*; do
	[ -d "$dev/device" ] || continue
	case "$(basename "$(readlink -f "$dev/device")")" in
	*.ethernet)
		for q in "$dev"/queues/rx-*; do
			[ -f "$q/rps_cpus" ] && echo 0 > "$q/rps_cpus"
		done
		;;
	esac
done

exit 0
