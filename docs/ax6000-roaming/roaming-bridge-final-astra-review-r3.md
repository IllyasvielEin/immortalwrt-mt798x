# 通用桥转发 r3：Astra 独立关键设计审查

2026-11-09。审核主合同为 `roaming-bridge-final-design-r3.md`，文档快照 `0de772dce7dbbe27f73a793e72f6ed0927abf1df`，实施基线 `c196312d3916fad77fa0bb47db57e4b685c15f03`。本次自行只读核验，仅新增本报告；未启动子代理、联网、实施、构建、提交、推送、操作路由器或查询 CI。

**裁决：当前快照 NO-GO，先补齐一项有限的 P1，再进入整套实施。** 固定无 BIND、无 NF flow cache 的收敛方向成立；P1-1、P1-3、P1-4 的设计阻断可以关闭。P1-2 的 E/D 通知与常驻 profile 恢复顺序也已明显闭合，但其依赖的实际外部 WARP 停流调用链仍会吞掉可检测失败，并在 STOP ACK 之前释放 RX DMA 资源。当前“沿用物理停流、在 FE 等待之后查 ABORT”的具体落点不能覆盖它。第 2 节给出反例和最小修正，不要求恢复通用 HNAT 生命周期、拓扑 epoch、CT 维护或最终 N9 ACK 框架。

此 NO-GO 不是因为尚未实施、构建或真机测试，也不否定普通三地址 AP 统一交桥。下文 P2 可直接随实现落实，不要求为这些细节再设计一套框架。关闭唯一 P1 后，设计 GO 的权限仍仅限指定隔离分支实施；构建、性能、刷机和生产验收各自独立。

## 1. 核验范围与四项原 P1 的裁决

先读了 r3 主合同和保留不变的 r2 Astra 报告，再读 activation、cpu-profile-activation、cpu-profile-ser、packet-ownership 的相关部分；scope-comparison 仅用于核对删减依据。以主合同的裁剪为准，没有采用比较稿的“关闭在线 FE SER”，也没有将 packet-ownership 旧正文的有线 BIND/CT 维护加入本版。

下列源码简称均以 `/home/illya/workspace/router/ax6000` 为根，行号对应所列本地文件，不使用 `.orig`：

| 简称 | 路径与范围 |
| --- | --- |
| F | `worktrees/bridge-forwarding`；本次所有仓库内产品文件读取及报告写入均在这里 |
| D | `sources/vendor-mt79xx-7.6.7.3/review-baseline/mt_wifi`，42 patch |
| H | `sources/hnat-6.12.112-review-baseline/drivers/net/ethernet/mediatek/mtk_hnat`，14 patch |
| E | `sources/linux-6.12.112-ethernet-review-postimage/drivers/net/ethernet/mediatek`，82 相关 patch、4 文件 |
| K | `sources/linux-6.12.112-bridge-review-postimage`，49 patch 选定文件 |
| V | `sources/linux-6.12.112-vlan-review-postimage`，5 patch |
| N | `sources/netifd-cbb83a185740/openwrt-netifd-cbb83a1` |
| W | `sources/vendor-warp-20250408-5f71ec/source/warp`，实际 external 源；同时检查 F 中 WARP 配方及相关 patches |
| T | F `.analysis/linux-6.12.112-tc-postimage`，stable 6.12.112 加 14 相关 patch 的单文件准备 |
| R | F `.analysis/linux-6.12.112-refs` 及 `sources/linux-6.12.112-roam-refs` 的 raw stable 参考 |

已读取准备清单 PATCHES.json，未重新解包、重放补丁或做完整 prepare。清单含既有 fuzz，不能声称所有文件 fuzz=0。W 的引用是给定 external 源行号；所核配方 patches 的格式、兼容性及监测线程修正没有修复第 2 节的错误传播。未取得最终模块/预处理产物，不能由源码搜索代替链接检查。

只读确认原生产 checkout HEAD 仍为 `c196312d3916fad77fa0bb47db57e4b685c15f03` 且 status 无改动。复核了 `research/evidence/roam-k-off-20261108-1735/{failure-summary,recovery-summary}.json`：FDB 已移有线后仍走旧同 wdev，老化后恢复的证据与 D `embedded/ap/ap_data.c:4079/4090` 相符。未重放现场实验，不把提交成功当实际 RF 交付；旧关联为何没有及时清理仍未知。未取得或声称读过 conntrack core 函数体，本版无需借其补建 BIND 维护。

