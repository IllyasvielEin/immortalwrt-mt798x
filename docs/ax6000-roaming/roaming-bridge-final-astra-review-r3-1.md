# 通用桥转发 r3.1：Astra 独立关键设计复审

2026-11-09。送审文档快照 `4a0c52dd7129ac9e90b9374f12c55c01cce8059c`，原 r3 文档快照 `0de772dce7dbbe27f73a793e72f6ed0927abf1df`，固件源码基线 `c196312d3916fad77fa0bb47db57e4b685c15f03`。工作区 `/home/illya/workspace/router/ax6000/worktrees/bridge-forwarding`，分支 `fix/ax6000-bridge-forwarding`。

**裁决：GO，允许按 r3.1 在上述隔离分支进入实施。上轮唯一剩余 P1——实际 WARP STOP 吞错、未证明停流即清理并成功 ACK——在设计层面关闭。** 原 P1-1、P1-3、P1-4 的设计关闭结论维持。下列 P2 是实现和实现复审的必做要求，不要求先增加另一套设计框架，也不是延期处理原 P1。

批准对象是完整的 checked STOP、终态 B 和真实析构准入合同；不能只做返回值改造而省略后两者。当前源码尚未实现这些规则，仍存在本文引用的旧反例。本次 GO 不表示代码、构建、CI、刷机、硬件复位、故障体验或性能已经验收；也不增加本次只读任务的执行权限。

## 1. 核验范围与依据

先读了 [上轮独立报告](roaming-bridge-final-astra-review-r3.md)，再读当前 [主合同](roaming-bridge-final-design-r3.md)、[WARP STOP 合同](bridge-r3-warp-stop-contract.md)、cpu-profile-ser、activation 及 cpu-profile-activation，并检查 `git diff 0de772dc..4a0c52dd`。主合同明确选择 B；比较用的 A、旧全局 backend/epoch、保留有线 BIND 的 CT 方案均不纳入批准范围。主稿第 8 节“不顺带重写 WARP 自有 ring 退出”须结合第 9 节理解：不扩通用卸载框架，但本轮列出的真实析构 guard 和安全清理边界必须实施。

本轮直接源码核验重点如下。简称以 `/home/illya/workspace/router/ax6000` 为根，行号均来自所列文件，不使用 bundled WARP 或 `.orig`。

| 简称 | 本轮读取范围 |
| --- | --- |
| F | `worktrees/bridge-forwarding`：上述文档、目标配置、WARP 配方 patches |
| D | `sources/vendor-mt79xx-7.6.7.3/review-baseline/mt_wifi`：HwCtrl/notifier、proxy/dispatcher、HIF 初始化/退出、close/adapter free/rbus remove，以及 RX reader 落点；构建选择另核相邻 `mt_wifi_ap/Makefile` |
| W | `sources/vendor-warp-20250408-5f71ec/source/warp`：实际 external 的 main、reg_v2、BM、SER、WO control/message、配置及 Makefile |
| E | `sources/linux-6.12.112-ethernet-review-postimage/drivers/net/ethernet/mediatek`：`mtk_eth_soc.c`、`mtk_eth_reset.c` 的 stop/reset/open/ACK 和销毁链 |
| K | `sources/linux-6.12.112-bridge-review-postimage/net/core/dev.c`：实际 ndo_stop 调用与返回值处理 |
| N | `sources/netifd-cbb83a185740/openwrt-netifd-cbb83a1`：相关 bridge/VLAN 调用线索、无线策略 setter 与 false 传播落点 |
| R | F `.analysis/linux-6.12.112-fatal-postimage`：8 个准确 reset 文件及 `PATCHES.json` |

R 的 8 个文件 SHA256 均与清单 postimages 一致；唯一列出的相关补丁是 MTK WDT timeout clamp。D/E 使用既有选定 postimage，准备清单含既有 fuzz；没有重新 prepare 或构建。W 的生产 patch 001 将检测线程睡眠改为可中断，008 将 remove 改为 void；二者不提供 checked STOP。实际配置选择 WARP v2/mt7986，`W/Makefile:59/75/86` 和 `warp_cfg.h:29/106` 支持本次检查的 RX-D 路径。

