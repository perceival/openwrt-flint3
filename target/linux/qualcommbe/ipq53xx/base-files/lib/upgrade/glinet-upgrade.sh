. /lib/upgrade/common.sh

# Kept free in RAM on top of what an upgrade stages in /tmp, for the tools doing
# the upgrade and whatever is still running.
GLINET_RAM_MARGIN_KB=32768

# /tmp is RAM. Say no before staging anything that does not fit, rather than
# fail or get something killed halfway. An input image in /tmp is already gone
# from MemAvailable and from the free space of /tmp, so only new bytes count.
# A failed df is no reason to refuse: extraction checks its own result.
glinet_emmc_fits_ram() {
	local bytes="$1"
	local what="$2"
	local need_kb=$(( (bytes + 1023) / 1024 ))
	local avail_kb tmp_kb

	avail_kb=$(awk '$1 == "MemAvailable:" { print $2 }' /proc/meminfo)
	[ "${avail_kb:-0}" -ge $((need_kb + GLINET_RAM_MARGIN_KB)) ] || {
		v "Not enough free RAM for $what: $need_kb KiB + $GLINET_RAM_MARGIN_KB KiB margin needed, ${avail_kb:-0} KiB available."
		return 1
	}

	tmp_kb=$(df -k /tmp 2>/dev/null | awk 'NR == 2 { print $4 }')
	[ -z "$tmp_kb" ] || [ "$tmp_kb" -ge "$need_kb" ] || {
		v "Not enough room in /tmp for $what: $need_kb KiB needed, $tmp_kb KiB free."
		return 1
	}
}

glinet_get_fit_part() {
	local image="$1"
	local name="$2"

	dumpimage -l "$image" 2>/dev/null | \
		sed -n "s/^[[:space:]]*Image \([0-9][0-9]*\) ($name)$/\1/p"
}

# A sysupgrade tar is written by emmc_upgrade_tar(), which dd's its kernel
# into 0:HLOS without any check. Refuse, before anything is written, a tar
# that would leave the unit unbootable: no kernel (the old kernel would boot
# the new rootfs and its modules), a kernel larger than 0:HLOS (truncated),
# or a kernel FIT without the "config-1" configuration stock U-Boot asks for
# ("Config not available", e.g. an image built from an older tree).
glinet_emmc_check_tar() {
	local image="$1"
	local gz board_dir kernel size limit hlos_dev ret=1

	command -v dumpimage >/dev/null || {
		v "dumpimage is missing, so the kernel cannot be checked for FIT configuration \"config-1\"."
		return 1
	}

	[ "$(identify_magic_long "$(get_magic_long "$image" cat)")" = "gzip" ] && gz=z
	board_dir=$(tar t${gz}f "$image" 2>/dev/null | grep -m 1 '^sysupgrade-.*/$')
	board_dir=${board_dir%/}
	[ -n "$board_dir" ] || {
		v "Not a sysupgrade image or a factory/stock FIT image."
		return 1
	}

	# Measured without storing it: it may be too large to store.
	size=$(tar x${gz}f "$image" "$board_dir/kernel" -O 2>/dev/null | wc -c)
	hlos_dev=$(find_mmc_part "0:HLOS")
	limit=$(( $(cat "/sys/class/block/${hlos_dev##*/}/size" 2>/dev/null || echo 0) * 512 ))

	if [ "${size:-0}" -eq 0 ]; then
		v "The sysupgrade image has no kernel."
	elif [ -z "$hlos_dev" ] || [ "$size" -gt "$limit" ]; then
		v "The kernel ($size bytes) does not fit 0:HLOS ($limit bytes)."
	elif ! glinet_emmc_fits_ram "$size" "the kernel"; then
		:
	elif ! kernel=$(mktemp /tmp/glinet-check-kernel.XXXXXX); then
		v "Unable to create a temporary file in /tmp."
	else
		tar x${gz}f "$image" "$board_dir/kernel" -O > "$kernel" 2>/dev/null
		if dumpimage -l "$kernel" 2>/dev/null | grep -q "^ *Configuration [0-9]* (config-1)$"; then
			ret=0
		else
			v "The kernel has no FIT configuration \"config-1\"; stock U-Boot cannot boot it."
		fi
		rm -f "$kernel"
	fi

	return $ret
}