| 原问题 | 本轮裁决 | 依据及剩余边界 |
| --- | --- | --- |
| P1-1 配置失败回 legacy | **设计关闭** | 默认整 BSS gate、同步 PREPARE、驱动队列 lease、完整 RTNL COMMIT、失败先阻流后拆关系，取代 external release/mark_up 的错误推断 |
| P1-2 SER 跨 RTNL 与恢复顺序 | **原全局 depth 问题被固定能力替代；仍有第 2 节 P1** | 无动态缓存，确实不需跨窗口 topology transaction；但正向 STOP ACK 尚缺实际 WARP 停流成功条件 |
| P1-3 HNAT hook/work/DMA 退出 | **由固定常驻能力关闭** | 不发布真实资源消费者、不启动维护者、禁实际 H/E 销毁入口、暴露后保留绑定；一次永久 RTNL callback 可行。此结论不覆盖 WARP 自有 RX ring 的停流失败 |
| P1-4 副本/私有包所有权 | **设计关闭** | 可信 descriptor 源头分类及一次消费，不查 FOE；禁 producer 异常故障化；普通和未知 native 数据走软件；无合法 BIND 账户，无新增 CT 合同 |

## 2. 唯一 P1：STOP ACK 之前的 WARP 停流失败尚未进入失败合同

### 2.1 设计缺口在哪里

主合同第 9 节及 `bridge-r3-cpu-profile-ser.md:14/55/72` 把 D 的关键失败落点放在 `hw_ctrl.c` 的 FE completion 等待之后、HIF free/reinit 之前，并拟沿用既有物理停流。主合同 `:104` 又正确限制“不顺带重写 WARP 自有 ring 退出”。两条不能被解释为既有 WARP SER hook 已提供可靠停止证明：**这个 hook 早于 FE 等待，已经会释放自己持有的 DMA 资源；其结果也没有传给 D。**

这是当前选择保留在线 L1 SER 的直接依赖，不是审查者要求另做通用 WARP 生命周期。应补的是本轮实际 STOP 成功条件和这一条路径的失败出口。

### 2.2 准确调用链与可执行反例

1. **目标确实编译这条路径。** F `defconfig/mt7986-ax6000.config:143/146` 选择 mt7986、WARP_VERSION=2；W `Makefile:59/75/86` 选择 reg_v2 和 `warp_bm_v2.o`。W `warp_cfg.h:19/29/106` 在该配置开启 `WED_RX_D_SUPPORT`。不是 bundled WARP 或 MT7988 专有路径。
2. **WARP 动作先于 D 的 ACK/等待。** D `embedded/hw_ctrl/hw_ctrl.c:1165` 在 stage switch 之前调用 `WLAN_HOOK_SER`；STOP_PDMA0 分支直到 `:1223/1227/1229` 才报告 STOP 并等 FE。D `embedded/plug_in/warp_proxy/warp_proxy.c:750/803` 转到 W `warp_ser_handler()`。
3. **WARP 在这时已经重置并释放资源。** W `warp_main.c:1314` 的 STOP_PDMA0 分支先执行 TX ring reset，`:1320` 尝试 index reset，失败后 `:1323` 再做 module reset却不检查返回；随后无条件 `:1326/1327` 调 `wed_rx_ring_reset()`、`wed_rx_bm_exit()`。后者在 W `warp_bm_v2.c:1707` 进入 ring exit；`:971/978/983/984` 清零 cell、drain 缓存 page 引用并释放 cell 和 DMA descriptor 内存；`warp_utility.c:49/55` 确认最后实际调用 `dma_free_coherent()`。
4. **下层错误本身也会丢失。** W `regs/reg_v2/warp_hw_v2.c:1587/1594/1601/1608/1615/1622/1629` 反复覆盖同一个 `ret`，最后 `:1640` 仅返回最后一次结果；上层 `warp_reset_hw():3047/3054/3063` 再重复这一问题。`:25` 的 `WHNAT_RESET` 在 poll 超限时只打印，不传失败。WO 状态消息还有 `mcu/warp_woctrl.c:197/213` 丢弃消息结果、`:398` void 包装的边界。这里没有声称这些 poll 全都无限等待；问题是**已检测失败不能可靠到达 caller**。
5. **现有 hook 调度也不传播结果。** D `os/linux/rt_txrx_hook.c:18/27/30` 调各 hook 后固定返回 0；只把某个 WARP 函数改成 int 仍不能让 D 知道失败。

