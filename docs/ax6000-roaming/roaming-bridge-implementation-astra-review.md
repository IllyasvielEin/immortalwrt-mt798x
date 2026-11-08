# AX6000 通用桥转发：实际实现独立审查

2026-11-09，Asia/Singapore。

**裁决：NO-GO。当前实现有两项 P1 阻断，应修复并复审后再推送隔离分支进入一次 CI 编译。另有一项 P3 非阻断问题。** 本裁决仅针对该步骤，不是刷机、正式发布、硬件故障体验或性能验收结论。没有将既定 CPU 转发代价、未知 DMA 停止时的终态 B，或尚未实测本身列为阻断。

## 1. 审查对象和执行边界

- 唯一工作目录：`/home/illya/workspace/router/ax6000/worktrees/bridge-forwarding`。
- 分支：`fix/ax6000-bridge-forwarding`；实际 HEAD：`550fa6ebee3af2b985cdc39cba228130196d059d`。
- 实现范围：`4a0c52dd7129ac9e90b9374f12c55c01cce8059c..692a32bd`，四个提交为 `48d6b2c7`、`b1643cc3`、`b0971266`、`692a32bd`。其后 `550fa6eb` 仅记录前次审核中止，没有新的实现修改。
- 生产基线：`c196312d3916fad77fa0bb47db57e4b685c15f03`。此次使用 worktree 内准确的 `.analysis/{kernel,driver,warp,netifd}-{base,next}`，并读取既有构建与补丁重放材料；未写原 `sources`。
- 先读 `PROGRESS.md`、`roaming-bridge-final-astra-review-r3-1.md`，再核主设计和 WARP STOP 合同。设计 GO 只作为预期行为，不作为实现通过的证据；前次平台过滤没有技术结论。
- 未联网、操作设备、启动子代理、修改实现、提交、推送、触发或查询 CI。只新增本报告；现有宿主测试的临时编译产物由测试自身管理。未重复完整 ARM64 构建。

下文源码简称全部以本 worktree 为根：K=`.analysis/kernel-next`；D=`.analysis/driver-next/mt_wifi`；W=`.analysis/warp-next`；N=`.analysis/netifd-next`。行号指当前真实 postimage，不是旧设计报告中的生产基线行号。每项问题另给出对应生产补丁位置。

## 2. 阻断问题

### P1-1：WO 响应在超时边界仍可能访问已释放的等待节点

**主要位置：** [W/mcu/warp_msg.c:609](/home/illya/workspace/router/ax6000/worktrees/bridge-forwarding/.analysis/warp-next/mcu/warp_msg.c:609)、[W/mcu/warp_msg.c:828](/home/illya/workspace/router/ax6000/worktrees/bridge-forwarding/.analysis/warp-next/mcu/warp_msg.c:828)。新停止协议的实际调用者是 [W/mcu/warp_woctrl.c:503](/home/illya/workspace/router/ax6000/worktrees/bridge-forwarding/.analysis/warp-next/mcu/warp_woctrl.c:503)，对应 [WARP 生产补丁:1577](/home/illya/workspace/router/ax6000/worktrees/bridge-forwarding/package/mtk/drivers/warp/patches/100-ax6000-checked-warp-ser.patch:1577)。

**触发条件：** 正常 WO 应答在 `WARP_MSG_WAIT_TYPE_RSP_STATUS` 的 timeout 边界到达，CCIF RX tasklet 与等待该应答的 HwCtrl 线程在不同 CPU 上交错。不需要异常报文，也不以某种未证实的硬件 busy 位语义为前提。

真实可执行顺序为：

