"""Execute production ucode with local control peers, never a router/ubus socket.

The pinned interpreter and real plugins are built into .analysis/host. Only the
mode file path and transport bindings are replaced in temporary source copies.
"""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
HOST = Path(os.environ.get('MTK_AP_BRIDGE_HOST_PREFIX', ROOT / '.analysis/host'))
UCODE = Path(os.environ.get('UCODE', HOST / 'bin/ucode'))
MTK = ROOT / 'package/mtk/applications/mtwifi-cfg-ucode'
WIFI = ROOT / 'package/network/config/wifi-scripts/files/lib/netifd'

WIRELESS_TEST = r'''
import * as wireless from "./wireless-device.uc";
import * as uloop from "uloop";
import * as ubus from "ubus";
uloop.init();
let events = [];
let fault = scenario;
let prepares = 0;
let handler_cb;
global.netifd = {
    wireless: { devices: {}, path: "/unused" },
    L_DEBUG: 0,
    log: () => {},
    interface_get_enabled: () => ({enabled: true, ifindex: 100}),
    interface_get_bridge: () => false,
    device_set: (name, data) => {
        push(events, ["device", name, data]);
        return !(fault == "device" && name == "ra1");
    },
    interface_handle_link: (data) => {
        push(events, ["link", data.ifname, data.up]);
        return !(fault == "link" && data.ifname == "ra1") &&
               !(fault == "unlink" && !data.up && data.ifname == "ra1");
    },
    ap_prepare: (name) => {
        push(events, ["prepare", name]); prepares++;
        return {ok: !(fault == "prepare" && name == "ra1")};
    },
    ap_commit: (name) => {
        push(events, ["commit", name]);
        return {ok: !(fault == "commit" && name == "ra1")};
    },
    process: (data) => {
        push(events, ["process"]); handler_cb = data.cb;
        return {cancel: () => {}};
    },
};
let vifs = [];
for (let i = 0; i < 2; i++)
    push(vifs, {name: `vif${i}`, config: {mode: "ap", network: ["lan"]}, vlan: [], sta: []});
let w = wireless.new({name: "radio0", config: {}, vif: vifs}, "unused", "mtwifi");
w.handler_data = {radio0: {type: "device", bridge_btnbssid: true}};
for (let i = 0; i < 2; i++)
    w.handler_data[`vif${i}`] = {type: "vif", ifname: `ra${i}`, config: vifs[i].config};
if (scenario == "false-role") {
    w.handler_data.vif1.config = {mode: "sta", network: ["lan"]};
    fault = null;
}
w.state = "setup";
let ret = w.notify({args: {command: 0}});
if (scenario == "teardown") {
    fault = "prepare";
    w.teardown();
}
if (scenario == "teardown-unlink") {
    fault = "unlink";
    w.teardown();
    handler_cb();
}
printf('%J\n', {state: w.state, ret, events, prepares, saved: keys(w.handler_data)});
'''

NOTIFY_TEST = r'''
import * as control from "./mtwifi/netifd.uc";
global.radio = "radio0";
let threw = false;
try { control.set_up(); } catch (e) { threw = true; }
printf('%J\n', {threw, calls: global.calls});
'''


