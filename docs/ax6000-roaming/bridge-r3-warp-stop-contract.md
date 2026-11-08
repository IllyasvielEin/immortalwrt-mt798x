# r3.1：WARP checked STOP 与不可恢复失败边界

2026-11-09。只读核验并新增本文；未实施、构建、提交、联网、操作路由器或启动子代理。修订对象仅为Astra r3第2节唯一P1。正常WARP与MT7986在线L1 SER保留，不恢复通用backend/epoch/CT框架。

**必要修订：把STOP拆成“确认硬件及CPU生产者停止”和“之后的ring清理/重建”。任何可检测失败都不得成功ACK，也不得进入free、unmap、软件ring/index reset或IRQ/DMA恢复。** 原来在FE等待之后检查ABORT太晚，必须在`WLAN_HOOK_SER`之前检查，并让实际WARP结果到达D。

不可证明DMA停止后有两个不同产品终态，必须明确选择，不能由实现偷偷决定：

主线程 r3.1 现拟选下表 B 送审，已向用户说明新增整机异常重启代价；尚未获得 Astra 放行、故障体验验收或刷机授权。A 仅保留作范围比较，不进入实施。

| 终态 | 代价与当前结论 |
| --- | --- |
| A：FAILED系统继续运行，等待用户重启 | 必须闭合所有后续ifdown/HIF_EXIT/adapterfree/设备解绑与code/context寿命，不能只让当前SER返回错误。实际D/W存在这些旁路；仅几个void early-return不够。本轮**不宣称A已关闭设计**，也不继续扩PCI核心移除框架 |
| B：不可恢复DMA停流失败立即进入整机致命错误终态，尝试硬复位；不能复位则不回正常执行 | 改变失败体验：整机异常重启，可能丢未落盘状态；硬件reset未成功可能需断电。可用少量同一fault闩/真实析构入口guard封住后续清理，不需要运行态retain/热移除框架。下面给出B的可复审执行合同，**不等于用户已接受该新增代价或Astra已放行** |

## 1. 已核来源与实际反例

D为42patch mt_wifi postimage；W为实际external `warp_20250408-5f71ec`源码，并核本worktree `package/mtk/drivers/warp/patches`。这些patch中001只把检测线程睡眠改`msleep_interruptible`，008把remove改void；不提供STOP错误传播。E为82patch四文件Ethernet postimage。reset链使用本worktree`.analysis/linux-6.12.112-fatal-postimage`的8个准确文件，已含唯一触及它们的MTK WDT timeout-clamp patch，清单有来源/hash；不是完整内核prepare。

- D `embedded/hw_ctrl/hw_ctrl.c:1165`先无条件调用SER hook；STOP ACK/FE等待在`:1223–1229`。`os/linux/rt_txrx_hook.c:18–30`始终返回0。
- D `embedded/plug_in/warp_proxy/warp_proxy.c:768`先按配置`whnat_en`跳过所有hook，`:750`的SER wrapper为void；W `warp_main.c:1283`也为void。
- W STOP_PDMA0 `:1314–1327`在硬件停止证明前写TX/WDMA软件ring，忽略module fallback结果，随后reset RX ring并`wed_rx_bm_exit`。`warp_bm_v2.c:971–984`最终drain page、vfree cell、释放DMA descriptor；`warp_utility.c:49/55`实际`dma_free_coherent`。
- W `regs/reg_v2/warp_hw_v2.c:1211/1566/3037`逐项覆盖ret，`WHNAT_RESET:25`只打印超时；`mcu/warp_woctrl.c:197/213/398`消息结果和返回值被吞。`reset_rx_d_traffic:1637`甚至会在STOP链末尾退出WO SER状态。
- 额外直接停机检查错误：`reset_wed_rx_dma():1470`当前用`value & WED_GLO_CFG_FLD_RX_DMA_BUSY`；`regs/coda/wed_hw.h:6/667`证明该宏是**位号3**，必须用正确BIT掩码检查，不能把原表达式算作已证明RX DMA idle。