1. 新 `warp_checked_wo()` 以剩余预算调用 `warp_msg_send_cmd()`；STOP 的 SER_RESET、RESUME 的 ENABLE 都使用此同步应答路径，`warp_set_rrocfg()` 也使用同一消息设施。
2. CCIF 的 `bus_rx_task()` 经 `warp_cmd_process_wo_ring()` 调到 `warp_msg_handle_rsp()`。后者在 W `mcu/warp_msg.c:609` 调用 `warp_msg_get_wait_node_by_uni_id()`；该 helper 在 `:464–473` **先从队列摘下 node，再释放 wait_lock，返回裸指针**。处理方随后还要访问 node 的 handler、wait_type、rsp_status 和 completion。
3. 此时发送线程在 `warp_msg_wait_rsp():554–558` 超时。`warp_msg_send_cmd():832–836` 再按 ID 摘取时已经找不到 node，却仍无条件对自己保存的 node 调用 `warp_msg_wait_node_deinit()`。
4. `warp_msg_wait_node_deinit():343–346` 清零并释放 node。响应方继续在 `:617–640` 读写同一对象或调用 `complete(&node->recv_done)`，形成 use-after-free。

**实际影响：** 新 checked STOP/RESUME 的超时出口可能在进入预期失败终态前发生内核内存破坏或崩溃。WARP control mutex、请求 permission/cancelled 和 D/E 析构许可均未保护消息等待节点，不能消除这个交错；“仅零为成功”和返回 `-ETIMEDOUT` 也不解决所有权问题。

这是 **base 中已存在、此次新停止协议直接依赖而未闭合的下层竞态**，不是声称这次只改 timeout errno 就新造了该 bug。r3.1 明确要求核对 WO timeout/迟到响应及实际 caller→leaf 生命周期，因此它属于本次实现验收的阻断。

**最小修正：** 在 wait_lock 下明确等待方、队列和响应方的节点所有权；例如摘取时取得响应方引用，发送方超时仅撤销仍排队的请求并释放自己的引用，响应方完成后释放其引用。响应已经认领时也不能让超时方直接 free。同步覆盖发送失败和队列退出；不要用无限等待迟到应答修补有界 timeout。增加使用真实 `warp_msg.c` 的并发测试，固定“响应摘队后、写状态前”与 timeout/free 的交错，以及 timeout 先摘队的相反顺序。

**证据限制：** 此项由真实源码交错确认，未在路由器运行。现有 WARP 测试的 `warp_msg_send_cmd` 是替身：`tests/ax6000_bridge_warp/test.c:102` 的 late-response 场景推进模拟时间，`tests/ax6000_bridge_kernel/test_wo.c:141` 直接返回预设结果；两者均不执行上述真实等待队列和释放逻辑。现有测试通过不能排除此项。

### P1-2：没有配置变化的 network reload 会关闭 AP gate，却没有恢复事务

**主要位置：** [N/device.c:1217](/home/illya/workspace/router/ax6000/worktrees/bridge-forwarding/.analysis/netifd-next/device.c:1217)、[N/bridge.c:1099](/home/illya/workspace/router/ax6000/worktrees/bridge-forwarding/.analysis/netifd-next/bridge.c:1099)，对应 [netifd 生产补丁:758](/home/illya/workspace/router/ax6000/worktrees/bridge-forwarding/package/network/config/netifd/patches/900-ax6000-transactional-ap-bridge.patch:758)。

**触发条件：** 已成功 COMMIT、无线状态为 up 的普通 AP，作为 hotplug 成员加入一个正常工作的、无 bridge-vlan 配置的桥；待初次激活的 timer 已结束后，执行配置内容未变化的网络 reload。无线配置、接口 ifindex 和成员均保持不变。仓库真实 `package/network/config/netifd/files/etc/init.d/network:32` 调用 `ubus call network reload`，经 N `ubus.c:43`、`main.c:276` 进入以下路径，没有额外无线 reconf 兜底。这里描述的是可执行的软件路径，本轮未实际执行网络 reload。

真实调用链为：