可注入的反例：FE START 已被 D 接受；D 在 STOP_PDMA0 的 WARP hook 中遇到 RX DMA/WO 停止失败，或 reset 完成读回失败。后续 RX BM reset 成功覆盖早先错误，或者 module fallback 仍失败却被忽略。WARP 继续释放 RX buffer/descriptor；D 然后按一次性 notify_fe 发出**成功 STOP ACK**。E 收齐 ACK 后继续 FE reset、restore/open/DONE。把 D `:1229` 改为有界等待、醒来检查 ABORT，不会追回已经发生的释放，也不会发现被抹掉的 WARP 错误。

这里指出的是“不能证明 DMA 已停止却继续释放并报告成功”的源码路径及故障注入反例，**不是声称现场已经发生该 UAF**。H 的 FOE 常驻只保护 H 的表，不能保护 W 自己释放的 RX buffer；无 BIND 同样不能排除 WED DMA。

### 2.3 最小修正和关闭条件

在主合同/最小 SER 合同补入以下有限规则，然后才能关闭该 P1：

- 将本轮正向 STOP ACK 定义为 D 与 WED 必要停流均已确认成功。为实际 WARP SER STOP 建立可检查的返回通道：可以使用产品专用的同步 checked helper/值型结果，也可以修正这类 hook 的返回传播；**不能依赖现有固定返回 0 的 dispatcher**。不用新 participant registry、reset-id 或最终 N9 ACK。
- 逐层保留 WO 命令、busy poll、reset 完成检查的失败。允许既有有界 fallback，但只能由**该 fallback 独立证明必要 engine 全部安全停止**后转成功，不能由最后一个子步骤为 0 掩盖其他失败。各实际等待纳入 STOP 总 deadline，错误必须能返回。
- 将 SER 路径中的 ring 重置/释放分成“已证明可安全操作”与“停止未确认”两种出口。失败时不再进入上述 RX BM free，也不向 E 报成功 STOP；保留可能被 DMA 触及的资源，D/E sticky fail、ABORT/失败通知唤醒并结束成功路径。可以复用现有 stop-fail 事件并纳入本轮状态，不需另造协议。**正常已确认停流时仍复用原 ring 清理；不要求重写所有 WARP unload/proc/ring 生命周期。**
- D 的已有 fe_failed/ABORT 检查应早于会产生副作用的 `WLAN_HOOK_SER` 入口，并在可阻塞的停流动作返回后、释放资源/成功 ACK 前再次核验。ABORT 早到不能因稍后 MCU event 又进入这条释放/恢复链。保留 r3 已要求的 HIF reinit、IRQ/DMA enable 前检查。

设计关闭凭据是一份短的实际 D→proxy/WARP→STOP ACK 顺序及错误出口，不要求先完整构建或真机。后续实现必须用真实 helper 注入：首个子步骤失败/末项成功、module fallback 失败、WO timeout、ACK 前 ABORT；结果必须为零次成功 ACK、零次未确认停止资源释放、零次 HIF/DMA 恢复。只有硬件已确认停止的成功路径才能清理旧 ring 并继续恢复。

## 3. P1-1 关闭依据与 P2 实现要求

**控制合同可执行。** N `device.c:175/760` 仍证明 external 与多 claimant 不等于 down；N `bridge.c:326/652` 的 delif/release 与 F `wireless-device.uc:456/460` 无条件 mark_up 也确需替换。r3 不再靠这些动作停止普通流量，而是先撤销 driver lease、drain active reader，整个 BSS 及 VLAN 后代共享一个事务，所有真实端口成功后才 COMMIT。already-up、部分 child 成功、down 失败与 handler 重启均没有自动恢复旧 READY 的出口。