glinet_emmc_check_image() {
	[ "$(identify_magic_long "$(get_magic_long "$1")")" != "fit" ] && {
		glinet_emmc_check_tar "$1"
		return
	}

	local hlos=$(glinet_get_fit_part "$1" hlos)
	local rootfs=$(glinet_get_fit_part "$1" rootfs)
	[ -n "$hlos" ] && [ -n "$rootfs" ] || {
		v "The factory image must contain hlos and rootfs sections."
		return 1
	}

	# The FIT path writes whole partitions and has nowhere to put a config
	# backup (emmc_copy_config only works after the sysupgrade-tar path), so
	# "keep settings" would be dropped silently. Say so up front.
	type notify_firmware_no_backup >/dev/null 2>&1 && notify_firmware_no_backup
	[ "$SAVE_CONFIG" = 1 ] && {
		v "Factory and stock images cannot keep settings; use sysupgrade -n."
		return 1
	}

	# Every section is inside the image, so its size bounds their sum.
	glinet_emmc_fits_ram "$(wc -c < "$1")" "the image sections"
}

glinet_emmc_staged() {
	echo "$GLINET_STAGE_DIR/$1.bin"
}

# Extract one FIT section to /tmp and check that it fits its partition.
# Nothing on the eMMC is touched here.
glinet_emmc_stage_fit_part() {
	local image="$1"
	local part="$2"
	local device="$3"
	local name="$4"
	local output="$(glinet_emmc_staged "$name")"
	local size limit

	v "Extracting $name"
	rm -f "$output"
	dumpimage -T flat_dt -p "$part" -o "$output" "$image" >/dev/null 2>&1 &&
		[ -s "$output" ] || {
		v "Unable to extract $name from the image."
		return 1
	}

	size=$(wc -c < "$output")
	limit=$(( $(cat "/sys/class/block/${device##*/}/size" 2>/dev/null || echo 0) * 512 ))
	[ "$size" -le "$limit" ] || {
		v "$name ($size bytes) does not fit $device ($limit bytes)."
		return 1
	}
}

# Compare the start of a partition with the hash of what was written there.
# conv=fsync only says that the flush was accepted, and a plain read would be
# answered from the page cache dd has just filled: sync and drop it first.
glinet_emmc_verify() {
	local want="$1"
	local device="$2"
	local size="$3"
	local got

	sync
	{ echo 3 > /proc/sys/vm/drop_caches; } 2>/dev/null

	got=$( { dd bs=1048576 count=$((size / 1048576))
		dd bs=512 count=$((size % 1048576 / 512))
		dd bs=1 count=$((size % 512)); } < "$device" 2>/dev/null | sha256sum)
	[ -n "$want" ] && [ "$want" = "$got" ]
}

glinet_emmc_write_staged() {
	local name="$1"
	local device="$2"
	local staged="$(glinet_emmc_staged "$name")"
	local want="$(sha256sum < "$staged")"
	local size="$(wc -c < "$staged")"
	local try

	for try in 1 2; do
		v "Writing $name to $device"
		dd if="$staged" of="$device" bs=1M conv=fsync &&
			glinet_emmc_verify "$want" "$device" "$size" && return 0
		v "$name does not read back correctly from $device."
	done

	v "Giving up on $name. The eMMC may be failing; use the U-Boot recovery."
	return 1
}

# emmc_upgrade_tar() writes the kernel with no check. Nothing is staged on this
# path, so the image itself is what the partition is compared with.
glinet_emmc_verify_tar_kernel() {
	local image="$1"
	local hlos_dev="$(find_mmc_part "$CI_KERNPART" "$CI_ROOTDEV")"
	local gz board_dir want size

	[ "$(identify_magic_long "$(get_magic_long "$image" cat)")" = "gzip" ] && gz=z
	board_dir=$(tar t${gz}f "$image" 2>/dev/null | grep -m 1 '^sysupgrade-.*/$')
	board_dir=${board_dir%/}

	size=$(tar x${gz}f "$image" "$board_dir/kernel" -O 2>/dev/null | wc -c)
	[ "$size" -gt 0 ] || return 0

	want=$(tar x${gz}f "$image" "$board_dir/kernel" -O 2>/dev/null | sha256sum)
	glinet_emmc_verify "$want" "$hlos_dev" "$size" && return 0

	v "The kernel does not read back correctly from $hlos_dev, writing it again."
	tar x${gz}f "$image" "$board_dir/kernel" -O 2>/dev/null |
		dd of="$hlos_dev" bs=1M conv=fsync
	glinet_emmc_verify "$want" "$hlos_dev" "$size" && return 0

	v "Giving up on the kernel. The eMMC may be failing; use the U-Boot recovery."
	return 1
}