工作区起始干净且 HEAD 与送审快照一致；从固件基线到 HEAD 的已跟踪差异仅在 docs。H/V/TC 及完整数据面不作本轮重新全量审计，沿用上轮结论并核对本轮合同没有撤销其约束。本轮没有读取生产 checkout 来重复宣称其状态，也没有写入那里；未重放既有路由器证据。仅新增本报告，旧独立报告不改。未启动子代理、联网、实施、构建、提交/推送、操作路由器或查询 CI。

## 2. 原唯一 P1 为什么可以关闭

| 原反例与准确源码 | r3.1 的关闭规则 |
| --- | --- |
| D `embedded/hw_ctrl/hw_ctrl.c:1165` 在 stage/Status 判定前调用 SER hook；dispatcher `os/linux/rt_txrx_hook.c:18` 固定返回 0；proxy `warp_proxy.c:768` 又可按 whnat_en 跳过 | 合法 phase 才调用；D 预置值型未完成结果，proxy 在配置早退之前处理同步 checked 请求，D 读取回写结果。未处理、缺 listener、未知资源状态均失败，不依赖 dispatcher 返回值 |
| W `warp_main.c:1317/1318/1326/1327` 在 FE 等待前重写 ring 并退出 RX BM；`warp_bm_v2.c:971/978/983/984` 确有 cell/page/descriptor 清理 | STOP 只阻生产、drain、disable 与检查 idle，保留软件 ring/index、buffer 和 DMA 映射；清理移至成功 FE_DONE、有效停止证明且无 ABORT 之后，并限定一次 |
| W `warp_hw_v2.c:1218/1225/1232/1239`、`:1587–1629`、`:3047/3054/3063` 多层覆盖 ret；旧 fallback 结果忽略；`WHNAT_RESET:25` 超时只打印 | 新 stop-only helper 不调用旧混合 reset wrapper，逐层 checked、首错锁存、统一 deadline；无未经证明的 module fallback |
| `warp_hw_v2.c:1469` 将 RX_DMA_BUSY 位号当掩码；`regs/coda/wed_hw.h:667` 定义其位号为 3；WO `warp_woctrl.c:197/213/398` 吞掉消息结果 | 正确 BIT/enable/idle 检查；WO 只接受精确的 0，正非零同样失败，只有成功才更新状态。STOP 后保持 SER_RESET，不执行旧 `:1637` 的自动恢复 |
| W `warp_bm_v2.c:778/781/1242/1473/1593/1628/1680` 可继续排 timer/tasklet；`warp_ser.c:247/288` 可检测和触发恢复 | 局部 gate 先禁止新进入、自重排及重开数据中断，排空已有 CPU 活动；保留 WO 应答通道，proc、DMA_SET、suspend/resume 和检测线程同样受限 |
| D `hw_ctrl.c:1227` 可在上述错误之后报告成功；E `mtk_eth_reset.c:64` 旧 stop-fail 会改事件，worker `mtk_eth_soc.c:6208` 有 fallback | 只有 checked 成功、stop_proven 且未取消才能一次 ACK；ABORT 在 hook 前、阻塞返回后和 ACK/清理前检查。真实 stop-fail 无论 E 是否 stop_waiting 都锁存故障，不启动下一轮 |
| D `hw_ctrl.c:1246` 忽略 HIF 返回；W `warp_main.c:1087/1095/1099` 忽略 BM 失败后继续开中断 | HIF_INIT 使用 checked 值型结果，分离准备与放开，WO/IRQ/DMA 恢复前复核 fault；任何新暴露/enable 前撤销旧停止证明 |
| 未确认停止时仅让当前 HwCtrl 返回，会留下未来 close/remove/free 旁路 | 未确认停止/完成进入同步 noreturn 终态 B；D/W/E/proxy 的实际破坏性入口与本轮 SER 共享准入，且先核全局 fatal |

因此，上轮“首项 stop 失败、末项成功覆盖 → RX BM free → 成功 STOP ACK → E 成功 DONE”的轨迹不再是符合 r3.1 的实现。缺 listener、whnat_en=0 但资源仍活、WO 正错误、ACK 前 ABORT 也不能取得成功凭据。正常成功路径仍是 checked 停流 → E checked reset/固定 profile restore/内部 PDMA restart → D 安全清理和 checked 重建 → 正常 N9/WED 恢复，没有通过关闭 WED 或在线 L1 SER规避问题。