COMMIT 的准确实现基础存在：raw `dev_ioctl.c:479/508/791` 的 private callback 已持 RTNL；K `br_private.h:459`、`br_vlan.c:1408/1500` 的桥内部数据能核对 port/master/VLAN。新增有限的普通导出值型 checker，比从 proprietary D 调现有 GPL-only 导出或复制 bridge 私有结构合适。完整 membership 在 COMMIT 比较，数据面只核本帧域，不需要全局 topology epoch。

以下为 **P2，随实现落实**：

1. **reader 范围及锁。** 早期 reader 必须覆盖读取 VLAN/隔离配置直到 BA/队列接管；晚期 reader 覆盖实际 Linux handoff 或 descriptor 提交/拒绝。不得持 reader 反向取 RTNL，也不得持 gate/MacTab/ring 锁等 drain。snapshot 引用替换和 NF hook 注销仍需 grace。软中断发现域失配/profile fault 只能闭锁；READY 发布与 sticky fault 使用同一串行规则，不能 copyout 后无条件写 READY。
2. **lease 继承。** raw `skbuff.c:1533/7101/7134` 支持标准 copy/clone 与 COW；不支持凭空推断厂商新包来源。逐点覆盖 `duplicate_pkt_vlan`、第二 MSDU、BA 子帧、ACK、PS/BMC、聚合/分片重建；缺继承拒绝，不能补当前 lease。首次创建扩展显式初始化，已有扩展写入用 `skb_ext_add()`，RX 与 TX 字段分开。全局 cookie 只解决实例复用，不变成站点 generation。保留 SKB_EXT_NUM/长度及实际配置检查。
3. **ioctl 失败不能等同已阻流。** CAP_NET_ADMIN、固定 netns、owner/token、定长兼容 ABI 按合同落实。还要检查 native/compat syscall 外层 copyout：raw `dev_ioctl.c:660/788` 的 private 分支未清 `need_copyout`，仅证明 handler 自己的 copy_to_user 先于 READY，尚不能声称整个 syscall 此后绝无用户拷贝失败。本轮没有外层 socket wrapper 函数体。控制面遇返回不确定时必须 GET 并重新 PREPARE 确认，绝不据失败直接拆关系；在完整源码中验证外层行为，必要时收窄“任何 copyout 失败必 BLOCKED”的措辞。这个澄清不要求逐包框架，也不破坏“无 blocked ACK 不清理”的安全规则。
4. **RX 安全分流和真实 handoff。** D `cmm_data.c:7405/7425/7454` 的前置读取、`:6754/6772` 的 eth_frame 路径和 `cmm_wpa.c:1506/1516` 证明不能只改 FALSE→TRUE。纯 helper 必须先有界解析全部实际封装；聚合不借首个 EAPOL 放行其它 MSDU。BLOCKED 期 TX 数据 EAPOL 不豁免，commit 后正常握手重试；raw management 路径也要按真实 frame type 分辨数据。D `ap_data.c:4057/4068/4079` 之前交桥，并绕过 `rt_profile.c:1823/1984` 的私有快转，保持一次 protocol assign 与一次 handoff。
5. **TX 最后边界。** D `qm.c:1544/1550` 的旧 TR 改 wdev 必须删除；qm/fp_qm/fp_fair_qm、ACK/PS/UAPSD/BMC、每个聚合 MSDU 都执行本 wdev、DA、TR/WCID、安全和 VLAN 校验。合同明确选“拒绝并计数”可执行；不能替换 WCID/密钥重试，不能在 Ethernet DA 已丢失后假装还能验证。Linux qdisc 首次入 driver 用当时出口许可；仅 driver 内旧队列 lease 失效，这一边界无需扩大。
6. **真实桥隔离与 VLAN。** NF_BR_PRE_ROUTING admission 保护普通 AP 入桥域，NF_BR_FORWARD 用实际 in/out 处理 NoForwarding/BTNBSSID；不能用 skb_iif、DA STA 或全部 BR_ISOLATED 代替。K `br_input.c:70/76/161` 说明本机投递与转发不同，只有 FORWARD 不足。V `vlan_core.c:18/26` 和 `vlan.c:149/158/180/194` 支持直接 ancestry/注册前 netns 约束。验证 VID0、双 tag、metadata+in-frame、DOWN upper、同 MAC 多 VID，保持真实设备引用与不可撤销 netns_local。外部移桥不自动扩大 standalone。hairpin 独立于 mcast_to_ucast，isolate 的 false 同样写入读回。