1. N `config.c:787` 的 `config_init_all()` 重新创建/应用已有设备配置，`device_create():1319` 进入 `device_apply_config()`。
2. 新 `device_apply_config():1217` **在判断配置差异之前** 调用 `device_prepare_change(dev, false)`。对 bridge，`bridge_prepare_change():1099–1101` 遍历全部成员并 PREPARE AP。N `ap-bridge.c:119` 的 GET/PREPARE 最终执行 K `net/core/mtk_ap_bridge.c:506`，令 gate `open=false` 并等待 reader 排空。
3. 没有变化的 bridge reload 仍走 `DEV_CONFIG_APPLIED`，设置 `config_pending`；后续 `bridge_config_init():1237–1250` 只更新成员列表。同一个旧成员在 `bridge_member_update():1002–1003` 直接返回，原 hotplug 成员也被保留。没有成员/VLAN/无线策略事件，因而不调用 `ap_bridge_reconcile()`。
4. N `ap-bridge.c:103–123` 的 PREPARE 本身不安排重新 COMMIT；唯一相关 timer 由 `ap_bridge_reconcile():298–305` 显式排入。`device_apply_config()` 和本次 `bridge_config_init()` 结束时均未补上这个步骤。
5. 无线层也不会兜底：`wireless-device.uc:400–403` 对相同配置直接返回；`config_start()` 虽调用 `start()`，但 [wireless-device.uc:427](/home/illya/workspace/router/ax6000/worktrees/bridge-forwarding/package/network/config/wifi-scripts/files/lib/netifd/wireless-device.uc:427) 对仍为 up 的设备立即返回，不再走 `wdev_mark_up()`/COMMIT。

**实际影响：** 用户态无线状态仍是 up，内核 gate 却长期 BLOCKED，所有该 BSS 的普通数据及受 gate 控制的数据 EAPOL 被拒绝。需要另一次确实进入完整无线激活流程的操作才能恢复。它影响普通维护操作，且与漫游目标无关的终端也会断流。对于显式配置但无变化的 simple AP device，`DEV_CONFIG_NO_CHANGE` 分支同样缺少准备后的收尾。

**最小修正：** 给非无线 controlled 事务发起的设备/桥配置预检配套完整收尾：在本轮所有配置、成员和 VLAN 更新完成之后，对本轮已 PREPARE 且原先 committed 的实际 AP owner 安排一次完整快照重建与 COMMIT；配置失败继续阻流并保留重试。或在无副作用的差异判断证明完全不变时避免 PREPARE。不能只对 bridge 调 `ap_bridge_reconcile(bridge)`，因为 owner 是成员 AP；也不能把裸 `READY` 恢复当作 COMMIT。避免在修改过程尚未完成时提前放行。

**必要回归：** 用真实 bridge/device/ap-bridge 调用链覆盖“已经 up → 无变化 network reload → 重新确认 READY/新有效 lease”，以及有多个 BSS/VLAN、只改无关设备、部分失败继续阻流、修正配置后重试。现有 `test_netifd_lifetime.c` 将 `device_set_config` 固定替换为 `DEV_CONFIG_RECREATE`，其 free/replace 测试没有经过本条普通 reload 路径；ucode 测试中的 `netifd.ap_prepare/ap_commit` 同样是控制接口替身。

## 3. 非阻断问题

### P3-1：禁用 fq_en 的返回值被实际 iwpriv setter 当作成功

**位置：** [D/embedded/common/fq_qm.c:1030](/home/illya/workspace/router/ax6000/worktrees/bridge-forwarding/.analysis/driver-next/mt_wifi/embedded/common/fq_qm.c:1030)，对应 [无线生产补丁:2343](/home/illya/workspace/router/ax6000/worktrees/bridge-forwarding/package/mtk/drivers/mt_wifi/patches-7673/044-ax6000-universal-ap-bridge.patch:2343)。实际命令注册在 D `embedded/ap/ap_cfg.c:2460`，名称是 `fq_en`；判错调用者在 [ap_cfg.c:3661](/home/illya/workspace/router/ax6000/worktrees/bridge-forwarding/.analysis/driver-next/mt_wifi/embedded/ap/ap_cfg.c:3661)。

**触发/影响：** CPU profile 下调用该命令，`set_fq_enable()` 返回 `-EOPNOTSUPP`，但调用者按 `if (!set_proc(...))` 判断 FALSE 失败，负值被当作 TRUE。因此命令表面成功、设置实际未执行。运行时队列替换确实被挡住，不会因此重新开放释放路径；不影响软件 HTB/IFB eQoS，所以不阻断本轮 CI 准入。