这些证明错误可被抹掉后继续释放/成功ACK，不证明现场已发生UAF。

## 2. 有限返回通道：值型结果，不改通用hook语义

在D既有`ERR_RECOVERY_CTRL`增加本轮同步SER请求/结果字段：phase、result、stop_proven、cleanup_done、cancelled及absolute deadline；不含临时资源指针，不是站点generation。D每次准备调用时先设`result=-EINPROGRESS`，`stop_proven=false`。proxy只在实际checked helper完成后写结果；D在现有固定0 dispatcher返回后读取该字段。

`client_handle`的SER分支必须移到`!client_whnat_en_get()`早退之前；它依据**实际已附着/已启动WARP资源状态**调用新`int warp_ser_checked(...)`，不能按配置开关跳过。资源曾经存在但entry查找失败、没有listener回写、请求未处理均失败（`-ENODEV/-EINPROGRESS`），不能默认成功。仅初始化记录明确证明从未建立相关engine/ring，或此前已完成停机且仍静止，才允许值型`QUIESCENT_NO_RESOURCE`成功；不是看到whnat_en=0或空指针就成功。

proxy为独立`mtk_warp_proxy.ko`，D core不能直接反向导入proxy函数形成循环模块依赖；沿现有hook传值正好避免这个问题。其他hook仍按旧语义，不为本P1改整个注册/注销框架。

实际状态/Status合法后才调用对应checked phase；不再让STOP_PDMA0的任意无关MCU事件触发ring清理。D在调用前、可阻塞操作返回后、成功ACK前检查fe_failed/cancelled。此规则对**Wi-Fi自主SER（notify_fe=0）也相同**，不能只放在FE等待分支。

## 3. STOP phase：不重置、不释放、不恢复

在D的可睡眠HwCtrl SER task执行，不持RTNL、MacTab/ring/ser自旋锁睡眠。W每owner用一个有限SER控制互斥/短gate串行硬件控制；ABORT notifier不获取该互斥，只原子置错/取消并唤醒。不能在RTNL notifier内等HwCtrl、WO或BM。

**A. 封住CPU生产者。** 在W局部gate下先置stopping，阻止datapath ISR、proc直接BM操作、timer/tasklet再次排队/重开中断；保持WO消息应答所需CCIF通道可用，不能先把它也关掉。已有运行者排空后才改变其共享ring/hardware状态。

实际需覆盖：TX `bm2high_monitor`及`tbuf_alloc/tkn_alloc/tbudge_refill/tbudge_release`；RX若实际启用则`rbudge_release/rbudge_refill/rbuf_alloc/rbuf_free`及`extend_monitor`（EXTEND_POLLING）；`warp_bm_v2.c:778/781/1473/1593/1628/1680`所有排队点。先禁止生产/自重排，再`del_timer_sync`与同步kill已初始化tasklet；**不调用仅kfree task结构的unregist_*作为drain**。仅对initialized/实际启用对象操作，不假定MT7986所有动态cap当前已开。

W的`wed_ser_task`（生产001为可中断睡眠）也须在stopping时不检测/触发新SER/MMIO恢复；进入检测的短活动计数排空，或在本局部gate下确认它已离开。无需销毁并重建该kthread。proc手动alloc/free、suspend/resume、DMA_SET与其它ring控制在SER中不得绕过gate。所有等待使用同一STOP总deadline；不是每个子步骤重新获得几秒。

现有`wed->ser_ctrl.ser_lock`只保护统计更新，不是上述串行证明。实施增加局部短gate/活动计数：变更BM/排队的caller先在gate内检查并占一个活动者，结束归还；STOP在同一gate内禁止新进入，锁外同步timer/tasklet并等待活动者归零后才动硬件。不能持该自旋锁做WO等待、同步kill或cache/poll。同步取消对象的初始化位与退出责任明确；某个等待无法在deadline内完成也不继续STOP成功/清理。