不要求对已交 Linux/DMA 的帧做全栈撤回；这些在途例外已经明确。也不重新增加 STA 指针、deauth generation、FDB 副本或按终端身份的候选。

## 4. P1-2 已成立的缩减，以及不能遗漏的 P2

**不用全局 depth、reset-id 和最终 N9 ACK 的论证成立，有适用条件。** E 同一 worker 串行；状态在第一次硬件 touch 之前置 notready；RTNL 外仅等待；D 仅 STOP_IDLE 且无 pending/failure 接受 START；成功周期 STOP 在 notifier 中同步交付，每 D 一次、E 仅 stop_waiting 接收；成员变化使整轮失败；任何失败 sticky 后无下一轮。这样不会把失败轮的迟到 ACK 计入新的成功轮。D 在 N9 恢复未完时拒第二 START，E 必须处理 busy，不需要为不存在的缓存准入再等最终 N9 ACK。第 2 节补的是这个 STOP 的真实性，不推翻上述简化。

准确 E `mtk_pending_work():6190/6207/6245/6252/6277/6288` 当前顺序确有问题；r3 的 restore-before-open/DONE 和取消 H `hnat_warm_init():1553/1569/1580` 的 NAT_DONE 消费正确。MT7986 用 D 已实现的 START_RESET/RESET_DONE，不借 V3 STOP_TRAFFIC 分支。E `mtk_open():4874/4888/4893` 必须检查 profile 并始终选择 PDMA，禁 L4S/TDMA 覆盖。

**P2 实现要求：**

- “checked hw_init”必须包含下层检查，不只检查 E `mtk_hw_init()` 的 int。它在 `:5743/5744` 调 void `mtk_hw_warm_reset()`，最后 `:5923` 返回 0；`:5374` warm poll 失败会 fallback，`:5405/5414` stage 检查只打印。需要将这些可检测失败逐层传出；成功 fallback 也须有自己的完成证明，不能以写寄存器成功冒充复位完成。这是主合同已要求的 checked reset 的实际落点。
- E 内部 SER restart 与普通 ndo_open 的资格要明确：ser_active 期间只允许已完成 checked restore 的 worker 内部重开；普通 open/COMMIT 仍拒绝。profile_ready 不能顺手开放不该发生的重入。首个端口已开、第二个失败时，先撤 ready/sticky fail，再安全停止已开端口，不能继续成功 DONE，也不能经旧 dev_close/unwind 无条件释放未知 DMA 资源。
- START 接受、D stage/pending/fe_result 的更新用明确同步；notifier busy 用 notifier 约定传播，E 检查返回。E wait timeout 后重取 RTNL，再次核失败/成员变化；完成恰好到达不能覆盖已决定的失败。ABORT 优先于 !START_UP 早退，晚到 DONE 不覆盖失败；退出先自唤醒再 drain。物理 WARP 步骤还遵第 2 节。
- **在线成员与已启动 DMA 不是同一个概念。** D `rbus_prop_dev.c:195` 在 probe 发布 ONLINE，而 `hw_ctrl_basic.c:396` 对未 START_UP 的 adapter 忽略 START。保留旧计数会让“模块已加载但 Wi-Fi 未启动”必然等超时。实施须使参与者的定义与实际能安全报告 STOP 的对象一致：在同一串行控制下把已确认无硬件活动的对象视为无需停流，或对其提供明确的静止确认；不能仅看一个 flag 假报成功。普通 up/down 与 notifier 注册失败也要被覆盖。无需为此引入热插拔代际框架。
- STOP 总 deadline 与 D FE 等待 deadline 根据实际有界子步骤预算设置；5 秒是初值，不是证据。正常 L1 后仍须验证 HIF/N9/WED 收发恢复，不能把 profile_ready 称为全无线恢复完成。

## 5. P1-3：固定常驻合同充分，但必须在真正入口落实

