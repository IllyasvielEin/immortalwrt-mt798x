#!/usr/bin/env python3
"""Validate the fixed CPU forwarding product and real built artifacts."""
import argparse
from pathlib import Path
import subprocess


def config(path):
    values = {}
    for line in Path(path).read_text().splitlines():
        if line.startswith('CONFIG_') and '=' in line:
            key, value = line.split('=', 1)
            values[key] = value
    return values


def require(values, expected):
    for name, value in expected.items():
        if values.get('CONFIG_' + name, 'n') != value:
            raise ValueError(f'CONFIG_{name}: expected {value}, got {values.get("CONFIG_" + name, "n")}')


def check_openwrt(path):
    require(config(path), {'KERNEL_MTK_AP_BRIDGE': 'y',
                          'PACKAGE_mtkhqos_util': 'n',
                          'PACKAGE_luci-app-eqos-mtk': 'y'})


def check_kernel(path):
    require(config(path), dict.fromkeys((
        'ARM64', 'NET_MEDIATEK_SOC', 'MEDIATEK_NETSYS_V2', 'MTK_AP_BRIDGE',
        'BRIDGE', 'BRIDGE_NETFILTER', 'NETFILTER_FAMILY_BRIDGE',
        'VLAN_8021Q', 'BRIDGE_VLAN_FILTERING', 'SKB_EXTENSIONS',
        'WATCHDOG', 'MEDIATEK_WATCHDOG'), 'y'))
    require(config(path), {'NET_MEDIATEK_HNAT': 'm', 'EFI': 'n',
                          'OF_OVERLAY': 'n', 'MODULE_FORCE_UNLOAD': 'n',
                          'MEDIATEK_NETSYS_V3': 'n'})


def symbols(nm, path):
    return {line.split()[-1] for line in subprocess.check_output(
        [nm, str(path)], text=True).splitlines() if line.split()}


def check_artifacts(kernel, hnat, warp, nm):
    built = symbols(nm, kernel / 'vmlinux')
    expected = {'mtk_ap_bridge_create', 'mtk_ap_bridge_ioctl',
                'mtk_cpu_dma_fatal', 'mtk_cpu_profile_ready',
                'mtk_cpu_wifi_participant_set_live'}
    if not expected <= built:
        raise ValueError(f'Missing built kernel helpers: {sorted(expected - built)}')
    built = symbols(nm, hnat)
    if 'hnat_cpu_probe' not in built or 'hnat_cpu_restore' not in built:
        raise ValueError('HNAT module does not contain the fixed CPU profile')
    if 'cleanup_module' in built or any(s.startswith('hnat_nf_hook') for s in built):
        raise ValueError('HNAT contains a live exit or legacy NF flow implementation')
    if 'warp_ser_checked' not in symbols(nm, warp):
        raise ValueError('WARP does not contain checked SER')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--openwrt', type=Path)
    parser.add_argument('--kernel', type=Path)
    parser.add_argument('--hnat', type=Path)
    parser.add_argument('--warp', type=Path)
    parser.add_argument('--mode-file', type=Path)
    parser.add_argument('--nm', default='llvm-nm')
    args = parser.parse_args()
    if not any((args.openwrt, args.kernel, args.mode_file)):
        parser.error('at least one real configuration or mode file is required')
    if args.openwrt:
        check_openwrt(args.openwrt)
    if args.kernel:
        check_kernel(args.kernel / '.config')
        if bool(args.hnat) != bool(args.warp):
            parser.error('--hnat and --warp must be supplied together')
        if args.hnat:
            check_artifacts(args.kernel, args.hnat, args.warp, args.nm)
    if args.mode_file and args.mode_file.read_bytes() != b'cpu-only\n':
        raise ValueError('Missing or incorrect forwarding mode in built rootfs')
    print('AX6000 fixed CPU forwarding build checks passed')


if __name__ == '__main__':
    main()
