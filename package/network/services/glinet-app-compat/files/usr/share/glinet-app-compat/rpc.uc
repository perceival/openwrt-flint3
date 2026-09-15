{%
// SPDX-License-Identifier: GPL-2.0-only

'use strict';

import * as digest from 'digest';
import * as fs from 'fs';
import * as libubus from 'ubus';
import * as log from 'log';
import * as uci from 'uci';

const STATE_DIR = '/var/run/glinet-app-compat';
const SESSION_TTL = 300;
/* SDK4's local challenge nonce is valid for one second. */
const CHALLENGE_TTL = 1;
const AUTH_MAX_FAILURES = 10;
const AUTH_BLOCK_TIME = 600;
const LOG_SUPPRESSION_TIME = 60;
const MAX_BODY_LENGTH = 64 * 1024;
const STATE_CLEANUP_INTERVAL = 60 * 1000;
const LOG_STATE_TTL = 60 * 60;

let ubus;
let rpc_enabled = true;
let rpc_debug_enabled = false;
let last_state_cleanup;

function monotonic_millis()
{
	let value = clock(true);
	return value ? value[0] * 1000 + value[1] / 1000 : time() * 1000;
}

function bool_value(value, fallback)
{
	if (value == null)
		return fallback;

	return value == true || value == 1 || value == '1' || value == 'true';
}

function config_section(package_name, section_name)
{
	try {
		return uci.cursor().get_all(package_name, section_name) ?? {};
	} catch (e) {
		return {};
	}
}

function init_logging()
{
	let config = config_section('glinet-app-compat', 'main');
	let level = config.log_level ?? 'info';

	if (type(level) != 'string' || !match(level, /^(debug|info|notice|warning|err)$/))
		level = 'info';

	log.ulog_open(['syslog'], 'daemon', 'glinet-app-compat');
	log.ulog_threshold(level);
	rpc_enabled = bool_value(config.enabled, true);
	rpc_debug_enabled = level == 'debug';
}

function state_file(prefix, key)
{
	return `${STATE_DIR}/${prefix}.${digest.md5(key)}`;
}

function read_state(path)
{
	let stat = fs.stat(path);

	if (!stat || stat.type != 'file' || stat.uid != 0 || stat.gid != 0 ||
	    stat.mode != 0o600)
		return null;

	let data = fs.readfile(path, 8192);
	if (data == null)
		return null;

	try {
		return json(data);
	} catch (e) {
		fs.unlink(path);
		return null;
	}
}

function remove_state(path)
{
	fs.unlink(path);
}

function cleanup_expired_state()
{
	let now = monotonic_millis();
	if (last_state_cleanup != null &&
	    now - last_state_cleanup < STATE_CLEANUP_INTERVAL)
		return;

	last_state_cleanup = now;
	let wall = time();

	for (let path in fs.glob(`${STATE_DIR}/session.*`) ?? []) {
		let state = read_state(path);
		if (!state || state.last == null || now < +state.last ||
		    now - +state.last > SESSION_TTL * 1000)
			remove_state(path);
	}

	for (let path in fs.glob(`${STATE_DIR}/challenge.*`) ?? []) {
		let state = read_state(path);
		if (!state || state.created == null || now < +state.created ||
		    now - +state.created > CHALLENGE_TTL * 1000)
			remove_state(path);
	}

	for (let path in fs.glob(`${STATE_DIR}/log.*`) ?? []) {
		let state = read_state(path);
		if (!state || state.last == null || wall < +state.last ||
		    wall - +state.last > LOG_STATE_TTL)
			remove_state(path);
	}

	for (let path in fs.glob(`${STATE_DIR}/failure.*`) ?? []) {
		let state = read_state(path);
		if (!state)
			remove_state(path);
		else if (!(+state.until || 0) > wall && state.updated != null &&
			 wall - +state.updated > AUTH_BLOCK_TIME)
			remove_state(path);
	}
}

function state_directory_ready()
{
	let stat = fs.stat(STATE_DIR);

	if (!stat)
		stat = fs.mkdir(STATE_DIR, 0o700) ? fs.stat(STATE_DIR) : null;

	if (!stat || stat.type != 'directory' || stat.uid != 0 || stat.gid != 0)
		return false;

	if (stat.mode != 0o700 && !fs.chmod(STATE_DIR, 0o700))
		return false;

	cleanup_expired_state();
	return true;
}

function write_state(path, value)
{
	if (!state_directory_ready())
		return false;

	if (!fs.writefile(path, sprintf('%.J\n', value)))
		return false;

	return !!fs.chmod(path, 0o600);
}

function log_once(priority, key, message)
{
	let now = time();

	if (state_directory_ready()) {
		let path = state_file('log', key);
		let previous = read_state(path);

		if (previous && now - (+previous.last || 0) < LOG_SUPPRESSION_TIME)
			return;

		write_state(path, { last: now });
	}

	log.ulog(priority, `glinet-app-compat: ${message}`);
}

function argument_summary(args)
{
	if (args == null)
		return 'null';

	if (type(args) == 'array')
		return `array(${length(args)})`;

	if (type(args) != 'object')
		return type(args);

	let names = [];
	for (let name in args) {
		if (length(names) >= 16)
			break;

		if (type(name) == 'string' &&
		    match(name, /^[A-Za-z0-9_.-]{1,64}$/))
			push(names, name);
	}

	return length(names) ? join(',', names) : 'object';
}

function log_call_debug(module, method, args, remote, auth, started, result)
{
	if (!rpc_debug_enabled)
		return;

	log_once('debug', `call:${module}.${method}`,
		`rpc module=${module} method=${method} args=[${argument_summary(args)}] ` +
		`source=${remote} auth=${auth} result=${result} ` +
		`duration_ms=${monotonic_millis() - started}`);
}

function backend_call(object, method, args, optional)
{
	if (!ubus) {
		log_once(optional ? 'info' : 'err', `backend:${object}.${method}`,
			`ubus unavailable for ${object}.${method}`);
		return null;
	}

	let result;
	try {
		result = ubus.call(object, method, args ?? {});
	} catch (e) {
		log_once(optional ? 'info' : 'err', `backend:${object}.${method}`,
			`ubus ${object}.${method} raised an exception`);
		return null;
	}

	let error = libubus.error(true);
	if (error != null) {
		log_once(optional ? 'info' : 'err', `backend:${object}.${method}`,
			`ubus ${object}.${method} failed with status ${error}`);
		return null;
	}

	return result;
}

function random_hex(bytes)
{
	let file;
	let data;

	try {
		file = fs.open('/dev/urandom', 'r');
		if (!file)
			return null;

		data = file.read(bytes);
		file.close();
	} catch (e) {
		if (file)
			file.close();
		return null;
	}

	if (!data || length(data) != bytes)
		return null;

	return join('', map(split(data, ''), (value) => sprintf('%02x', ord(value))));
}

function normalize_mac(value)
{
	if (value == null)
		return null;

	let mac = lc(trim(`${value}`));
	return match(mac, /^[0-9a-f]{2}(:[0-9a-f]{2}){5}$/) ? mac : null;
}

function normalize_ip(value)
{
	if (type(value) != 'string' || length(value) > 64 ||
	    !match(value, /^[0-9A-Fa-f:.]+$/))
		return null;

	return value;
}

function normalize_hostname(value)
{
	if (type(value) != 'string' || length(value) > 253 ||
	    !match(value, /^[A-Za-z0-9_.:-]+$/))
		return null;

	return value;
}

function safe_ifname(value)
{
	return value && match(`${value}`, /^[A-Za-z0-9_.:@-]+$/) ? `${value}` : null;
}

function rpc_label(value)
{
	return type(value) == 'string' && length(value) <= 64 &&
		match(value, /^[A-Za-z0-9_.-]+$/) ? value : '<invalid>';
}

function read_factory_value(name)
{
	let value = fs.readfile(`/proc/gl-hw-info/${name}`, 256);
	return value == null ? null : trim(value);
}

function read_net_mac(ifname)
{
	ifname = safe_ifname(ifname);
	if (!ifname)
		return null;

	return normalize_mac(fs.readfile(`/sys/class/net/${ifname}/address`, 64));
}

function ipv4_number(address)
{
	if (type(address) != 'string')
		return null;

	let parts = split(address, '.');
	if (length(parts) != 4)
		return null;

	let result = 0;
	for (let part in parts) {
		if (!match(part, /^[0-9]{1,3}$/))
			return null;

		let value = +part;
		if (value > 255)
			return null;

		result = result * 256 + value;
	}

	return result;
}

function ipv4_prefix_mask(prefix)
{
	if (prefix == null || prefix == '')
		return null;

	prefix = +prefix;
	if (prefix < 0 || prefix > 32)
		return null;

	return prefix == 0 ? 0 : 0xffffffff << (32 - prefix);
}

function ipv4_mask_string(prefix)
{
	let mask = ipv4_prefix_mask(prefix);
	if (mask == null)
		return null;

	return `${(mask >> 24) & 255}.${(mask >> 16) & 255}.${(mask >> 8) & 255}.${mask & 255}`;
}

function ipv6_link_local(address)
{
	if (type(address) != 'string' || length(address) > 39 ||
	    !match(address, /^[0-9A-Fa-f:]+$/) ||
	    !match(lc(address), /^fe[89ab][0-9a-f]:/))
		return false;

	let compressed = split(address, '::');
	if (length(compressed) > 2)
		return false;

	let groups = 0;
	for (let part in compressed) {
		if (!length(part))
			continue;

		for (let group in split(part, ':')) {
			if (!length(group) || length(group) > 4 ||
			    !match(group, /^[0-9A-Fa-f]+$/))
				return false;

			groups++;
		}
	}

	return match(address, /::/) ? groups < 8 : groups == 8;
}

function lan_source_allowed(remote)
{
	if (type(remote) != 'string' || !length(remote) || length(remote) > 64)
		return false;

	if (remote == '127.0.0.1' || remote == '::1')
		return true;

	/* IPv6 is deliberately limited to link-local callers in this first PR. */
	if (ipv6_link_local(remote))
		return true;

	let source = ipv4_number(remote);
	if (source == null)
		return false;

	let status = backend_call('network.interface.lan', 'status', {}, true);
	if (!status)
		return false;

	for (let address in status['ipv4-address'] ?? []) {
		let network = ipv4_number(address.address);
		let mask = ipv4_prefix_mask(address.mask);

		if (network == null || mask == null)
			continue;

		if ((source & mask) == (network & mask))
			return true;
	}

	return false;
}

function shadow_parameters()
{
	let shadow = fs.readfile('/etc/shadow', 64 * 1024);
	if (shadow == null)
		return null;

	for (let line in split(shadow, '\n')) {
		let fields = split(trim(line), ':');
		if (length(fields) < 2 || fields[0] != 'root')
			continue;

		let hash = fields[1];
		if (!hash || match(hash, /^[!*]/))
			return null;

		let parts = split(hash, '$');
		if (length(parts) != 4 || parts[0] != '' || !parts[3])
			return null;

		if (parts[1] != '1' && parts[1] != '5' && parts[1] != '6')
			return null;

		if (!match(parts[2], /^[A-Za-z0-9./]{1,16}$/) ||
		    !match(parts[3], /^[A-Za-z0-9./]+$/))
			return null;

		return { hash, alg: +parts[1], salt: parts[2] };
	}

	return null;
}

function auth_failure_state(remote)
{
	return read_state(state_file('failure', remote)) ?? { count: 0, until: 0 };
}

function auth_is_blocked(remote)
{
	let state = auth_failure_state(remote);
	return (+state.until || 0) > time();
}

function record_auth_failure(remote)
{
	let now = time();
	let state = auth_failure_state(remote);
	let until = +state.until || 0;

	if (until > now)
		return;

	let count = +state.count || 0;
	if (until && until <= now)
		count = 0;

	count++;
	until = count >= AUTH_MAX_FAILURES ? now + AUTH_BLOCK_TIME : 0;
	write_state(state_file('failure', remote), { count, until, updated: now });

	log_once('warning', `auth-failure:${remote}`,
		`authentication failed from ${remote}`);
	if (until)
		log_once('warning', `auth-blocked:${remote}`,
			`authentication rate limited from ${remote}`);
}

function clear_auth_failures(remote)
{
	remove_state(state_file('failure', remote));
}

function challenge_for(remote)
{
	let parameters = shadow_parameters();
	if (!parameters)
		return null;

	let nonce = random_hex(16);
	if (!nonce)
		return null;

	let challenge = {
		username: 'root',
		alg: parameters.alg,
		salt: parameters.salt,
		nonce,
		created: monotonic_millis(),
	};

	if (!write_state(state_file('challenge', remote), challenge))
		return null;

	return {
		alg: challenge.alg,
		salt: challenge.salt,
		nonce: challenge.nonce,
		'hash-method': 'md5',
	};
}

function session_valid(sid, remote)
{
	if (type(sid) != 'string' || !match(sid, /^[0-9a-f]{32}$/))
		return false;

	let path = state_file('session', sid);
	let stat = fs.stat(path);
	if (!stat || stat.type != 'file' || stat.uid != 0 || stat.gid != 0 ||
	    (stat.mode & 0o077))
		return false;

	let session = read_state(path);
	let now = monotonic_millis();
	if (!session || session.username != 'root' || session.last == null ||
	    now < +session.last ||
	    now - +session.last > SESSION_TTL * 1000) {
		remove_state(path);
		return false;
	}

	/* A foreign source must not be able to invalidate an active session. */
	if (session.remote != remote)
		return false;

	/* Rewriting the same root-owned file refreshes the inactivity timeout. */
	session.last = now;
	return write_state(path, session);
}

function create_session(remote)
{
	let sid = random_hex(16);
	if (!sid)
		return null;

	if (!write_state(state_file('session', sid), {
		username: 'root',
		remote,
		last: monotonic_millis(),
	}))
		return null;

	return sid;
}

function rpc_error(id, code, message)
{
	return { jsonrpc: '2.0', id: id ?? null, error: { code, message } };
}

function rpc_result(id, result)
{
	return { jsonrpc: '2.0', id: id ?? null, result };
}

function response_session_id(response)
{
	let sid = response?.result?.sid;
	return type(sid) == 'string' && match(sid, /^[0-9a-f]{32}$/) ? sid : null;
}

function valid_request_id(value)
{
	return value == null || type(value) == 'string' || type(value) == 'int' ||
		type(value) == 'double';
}

function interface_status(name, required)
{
	if (!safe_ifname(name))
		return null;

	let data = backend_call(`network.interface.${name}`, 'status', {}, !required);
	if (!data || type(data) != 'object')
		return null;

	let result = {
		interface: name,
		up: bool_value(data.up, false),
		available: bool_value(data.available, true),
	};

	for (let field in [
		'proto', 'device', 'l3_device', 'ipv4-address', 'ipv6-address',
		'route', 'dns-server', 'uptime', 'metric'
	]) {
		if (data[field] != null)
			result[field] = data[field];
	}

	return result;
}

function first_address(status, family)
{
	let list = status?.[family] ?? [];
	return length(list) ? list[0] : null;
}

function has_default_route(status)
{
	for (let route in status?.route ?? []) {
		if ((route.target == '0.0.0.0' || route.target == '::') &&
		    route.mask != null && +route.mask == 0)
			return true;
	}

	return false;
}

function interface_online(status)
{
	if (!status)
		return null;

	return !!status.up && (has_default_route(status) ||
		length(status['ipv4-address'] ?? []) > 0 ||
		length(status['ipv6-address'] ?? []) > 0);
}

function system_sections()
{
	let result = {};
	try {
		uci.cursor().foreach('system', 'system', (section) => {
			if (!result['.name'])
				result = section;
		});
	} catch (e) {
		return {};
	}

	return result;
}

function system_info_data()
{
	let board = backend_call('system', 'board', {}, false);
	let runtime = backend_call('system', 'info', {}, true);
	if (!board || type(board) != 'object')
		board = {};
	if (!runtime || type(runtime) != 'object')
		runtime = {};
	if (!length(keys(board)) && !length(keys(runtime)))
		return null;

	let release = board.release;
	if (!release || type(release) != 'object')
		release = {};
	let lan = interface_status('lan', false);
	let wan = interface_status('wan', false);
	let factory_mac = normalize_mac(read_factory_value('device_mac'));
	let lan_mac = read_net_mac(lan?.l3_device ?? lan?.device);
	let wan_mac = read_net_mac(wan?.l3_device ?? wan?.device);
	let mac = factory_mac ?? lan_mac;
	let system = system_sections();
	let firmware = {};

	for (let field, value in release)
		firmware[field] = value;
	firmware.stock = false;
	firmware.compatibility = 'glinet-app-compat';

	let result = {
		model: board.model ?? board.board_name ?? 'OpenWrt',
		board_name: board.board_name,
		board_info: board.board_name,
		vendor: 'GL.iNet',
		firmware_version: release.version ?? 'unknown',
		firmware_type: 'openwrt',
		firmware,
		mac,
		time: time(),
		hostname: board.hostname ?? system.hostname,
		kernel: board.kernel,
		uptime: runtime.uptime ?? board.uptime,
		timezone: system.timezone,
		compatibility: 'glinet-app-compat',
		stock_firmware: false,
	};

	let serial = read_factory_value('device_sn');
	let country = read_factory_value('country_code');
	if (serial) {
		result.serial = serial;
		result.sn = serial;
	}
	if (country) {
		result.country = country;
		result.country_code = country;
	}
	let serial_backup = read_factory_value('device_sn_bak');
	if (serial_backup)
		result.sn_bak = serial_backup;
	if (wan_mac)
		result.wan_mac = wan_mac;
	if (lan_mac)
		result.lan_mac = lan_mac;

	return result;
}

function ui_check_initialized_result()
{
	let info = system_info_data() ?? {};
	let result = {
		initialized: !!shadow_parameters(),
		model: 'GL-BE9300',
		firmware_version: info.firmware_version ?? 'unknown',
	};

	for (let field in ['hostname', 'mac']) {
		if (info[field] != null)
			result[field] = info[field];
	}

	return result;
}

function wifi_iface_from_config(config, radio, ifname)
{
	let name = ifname ?? config['.name'];
	return {
		name,
		ifname: safe_ifname(ifname),
		device: radio,
		enabled: !bool_value(config.disabled, false),
		ssid: config.ssid ?? config.mesh_id,
		encryption: config.encryption,
		network: config.network,
		mode: config.mode ?? 'ap',
		key: null,
	};
}

function radio_band(config)
{
	if (config.band)
		return config.band;

	switch (config.hwmode) {
	case '11a':
		return '5';
	case '11g':
	case '11b':
		return '2.4';
	}

	return null;
}

function wifi_data_from_uci()
{
	let radios = {};
	let cursor = uci.cursor();

	try {
		cursor.foreach('wireless', 'wifi-device', (config) => {
			let name = config['.name'];
			radios[name] = {
				name,
				enabled: !bool_value(config.disabled, false),
				channel: config.channel,
				band: radio_band(config),
				state: bool_value(config.disabled, false) ? 'down' : 'unknown',
				ifaces: [],
			};
		});

		cursor.foreach('wireless', 'wifi-iface', (config) => {
			let radio = config.device ?? 'unknown';
			if (!radios[radio])
				radios[radio] = { name: radio, enabled: true, ifaces: [] };

			push(radios[radio].ifaces,
				wifi_iface_from_config(config, radio, config.ifname));
		});
	} catch (e) {
		return [];
	}

	let result = [];
	for (let name, radio in radios)
		push(result, radio);

	return result;
}

function wifi_data()
{
	let status = backend_call('network.wireless', 'status', {}, true);
	let result = [];

	if (status && type(status) == 'object') {
		for (let name, data in status) {
			if (!data || type(data) != 'object')
				continue;

			let config = data.config ?? {};
			let radio = {
				name,
				enabled: !bool_value(config.disabled, false),
				up: bool_value(data.up, false),
				state: bool_value(data.up, false) ? 'up' : 'down',
				channel: config.channel,
				band: radio_band(config),
				ifaces: [],
			};

			for (let index, iface in data.interfaces ?? []) {
				if (!iface || type(iface) != 'object')
					continue;

				let iface_config = iface.config ?? {};
				let ifname = iface.ifname ?? iface_config.ifname;
				push(radio.ifaces,
					wifi_iface_from_config(iface_config, name, ifname));
			}

			push(result, radio);
		}
	}

	if (!length(result))
		result = wifi_data_from_uci();

	return result;
}

function wifi_status_result()
{
	let result = [];
	for (let radio in wifi_data()) {
		push(result, {
			name: radio.name,
			band: radio.band,
			channel: radio.channel,
			state: radio.state,
			enabled: radio.enabled,
		});
	}

	return { res: result };
}

function wifi_config_result()
{
	return { res: wifi_data() };
}

function static_bindings()
{
	let result = [];
	let cursor = uci.cursor();

	try {
		cursor.foreach('dhcp', 'host', (section) => {
			let macs = type(section.mac) == 'array' ? section.mac : [section.mac];
			for (let mac in macs) {
				mac = normalize_mac(mac);
				if (!mac)
					continue;

				let binding = { mac, online: false };
				if (section.ip)
					binding.ip = section.ip;
				if (section.name)
					binding.name = section.name;
				push(result, binding);
			}
		});
	} catch (e) {
		return [];
	}

	return result;
}

function dhcp_lease_clients()
{
	let data = fs.readfile('/tmp/dhcp.leases', 256 * 1024);
	let result = {};
	if (data == null)
		return result;

	let now = time();
	for (let line in split(data, '\n')) {
		let fields = split(trim(line), /\s+/);
		if (length(fields) < 4)
			continue;

		let mac = normalize_mac(fields[1]);
		let expires = +fields[0];
		if (!mac || (expires && expires <= now))
			continue;

		let client = { mac, online: true };
		let ip = normalize_ip(fields[2]);
		let hostname = fields[3] == '*' ? null : normalize_hostname(fields[3]);
		if (ip)
			client.ip = ip;
		if (hostname)
			client.hostname = hostname;
		result[mac] = client;
	}

	return result;
}

function associated_clients()
{
	let clients = {};
	let sources = [];
	let radios = wifi_data();

	for (let radio in radios) {
		for (let iface in radio.ifaces ?? []) {
			if (!iface.ifname)
				continue;

			let data = backend_call('iwinfo', 'assoclist',
				{ device: iface.ifname }, true);
			if (!data || type(data.results) != 'array')
				continue;

			if (index(sources, 'iwinfo') < 0)
				push(sources, 'iwinfo');

			for (let entry in data.results) {
				let mac = normalize_mac(entry.mac);
				if (!mac)
					continue;

				let client = clients[mac] ?? {
					mac,
					online: true,
					type: 'wireless',
					interfaces: [],
				};

				client.interface = iface.ifname;
				client.ssid = iface.ssid;
				if (index(client.interfaces, iface.ifname) < 0)
					push(client.interfaces, iface.ifname);

				for (let field in [
					'signal', 'signal_avg', 'noise', 'inactive', 'connected_time',
					'authorized', 'authenticated', 'rx', 'tx'
				]) {
					if (entry[field] != null)
						client[field] = entry[field];
				}

				clients[mac] = client;
			}
		}
	}

	for (let mac, lease in dhcp_lease_clients()) {
		if (index(sources, 'dnsmasq') < 0)
			push(sources, 'dnsmasq');

		let client = clients[mac] ?? { mac, online: lease.online };
		client.online = lease.online;
		if (lease.ip)
			client.ip = lease.ip;
		if (lease.hostname)
			client.hostname = lease.hostname;
		clients[mac] = client;
	}

	for (let binding in static_bindings()) {
		if (index(sources, 'uci-dhcp-host') < 0)
			push(sources, 'uci-dhcp-host');

		let client = clients[binding.mac] ?? { mac: binding.mac, online: false };
		if (binding.ip)
			client.ip = binding.ip;
		if (binding.name)
			client.name = binding.name;
		clients[binding.mac] = client;
	}

	let result = [];
	for (let mac, client in clients)
		push(result, client);

	return { clients: result, sources, wired_available: false, complete: false };
}

function lan_config_result()
{
	let status = interface_status('lan', false);
	if (!status)
		return null;

	let network = config_section('network', 'lan');
	let dhcp = config_section('dhcp', 'lan');
	let address = first_address(status, 'ipv4-address');
	let lan_mac = read_net_mac(status.l3_device ?? status.device) ??
		normalize_mac(read_factory_value('device_mac'));
	let config = {
		name: 'lan',
		interface: 'lan',
		proto: network.proto ?? status.proto,
		up: status.up,
		ipaddr: address?.address,
		ip: address?.address,
		prefix: address?.mask,
		netmask: ipv4_mask_string(address?.mask),
		mac: lan_mac,
		dhcp: {
			enabled: !bool_value(dhcp.ignore, false),
			start: dhcp.start,
			limit: dhcp.limit == null ? null : +dhcp.limit,
			leasetime: dhcp.leasetime,
		},
	};

	return { res: [config], list: [config] };
}

function wan_status_result()
{
	let status = interface_status('wan', false);
	let address = first_address(status, 'ipv4-address');
	let gateway = null;
	let dns = status?.['dns-server'] ?? [];

	for (let route in status?.route ?? []) {
		if (route.target == '0.0.0.0' && route.mask != null && +route.mask == 0) {
			gateway = route.nexthop;
			break;
		}
	}

	return {
		available: !!status,
		interface: 'wan',
		up: status?.up,
		connected: status ? !!interface_online(status) : null,
		protocol: status?.proto,
		status: status ? (status.up ? 1 : 0) : null,
		ipv4: status ? { ip: address?.address, gateway, dns } : null,
		openwrt: status,
	};
}

function system_status_result()
{
	let board = backend_call('system', 'board', {}, true);
	let runtime = backend_call('system', 'info', {}, true);
	if (!board || type(board) != 'object')
		board = {};
	if (!runtime || type(runtime) != 'object')
		runtime = {};
	let lan = interface_status('lan', false);
	let wan = interface_status('wan', false);
	if (!length(keys(board)) && !length(keys(runtime)) && !lan && !wan)
		return null;

	let network = [];
	for (let status in [wan, lan]) {
		if (!status)
			continue;

		push(network, {
			interface: status.interface,
			up: status.up,
			online: interface_online(status),
			status,
		});
	}

	let system = {
		hostname: board.hostname ?? system_sections().hostname,
		uptime: runtime.uptime ?? board.uptime,
		load_average: runtime.load_average ?? runtime.load,
		memory: runtime.memory,
	};
	let result = {
		service: [],
		network,
		system,
		wan,
		lan,
		wifi: [],
		internet: wan ? !!interface_online(wan) : null,
		diagnostics: {
			wired_clients_available: false,
			stock_glinet_backend: false,
			firmware_upgrade: false,
			cloud: false,
		},
	};

	for (let radio in wifi_data()) {
		for (let iface in radio.ifaces ?? []) {
			push(result.wifi, {
				name: iface.name,
				ssid: iface.ssid,
				passwd: null,
				enabled: radio.enabled && iface.enabled,
				band: radio.band,
				channel: radio.channel,
				state: radio.state,
			});
		}
	}

	return result;
}

function system_load_result()
{
	let runtime = backend_call('system', 'info', {}, true);
	if (!runtime || type(runtime) != 'object')
		return null;

	let result = {};
	for (let field in ['load_average', 'load', 'uptime', 'memory']) {
		if (runtime[field] != null)
			result[field] = runtime[field];
	}

	return length(keys(result)) ? result : null;
}

function call_method(module, method, args)
{
	switch (`${module}.${method}`) {
	case 'ui.check_initialized':
		return ui_check_initialized_result();
	case 'system.get_info':
		return system_info_data();
	case 'system.get_status':
		return system_status_result();
	case 'system.get_load':
		return system_load_result();
	case 'cable.get_status':
		return wan_status_result();
	case 'lan.get_config_list':
		return lan_config_result();
	case 'lan.get_static_bind_list':
		return { static_bind_list: static_bindings() };
	case 'wifi.get_config':
		return wifi_config_result();
	case 'wifi.get_status':
		return wifi_status_result();
	case 'clients.get_list':
		return associated_clients();
	case 'clients.get_status': {
		let clients = associated_clients();
		let wireless = 0;
		for (let client in clients.clients)
			if (client.online)
				wireless++;

		return {
			cable_total: null,
			wireless_total: wireless,
			wired_available: false,
			complete: false,
		};
	}
	}

	let label = `${rpc_label(module)}.${rpc_label(method)}`;
	log_once('info', `unsupported:${label}`, `unsupported method: ${label}`);
	return { __unsupported: true };
}

function no_auth_method(module, method)
{
	return module == 'ui' && method == 'check_initialized';
}

function read_request_body(env)
{
	let body_length = +env.CONTENT_LENGTH;
	if (body_length <= 0 || body_length > MAX_BODY_LENGTH)
		return null;

	let result = '';
	while (length(result) < body_length) {
		let chunk = uhttpd.recv(body_length - length(result));
		if (chunk == null || !length(chunk))
			break;
		result += chunk;
	}

	return length(result) == body_length ? result : null;
}

function handle_authentication(request, remote)
{
	let id = request.id ?? null;

	if (request.method == 'challenge') {
		if (type(request.params) != 'object' || request.params.username != 'root') {
			log_once('warning', `auth-challenge:${remote}`,
				`authentication challenge rejected from ${remote}`);
			return rpc_error(id, -32000, 'authentication failed');
		}

		let result = challenge_for(remote);
		if (!result) {
			log_once('err', 'authentication:challenge',
				'could not create authentication challenge');
			return rpc_error(id, -32001, 'authentication unavailable');
		}

		return rpc_result(id, result);
	}

	if (request.method == 'login') {
		if (auth_is_blocked(remote))
			return rpc_error(id, -32000, 'authentication failed');

		let params = request.params;
		if (type(params) != 'object' || params.username != 'root' ||
		    type(params.hash) != 'string' ||
		    !match(lc(params.hash), /^[0-9a-f]{32}$/)) {
			record_auth_failure(remote);
			return rpc_error(id, -32000, 'authentication failed');
		}

		let path = state_file('challenge', remote);
		let challenge = read_state(path);
		remove_state(path);
		let shadow = shadow_parameters();
		let challenge_age = challenge ? monotonic_millis() - (+challenge.created || 0) : -1;
		if (!challenge || !shadow || challenge.username != 'root' ||
		    challenge_age < 0 || challenge_age > CHALLENGE_TTL * 1000 ||
		    challenge.alg != shadow.alg || challenge.salt != shadow.salt) {
			record_auth_failure(remote);
			return rpc_error(id, -32000, 'authentication failed');
		}

		let expected = digest.md5(`root:${shadow.hash}:${challenge.nonce}`);
		if (lc(params.hash) != expected) {
			record_auth_failure(remote);
			return rpc_error(id, -32000, 'authentication failed');
		}

		let sid = create_session(remote);
		if (!sid) {
			log_once('err', 'authentication:session',
				'could not create authenticated session');
			return rpc_error(id, -32001, 'authentication unavailable');
		}

		clear_auth_failures(remote);
		return rpc_result(id, { username: 'root', sid });
	}

	return null;
}

function handle_call(request, remote)
{
	let id = request.id ?? null;
	let params = request.params;
	if (type(params) != 'array' || (length(params) != 3 && length(params) != 4)) {
		log_once('warning', 'request:call-params',
			'invalid call parameters');
		return rpc_error(id, -32602, 'invalid params');
	}

	let sid = params[0];
	let module = params[1];
	let method = params[2];
	let args = length(params) == 4 ? params[3] : {};
	if (type(sid) != 'string' || type(module) != 'string' ||
	    type(method) != 'string' ||
	    length(module) > 64 || length(method) > 64 ||
	    !match(module, /^[A-Za-z0-9_-]+$/) ||
	    !match(method, /^[A-Za-z0-9_-]+$/) ||
	    (args != null && type(args) != 'object' && type(args) != 'array')) {
		log_once('warning', 'request:call-params',
			'invalid call parameters');
		return rpc_error(id, -32602, 'invalid params');
	}

	let started = monotonic_millis();
	let auth = no_auth_method(module, method) ? 'preauth' : 'session';
	if (!no_auth_method(module, method) && !session_valid(sid, remote)) {
		log_once('warning', `auth-session:${remote}`,
			`invalid or expired session from ${remote}`);
		log_call_debug(module, method, args, remote, 'invalid', started,
			'authentication-failed');
		return rpc_error(id, -32000, 'authentication failed');
	}

	let result = call_method(module, method, args ?? {});
	if (result?.__unsupported) {
		log_call_debug(module, method, args, remote, auth, started,
			'method-not-found');
		return rpc_error(id, -32601, 'method not found');
	}
	if (result == null) {
		log_call_debug(module, method, args, remote, auth, started,
			'backend-unavailable');
		return rpc_error(id, -32001, 'backend unavailable');
	}

	log_call_debug(module, method, args, remote, auth, started, 'success');
	return rpc_result(id, result);
}

function send_json(status, response, sid)
{
	let headers = `Status: ${status}\r\n` +
		'Content-Type: application/json\r\n' +
		'Cache-Control: no-store\r\n' +
		'X-Content-Type-Options: nosniff\r\n';
	if (sid)
		headers += `Set-Cookie: Admin-Token=${sid}; Path=/; HttpOnly\r\n`;
	print(headers + '\r\n');
	print(sprintf('%.J', response));
}

function handle_request(env)
{
	let remote = env.REMOTE_ADDR ?? '';

	if (!rpc_enabled) {
		send_json('404 Not Found', rpc_error(null, -32601, 'method not found'));
		return;
	}

	if (env.REQUEST_METHOD != 'POST') {
		send_json('405 Method Not Allowed', rpc_error(null, -32600, 'invalid request'));
		return;
	}

	ubus ??= libubus.connect(null, 3);

	if (!lan_source_allowed(remote)) {
		log_once('warning', `source:${remote}`, `rejected non-LAN request from ${remote}`);
		send_json('403 Forbidden', rpc_error(null, -32003, 'LAN access required'));
		return;
	}

	let body = read_request_body(env);
	if (!body) {
		log_once('warning', 'request:body',
			'malformed or oversized request body');
		send_json('400 Bad Request', rpc_error(null, -32600, 'invalid request'));
		return;
	}

	let request;
	try {
		request = json(body);
	} catch (e) {
		log_once('warning', 'request:json', 'malformed JSON request');
		send_json('400 Bad Request', rpc_error(null, -32700, 'parse error'));
		return;
	}

	if (!request || type(request) != 'object' || request.jsonrpc != '2.0' ||
	    !valid_request_id(request.id) || type(request.method) != 'string') {
		log_once('warning', 'request:invalid', 'invalid JSON-RPC request');
		send_json('200 OK', rpc_error(null, -32600, 'invalid request'));
		return;
	}

	let response;
	if (request.method == 'challenge' || request.method == 'login')
		response = handle_authentication(request, remote);
	else if (request.method == 'call')
		response = handle_call(request, remote);
	else {
		let label = rpc_label(request.method);
		log_once('info', `unsupported-rpc:${label}`, `unsupported method: ${label}`);
		response = rpc_error(request.id, -32601, 'method not found');
	}

	send_json('200 OK', response ?? rpc_error(request.id, -32600, 'invalid request'),
		request.method == 'login' ? response_session_id(response) : null);
}

init_logging();

global.handle_request = handle_request;