在本版能力下，一次 RTNL 永久 restore callback 不需要通用 RCU/SRCU backend registry：E built-in；H 无 exit/不可强卸；H/E 解绑及 runtime overlay 入口禁用；context 和硬件可见表不换所有权、不释放；callback 只在 RTNL 控制路径同步调用，数据面只读 E 状态。不存在动态注销与 callback 并发，自然不需要给它新增注销 grace。

这不是“无 BIND 所以无读者”的推理。本轮复核到的真实旧入口包括：

- H `hnat.c:1477` 发布 RX/TX、dev-register、clear-bind、WDMA port hooks，`:1494` 发布 delete hooks，`:1818` 起注册 NF/notifier、neighbor、mcast、timer、roam、pending worker。这整个产品分支都应取消，不能只让最后 BIND 函数返回。
- D `rt_profile.c:1823/1839/1844`、`cmm_data.c:7655`，D `include/os/rt_linux.h:285` 注册宏、`cmm_cfg.c:10900` setter、`ap_mgmt_assoc.c:3097` delete hook，及 `sta_mgmt_cntl.c:2539/3775` 保存/恢复 RX hook；W `warp_wifi.c:130/164` 检查并调用 TX hook。主合同的“取消真实调用/保存/恢复”覆盖这些，不可仅保持 H 的 NULL 发布而留下一条写回旧槽位的产品路径。
- H `hnat_api.c:167/173/182/259/281` 等 API 在 CFG_PPE_NUM/hnat_priv 前必须拒绝；包括导出回调注册、表指针、更新/删除/统计/crypto/xlat/static/cache 入口。不能用可写的所谓只读诊断重新开表资源访问。
- 真正销毁是 H `hnat.c:1852/1868/1882/1958` 的失败 unwind/remove/module_platform_driver，以及 E `mtk_eth_soc.c:7755/7777/8111/8120`。只改 remove 返回值、仅加 module 引用都不够；主合同提出的无 module_exit、suppress_bind_attrs、force-unload=n、overlay=n 及暴露后返回 0 保留 FAILED 绑定，才关闭这些受支持入口。

在已检查的 D/W C/H 和配方范围内，没有发现额外的 D/W 直接 FOE 表 API 依赖足以推翻“撤掉 H 资源 hooks 仍保留基础 WED”的设计；最终 undefined-symbol 和生产宏仍是实现门，不把此搜索结论扩展到未检查的二进制/可选 provider。WARP 自有资源释放的已发现反例另列第 2 节，不能用 H 常驻遮盖。

**P2：**永久 callback 的发布本身也是寿命承诺点。安排在可失败的冷准备之后、同一 RTNL 最后发布；若已发布后出错，即使尚未写 DMA BASE，也不能释放 callback context 或让承载代码退出。对“DMA 未暴露可回收”的例外同时检查有没有永久引用已发布，避免一条 probe-error 回收留下悬空 callback。

**P2：**H `hnat_start():1300/1301/1314` 现有先写 BASE、后清零、随后分配失败的顺序必须换成全部必需表准备完成后暴露。固定 profile 不走旧 hnat_hw_init 的 BUILD_ENTRY/KA/MIB/cache 配置；同一 checked helper 用于 cold/SER，禁 private producer 和学习。故障状态查询不得解引用未初始化表，已 READY BSS 的后续普通 gate 也必须看到 fault。常驻 FOE 的内存代价保留，禁 cache 不是允许 DMA free 的证明。

## 6. P1-4：源头 ownership 合同足以关闭原反例

E `mtk_eth_soc.c:2860` 取得合法 descriptor 后即可分类，必须早于 `:2884` 的 QDMA/PPD 处理、`:2991` head metadata、`:3006` FORCE_TO_CPU redirect，以及 XDP/GRO 等后续交付。D `hif_pci.c:1559` 有真实 PPE_VLD；分类放在 `:1564` WARP hook 之前，不依赖 `warp_proxy.c:768` 的 whnat 开关。两个 caller `:1716/1735/1741`、`:2222/2227/2236` 已有 drop/free 与补环路径。