**最小修正：** 在这个 BOOLEAN-style setter 返回 `FALSE`，由已有 ioctl 层转换为 `-EINVAL`；若需要精确的 `EOPNOTSUPP`，须显式调整对应分派返回约定。测试应经过分派层，而不只是断言叶函数返回负值。

## 4. 已检查实现范围与其他核验结果

已读全部六个生产补丁的变更文件，并沿以下真实 postimage 调用链核对；没有用宿主测试代替源码审查。

| 范围 | 已检查内容与结论边界 |
| --- | --- |
| `999-zzz-5400` | UAPI 固定布局、cookie 不复用、gate 默认关闭、reader drain、copyout 后重新检查再发布、skb extension/COW/继承；bridge RTNL 快照和 RCU 查询、直接 VLAN ancestry、metadata/帧内标签解析、namespace 固定；nf_flow add/lookup/refresh 以及 nft/xt 入口禁用。未发现另一个足以列为阻断的具体路径；真实 RCU/拓扑竞态仍须后续环境验证。 |
| `999-zzz-5401` | HNAT Kbuild 替换为 CPU profile/API 两个对象，分配/清零先于地址暴露，零表检查、寄存器首错和读回、GMAC→PDMA、永久 callback/context；全部导出资源型接口拒绝或保持 NULL，禁 force-unload/OF overlay。核对没有先启动旧 HNAT producer 再补关。 |
| `999-zzz-5402` | participant 身份与 live 判定、STOP 唯一 ACK、失败不依赖 stop_waiting、FE/Wi-Fi/teardown 准入、fatal 原子选举；PDMA/QDMA disable+busy 检查先于 free、warm reset 完整掩码与错误传播、restore 先于内部 open、后续 open 失败回滚、无假 DONE；native RX reason 消费/补环及实际 ndo TC 硬件拒绝。 |
| 无线 `044` 的数据面 | `check_rx_pkt_allowed()` 在 AP ops 配置读取前进入 reader；实际 `ap_rx_pkt_allowed()` 的 Class2/3、Radius 检查，以及两个 announce 中的真实 peer/TR、ToDS/FromDS、PrivacyFilter、PortSecured、聚合控制例外。保留原解密/PN/MIC、去重与分片检查链。common announce 的 late reader 在 MAP/VLAN 读取前，桥域直接进入一次 Linux handoff，跳过旧 DA 内转和私有快转。 |
| 无线聚合与出口 | SW A-MSDU 的 `ClonePacket()` 使用 `skb_clone` 继承 extension；Ralink 第二 MSDU、分片缓存、额外 skb 分配有显式继承/旧 lease 核验；BA 出队使用 late reader 而非补发当前许可。TX 首次进入先 stamp 原域，`MacTableLookup2()` 按原 wdev 查找；qm/fp_qm/fp_fair_qm、ACK/PS/BMC 到共同 `ap_tx_pkt_handle()` 的逐 MSDU 校验，以及禁止按旧 TR 改 wdev 的落点均已核对。实际无线重排序/加密/聚合输出未实测。 |
| 无线控制与生命周期 | isolation/VLAN/MAP/MWDS setter 的配置互斥、注册前 gate/netns_local、NF_BR_PRE_ROUTING/FORWARD、hook 退出同步；HwCtrl FE 与自主 SER、watchdog/ABORT、HIF/qm/tm 初始化与退出、token 预约；真正被选中的 rbus frontend、close/free/adapter、proxy module 引用及 SYS_DOWN 交接。fq_en 返回值问题见 P3-1。 |
| WARP `100` | 实际 external v2/MT7986 路径；producer gate、timer/tasklet 排队和重排、检测线程、proc、DMA_SET、suspend/resume；stop-only 检查、cleanup 前复核停止及 reset completion、准备/enable 分离、proof 撤销。检查实际 register map：普通禁用访问返回 handled，不落回 native MMIO；BIT29 N9 控制状态/掩码保留，WO/CCIF 应答通道不随数据 gate 一起盲关。WO、CCIF、FWDL、BM/WDMA/WED 分配、映射、暴露、退出和真实消息调用者均纳入；消息节点竞态见 P1-1。 |
| netifd `900` | ap-bridge GET/PREPARE/STAGE/COMMIT、outer ioctl 不确定后的新 PREPARE；bridge port 显式 0/1 写回和读回、完整 VLAN 快照、父/子策略、free/replace/失败成员保留、重试路径；C→ucode→wireless-device→MTK notify/config/driver 的实际连接。普通无变更 reload 漏收尾见 P1-2。 |
| 用户态与 eQoS | MTK DBDC 所有受影响 AP 先 PREPARE 再写 DAT/ifdown，错误向上抛出；mode 文件按产品 feature 安装。eQoS CLI/init/LuCI/翻译/ACL 一起核对：规则 1–65533、父类 `1:fffe`、默认叶 `1:ffff`，零单设备限速回默认叶、IPv4/IPv6 u32/mirred `skip_hw`，CPU 分支避开 HNAT/QDMA/turboacc 写入。未承诺 br-lan qdisc 覆盖全部纯桥流量。 |
| 产品与 CI | 仅真实 AX6000 MTK U-Boot 单设备 defconfig 选择能力，排除 mtkhqos_util，未把历史 netcore_n60/multi-profile 文件误当本产品。检查 kernel Kconfig、包配方真实 patch 目录、mode 打包、实际产物/config/symbol 校验和工作流宿主测试入口。HNAT 包已有 `BRIDGE_NETFILTER=y`/`NETFILTER_FAMILY_BRIDGE=y` 设置；本地真实 WARP 模块仍声明对 mtkhnat 的依赖，不能仅因 D 移除 HNAT hooks 推断启动必定漏加载。 |

