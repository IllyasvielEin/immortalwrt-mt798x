#!/usr/bin/env python3
"""Compile the complete production warp_msg.c and its actual message structs.

OS edges are pthread/refcount/transport adapters; queue/node code is not copied.
Production synchronous callback users checked: external WARP warp_proc.c and
mt_wifi embedded/plug_in/warp_proxy/warp_proxy.c (&cmd_id is stack-owned).
No WAIT_NONE+handler call was found in external WARP or production proxy source;
that unsupported combination is tested as an explicit API error.
"""
from pathlib import Path
import os
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[2]
WARP = Path(os.environ.get('MTK_AP_BRIDGE_WARP_TREE', ROOT / '.analysis/warp-next'))
with tempfile.TemporaryDirectory(prefix='ax6000-msg-') as directory:
    temp = Path(directory)
    (temp / 'linux').mkdir()
    for name in ('warp.h', 'warp_wo.h', 'warp_meminfo_list.h'):
        (temp / name).write_text('/* OS integration declarations provided by test_msg.c. */\n')
    (temp / 'warp_utility.h').write_text('/* Actual types use host OS adapters declared by test_msg.c. */\n')
    for name in ('kthread.h', 'refcount.h'):
        (temp / 'linux' / name).write_text('/* Host OS adapter. */\n')
    (temp / 'warp_evt.h').write_text('''
#ifndef TEST_WARP_EVT_H
#define TEST_WARP_EVT_H
#define MSG_Q_UNKNOWN 0
#define MSG_Q_INITED 1
struct warp_evt_hdlr_queue { int unused; };
#endif
''')
    # These are byte-for-byte copies, not a synthetic queue implementation.
    for name in ('warp_msg.c', 'warp_msg.h', 'warp_cmd.h'):
        (temp / name).write_bytes((WARP / 'mcu' / name).read_bytes())
    command = [os.environ.get('CC', 'cc'), '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra',
               '-Werror', '-Wno-unused-parameter', '-Wno-unused-function', '-Wno-unused-variable',
               '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-pthread',
               '-I' + str(temp), str(Path(__file__).with_name('test_msg.c')),
               '-o', str(temp / 'test-msg')]
    if 'refcount_t refs;' not in (WARP / 'mcu/warp_msg.h').read_text():
        # Old production still runs the same first claim/timeout schedule.
        # Only new-only accounting assertions are disabled for compilation.
        command[1:1] = ['-DTEST_WARP_MSG_OLD=1', '-Wno-error=pointer-sign', '-Wno-error=sign-compare']
    subprocess.run(command, check=True)
    subprocess.run([str(temp / 'test-msg')], check=True, timeout=30,
                   env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0:detect_stack_use_after_return=1',
                        'UBSAN_OPTIONS': 'halt_on_error=1'})