r3 的分类结果正确：可信 0x15 消费一次，不访问 FOE；0x13/14、0x16、0x18/19、0x1b 和可信私有实例作为被禁 producer 异常终结并 fault；TTL/options/fragment/UN_HIT/UNBIND/MTU 和未知 native 普通包继续软件。无 PPE_VLD、只有 payload magic 或 0x5678 的用户包不获可信身份。0x18/19 不能宣称整份都是已发副本，但在本产品从源头禁止后，可明确作为故障丢弃而非冒称无损回退。

**P2：**E 在建 skb 前消费时可复用原 descriptor/release 路径，建 skb 后用正确 skip/refill 路径；不能混用两种所有权导致复用已 free buffer。D 返回 drop 位必须与既有 RXD_DROP/TO_HOST_A 等错误合并，helper 不另 free。新产品编译条件要覆盖两个 caller，不能为删 FAST_NAT hooks 把 gen_tuple 的新 ownership 代码一起编掉。副本消费不依赖 extension 分配，普通数据的 lease 分配失败按普通准入错误处理，不伪称副本。

MT7986 不套用 E `:3028` 的 MT7988 reason 重编码；未知 reason 不凭当前 FOE 状态“补判”。32 个 reason、负载伪装、缺 valid、畸形 descriptor、无 callback、profile fault、补环/释放均需要真实 helper 测试。

所有 BIND 从启动不可建立时，已经没有需要副本维持的合法硬件 CT 账户。正常软件栈承担普通 conntrack，故删除 slot owner、producer epoch、MIB/CT refresh 和维护 worker 是有效收敛，不构成连接寿命遗漏。

## 7. 产品依赖、能力损失与范围复核

固定关闭 NF flow cache 的落点成立：K `nf_flow_table_core.c:306/312/329/331` 在共同 add 发布/CT timeout/work 前拒绝，`:340/534` 的 refresh/lookup 配套；nft 前端必须在 `nft_flow_offload.c:662/678` 的 OFFLOAD bit/TCP 准备副作用前跳过，保留 `:694/702` 的正常释放/clear 路径。T `mtk_ppe_offload.c:1053` 确实只处理 BLOCK/FT，可在实际 ndo 固定拒硬件 provider；普通软件 qdisc 不需删除。保留 HNAT 编译分支避免 E 启动另一套 generic PPE，这一点必要。

**QoS 适配是既有依赖的必要收口，不是新增 QoS 引擎。** F 的 eQoS CLI `:24/461` 是 1–31 硬件、32 起软件；`start_all():531` 即先要求 HNAT，`enable_hnat():209` 会开 hooks、写 BIND threshold/turboacc，`stop_all():538` 也会碰 HNAT。只把规则分支改软件仍不能启动。`mtkhqos_util/files/mtkhqos:35` 在检查 HNAT 前先关 bridge netfilter sysctl，因此移出镜像比让它启动报错合适。

主合同已覆盖这些实际问题：所有 start/stop/enable/add 路径不写 HNAT/QDMA/mark/turboacc；复用 HTB/IFB；父 `1:fffe`、默认叶 `1:ffff` 与规则 1–65533 分离。旧 `start_qos():135` 没 default，旧 `program_tc_queue():351` 用父 1:1，规则 1 直接切软件会撞父类；当前约定避免了冲突。默认叶解决未命中及零单设备限速绕总带宽的问题，软件 filter 明确 skip_hw。

**P2 产品要求：**

- 同步 init validation `root/etc/init.d/eqos:35`、CLI 上限及 LuCI `eqos.js:144/145` 的说明/范围；已有 65534/65535 配置明确报错或做显式迁移，不能静默占用父/默认类。保留 tc 命令失败回传及只清理自己资源的行为。
- 总带宽启动就建立 HTB/IFB 和默认叶，不能等第一条非零规则才创建。新 root 的 default、父/叶 rate/ceil 关系、IPv4/IPv6 u32/mirred、未匹配流、零限速、stop/restart 都验收；不把默认叶设成根之外的旁路。
- 旧硬件加速 UI/状态不能宣称已生效；生产 guard、生成配置、依赖包一起核。移除 mtkhqos 的自动运行，不额外关闭 Linux bridge/NF、IPv6 或软件 NAT。不得为了禁硬件 TC 而顺手删普通 tc/IFB 包。

