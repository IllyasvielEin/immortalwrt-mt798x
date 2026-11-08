'use strict';
import * as libubus from 'ubus';
import * as fs from 'fs';

import { log } from 'mtwifi.utils';

const CMD_UP = 0;
const CMD_SET_DATA = 1;
const CMD_PROCESS_ADD = 2;
const CMD_PROCESS_KILL_ALL = 3;
const CMD_SET_RETRY = 4;
const CMD_AP_PREPARE = 5;
export const cpu_profile = fs.readfile("/usr/share/ax6000/forwarding-mode") == "cpu-only\n";

global.ubus = libubus.connect();

export function notify(command, params, data) {
	params ??= {};
	data ??= {};

	let result = global.ubus.call('network.wireless', 'notify', { command, device: global.radio, ...params, data });
	let error = global.ubus.error();
	if (error != null) die(`[Netifd] notify ${command} failed: ${error}`);
	return result;
};

export function set_up() {
	return notify(CMD_UP);
};

export function set_data(data) {
	return notify(CMD_SET_DATA, null, data);
};

export function add_process(exe, pid, required, keep) {
	exe = fs.realpath(exe);

	notify(CMD_PROCESS_ADD, null, { pid, exe, required, keep });
};

export function set_retry(retry) {
	return notify(CMD_SET_RETRY, null, { retry });
};

export function set_vif(interface, ifname) {
	return notify(CMD_SET_DATA, { interface }, { ifname });
};

export function set_vlan(interface, ifname, vlan) {
	return notify(CMD_SET_DATA, { interface, vlan }, { ifname });
};

export function setup_failed(reason) {
	log.error(`[Netifd] Device setup failed: ${reason}`);
	printf('%s\n', reason);
	set_retry(false);
};

// Existing APs must confirm stopped before DAT/down/reload changes. New APs
// are BLOCKED by the driver from first registration and are prepared at CMD_UP.
export function prepare_vifs(vifs, dev) {
	if (!cpu_profile) return;
	let names = [];
	for (let vif in vifs) {
		// L1 profile enumerates interface roles; ioctl independently validates AP.
		if (dev.apcli_ifname && index(vif, dev.apcli_ifname) == 0) continue;
		push(names, vif);
	}
	if (length(names)) return notify(CMD_AP_PREPARE, null, { ifnames: names });
};