**B. checked WO与engine stop。** 新stop-only helper提取原函数里的disable与idle检查，不调用带有reset/free/自动恢复副作用的旧`warp_reset_hw()`。顺序保持硬件管线依赖：WO成功进入SER_RESET；TX WED DMA、WDMA RX及其driver、TX free/BM、WPDMA TX/RX agent；RX-D driver、RRO QM、route QM drain、WDMA TX及driver、WED RX DMA/RX BM的全部必要idle。按原管线可继续执行不释放资源的安全disable以尽量停流，但首个错误锁存；任一步失败最终必失败，后面的0不得覆盖它。

所有busy检查使用真实位号/BIT、明确enable清除及必要读回；poll要有remaining deadline/cancel检查。`WHNAT_RESET`的复位写入不属于STOP成功的捷径。**本候选不保留“STOP失败后自动module reset”fallback**，因其没有独立全engine停止证明；正常L1停流成功仍在线恢复，确实停不了则走所选不可恢复终态，不关WARP。

WO消息检查`warp_msg_send_cmd`返回**恰为WARP_OK_STATUS=0**，正的firmware拒绝/状态值不能被`ret<0`遗漏。checked wrapper转换vendor错误为负errno（超时`-ETIMEDOUT`，取消`-ECANCELED`，FW非零/不可判定`-EIO/-EREMOTEIO`）；消息wait timeout叶节点须保留超时原因，不能由void外壳吞掉。`warp_msg.c:554`已有有界completion等待，参数改为剩余预算；WO control只在实际成功后更新cur_state。STOP成功后**保持WO SER_RESET**，不执行原`:1637`自动exit/ENABLE。

整个phase无`wed_*_ring_reset`、无WDMA软件descriptor重写、无page drain、无DMA unmap/free、无IRQ/DMA enable。源码没有显示软件ring reset是上述idle检测的先决条件；原来“先改软件ring，再确认停机”不能继续作为顺序依据。硬件是否需要其它有限drain动作由实现/实机核验，不能允许它在未证明停止时free。

全部CPU/engine停止且未cancel，才设stop_proven=true/result=0。D之后在短RTNL通知区再次检查结果，仅在此条件发一次成功STOP ACK。失败通知可复用STOP_FAIL，但E在**任意状态**收到真实stop-fail都锁存fault；不能进入原V3 fallback、排新FE reset或只在stop_waiting时才识别它。

## 4. FE_DONE之后才清理、重建与恢复

保持r3已审顺序：E stop成功后reset FE，checked restore固定CPU profile，内部分阶段open PDMA，再发成功FE_DONE。D醒来确认结果为成功、stop_proven且未ABORT，才进入cleanup phase：

1. 在仍停止的WED/WDMA上做所需index/reset完成检查；仍用checked返回，不能调用会重新停止/重启WO的旧混合wrapper。
2. 复用原`wed_tx_ring_reset`、`wdma_tx_ring_reset`、`wed_rx_ring_reset`和`wed_rx_bm_exit`，只针对已证明可安全触及的旧资源。caller独占这些资源；清理最多一次，ABORT先于本次mutation许可则不执行。不存在撤回已安全完成清理的承诺。
3. 随后才允许D原HIF free/reinit。`HwRecoveryFromError:1246`必须检查`WfHifInit`结果；`WfHifInit:197`的HIF_INIT hook也用同类值型checked结果，不能其返回固定0后照开IRQ/DMA。
4. W `warp_ring_init:1063`必须逐层检查`wed_rx_bm_init`、ring/WO配置等实际返回，拆开“配置/分配”与IRQ、EINT、ATC、DMA/WO恢复。全部准备成功、再次检查未fault后，才checked退出WO SER_RESET并执行正常恢复。任何失败不进原无条件enable路径；新分配但尚未暴露的资源可安全逆序回收，已可能暴露的资源不能自动走普通err cleanup。

`warp_ser_handler`的其它stage、`warp_dma_handler`、HIF_INIT及早期STOP_IDLE_DONE分支同样先检查本轮fault，防下一条MCU消息又重开IRQ。必要的正常控制中断与用户DMA区分，WO响应通道在等待期间保留；不为修失败分支盲删正常L1所需控制中断。