**已经公开且需独立验收的代价：**独立非桥有线路由也失去 PPE/NF-flow 加速；硬件 HQOS/mcast/sampling/私有 continuation 不再提供；H/E 不支持运行时卸载解绑，失败可能需重启；整 BSS 配置事务会中断普通数据及数据 EAPOL，group 隔离行为更严格；未闭合 WDS/APCLI/A4/MWDS/backhaul/GO/动态 perSTA VLAN 拒激活。WED 基础 DMA/token/无线 crypto/reorder 和正常成功 L1 SER 是保留承诺，不能悄悄改成“关 WARP”或“禁在线 SER”来绕过第 2 节。

未承诺 br-lan qdisc 覆盖所有纯桥内部流量，这一限制应保留在 UI/维护说明中。当前没有吞吐测试，不填写损失比例，也不把典型 br-lan 拓扑当成独立有线性能不会下降的证据。用户未回复性能偏好不构成性能接受或刷机许可。

除第 2 节的直接 SER 依赖外，本轮没有找到需要重新加入全局 topology epoch、动态 backend、pending 世代、CT 维护或全 Linux xmit 撤回的理由。配置 gate/lease、真实桥策略 checker 和受限静态 VLAN 支持仍有实质代码量，不能宣传为只改 AP callback 几行。

## 8. 关闭 P1 后仍不可省略的实施门与验收

1. **精确实现审查。** 在本 worktree 按主合同分组实施；从真实 postimage 生成补丁再核全系列。审查每个普通 RX/最终 TX/新 skb 继承点、控制面所有失败返回、全部 FOE/direct API/NF-flow/TC 入口以及永久 callback/probe 失败顺序。第 2 节 WARP STOP 的错误传播与资源边界必须实际进入补丁。
2. **真实 helper 与故障注入。** EAP 长度/封装/混合聚合，clone-COW/新 skb/旧 lease；already-up、多 claimant、单 child 失败、addif/hairpin/VLAN/down/copyout/commit 错误；意外移桥与 ifindex 复用；最终 wdev/WCID/DA/VLAN 错配；全部 reason 的消费、普通包软件继续和补环；probe 半初始化/暴露后失败，STOP 缺失/重复/成员变化、WARP stop failure、FE reset/restore/readback/open/ABORT/迟到 DONE。调用实际 helper，不能只搜 patch 文本或复制算法造测试。
3. **完整构建门另行执行。** 真实 MT7986 kernel/D/W/netifd/ucode、生成配置、普通导出与模块许可、undefined-symbol、skb extension 边界、force-unload/overlay/解绑限制、eQoS 软件依赖、RRM/WNM/FT 及现有生产 guard。选定源码准备不等于完整 prepare/build。本轮未运行，也不查询 CI。
4. **硬件正确性需另获刷机授权。** 冷启动及正常 L1 后验证同一常驻 CPU profile、有效 FOE 地址/大小、SMA/默认 CPU 出口、无 BIND/KA/cache/private producer；确认 WED 必要收发/crypto/reorder 与 FE/PPE/WED 的真实停止/复位完成条件。任意客户端同/跨 BSS、跨频、外部 AP 来回且旧 STA 保留时，关联请求/回包、FDB/实际出口及业务交付；覆盖 IPv4/6、ARP/ND、非 IP L2、group、BA/聚合、hairpin/隔离/静态 VLAN/动态 reload 和成功/失败 SER。
5. **性能与产品验收独立。** LAN/WAN、有线/无线吞吐、CPU、时延、重配置停顿及 eQoS 总带宽/规则/默认叶。WED 慢路径不成立、任何绕桥缓存、错误出口/越权/重复、DMA 资源误释放、失败假成功均停止验收。用户通知构建完成后再处理产物，不持续监控、不自动刷机或发布。

当前批准保留的设计结论是“固定能力可以替代 r2 大部分动态加速框架”；当前未批准的是把未经 checked 的 WARP STOP 当作该最小 SER 的物理成功凭据。补齐这一条有限合同即可重新裁决，不需退回双 MAC/固定 IP 候选，也不需等旧 STA 未清理根因全部查明。
