#!/usr/bin/env python3
"""Exercise actual WO/CCIF allocation, publication and shutdown helpers."""
from pathlib import Path
import os
import re
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[2]
WARP = Path(os.environ.get('MTK_AP_BRIDGE_WARP_TREE', ROOT / '.analysis/warp-next'))

def function(source, name):
    match = re.search(r'(?m)^(?:static\s+)?(?:void|bool|int|int32_t|u32|irqreturn_t)\s*\n?' + name + r'\s*\([^;]*?\)\s*\{', source)
    assert match, name
    brace = source.index('{', match.start())
    masked = re.sub(r'/\*[\s\S]*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"', lambda m: ' ' * len(m[0]), source[brace:])
    depth = 0
    for i, char in enumerate(masked):
        depth += (char == '{') - (char == '}')
        if not depth:
            return source[match.start():brace + i + 1] + '\n'
    raise AssertionError(name)

with tempfile.TemporaryDirectory(prefix='ax6000-wo-') as directory:
    temp = Path(directory)
    structs = ''
    for file, names in [('warp_utility.h', ('warp_dma_buf', 'warp_dma_cb', 'warp_bus_ring')),
                        ('mcu/warp_fwdl.h', ('fw_info_t', 'fwdl_region', 'warp_fwdl_ctrl')),
                        ('mcu/warp_woctrl.h', ('warp_wo_ctrl',)),
                        ('mcu/warp_wo.h', ('wo_ring_ctrl', 'wo_exception_ctrl', 'woif_bus', 'woif_entry')),
                        ('mcu/warp_ccif.h', ('ccif_ring_ctrl', 'ccif_entry'))]:
        source = (WARP / file).read_text()
        for name in names:
            structs += re.search(r'struct ' + name + r' \{[\s\S]*?\n\};', source)[0] + '\n'
    enums = (WARP / 'mcu/warp_woif.h').read_text()
    structs += '\n'.join(re.search(r'enum ' + name + r' \{[\s\S]*?\n\};', enums)[0] for name in ('wo_cmd_id', 'wo_state'))
    (temp / 'actual-wo-state.inc').write_text(structs)
    wo = (WARP / 'mcu/warp_wo.c').read_text()
    ccif = (WARP / 'mcu/warp_ccif.c').read_text()
    production = re.search(r'struct bus_dmad \{[\s\S]*?\n\} __packed __aligned\(32\);', wo)[0] + '\n'
    production += '\n'.join(re.search(r'static struct ccif_ring_ctrl ' + name + r' = \{[\s\S]*?\n\};', ccif)[0] for name in ('ccif_tx_ring', 'ccif_rx_ring'))
    production += '\n'.join(function(ccif, name) for name in ('ccif_init', 'ccif_activate', 'ccif_quiesce', 'ccif_pre_rx', 'ccif_post_rx', 'ccif_kickout', 'ccif_exit', 'ccif_wo_exception', 'ccif_clear_int', 'ccif_bus_register'))
    production += '\n'.join(function(wo, name) for name in ('tx_dma_cb_init', 'tx_dma_cb_exit', 'tx_ring_init', 'tx_ring_exit', 'rx_dma_cb_init', 'rx_dma_cb_exit', 'rx_ring_init', 'rx_ring_exit', 'bus_ring_init', 'bus_ring_exit', 'bus_rx_schedule', 'bus_rx_task', 'woif_bus_isr', 'woif_bus_exit', 'woif_bus_init', 'wo_exception_alloc', 'wo_exception_publish', 'wo_exception_exit', 'woif_reset_control', 'woif_checked_halt', 'woif_checked_enable', 'woif_boot_release', 'woif_dma_exposed', 'woif_prepare_exit', 'woif_init', 'woif_exit'))
    (temp / 'actual-wo.inc').write_text(production)
    command = [os.environ.get('CC', 'cc'), '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-Wno-unused-function', '-Wno-sign-compare', '-g', '-fsanitize=address,undefined', '-I' + str(temp), str(Path(__file__).with_name('test_wo.c')), '-o', str(temp / 'wo-test')]
    subprocess.run(command, check=True)
    subprocess.run([str(temp / 'wo-test')], check=True, env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0', 'UBSAN_OPTIONS': 'halt_on_error=1'})

    # Firmware leaf failures must survive every caller up to cold probe.
    fwdl = (WARP / 'mcu/warp_fwdl.c').read_text()
    header = (WARP / 'mcu/warp_fwdl.h').read_text()
    (temp / 'actual-fwdl-state.inc').write_text('\n'.join(re.search(r'struct ' + name + r' \{[\s\S]*?\n\};', header)[0] for name in ('fw_info_t', 'fwdl_region', 'warp_fwdl_ctrl')))
    (temp / 'actual-fwdl.inc').write_text('\n'.join(function(fwdl, name) for name in ('img_get_8bit', 'img_get_32bit', 'warp_fwdl_unmap_regions', 'warp_fw_init', 'warp_fwdl_mcu_mode', 'warp_fwdl_ready_check_mcu_mode')))
    command[-3:] = [str(Path(__file__).with_name('test_fwdl.c')), '-o', str(temp / 'fwdl-test')]
    subprocess.run(command, check=True)
    subprocess.run([str(temp / 'fwdl-test')], check=True, env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0', 'UBSAN_OPTIONS': 'halt_on_error=1'})
