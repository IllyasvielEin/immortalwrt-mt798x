#!/usr/bin/env python3
"""Local integration gate; requires the pinned native netifd/host build.

This is deliberately separate from the dependency-free CI helper suite. Missing
native prerequisites are an ERROR, never a skip or a successful P1 regression.
"""
import os
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
source = Path(os.environ.get('MTK_AP_BRIDGE_NETIFD_TREE', root / '.analysis/netifd-next'))
host = Path(os.environ.get('MTK_AP_BRIDGE_HOST_PREFIX', root / '.analysis/host'))
build = Path(os.environ.get('MTK_AP_BRIDGE_NETIFD_BUILD', root / '.analysis/host-build/netifd'))
assert (host / 'include/libubox/uloop.h').exists(), 'Build pinned host dependencies first'
assert (build / 'CMakeFiles/netifd.dir/config.c.o').exists(), 'Build native netifd first'
with tempfile.TemporaryDirectory() as directory:
    work = Path(directory)
    common = ['cc', '-std=gnu99', '-g', '-DDUMMY_MODE', '-I' + str(source),
              '-I' + str(host / 'include'), '-I' + str(build)]
    subprocess.run([*common, '-c', str(source / 'system-dummy.c'), '-o', str(work / 'system.o')], check=True)
    objects = list((build / 'CMakeFiles/netifd.dir').glob('*.c.o'))
    objects = [p for p in objects if p.name not in ('main.c.o', 'system-linux.c.o', 'system-dummy.c.o')]
    subprocess.run(['objcopy', '--redefine-sym', 'main=netifd_daemon_main',
                    str(build / 'CMakeFiles/netifd.dir/main.c.o'), str(work / 'main.o')], check=True)
    wraps = ['open', 'read', 'close', 'socket', 'ioctl', 'if_nametoindex',
             'netifd_ubus_get_procd_data', 'netifd_ubus_device_notify', 'netifd_ucode_config_load',
             'system_bridge_delif']
    libs = ['ubox', 'ubus', 'uci', 'blobmsg_json', 'udebug', 'ucode', 'nl-tiny']
    binary = work / 'reload'
    subprocess.run([*common, str(Path(__file__).with_name('test_netifd_reload.c')),
                    str(work / 'main.o'), str(work / 'system.o'), *map(str, objects),
                    *['-Wl,--wrap=' + name for name in wraps], '-L' + str(host / 'lib'),
                    '-Wl,-rpath,' + str(host / 'lib'), *['-l' + name for name in libs], '-ljson-c',
                    '-o', str(binary)], check=True)
    subprocess.run([str(binary), str(work)], check=True)