## 5. 选B时的noreturn终态及最小析构guard

只在“可能仍有DMA，但不能证明安全停止/完成”的严重错误进入B；普通包错、软件配置错误不触发整机reset。D在HwCtrl中checked STOP返回失败后，先原子发布统一fatal DMA fault、关闭普通数据gate，再**同步调用不返回的故障入口**；不能排reboot work后先返回，也不能进入成功ACK/FE等待/普通err unwind。E STOP总timeout若存在未确认停止的活动participant，同样直接进入该终态。ABORT notifier在RTNL中仅置错/唤醒；实际HwCtrl醒来/阻塞操作返回后进入终态，不能在notifier等待自己。

实际资源析构旁路需加同一局部SER/teardown互斥准入和fault guard，数量有限：

| 真实入口 | 必须在什么动作之前检查 |
| --- | --- |
| D `mt_wifi_close`、void `RTMPDrvClose` | 任何普通close/资源退出之前 |
| D `WfHifSysExit:340`及`WfSysPosExit`/init错误出口 | HIF_EXIT、free IRQ/txrx/token、DMA reset之前；不能下层拒绝后上层仍继续 |
| D `RtmpRaDevCtrlExit:3091`、`RTMPFreeAdapter:2241` | 释放adapter/hdev/os_cookie之前 |
| 实际MT7986 `rbus_prop_dev.c:213 mt_rbus_remove`及module exit | RtmpPhyNetDevExit、iounmap、pci_release_regions、free netdev/hif之前；不能在void remove里返回errno |
| D proxy `warp_proxy.c:860 warp_proxy_exit` | `mt_wlan_hook_unregister`之前；与同步checked SER共享准入，不能回调中途卸掉proxy代码 |
| W `warp_ring_exit`、`warp_client_remove`、`warp_remove`、module exit | reset/interface、platform_driver_unregister、devm资源释放/全局entry释放之前 |
| E `mtk_stop`/DMA销毁入口 | SER外部close或全局fatal时不得释放尚未确认停止的共享DMA |

MT7986 Makefile块固定PROPRIETARY=y→PLATFORM_DRV_SUPPORT=n，选择rbus_prop PCI frontend；最终构建仍要核确认，不把另一个未编译frontend当保护。**每个guard在fatal时调用同一noreturn入口，绝不返回到会继续free的caller。** 因remove callback不返回，drivercore不会接着释放devres；因此不需要新增PCI核心veto字段/移除框架。

不能只“入口读一次fatal，然后与SER并行free”。首次硬件touch前，D/W owner的SER与破坏性入口以短状态准入串行：正常析构已持准入则拒开始本轮SER；SER已持准入时能可靠返busy/defer则拒，否则void销毁/跨入清理进入同一终态。计划内cleanup仅由持有本轮准入的同一SER同步控制栈在已证明停止后执行，外部close不能冒用；嵌套的D→proxy→W cleanup显式传同一控制许可，不用跨RTNL的全局嵌套深度。proxy退出亦参与，callback中途不能unregister/free代码。不在RTNL close里等会取RTNL的HwCtrl。成功结束才释放准入，不可恢复失败不回正常态。销毁交叉也可能触发整机故障终态，必须公开。

## 6. 准确arm64 reset链：可实现，不能承诺必成功

由builtin E提供产品专用普通`EXPORT_SYMBOL`的`__noreturn`硬件故障入口，专有D/proxy只调用它。原子发布fatal并选首个CPU为复位执行者，设置`reboot_mode=REBOOT_HARD`后直接`machine_restart(NULL)`；其它进入者关本地中断并停留，兜底亦不返回，不争用可能被停住CPU持有的锁。不调用normal `kernel_restart`、`orderly_reboot`或shutdown链；不把D直接调用GPL-only符号作为设计。该API属于实际E硬件owner的严重故障处理，不是向模块重新导出任意arch调用。