这里关闭的是状态与控制流设计。busy 位、WO 应答究竟能否充分证明目标硬件静止，以及 FE reset 后哪些 engine 状态仍然有效，仍需实现核对和硬件验证；不得用软件布尔值替代这些证据。在本轮直接检查的调用链中，没有找到一条既遵守上述规则、又必然继续未停 free/假成功 ACK/错误恢复 DMA 的具体反例。

## 3. 终态 B 与真实析构准入足以支持这个有限方案

**B 的充分性来自“不返回 + 析构与 SER 互斥准入”，不是仅来自本 CPU 停住。** 两个先后关系都已有明确出口：析构先取得准入，则不允许 SER 开始硬件操作；SER 先取得准入，则外部析构只能在上层确能处理时 busy/defer，否则同步进入同一 fatal 入口。已获得本轮控制许可的同步 SER 栈，仅在相关资源确已静止时做计划内 cleanup；外部 close 不因全局 ser_active 就获得清理许可。fatal 后不再释放准入回正常态，也不睡等一个永不退出的 owner。

这些 guard 落在真实路径上，而非只落在通知函数：D `rt_main_dev.c:226/247/258` 的 close 会连续调用 RTMPDrvClose 和 SYS_DOWN；`rtmp_init_inf.c:1192–1200`、`rtmp_init.c:340–354` 会退出队列/HIF并继续 reset/free；`rtmp_init.c:3091/3133` 和 `rt_profile.c:2241` 会释放 adapter。MT7986 的 `mt_wifi_ap/Makefile:2277/2281/2794` 选择 proprietary rbus frontend，其 `rbus_prop_dev.c:213/238/240/246/254/256` 确实继续到 notifier exit、网络设备退出、iounmap、netdev/HIF free；module exit 在 `:294`。不能只修改另一条 platform frontend。

W `warp_main.c:1042/1052` 的 client remove 会注销 platform driver，`:457/479/489/491` 的 remove 继续释放 WDMA/WED/WO，`:1492–1499` 的 module exit 更早就拆 WO/bus；proxy `warp_proxy.c:860/862` 会直接从裸 hook 链摘除自身。r3.1 已将这些入口都纳入，且明确 guard 必须早于副作用。命中 fatal 的 remove 不返回，调用栈就不能继续后续释放；无须为本方案再给 PCI core 增加 veto。

R `arch/arm64/kernel/process.c:127–147` 支持所选直接 machine_restart 路径：关本地中断、尝试停其他 CPU、调用 restart handlers，失败则本 CPU 不返回。R `kernel/reboot.c:92–97` 的普通 restart preparation 会执行 notifier/device_shutdown，确实不适合未知 DMA 状态；`:84–90` 的 emergency_restart 为 GPL-only 且先 dump。选由 builtin E 提供本产品硬件故障 API、D 只调用该普通导出，可避免模块直接依赖未导出的 arch 符号或 GPL-only emergency_restart。

R `arch/arm64/kernel/smp.c:1133–1213` 明确允许其他 CPU 未停住；r3.1 没有隐去该限制，并以准入排除“析构早已通过 guard、随后与失败 SER 并行 free”的窗口。PSCI `psci.c:308/326/628` 是 priority 129 的注册候选，HARD 走 SYSTEM_RESET；WDT `mtk_wdt.c:228/442/449` 是 priority 128 的候选，关闭 IRQ 模式后重复写 SWRST。**这些源码没有证明目标硬件一定完成整机复位。** PSCI 卡住时不会自动走到 WDT，WDT 循环也不是复位成功 ACK；仍按可能断电恢复的产品边界处理。

QUIESCENT_FAILED 的受控 teardown 同样成立，但只适用于相关资源已有有效静止证明、未重新暴露/启动，且本轮任务已退出并 drain 的情形。它不能恢复数据或发起下一轮 SER。无证明、证明失效和无法安全串行的销毁交叉仍走 B。无需因此恢复运行态 retain、动态 backend、拓扑 epoch 或 CT 框架。

## 4. 必要 P2：随实现落实并在代码复审逐项验收

