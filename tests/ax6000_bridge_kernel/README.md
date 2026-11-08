Run from the worktree root:

```sh
UV_CACHE_DIR=/tmp/ax6000-bridge-uv-cache uv run --no-project tests/ax6000_bridge_kernel/run.py
```

The runner compiles the actual `kernel-next/net/core/mtk_ap_bridge.c`, the new
bridge checker extracted unchanged from `br_if.c`, and the actual three common
nf_flow entry points. It exercises admission, transactional failure, stale
leases, extension COW/inheritance, bridge/VLAN intent, concurrent drain and
publication refusal. It also compiles the fixed UAPI layout for 32- and 64-bit
callers. ASan and UBSan are enabled for the gate tests. LSan is disabled because
this execution environment uses ptrace; netdev/namespace reference balances are
checked explicitly.

The same runner also compiles actual HNAT CPU-mode register/zero-table helpers,
all 38 public API refusals, Ethernet physical-participant/STOP/lifecycle helpers,
all native RX reason values and the actual FE reset worker. It injects stop,
reset, restore and open failures and checks that failure cannot send a false
success notification. WO/CCIF tests compile the production allocation/map,
IRQ, BASE publication, checked HALT, active-low reset and shutdown functions;
every cold allocation/mapping/IRQ failure unwinds before exposure, while errors
after publication retain buffers. The host reset model checks software polarity,
readback and completion contracts; it does not prove a silicon reset is idle or
that firmware accepts a requested state transition.

`shim.h` provides bounded host adapters for kernel APIs. This verifies the real
helper algorithms, not actual Linux RCU scheduling, skb allocation internals,
netdevice ioctls, driver queues or hardware. The exact prepared Linux kernel has also been built locally through ARM64
vmlinux/modules and real Module.symvers using GCC 13.3. This local configuration
is a diagnostic package subset; full OpenWrt CI with the production GCC 14.3
configuration and hardware acceptance remain separate requirements.
