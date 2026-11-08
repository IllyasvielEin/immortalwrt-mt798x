"""Actual C helper and shell command tests. No router or host network changes."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
NETIFD = Path(os.environ.get("MTK_AP_BRIDGE_NETIFD_TREE", ROOT / ".analysis/netifd-next"))
KERNEL = Path(os.environ.get("MTK_AP_BRIDGE_KERNEL_TREE", ROOT / ".analysis/kernel-next"))
DRIVER = Path(os.environ.get("MTK_AP_BRIDGE_DRIVER_TREE", ROOT / ".analysis/driver-next"))
EQOS = ROOT / "package/mtk/applications/luci-app-eqos-mtk/root/usr/sbin/eqos"

STUB = r'''#!/usr/bin/env python3
import json, os, pathlib, sys
p = pathlib.Path(os.environ['SANDBOX'])
name = pathlib.Path(sys.argv[0]).name
a = sys.argv[1:]
with (p/'commands').open('a') as f: f.write(json.dumps([name]+a)+'\n')
state = json.loads((p/'state').read_text())
if name == 'uci':
    print('100'); sys.exit(0)
if name == 'nft': sys.exit(0)
if name == 'ip':
    if a[:2] == ['link', 'add']:
        (p/'net'/a[2]).mkdir(parents=True, exist_ok=True)
    elif a[:3] == ['link', 'del', 'dev']:
        d=p/'net'/a[3]
        if d.exists(): d.rmdir()
elif name == 'tc':
    dev=a[a.index('dev')+1] if 'dev' in a else ''
    if a[:2] == ['qdisc', 'show']:
        q=state.get(dev)
        if q: print('qdisc '+q+' 1: root refcnt 2')
        sys.exit(0)
    if a[:2] == ['qdisc', 'add'] and 'root' in a: state[dev]='htb'
    if a[:2] == ['qdisc', 'del'] and 'root' in a: state.pop(dev, None)
    if a[:2] in (['class','add'],['class','replace']):
        if os.environ.get('FAIL_CLASS'): sys.exit(1)
        if a[a.index('parent')+1] == a[a.index('classid')+1]: sys.exit(2)
else:
    # Attempting module/HNAT tools is an explicit test failure.
    sys.exit(88)
(p/'state').write_text(json.dumps(state))
'''


class Activation(unittest.TestCase):
    def test_real_netifd_free_replace_and_failed_member_retention(self):
        with tempfile.TemporaryDirectory() as directory:
            d = Path(directory)
            generic = (NETIFD / 'device.c').read_text()
            bridge = (NETIFD / 'bridge.c').read_text()
            regions = {
                'NETIFD_GENERIC_FREE_SOURCE': (generic, 'int device_prepare_change(', '\nstatic void\n__device_free_unused'),
                'NETIFD_REPLACE_SOURCE': (generic, 'static int\ndevice_replace(', '\nvoid\ndevice_reset_config('),
                'NETIFD_APPLY_SOURCE': (generic, 'enum dev_change_type\ndevice_apply_config(', '\nstatic int\ndevice_replace('),
                'NETIFD_BRIDGE_PREPARE_SOURCE': (bridge, 'static int bridge_prepare_change(struct device *dev, bool remove)\n{', '\nstatic void\nbridge_free('),
                'NETIFD_DELETE_SOURCE': (generic, 'static void\ndevice_delete(', '\nstatic int device_cleanup_cb('),
                'NETIFD_CREATE_SOURCE': (generic, 'struct device *\ndevice_create(', '\nvoid\ndevice_dump_status('),
            }
            defines = []
            for key, (text, start, end) in regions.items():
                a = text.index(start)
                b = text.index(end, a)
                path = d / f'{key}.c'
                path.write_text(text[a:b])
                defines.append(f'-D{key}="{path}"')
            # Compile the exact failed-deletion prefix, stopping before the
            # successful hardware/VLAN cleanup covered by native N compilation.
            a = bridge.index('static void\nbridge_free_member(')
            b = bridge.index('\nrestart:', a)
            prefix = bridge[a:b].replace('bridge_free_member(', 'bridge_free_member_failure(', 1)
            path = d / 'member-failure.c'
            path.write_text(prefix + '\n(void)ifname; (void)vlan; member_frees++;\n}\n')
            defines.append(f'-DNETIFD_MEMBER_FAILURE_SOURCE="{path}"')
            binary = d / 'lifetime'
            subprocess.run(['cc', '-std=gnu99', '-Wall', '-Wextra', '-Werror',
                            '-O1', '-g', '-fsanitize=address,undefined', *defines,
                            str(ROOT / 'tests/ax6000_bridge_control/test_netifd_lifetime.c'),
                            '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True,
                           env=dict(os.environ, ASAN_OPTIONS='detect_leaks=0'))

    def test_real_driver_proxy_history_and_dma_readback(self):
        with tempfile.TemporaryDirectory() as directory:
            d = Path(directory)
            driver = DRIVER / 'mt_wifi'
            proxy = (driver / 'embedded/plug_in/warp_proxy/warp_proxy.c').read_text()
            a = proxy.index('static void *cpu_proxy_owner;')
            b = proxy.index('\n#endif', a)
            # The state block contains a function, but no nested ifdef.
            (d / 'proxy-state.c').write_text(proxy[a:b])
            a = proxy.index('static u32\nclient_handle(')
            b = proxy.index('\nstatic struct mt_wlan_hook_ops ', a)
            (d / 'proxy-handle.c').write_text(proxy[a:b])
            text = (driver / 'chips/mt7986.c').read_text()
            a = text.index('static INT hif_set_WFDMA(')
            b = text.index('\nstatic BOOLEAN hif_wait_WFDMA_idle(', a)
            (d / 'dma.c').write_text(text[a:b])
            text = (driver / 'embedded/common/fq_qm.c').read_text()
            a = text.index('INT set_fq_enable(')
            b = text.index('\nINT show_fq_info(', a)
            (d / 'fq.c').write_text(text[a:b])
            for name, defines in (
                ('proxy', {'DRIVER_PROXY_STATE_SOURCE': d / 'proxy-state.c',
                           'DRIVER_PROXY_HANDLE_SOURCE': d / 'proxy-handle.c'}),
                ('dma', {'DRIVER_DMA_SOURCE': d / 'dma.c',
                         'DRIVER_FQ_SOURCE': d / 'fq.c'}),
            ):
                binary = d / name
                subprocess.run(['cc', '-std=gnu99', '-Wall', '-Wextra', '-Werror',
                                '-O1', '-g', '-fsanitize=address,undefined',
                                f'-DMTK_WARP_SER_HEADER="{KERNEL}/include/linux/mtk_warp_ser.h"',
                                *[f'-D{k}="{v}"' for k, v in defines.items()],
                                str(ROOT / f'tests/ax6000_bridge_control/test_driver_{name}.c'),
                                '-o', str(binary)], check=True)
                subprocess.run([str(binary)], check=True,
                               env=dict(os.environ, ASAN_OPTIONS='detect_leaks=0'))

    def test_real_driver_ser_helpers(self):
        with tempfile.TemporaryDirectory() as directory:
            d = Path(directory)
            text = (DRIVER / 'mt_wifi/embedded/hw_ctrl/hw_ctrl.c').read_text()
            start = text.index('/* The request permission is a linearization point,')
            end = text.index('\n#endif\n\nNTSTATUS HwRecoveryFromError(', start)
            source = d / 'driver-ser.c'
            source.write_text(text[start:end])
            text = (DRIVER / 'mt_wifi/embedded/hw_ctrl/hw_ctrl_basic.c').read_text()
            start = text.index('static void mtk_wifi_cpu_ser_timeout(')
            end = text.index('\n#endif\n\nstatic int mtk_fe_event(', start)
            watchdog = d / 'watchdog.c'
            watchdog.write_text(text[start:end])
            start = text.index('static int mtk_fe_event(')
            end = text.index('\nunsigned int mtk_fe_reset_notifier_init(', start)
            notifier = d / 'notifier.c'
            notifier.write_text(text[start:end])
            text = (DRIVER / 'mt_wifi/embedded/common/rtmp_init_inf.c').read_text()
            start = text.index('int mtk_wifi_cpu_queues_stop(')
            end = text.index('\n#endif\n\nstatic INT rtmp_sys_init(', start)
            init_failure = d / 'init-failure.c'
            init_failure.write_text(text[start:end])
            binary = d / 'driver-ser'
            subprocess.run(['cc', '-std=gnu99', '-Wall', '-Wextra', '-Werror',
                            '-O1', '-g', '-fsanitize=address,undefined',
                            f'-DDRIVER_SER_SOURCE="{source}"',
                            f'-DMTK_WARP_SER_HEADER="{KERNEL}/include/linux/mtk_warp_ser.h"',
                            f'-DDRIVER_WATCHDOG_SOURCE="{watchdog}"',
                            f'-DDRIVER_NOTIFIER_SOURCE="{notifier}"',
                            f'-DDRIVER_INIT_FAILURE_SOURCE="{init_failure}"',
                            str(ROOT / 'tests/ax6000_bridge_control/test_driver_ser.c'),
                            '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True,
                           env=dict(os.environ, ASAN_OPTIONS='detect_leaks=0'))

    def test_real_port_write_and_readback_helpers(self):
        with tempfile.TemporaryDirectory() as directory:
            d = Path(directory)
            port = d / 'ra0/brport'
            port.mkdir(parents=True)
            attrs = ['hairpin_mode', 'learning', 'unicast_flood', 'proxyarp',
                     'proxyarp_wifi', 'multicast_flood', 'multicast_to_unicast',
                     'vlan_tunnel', 'broadcast_flood', 'neigh_suppress', 'isolated',
                     'locked', 'mab', 'neigh_vlan_suppress', 'bpdu_filter']
            for name in attrs:
                (port / name).write_text('1\n')
            source = (NETIFD / 'system-linux.c').read_text()
            a = source.index('static int read_file(')
            b = source.index('\n\nstatic const char *', a)
            helpers = source[a:b]
            a = source.index('int system_bridge_update_ap_port(')
            b = source.index('\nint system_bridge_addif(', a)
            helpers += source[a:b].replace('/sys/class/net/', str(d) + '/')
            prefix = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <assert.h>
#define ARRAY_SIZE(x) (sizeof(x)/sizeof((x)[0]))
#define DEV_OPT_ISOLATE 1
struct device {char ifname[16]; bool wireless_bridge_isolate, bpdu_filter;
    struct {unsigned flags; bool isolate;} settings;};
static struct device *ap_bridge_owner(struct device *dev) {return dev;}
static uint64_t ap_bridge_port_flags(struct device *bridge, struct device *dev)
{(void)bridge;(void)dev;return 1ULL;}
static void system_bridge_set_wireless(struct device *b,struct device *d) {(void)b;(void)d;}
static void system_bridge_set_isolated(struct device *d,const char *v) {(void)d;(void)v;}
static void system_bridge_set_bpdu_filter(struct device *d,const char *v) {(void)d;(void)v;}
'''
            main = '''
int main(int argc, char **argv) {
    struct device d = {.ifname="ra0"};
    (void)argv;
    int ret = system_bridge_update_ap_port(&d,&d);
    assert(argc == 1 ? ret == 0 : ret < 0);
    return 0;
}
'''
            c = d / 'port.c'
            c.write_text(prefix + helpers + main)
            binary = d / 'port'
            subprocess.run(['cc', '-std=gnu99', '-Wall', '-Wextra', '-Werror',
                            str(c), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)
            for name in attrs:
                self.assertEqual((port / name).read_text(), '1\n' if name == 'hairpin_mode' else '0\n')
            (port / 'locked').unlink()  # Optional false capability is checked at COMMIT.
            subprocess.run([str(binary)], check=True)
            (port / 'hairpin_mode').unlink()
            subprocess.run([str(binary), 'must-fail'], check=True)

    def test_real_c_helpers(self):
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            # Compile the exact policy function from device.c, not a test copy.
            text = (NETIFD / 'device.c').read_text()
            start = text.index('void device_wireless_refresh(')
            end = text.index('\nvoid device_foreach(', start)
            policy = d / 'policy.c'
            policy.write_text(text[start:end])
            binary = d / 'activation'
            subprocess.run(['cc', '-std=gnu99', '-Wall', '-Wextra', '-Werror',
                            '-O1', '-g', '-fsanitize=address,undefined',
                            f'-DDEVICE_POLICY_SOURCE="{policy}"',
                            '-I' + str(NETIFD),
                            f'-DMTK_NETIFD_AP_SOURCE="{NETIFD / "ap-bridge.c"}"',
                            str(ROOT / 'tests/ax6000_bridge_control/test_activation.c'),
                            '-o', str(binary)], check=True)
            env = dict(os.environ, ASAN_OPTIONS='detect_leaks=0')
            subprocess.run([str(binary)], env=env, check=True)

    def test_unique_abi(self):
        self.assertEqual((NETIFD / 'mtk_ap_bridge.h').read_bytes(),
                         (KERNEL / 'include/uapi/linux/mtk_ap_bridge.h').read_bytes())

    def test_mode_file_package_condition(self):
        pkg = ROOT / 'package/mtk/applications/mtwifi-cfg-ucode'
        source = (pkg / 'Makefile').read_text()
        source = source[source.index('ifeq ($(CONFIG_KERNEL_MTK_AP_BRIDGE),y)'):]
        source = source[:source.index('$(eval $(call BuildPackage,')]
        with tempfile.TemporaryDirectory() as directory:
            p = Path(directory)
            makefile = p / 'Makefile'
            makefile.write_text('CP:=true\nINSTALL_DIR:=mkdir -p\nINSTALL_DATA:=install -m 0644\n' + source +
                                f'\nall:\n\t$(call Package/mtwifi-cfg-ucode/install,{p}/root)\n')
            for value in ('n', 'y'):
                subprocess.run(['make', '-s', '-f', str(makefile),
                                f'CONFIG_KERNEL_MTK_AP_BRIDGE={value}'], cwd=pkg, check=True)
                mode = p / 'root/usr/share/ax6000/forwarding-mode'
                self.assertEqual(mode.exists(), value == 'y')
                if value == 'y':
                    self.assertEqual(mode.read_bytes(), b'cpu-only\n')


class Eqos(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.p = Path(self.tmp.name)
        (self.p / 'net').mkdir()
        (self.p / 'bin').mkdir()
        (self.p / 'state').write_text('{}')
        (self.p / 'mode').write_text('cpu-only\n')
        tool = self.p / 'tool'
        tool.write_text(STUB)
        tool.chmod(0o755)
        for name in ('tc', 'ip', 'uci', 'nft', 'insmod', 'modprobe'):
            (self.p / 'bin' / name).symlink_to(tool)
        script = EQOS.read_text().replace('/usr/share/ax6000/forwarding-mode', str(self.p / 'mode'))
        script = script.replace('/sys/class/net/', str(self.p / 'net') + '/')
        script = script.replace('/sys/kernel/debug/hnat', str(self.p / 'hnat-must-not-be-opened'))
        script = script.replace('/usr/sbin/nft', str(self.p / 'bin/nft'))
        self.script = self.p / 'eqos'
        self.script.write_text(script)
        self.env = dict(os.environ, SANDBOX=str(self.p),
                        PATH=str(self.p / 'bin') + ':' + os.environ['PATH'])

    def run_cli(self, *args, ok=True, **env):
        result = subprocess.run(['sh', str(self.script), *args],
                                env=dict(self.env, **env), text=True, capture_output=True)
        if ok:
            self.assertEqual(result.returncode, 0, result.stderr)
        else:
            self.assertNotEqual(result.returncode, 0)
        return result

    def commands(self):
        p = self.p / 'commands'
        return [json.loads(line) for line in p.read_text().splitlines()] if p.exists() else []

    def test_all_slots_software_and_aggregate_default(self):
        self.run_cli('start', '100', '50')
        self.run_cli('add', '1', '10000', '2000', '192.0.2.13', '100', '50')
        self.run_cli('add', '65533', '10000', '2000', '2001:db8::32', '100', '50')
        self.run_cli('enable')
        commands = self.commands()
        classes = [c for c in commands if c[:2] == ['tc', 'class']]
        self.assertTrue(any('1:ffff' in c and '100mbit' in c and 'ceil' in c for c in classes))
        self.assertTrue(any('1:ffff' in c and '50mbit' in c and 'ceil' in c for c in classes))
        self.assertTrue(any('1:1' in c and '1:fffe' in c for c in classes))
        self.assertTrue(any('1:fffd' in c and '1:fffe' in c for c in classes))
        filters = [c for c in commands if c[:3] == ['tc', 'filter', 'add']]
        self.assertTrue(filters)
        self.assertTrue(all('skip_hw' in c for c in filters))
        self.assertFalse(any(c[0] in ('insmod', 'modprobe') for c in commands))
        self.assertTrue(all(c[1:3] == ['delete', 'table'] for c in commands if c[0] == 'nft'))
        self.run_cli('stop')
        self.assertFalse((self.p / 'net/ifb-eqos').exists())
        self.assertFalse(json.loads((self.p / 'state').read_text()))

    def test_reserved_class_ids_rejected(self):
        for slot in ('0', '65534', '65535'):
            self.run_cli('add', slot, '1', '1', '192.0.2.1', ok=False)
        self.assertFalse(self.commands())

    def test_class_failure_unwinds_own_queues(self):
        self.run_cli('start', '100', '50', ok=False, FAIL_CLASS='1')
        self.assertFalse(json.loads((self.p / 'state').read_text()))
        self.assertFalse((self.p / 'net/ifb-eqos').exists())

    def test_unrelated_qdisc_is_preserved(self):
        (self.p / 'state').write_text('{"br-lan":"fq_codel"}')
        self.run_cli('start', '100', '50', ok=False)
        self.assertEqual(json.loads((self.p / 'state').read_text())['br-lan'], 'fq_codel')

    def test_syntax(self):
        for rel in ('root/usr/sbin/eqos', 'root/etc/init.d/eqos'):
            subprocess.run(['sh', '-n', str(EQOS.parents[3] / rel)], check=True)


if __name__ == '__main__':
    unittest.main()