以下均是已选合同的具体落点；遗漏会使实现不合格，不能因列为 P2 就省略。

1. **phase、取消与 proof 的作用域。** 明确每阶段合法事件、结果初始化、同步发布及唯一完成者；取消与 ACK/cleanup/enable 的许可发放必须按同一串行规则裁定，不能裸读一次布尔值后任意继续。`stop_proven` 只证明对应 owner/资源及仍保持禁用的状态，不证明整个 E/D/W 系统。STOP 请求开始可清旧 proof；进入 CLEANUP/HIF 准备不能机械照抄“每次调用清 false”而丢失输入前提，也不能在重新暴露后补回旧 true。Wi-Fi 自主 SER 的 `notify_fe=0` 分支复用相同 checked STOP/清理规则，按合法本地 N9 RESET_DONE 继续，**不等待一条未发起 FE 事务的 FE_DONE，也不伪造 FE 成功 ACK**；发生 FE 事务交叉则按现有 busy/ABORT 合同处理。

2. **有界 drain 必须落到实际运行者。** `unregist_*_dybm_task()` 在 W `warp_bm_v2.c:733/1634` 只是 free，不能作同步屏障。每个排队、回调重排及 EINT enable 点都检查 stopping/fatal；已排队但尚未运行的对象也在责任范围。`del_timer_sync()`、tasklet 同步 kill 本身没有 timeout 参数，不能在其前后读 deadline 就声称整个等待有界：应先封住重排，使实际 callback 可有限退出，并用可核验的活动排空/取消顺序兑现预算；不能完成时转失败，绝不靠强行 free 结束。预算到零直接返回超时；W `mcu/warp_msg.c:551` 的 timeout=0 会回默认值，不能用四舍五入为零的 remaining-ms 意外续借时间。保持 CCIF 应答所需 IRQ/任务可运行。

3. **可靠 busy 的范围很窄，尤其不是 ndo_stop。** K `net/core/dev.c:1613–1653` 先清 running/deactivate，再调用 ndo_stop，忽略其 int 返回并清 IFF_UP。因此 D/E 的普通 close 不能仅返回 `-EBUSY` 就宣称 core 停止销毁；无法可靠 defer 的 SER 交叉必须进入 B。module exit、void remove、HIF error unwind 也不能靠下层 early-return 阻止上层继续。D→proxy→W 的显式控制许可核对 owner、阶段和当前控制栈，正常外部 close 无权借用；入口与首次副作用之间不能释放准入。proxy 模块级退出须覆盖所有相关 owner 和在途同步调用，不能只看一个 adapter，也不能让准入状态或活动调用对象先被释放；本项无需更改 dispatcher 的通用返回语义。

4. **E 自己的 PDMA/QDMA stop 也必须 checked，不能借 WARP proof。** E `mtk_stop_dma():4975–4996` 现为 void，busy 达到 10 次后仍返回；`mtk_stop():5046–5053` 接着 free。这是实现时必须改掉的具体落点，包括 SER 内部计划性 stop；WED STOP ACK 不证明这两套 E ring 已静止。stop 失败进入 B 前不能到 `mtk_dma_free()`。同样检查 `mtk_start_dma():4664–4666` 的分配错误 unwind：仅尚未暴露的分配可回收。warm reset 的 regmap/poll/stage/fallback 逐层传播；`mtk_hw_warm_reset():5381–5399` 当前还有先 `rst_mask |=`、后按 SoC 覆盖赋值的问题，实施应先构造完整 mask，再加所需位。`:5405` 的 `!(val & rst_mask)` 只检查“一个位也没置”，不能证明全部预期位已置，成功条件应覆盖完整 mask；deassert 同理核全量结果。普通 ndo_open 与 SER 内部 restart 分开，后续端口失败不能进入旧 dev_close/free 后仍发 DONE 的路径。