glinet_emmc_cleanup_staged() {
	rm -rf "$GLINET_STAGE_DIR"
}

glinet_emmc_do_fit_upgrade() {
	local image="$1"
	local hlos=$(glinet_get_fit_part "$image" hlos)
	local rootfs=$(glinet_get_fit_part "$image" rootfs)
	local wifi_fw=$(glinet_get_fit_part "$image" wifi_fw)
	local hlos_dev=$(find_mmc_part "$CI_KERNPART" "$CI_ROOTDEV")
	local rootfs_dev=$(find_mmc_part "$CI_ROOTPART" "$CI_ROOTDEV")
	local wifi_fw_dev
	local rootfs_size

	[ -n "$wifi_fw" ] && wifi_fw_dev=$(find_mmc_part "0:WIFIFW" "$CI_ROOTDEV")
	command -v sha256sum >/dev/null || {
		v "sha256sum is missing, so nothing written could be verified."
		return 1
	}

	[ -n "$hlos" ] && [ -n "$rootfs" ] && \
		[ -n "$hlos_dev" ] && [ -n "$rootfs_dev" ] || {
		v "Unable to find the required GL.iNet image sections or eMMC partitions."
		return 1
	}
	[ -z "$wifi_fw" ] || [ -n "$wifi_fw_dev" ] || {
		v "Unable to find the GL.iNet Wi-Fi firmware partition."
		return 1
	}

	# Checked here too: sysupgrade -F goes past the image check.
	glinet_emmc_fits_ram "$(wc -c < "$image")" "the image sections" || return 1

	GLINET_STAGE_DIR=$(mktemp -d /tmp/glinet-emmc.XXXXXX) || {
		v "Unable to create a temporary directory in /tmp."
		return 1
	}

	# Extract and size-check every section first: a failure here (a bad
	# image, /tmp running out of space) leaves the installed system intact.
	glinet_emmc_stage_fit_part "$image" "$hlos" "$hlos_dev" hlos &&
	glinet_emmc_stage_fit_part "$image" "$rootfs" "$rootfs_dev" rootfs &&
	{ [ -z "$wifi_fw" ] ||
		glinet_emmc_stage_fit_part "$image" "$wifi_fw" "$wifi_fw_dev" wifi_fw; } || {
		glinet_emmc_cleanup_staged
		return 1
	}

	# Point of no return. Invalidate the kernel first and write it last, so
	# an interrupted upgrade cannot boot a kernel against a partial rootfs.
	dd if=/dev/zero of="$hlos_dev" bs=512 count=8 conv=fsync || return 1
	glinet_emmc_write_staged rootfs "$rootfs_dev" || return 1

	# The firmware just written looks for its overlay right behind its
	# squashfs, at roundup(size, 64 KiB). Clear that block so neither an
	# old stock overlay nor our previous f2fs is mounted instead of a fresh
	# one (stock's own writer zeroes rootfs + 20 MiB for the same reason).
	# OpenWrt factory images are padded past that point already.
	rootfs_size=$(wc -c < "$(glinet_emmc_staged rootfs)")
	dd if=/dev/zero of="$rootfs_dev" bs=65536 count=1 conv=fsync \
		seek=$(( (rootfs_size + 65535) / 65536 )) 2>/dev/null

	[ -z "$wifi_fw" ] || \
		glinet_emmc_write_staged wifi_fw "$wifi_fw_dev" || return 1
	glinet_emmc_write_staged hlos "$hlos_dev" || return 1
	glinet_emmc_cleanup_staged
}

glinet_emmc_do_upgrade() {
	case "$(identify_magic_long "$(get_magic_long "$1")")" in
	fit)
		glinet_emmc_do_fit_upgrade "$1"
		;;
	*)
		command -v sha256sum >/dev/null || {
			v "sha256sum is missing, so nothing written could be verified."
			return 1
		}
		emmc_do_upgrade "$1" && glinet_emmc_verify_tar_kernel "$1"
		;;
	esac
}