调用前释放短gate/统计/entry自旋锁，不拿RTNL等待别的线程，也不把console输出、文件写入或用户态通知作为reset前提；固定错误码可先存入无分配状态。SER排他准入不再回到正常态。其它CPU的析构guard先读全局fatal，再尝试准入，避免在已有fatal时睡到一个永不返回的SER互斥上。

准确postimage证明：

- `kernel/reboot.c:92–97`的normal restart_prepare调用reboot notifier、`device_shutdown`，不适用于未知DMA状态。
- `emergency_restart():84`是GPL-only导出，且先执行kmsg dump；`panic()`虽为普通导出，但`panic.c:425/436/451`可执行crash_kexec/panic notifier，超时可变，不能当作“必定立即硬重启且不清理”。因此本有限入口选择直接arm64 machine_restart，避免这两类额外链。
- `arch/arm64/kernel/process.c:127–147`先关本地中断、`smp_send_stop`，再调用restart handlers；若均不能复位，本CPU永远不返回。E本来builtin，调用未导出的全局arch函数不要求给专有模块导出它。生成配置必须确认ARM64、E builtin、EFI=n；当前generic为EFI=n。
- `arch/arm64/kernel/smp.c:1133–1220`明确SMP stop **best effort**：IPI等待1秒，具备能力时再NMI10ms，仍失败只警告。故“当前CPU不返回”不等于全系统绝对不执行ifdown；第5节析构/控制准入与全局fault guard是必需补充，不能省掉。
- PSCI `psci.c:308/326`优先级129，HARD模式走SYSTEM_RESET；MTK watchdog `mtk_wdt.c:228/439`优先级128，关闭IRQ模式后循环写SWRST。目标配置WATCHDOG=y、DTS watchdog okay，支持这条实际候选链。注册是否成功、ATF与整机复位范围、watchdog能否完成仍须实机验证；不能因C函数while(1)声称已复位。

若其它CPU没有停住，它一旦进入本资源的close/reset/free/module-exit guard，也会进入相同noreturn链；不能继续清理相关DMA/context。停止前已在破坏性操作中的竞争由第5节SER准入排除。若CPU或总线硬件已严重失常、连guard/复位请求都不能执行，不作软件绝对保证；这属于硬件致命故障验收/断电恢复范围，不伪称“安全继续运行”。

可确认静止且尚未重开/暴露DMA的其它恢复错误允许QUIESCENT_FAILED：本轮任务退出并drain后可转受控teardown许可，错误闩不清、数据不恢复、无下一轮SER。noreturn入口只处理缺停止/完成证明或无法安全串行的销毁交叉；不能把所有软件错误都升级整机复位。任何新DMA/WO enable或新资源暴露前撤销旧stop_proven，避免错误重用旧证明。

## 7. 关闭P1的设计/实现门

主稿先明确选A还是B及产品代价；若选A，本轮尚缺运行态retain生命周期，不能直接GO。若选B，可按上述有限合同复审，不增加PCI核心或通用WARP卸载框架；需明确真实析构入口和SER准入作为硬条件。

真实helper注入必须包括：首项失败末项成功、WO返回正错误/timeout、错误busy位/一直busy、缺listener、whnat_en关闭但资源仍活、ABORT在调用前/WO等待中/返回后到cleanup前、Wi-Fi自主SER、E并非stop_waiting时收到stop-fail、ifdown/HIF_EXIT/remove与SER竞争。结果为零成功STOP ACK、零未确认停流的ring reset/free/unmap、零HIF/IRQ/DMA恢复；B还须证明故障入口不返回，不能用真机直接无授权触发reset。

正常成功L1仍须恢复WED基础收发/crypto/reorder与固定无BIND profile。硬件busy/WO协议含义、复位完成和致命故障实际重启都需另获授权的真机验收；源码审查不替代这些结果。设计中的返回、错误闩、清理边界必须先闭合，不能把唯一P1留给真机“试试看”。