5. **checked 准备覆盖所有下层，暴露边界不能靠函数名猜。** D `hw_ctrl/hw_init.c:196` 还忽略 `WfHifHwInit()` 的 int；后者 `:25/29` 返回真实 hif_sys_init 结果，必须接住，不仅修改 HIF_INIT hook。D 的 qm/tm 等准备按实际返回约定检查。W `warp_ring_init():1081–1110` 混合地址/ring 配置、BM 分配、IRQ/EINT/ATC 和 WO 配置，必须真正拆开；新地址可能暴露、WO 恢复或任何 DMA enable 之前撤销旧 proof，错误出口按是否仍可证明静止分流。E reset/重新 open 之后，不能仅因旧 stop_proven=true 就假定 WED/WDMA 状态未变；核对并保持其 disable/idle 前提，再授予 cleanup。已暴露后失败不进入普通 HIF/BM init 的隐式 free 路径。

6. **fatal 实现不能依赖即将停住的 CPU。** 统一 fatal 发布/首执行者选举及各 guard 用明确原子顺序；选举和进入 restart 前控制本 CPU 的抢占/中断窗口，避免已选执行者被同 CPU 再入的“其他进入者”永久停住。调用者先放短自旋锁，不同步 drain、不等 RTNL/HwCtrl，不分配内存或强制打印后才尝试复位。builtin wrapper 的 `__noreturn` 要有实际兜底停留，不能只靠声明；准确头文件的 machine_restart 声明自身没有该标注。它只接本产品严重硬件故障，不做可供模块调用任意 arch 操作的转发器。底层 arch/restart 链仍是 best effort，不能将软件模拟的“不返回”记录当成实机复位证明。

7. **把上轮并入的 P2 保留到代码，而非只保留在主稿。** D `cmm_data.c:6987/6991` 的实际 peer wdev/ops 调用是早期 reader 边界；不能等 `dev_rx_*():7055/7105` 返回之后才保护已经发生的配置读取。源授权后才 stamp；common announce 从 `:3515` 开始，reader 要早于 `:3541/3553` 的 MAP/VLAN 配置访问，BA 出队不可补当前 lease。每个提前 return 配对退出，不持 reader 反向等 RTNL。N `system-linux.c:940/1002/1006` 的 void setter、true-only isolated/bpdu_filter 分支落实完整 0/1/false 和错误回传。COMMIT 的 handler 自身 copyout 后再串行复核 fault；外层 native/compat 负返回/超时仍是不确定结果，GET 加重新 PREPARE 成功 blocked ACK 前不拆关系、不 mark_up。永久 callback 发布即寿命承诺，不能以尚未写 DMA BASE 回收已发布 context；participant 则按实际 DMA/初始化活动确认，不能将 probe ONLINE 或 !START_UP 等同静止。

辅助文档仍有少量旧措辞，例如 activation 状态表/errno 段的 backend barrier，以及 cpu-profile-activation 对 READY 后 copyout 的概括。按本轮主稿 `:74–76` 和 activation `:117/180` 的明确替代规则执行即可，后续文档整理不得借这些残留恢复旧框架；本次不修改它们。

## 5. 实施、CI、硬件与性能门

**实施及代码复审：**在指定 worktree 按主稿分组实现，基于准确 postimage 生成补丁并核完整系列。必须审查真实 caller 到叶节点的错误传播、每处 enable/地址暴露与 proof 撤销、全部列出的析构入口、proxy 代码寿命及锁顺序。新增值型字段和导出接口同步到 D/proxy/W，禁止循环模块依赖；H 固定常驻和 E builtin 限制继续成立。固定无 BIND、无 nf_flow、TC 硬件拒绝、一次交桥、最终 TX 绑定、lease/NF policy、netifd 全链及 eQoS 软件适配仍在整套实施范围。

**真实 helper/故障注入：**至少覆盖首项失败末项成功、WO 正非零/timeout/迟到响应、每个必要 busy 位持续为 1、缺 listener、whnat_en=0 但资源活、预算耗尽、timer/tasklet 自重排、检测线程/proc/DMA_SET 竞争；ABORT 在 hook 前、WO 等待中、返回后、ACK/cleanup/enable 边界；自主 SER、E 不在 stop_waiting 时收到 stop-fail、重复/缺失 ACK、真实活动成员变化。覆盖析构先入、SER 先入、proxy 退出、多 CPU fatal、首个 E open 成功后后续失败、HIF/BM 各准备阶段失败。停止未确认的失败分支必须是零成功 STOP ACK、零 ring/index mutation/free/unmap、零恢复 DMA；恢复阶段失败则核“失败后不再继续恢复、未确认资源不回收”，不能要求抹去先前合法成功动作。已证明静止的 QUIESCENT_FAILED 单独验证其受控 teardown 和无下一轮 SER。