其他产品主要由 `CONFIG_MTK_AP_BRIDGE` 或 mode 文件分支保留旧行为，但 WARP 的 CCIF/FWDL/部分分配清理、netifd 的通用设备/策略处理、ucode 错误传播和 tc `skip_hw` 也存在无条件改动。已检查这些差异，没有仅凭“共享文件改变”另列阻断；**没有构建或运行其他 SoC/产品，不能据 AX6000 链接成功宣称其他产品已验收。**

## 5. 本轮验证及其局限

本轮以 `UV_OFFLINE=1`、`uv run --no-project` 运行仓库现有宿主测试，缓存限定于 `/tmp`，没有下载或安装依赖：

- `tests/ax6000_bridge_kernel/run.py`：通过。包含 123 项实际 bridge helper 检查、32/64 位 UAPI 编译、nf_flow 三入口、HNAT 寄存器/首错与 38 个 API、Ethernet STOP/native reason/真实 FE worker，以及 WO/CCIF 和 FWDL helper 故障注入。
- `tests/ax6000_bridge_lifecycle/run.py`：通过。实际 value-state 转移、IRQ/抢占先于选举、唯一 reset 执行者以及底层 reset 返回后不进入正常 continuation。
- `tests/ax6000_bridge_warp/run.py`：通过。实际 control/MMIO/helper 路径、15 个 busy 位、取消/proof、173 项 proxy/IO/permission 检查。
- `tests/ax6000_bridge_driver/run.py`：通过。79 项 wire/binding、33 项源授权，320 个并发 token 请求产生 127 个唯一有效 slot。
- `test_control.py` 的 12 项及 `test_ucode.py` 的 8 项：分别运行，全部通过。后者使用本地已编解释器及插件，不连接真实 ubus。
- `git diff --check 4a0c52dd..692a32bd`：通过。

核对了 `.analysis/production-patch-replay.json`，并重新计算全部六个生产补丁的 SHA256，与该记录一致。记录为新补丁 fuzz=0、postimage 无差异；本轮未重复完整重放，不将原生产补丁系列也称为 fuzz=0。读取既有 `kernel-final-build.log`、`warp-final-build.log`、`driver-final-modules-build.log`，尾部有真实 vmlinux/module 链接和 MODPOST 完成；这些是既有本地证据，不是本轮重新构建。顶层 72 项、因缺 locked feed 跳过 1 项的结果沿用 PROGRESS 记录，本轮未重新执行该组。

