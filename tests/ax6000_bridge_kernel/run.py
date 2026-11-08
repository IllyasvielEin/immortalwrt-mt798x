#!/usr/bin/env python3
"""Compile actual helper code with bounded host API adapters; no router access."""
from pathlib import Path
import os
import re
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
kernel = Path(os.environ.get('MTK_AP_BRIDGE_KERNEL_TREE', root / '.analysis/kernel-next'))
with tempfile.TemporaryDirectory(prefix='ax6000-bridge-kernel-') as directory:
    temporary = Path(directory)
    inc = temporary / 'include'
    for name in ('atomic', 'capability', 'err', 'etherdevice', 'if_bridge',
                 'if_vlan', 'mtk_cpu_profile', 'netdevice', 'rcupdate',
                 'rtnetlink', 'skbuff', 'slab', 'spinlock', 'uaccess', 'wait', 'types'):
        file = inc / 'linux' / (name + '.h')
        file.parent.mkdir(parents=True, exist_ok=True)
        file.write_text('#include "shim.h"\n')
    file = inc / 'net/net_namespace.h'
    file.parent.mkdir(parents=True, exist_ok=True)
    file.write_text('#include "shim.h"\n')
    bridge = (kernel / 'net/bridge/br_if.c').read_text()
    marker = '\n#ifdef CONFIG_MTK_AP_BRIDGE\n'
    assert bridge.count(marker) == 1
    (temporary / 'bridge_checker.inc').write_text(bridge.split(marker, 1)[1].rsplit('#endif', 1)[0])
    command = [os.environ.get('CC', 'cc'), '-std=gnu11', '-Wall', '-Wextra', '-Werror',
               '-g', '-fsanitize=address,undefined', '-pthread',
               '-I' + str(inc), '-I' + str(Path(__file__).parent),
               '-I' + str(kernel / 'include'), '-I' + str(temporary),
               '-DMTK_AP_SOURCE="' + str(kernel / 'net/core/mtk_ap_bridge.c') + '"',
               str(Path(__file__).parent / 'test.c'), '-o', str(temporary / 'test')]
    subprocess.run(command, check=True)
    subprocess.run([str(temporary / 'test')], check=True,
                   env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0', 'UBSAN_OPTIONS': 'halt_on_error=1'})
    # UAPI structure sizes/offsets must agree under both outer ifreq ABIs.
    abi_inc = temporary / 'abi-include/linux'
    abi_inc.mkdir(parents=True)
    (abi_inc / 'types.h').write_text('''
typedef unsigned char __u8; typedef unsigned short __u16;
typedef unsigned int __u32; typedef int __s32;
typedef unsigned long long __u64;
#define __aligned_u64 __u64 __attribute__((aligned(8)))
''')
    (temporary / 'abi.c').write_text('''
#include <uapi/linux/mtk_ap_bridge.h>
_Static_assert(sizeof(struct mtk_ap_bridge_domain) == 2112, "domain");
_Static_assert(sizeof(struct mtk_ap_bridge_request) == 2184, "request");
_Static_assert(__builtin_offsetof(struct mtk_ap_bridge_request, owner) == 16, "owner");
_Static_assert(__builtin_offsetof(struct mtk_ap_bridge_request, domain) == 72, "domain offset");
''')
    for bits in (32, 64):
        subprocess.run([command[0], '-m' + str(bits), '-ffreestanding', '-fsyntax-only',
                        '-I' + str(abi_inc.parent), '-I' + str(kernel / 'include'),
                        str(temporary / 'abi.c')], check=True)
    print('UAPI layout: 32-bit and 64-bit compile checks passed')
    def function(source, name):
        match = re.search(r'^(?:int |void )?' + re.escape(name) + r'\(', source, re.M)
        assert match is not None
        start = match.start()
        # lookup has its return type on the preceding line.
        if name == 'flow_offload_lookup':
            start = source.rfind('\n', 0, start - 1) + 1
        opening = source.index('{', start)
        depth = 1
        end = opening + 1
        while depth:
            depth += (source[end] == '{') - (source[end] == '}')
            end += 1
        return source[start:end] + '\n'
    flow = (kernel / 'net/netfilter/nf_flow_table_core.c').read_text()
    functions = ''.join(function(flow, name) for name in
                        ('flow_offload_add', 'flow_offload_refresh', 'flow_offload_lookup'))
    (temporary / 'flow_functions.inc').write_text(functions)
    subprocess.run([command[0], '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    '-I' + str(temporary), str(Path(__file__).parent / 'test_flow.c'),
                    '-o', str(temporary / 'test-flow')], check=True)
    subprocess.run([str(temporary / 'test-flow')], check=True)
    hnat = kernel / 'drivers/net/ethernet/mediatek/mtk_hnat'
    header = (hnat / 'hnat.h').read_text()
    # Register constants come from the production header, not a second map.
    definitions = header[header.index('#define PPE_GLO_CFG'):header.index('/* default binding threshold:')]
    (temporary / 'hnat_registers.inc').write_text(definitions)
    cpu = (hnat / 'hnat_cpu_profile.c').read_text()
    def cpu_function(name):
        match = re.search(r'^static int ' + name + r'\(', cpu, re.M)
        assert match is not None
        start = match.start()
        opening = cpu.index('{', start)
        depth, end = 1, opening + 1
        while depth:
            depth += (cpu[end] == '{') - (cpu[end] == '}')
            end += 1
        return cpu[start:end] + '\n'
    (temporary / 'hnat_helpers.inc').write_text(''.join(cpu_function(name) for name in
                                             ('cpu_write', 'cpu_gmacs', 'cpu_ppe', 'hnat_cpu_restore')))
    subprocess.run([command[0], '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    '-Wno-misleading-indentation', '-I' + str(temporary),
                    str(Path(__file__).parent / 'test_hnat.c'), '-o',
                    str(temporary / 'test-hnat')], check=True)
    subprocess.run([str(temporary / 'test-hnat')], check=True)
    api = (hnat / 'hnat_cpu_api.c').read_text()
    (temporary / 'hnat_api.inc').write_text('\n'.join(line for line in api.splitlines()
                                                   if not line.startswith('#include')))
    calls = []
    function_pattern = re.compile(r'^(int|void|struct \w+\s*\*)\s*(\w+)\(([^{}]*?)\)\n\{', re.M)
    for match in function_pattern.finditer(api):
        result, name, arguments = match.groups()
        # Split parameter declarations at commas outside callback parentheses.
        depth, parameters, current = 0, [], ''
        for char in arguments:
            depth += (char == '(') - (char == ')')
            if char == ',' and depth == 0:
                parameters.append(current)
                current = ''
            else:
                current += char
        parameters.append(current)
        args = ','.join('0' for argument in parameters if argument.strip() not in ('', 'void'))
        call = name + '(' + args + ')'
        if result == 'void':
            calls.append(call + ';')
        elif result.startswith('struct foe_entry'):
            calls.append('assert(PTR_ERR(' + call + ')==-EOPNOTSUPP);')
        elif result.startswith('struct'):
            calls.append('assert(!' + call + ');')
        elif name == 'is_entry_binding':
            calls.append('assert(' + call + '==0);')
        else:
            calls.append('assert(' + call + '==-EOPNOTSUPP);')
    (temporary / 'test-api.c').write_text('''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
struct sk_buff; struct net_device; struct ethhdr; struct foe_entry;
struct hnat_tuple; struct hnat_accounting; struct mtk_hnat; struct npu_hnat_mcast_ops;
#define EXPORT_SYMBOL(n)
#define ERR_PTR(e) ((void *)(intptr_t)(e))
#define PTR_ERR(p) ((int)(intptr_t)(p))
#include "hnat_api.inc"
int main(void) {
''' + '\n'.join(calls) + '\nputs("HNAT public API: ' + str(len(calls)) +
        ' actual functions reject safely with null/zero inputs"); return 0; }\n')
    subprocess.run([command[0], '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-parameter', '-I' + str(temporary),
                    str(temporary / 'test-api.c'), '-o', str(temporary / 'test-api')], check=True)
    subprocess.run([str(temporary / 'test-api')], check=True)
    reset = (kernel / 'drivers/net/ethernet/mediatek/mtk_eth_reset.c').read_text()
    eth = (kernel / 'drivers/net/ethernet/mediatek/mtk_eth_soc.c').read_text()
    def named_function(source, name):
        match = re.search(r'^(?:static )?(?:int|void|bool) ' + name + r'\(', source, re.M)
        assert match is not None
        opening = source.index('{', match.start())
        depth, end = 1, opening + 1
        while depth:
            depth += (source[end] == '{') - (source[end] == '}')
            end += 1
        return source[match.start():end] + '\n'
    (temporary / 'eth_helpers.inc').write_text(''.join(named_function(reset, name) for name in
        ('mtk_cpu_control_permitted', 'mtk_cpu_wifi_participant_register', 'mtk_cpu_wifi_participant_set_live', 'mtk_cpu_wifi_participant_unregister',
         'mtk_cpu_wifi_stop_prepare', 'mtk_cpu_wifi_stop_result', 'mtk_cpu_wifi_stop_wait')))
    (temporary / 'eth_rx.inc').write_text(named_function(eth, 'mtk_cpu_rx_consume'))
    reasons = (hnat / 'nf_hnat_mtk.h').read_text()
    (temporary / 'eth_reasons.inc').write_text('\n'.join(line for line in reasons.splitlines()
        if re.match(r'#define (HIT_BIND_|UN_HIT|HIT_UNBIND|PKT_FWD_WITHOUT_PPE)', line)))
    subprocess.run([command[0], '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-pthread',
                    '-I' + str(temporary), '-I' + str(kernel / 'include'),
                    str(Path(__file__).parent / 'test_eth.c'), '-o',
                    str(temporary / 'test-eth')], check=True)
    subprocess.run([str(temporary / 'test-eth')], check=True)
    (temporary / 'eth_pending.inc').write_text(named_function(eth, 'mtk_cpu_pending_work'))
    subprocess.run([command[0], '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    '-I' + str(temporary), str(Path(__file__).parent / 'test_pending.c'),
                    '-o', str(temporary / 'test-pending')], check=True)
    subprocess.run([str(temporary / 'test-pending')], check=True)

# Actual WARP allocation/publication/control stop helpers use separate OS-edge adapters.
subprocess.run([__import__("sys").executable, str(Path(__file__).with_name("test_wo.py"))], check=True)