fatal 测试在隔离测试环境核验真实控制路径不可落入成功/free continuation、只选一个执行者及其他入口不返回；可替换底层复位执行动作，但不得用“mock 返回了”证明生产 noreturn。此处不授权在路由器注入硬重启。

activation/数据面沿上轮测试门：reader 与配置并发、授权前不能 stamp、BA/新 skb/clone-COW/旧 lease、混合聚合及畸形封装、实际 bridge/VLAN 0/1 策略、外层 copyout 不确定后的重新阻流、同 MAC 多 VID、最终 wdev/WCID/DA 错配、所有 native reason 的消费/补环。检查永久 callback 发布后失败、probe 半初始化、模块未启动的活动判定；不以 grep 补丁或复制算法的测试替代实际 helper。

**完整构建/CI：**实际 MT7986 kernel/D/W/proxy/netifd/ucode、生成配置、普通导出/模块许可/undefined-symbol、ARM64/E builtin/EFI=n、force-unload/overlay/解绑限制、skb extension 边界、RRM/WNM/FT 与生产 guard。确认 external WARP 配方、真实 rbus frontend 和实际动态 BM 宏；选定源码准备不等于完整 prepare/build。实施经独立代码复审后再按既有授权流程推送 CI；本轮未触发、查询或监控 CI。

**硬件门另需刷机/实验授权：**冷启动与正常在线 L1 后核固定 CPU profile、有效常驻 FOE 地址/大小、无 BIND/cache/private producer，且基础 WED RX/TX、token、crypto/reorder 正常。验证 stop-only 顺序在目标 WO/硬件上确可成功、busy/应答/必要 reset 的真实完成语义，FE reset/open 不会使 W 的旧 proof 被误用。核 PSCI/WDT 的实际注册、调用 CPU、ATF 行为、CPU/FE/PPE/WED/WO 等整机复位范围及完成，失败时断电恢复程序；不得从函数循环推定已重启。按原证据场景验证任意终端漫游、旧 STA 尚存时的 FDB/物理出口/请求回包及业务交付，并覆盖 IPv4/6、ARP/ND、非 IP L2、group、BA/聚合、静态 VLAN/隔离/动态 reload、正常和失败 SER。

**性能门：**LAN/WAN、独立有线、有线/无线桥转发的吞吐、CPU、时延与重配置停顿；eQoS 的 HTB/IFB 总带宽、默认叶、规则 1–65533、零单设备限速、IPv4/6、stop/restart。不得保留 start/stop 对 HNAT/QDMA/turboacc 的写入；不承诺 br-lan qdisc 覆盖全部纯桥流量。任何缓存绕桥、越权/跨 VLAN、重复/错误出口、DMA 误释放、死锁或失败仍放行都停止验收，不能用性能收益抵消。

## 6. 有限产品代价与批准边界

保留上轮公开的固定软件转发代价：独立非桥有线也失去 PPE/NF-flow 加速，取消硬件 HQOS/私有加速能力，H/E 常驻并限制运行时卸载解绑；配置事务可中断整 BSS 普通数据及数据 EAPOL，未闭合的特殊角色/动态 per-STA VLAN 拒激活。不填写未测性能损失比例。

r3.1 额外增加：无法证明 DMA 停止/完成，或 SER 与不能可靠拒绝的销毁交叉，可能直接中断整机并丢失未落盘状态；复位未完成可能需断电。并非只有“芯片已经坏了”才有这项代价，维护 reload/卸载与 SER 的竞态也可能触发。普通软件配置错误、普通坏包和仍有有效静止证明的失败，不应被任意升级为该终态。

用户已授权修复和隔离分支实施，尚未完成故障体验/性能验收不构成设计 NO-GO，也不等于自动刷机许可。**本报告关闭原唯一 P1并批准进入实施；保留 WED 基础、正常成功在线 L1 SER 和既有 eQoS 软件适配承诺。** 不恢复双 MAC、全局 backend/epoch 或有线 BIND/CT 方案。