上述 host adapters 没有模拟完整 Linux 调度、真实 sk_buff/RCU/驱动队列、netifd 全进程事件循环或硬件。尤其 P1-1 的消息节点所有权、P1-2 的完整网络 reload 不在它们目前的实际覆盖中。P3-1 的叶函数测试也没有经过 BOOLEAN-style ioctl 分派。不能把这些通过记录解释为三个问题已经验证无误。

当前 CI 新步骤只发现 `test_control.py`（`.github/workflows/build-redmi-ax6000.yml:266`），并未执行本轮另外通过的 8 项 `test_ucode.py`。后续应准备对应宿主 ucode/plugin 后纳入持续验证，或明确保留其独立执行记录，不能把该 CI 步骤描述为已包含全部 20 项控制测试。

## 6. r3.1 故障终态与剩余验收门

终态 B 的主要实现与批准设计一致：不能确认停流、停止证明失效或不能可靠拒绝的析构/SER 交叉，走 builtin Ethernet 的同步 `__noreturn` 入口；先关闭本 CPU 中断/抢占再原子选举，直接 `machine_restart(NULL)`，有实际不返回兜底。它没有依赖普通 shutdown 链，也没有把 ndo_stop 的 errno 当作可靠拒绝析构。静止证明仍有效的失败与未暴露的冷分配失败有不同处理路径。

正常 WO 关闭在 `woif_checked_halt()` 中清旧状态、发送 HALT、等待新鲜 HALT_DONE，再确认 active-low reset、停止 CCIF 并释放资源；正常来源是 ENABLE。代码也接受 SER_RESET 作为失败后关闭的来源，但**没有把发出请求当作完成**：仍须新鲜 HALT_DONE，否则失败并走终态。目标固件是否支持该异常状态转换、HALT_DONE/状态应答/busy 位是否足以证明目标 DMA 静止，仍是已公开的硬件门，不作为未经验证的新事实。P1-1 则是它之前的确定软件生命周期缺口，不能推迟给硬件验证。

修复两项 P1 后，进入一次 CI 前至少需要针对真实消息节点并发和实际 netifd reload 收尾补上回归，并核修正未改变停止/阻流失败的安全出口。P3 可单独修正。之后仍需：

1. **完整生产 CI：** GCC 14.3 的 OpenWrt prepare/config/build/image，实际内核、D/W/proxy/netifd/ucode/rootfs 和模块依赖；核 force-unload/overlay/解绑限制、skb extension、RRM/WNM/FT、external WARP 与 rbus 选择。现有 GCC 13.3 和手工组合内核配置只是诊断证据。共享改动另做相关其他产品的构建/回归。
2. **另获授权的硬件验收：** 冷启动、正常在线 L1、正常关闭/重开与失败恢复；FOE 地址/空表/固定 CPU profile、无 BIND/cache/private producer、基础 WED/token/crypto/reorder、WO/CCIF 应答及停止/reset 实际完成、FE reset 后不误用旧 proof。核 PSCI/WDT/ATF 的真实复位范围与完成，保留丢未落盘状态和必要时断电恢复的代价。
3. **转发验收：** 任意普通终端漫游后，旧 STA 尚存时核 FDB、桥选端口、物理出口和业务回包；IPv4/IPv6、ARP/ND、非 IP L2、group、同 MAC 不同 VID、VLAN/隔离、BA/SW/HW 聚合、动态 reload 及失败重试均需验证。重新启动后先重新读取实际状态，不能复用旧关联/FDB 当当前事实。
4. **性能/eQoS：** 有线、无线、LAN/WAN、桥转发的吞吐、CPU、时延、重配置停顿；HTB/IFB 总带宽、默认叶、零设备限速、规则边界和 stop/restart。当前没有性能数据，不能承诺原峰值或给出损失百分比。

本报告没有批准推送当前 HEAD。修正上述两项阻断并完成针对性复核，才具备重新判断 CI 准入的基础；其后也不自动获得刷机或发布许可。
