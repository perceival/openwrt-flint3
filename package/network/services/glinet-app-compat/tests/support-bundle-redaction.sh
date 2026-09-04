#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only

set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
support_script="$script_dir/../files/usr/libexec/glinet-app-compat-support"
test_dir=$(mktemp -d /tmp/glinet-app-compat-redaction.XXXXXX)
existing_output="/tmp/glinet-app-compat-redaction-existing.$$"
symlink_output="/tmp/glinet-app-compat-redaction-link.$$"
trap 'rm -rf "$test_dir" "$existing_output" "$symlink_output"' EXIT HUP INT TERM

bin_dir="$test_dir/bin"
mkdir "$bin_dir"

make_stub()
{
	name="$1"
	body="$2"

	printf '%s\n' '#!/bin/sh' "$body" > "$bin_dir/$name"
	chmod 755 "$bin_dir/$name"
}

make_stub uci 'printf "%s\n" \
	"wireless.@wifi-iface[0].key=synthetic-wifi-key" \
	"network.wan.password=synthetic-password" \
	"firewall.@redirect[0]=redirect" \
	"firewall.@redirect[0].token=synthetic-token"'
make_stub ubus 'printf "%s\n" \
	"sid=synthetic-sid" \
	"nonce=synthetic-nonce" \
	"Admin-Token=synthetic-admin-token" \
	"private_key=synthetic-vpn-private-key" \
	"shadow_hash=synthetic-shadow-hash"'
make_stub logread 'printf "%s\n" \
	"glinet-app-compat: sid=synthetic-sid" \
	"nonce=synthetic-nonce" \
	"Admin-Token=synthetic-admin-token" \
	"private_key=synthetic-vpn-private-key" \
	"wifi_key=synthetic-wifi-key"'
make_stub opkg 'printf "%s\n" "Package: glinet-app-compat" "Version: 1.0"'
make_stub uname 'printf "%s\n" "Linux test-router 6.12-test"'
make_stub service 'printf "%s\n" "firewall reload/status: ok"'

output=$(PATH="$bin_dir:$PATH" sh "$support_script" -)

printf '%s\n' 'do-not-overwrite' > "$existing_output"
ln -s "$existing_output" "$symlink_output"
escape_output='/tmp/../etc/glinet-app-compat-escape'

if PATH="$bin_dir:$PATH" sh "$support_script" "$escape_output" >/dev/null 2>&1; then
	printf 'path traversal was accepted\n' >&2
	exit 1
fi

if PATH="$bin_dir:$PATH" sh "$support_script" "$symlink_output" >/dev/null 2>&1; then
	printf 'symlink output was accepted\n' >&2
	exit 1
fi

case "$(cat "$existing_output")" in
	'do-not-overwrite') ;;
	*)
		printf 'existing output was modified\n' >&2
		exit 1
		;;
esac

for secret in \
	synthetic-wifi-key \
	synthetic-password \
	synthetic-token \
	synthetic-sid \
	synthetic-nonce \
	synthetic-admin-token \
	synthetic-vpn-private-key \
	synthetic-shadow-hash; do
	case "$output" in
		*"$secret"*)
			printf 'secret leaked: %s\n' "$secret" >&2
			exit 1
			;;
	esac
done

case "$output" in
	*'<redacted>'*) ;;
	*)
		printf 'no redaction marker found\n' >&2
		exit 1
		;;
esac

printf 'support bundle redaction: PASS\n'