@unittest.skipUnless(UCODE.is_file(), 'build the pinned .analysis/host interpreter first')
class Ucode(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.p = Path(self.tmp.name)
        (self.p / 'mode').write_text('cpu-only\n')

    def run_ucode(self, source, **defines):
        script = self.p / 'test.uc'
        script.write_text(source)
        cmd = [str(UCODE), '-L', str(HOST / 'lib/ucode/*.so')]
        for key, value in defines.items():
            cmd.extend(['-D', f'{key}={json.dumps(value)}'])
        cmd.append(str(script))
        res = subprocess.run(cmd, text=True, capture_output=True)
        self.assertEqual(res.returncode, 0, res.stderr)
        return json.loads(res.stdout)

    def wireless(self, scenario):
        src = (WIFI / 'wireless-device.uc').read_text()
        src = src.replace('/usr/share/ax6000/forwarding-mode', str(self.p / 'mode'))
        (self.p / 'wireless-device.uc').write_text(src)
        shutil.copy2(ROOT / 'package/network/config/netifd/files/lib/netifd/utils.uc',
                     self.p / 'utils.uc')
        return self.run_ucode(WIRELESS_TEST, scenario=scenario)

    def test_complete_order_and_expected_policy(self):
        r = self.wireless('ok')
        self.assertEqual((r['state'], r['ret']), ('up', 0))
        kinds = [e[0] for e in r['events']]
        self.assertEqual(kinds, ['prepare', 'prepare', 'device', 'link',
                                 'device', 'link', 'commit', 'commit'])
        for event in r['events']:
            if event[0] == 'device':
                self.assertTrue(event[2]['wireless_bridge_forwarding'])
                self.assertEqual(event[2]['wireless_policy'], 2)
                self.assertEqual(event[2]['multicast_to_unicast'], -1)

    def test_failures_never_mark_up_or_detach(self):
        for scenario in ('prepare', 'device', 'link', 'commit'):
            with self.subTest(scenario=scenario):
                r = self.wireless(scenario)
                self.assertEqual(r['state'], 'fault')
                self.assertNotEqual(r['ret'], 0)
                self.assertFalse(any(e[0] == 'link' and not e[2] for e in r['events']))
                self.assertFalse(any(e[0] == 'process' for e in r['events']))
                commits = [e for e in r['events'] if e[0] == 'commit']
                self.assertEqual(len(commits), 2 if scenario == 'commit' else 0)
                if scenario == 'commit':
                    self.assertEqual(r['events'][-2:], [['prepare', 'ra0'], ['prepare', 'ra1']])

    def test_explicit_false_fields(self):
        r = self.wireless('false-role')
        self.assertEqual(r['state'], 'up')
        data = next(e[2] for e in r['events'] if e[:2] == ['device', 'ra1'])
        for flag in ('wireless_ap', 'wireless_bridge_forwarding', 'wireless_proxyarp',
                     'wireless_isolate', 'isolate'):
            self.assertIs(data[flag], False)
        self.assertEqual(data['multicast_to_unicast'], -1)

    def test_failed_stop_preserves_relationships(self):
        r = self.wireless('teardown')
        self.assertEqual(r['state'], 'fault')
        self.assertFalse(any(e[0] == 'process' for e in r['events']))
        self.assertFalse(any(e[0] == 'link' and not e[2] for e in r['events']))

    def test_failed_unlink_retains_retry_context(self):
        r = self.wireless('teardown-unlink')
        self.assertEqual(r['state'], 'fault')
        self.assertEqual(set(r['saved']), {'radio0', 'vif0', 'vif1'})

    def test_notify_null_success_and_actual_error(self):
        # The real libubus binding returns null when success has no DATA frame.
        # Only the connection transport is replaced; production notify runs.
        (self.p / 'mtwifi').mkdir()
        src = (MTK / 'files/usr/share/ucode/mtwifi/netifd.uc').read_text()
        src = src.replace("from 'ubus'", "from '../ubus.uc'")
        src = src.replace("from 'mtwifi.utils'", "from './utils.uc'")
        src = src.replace('/usr/share/ax6000/forwarding-mode', str(self.p / 'mode'))
        (self.p / 'mtwifi/netifd.uc').write_text(src)
        (self.p / 'mtwifi/utils.uc').write_text('export const log = {error: () => {}};\n')
        (self.p / 'ubus.uc').write_text('''
global.calls = [];
export function connect() {
    return {call: (...args) => {push(global.calls, args); return null;},
            error: () => transport_error};
};
''')
        for error in (None, 'Permission denied', 'Request timed out'):
            with self.subTest(error=error):
                r = self.run_ucode(NOTIFY_TEST, transport_error=error)
                self.assertEqual(r['threw'], error is not None)
                self.assertEqual(len(r['calls']), 1)

    def test_dbdc_prepares_all_before_any_profile_write(self):
        # Real config.uc orchestrates the transaction; peers record operations
        # in place of DAT disk writes, interface commands, or transport calls.
        mod = self.p / 'mtwifi'
        mod.mkdir()
        source = (MTK / 'files/usr/share/ucode/mtwifi/config.uc').read_text()
        for name in ('defaults', 'utils', 'driver', 'netifd', 'converter'):
            source = source.replace(f"from 'mtwifi.{name}'", f"from './{name}.uc'")
        source = source.replace("from 'datconf'", "from './datconf.uc'")
        (mod / 'config.uc').write_text(source)
        (mod / 'defaults.uc').write_text('export const defs = {REINSTALL_CFGS: [], CHIP_CFGS: {chip:["Chip",0]}};\n')
        (mod / 'utils.uc').write_text('export const log = {debug:()=>{}, info:()=>{}, notice:()=>{}};\n')
        (mod / 'converter.uc').write_text('export function convert(c) {return {Chip:"new", NoForwardingBTNBSSID:"1"};};\n')
        (mod / 'datconf.uc').write_text('''
export function open(path) {
    push(global.events, ["open", path]);
    return {
        getall: () => ({Chip: "old"}),
        merge: (data) => {push(global.events, ["merge",path]); return true;},
        commit: () => {
            push(global.events, ["commit",path]);
            return ((scenario == "sib-commit" && path == "dbdc-sibling") ||
                    (scenario == "main-commit" && path == "dbdc-main")) ? null : true;
        }, close: () => true,
    };
};
''')
        (mod / 'driver.uc').write_text('''
export function is_kmod() {return true;};
export function scan_related_vifs(dev) {return [dev.main_ifname];};
export function ifdown(name) {push(global.events,["down",name]);};
export function ifup(name) {push(global.events,["up",name]);};
export function reload() {die("unexpected reload");};
export function is_vif_inited(name) {return true;};
export function init_dbdc_card(name) {die("unexpected init");};
export function apply_runtime_hooks(c,n) {};
export function trigger_apcli(n) {};
''')
        (mod / 'netifd.uc').write_text('''
export const cpu_profile = true;
export function prepare_vifs(names, dev) {
    push(global.events,["prepare",names[0]]);
    if (scenario == "prepare" && names[0] == "rax0") die("cannot block sibling");
};
export function set_data(data) {push(global.events,["set-data",data]);};
''')
        script = '''
import * as cfg from "./mtwifi/config.uc";
global.events = [];
global.system = (cmd) => {push(global.events,["system",cmd]);return 0;};
let devs = {
    MT7986_1_1: {INDEX:"MT7986",mainidx:1,main_ifname:"ra0",profile_path:"dbdc-main"},
    MT7986_1_2: {INDEX:"MT7986",mainidx:1,main_ifname:"rax0",profile_path:"dbdc-sibling"},
};
let threw = false;
try {cfg.setup({device:"MT7986_1_1",config:{dbdc_main:true},
    interfaces:{"01":{mtwifi_ifname:"ra0",config:{mode:"ap"}}}}, devs);}
catch(e) {threw=true;}
printf('%J\\n',{events:global.events,threw});
'''
        for scenario in ('ok', 'prepare', 'sib-commit', 'main-commit'):
            with self.subTest(scenario=scenario):
                r = self.run_ucode(script, scenario=scenario)
                self.assertEqual(r['threw'], scenario != 'ok')
                e = r['events']
                prepares = [i for i, event in enumerate(e) if event[0] == 'prepare']
                writes = [i for i, event in enumerate(e) if event[0] in ('merge', 'commit', 'down')]
                self.assertEqual(len(prepares), 2)
                if writes:
                    self.assertLess(max(prepares), min(writes))
                if scenario == 'prepare':
                    self.assertFalse(writes)
                if scenario != 'ok':
                    self.assertFalse(any(event[0] in ('up', 'set-data') for event in e))
                else:
                    self.assertTrue(any(event[:2] == ['system', '/sbin/wifi up MT7986_1_2 &'] for event in e))

    def test_real_datconf_error_is_null(self):
        # Verify the real pinned C binding failure contract used by config.uc.
        dat = self.p / 'profile.dat'
        dat.write_text('Default\nA=1\n')
        source = f'''
import * as datconf from "datconf";
let ctx = datconf.open({json.dumps(str(dat))});
if (!ctx) die("cannot open local profile fixture");
ctx.close();
let result = ctx.commit();
printf('%J\\n', {{result, error: datconf.error()}});
'''
        r = self.run_ucode(source)
        self.assertIsNone(r['result'])
        self.assertTrue(r['error'])


if __name__ == '__main__':
    unittest.main()
