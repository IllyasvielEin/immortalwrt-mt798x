# 固定 CPU profile：保留 MT7986 L1 SER 的最小合同

2026-11-09。最后一轮有限只读核验；未实施、联网、操作路由器或启动子代理。本文替代范围比较稿中“为省去SER框架而拒绝全部在线FE恢复”的建议，**不额外关闭在线FE L1 SER**。主方案选择仍由主线程/用户与Astra复审决定。

结论：**可执行，不需要r3完整的拓扑epoch、动态backend列表、reset-id或最终N9 ACK。** 前提是固定CPU产品确实从不BIND、不发布HNAT资源型packet/control hooks、不运行HNAT维护work，FOE资源常驻且不换所有权。保留现有MT7986 L1物理恢复流程，只把固定profile恢复提前、检查失败并给现有等待增加有界失败出口。不能据此称现有匿名ACK与无限等待已正确。

## 1. 本轮准确入口

E=`sources/linux-6.12.112-ethernet-review-postimage/drivers/net/ethernet/mediatek`；D=`sources/vendor-mt79xx-7.6.7.3/review-baseline/mt_wifi`；H=`sources/hnat-6.12.112-review-baseline/drivers/net/ethernet/mediatek/mtk_hnat`。相对根为`/home/illya/workspace/router/ax6000`。

- E `mtk_eth_soc.c:6176 mtk_pending_work`为共同恢复worker；`:6184`拿RTNL，`:6190`已经prepare硬件，`:6204`释放锁，`:6207`无限等Wi-Fi STOP ACK。`:6229`停止netdev，`:6246`调用`mtk_hw_init(true)`却忽略返回，`:6253`开始`mtk_open`，失败只dev_close后继续；`:6277`通知Wi-Fi RESET_DONE，`:6288`才NAT_DONE。
- E `mtk_open():4860`先start DMA，`:4886–4896`按offload_version选GMAC→PPE。固定产品必须始终选择已有`MTK_GDMA_TO_PDMA`，且profile未准备时不能开始DMA。
- D `hw_ctrl_basic.c:400`仅处理FE START_RESET，置`notify_fe=1`并发L1命令；`:411`收到FE RESET_DONE只complete。目标未实现STOP_TRAFFIC/START_TRAFFIC。因此MT7986一律使用START_RESET→RESET_DONE，不复用V3轻量事件分支。
- D `hw_ctrl.c:1223–1229`在STOP_PDMA0阶段向E回`MTK_WIFI_RESET_DONE`后无限等待FE；后面`:1237–1265`才开始free/reinit HIF、enable IRQ/DMA。因此这里是最小ABORT检查点：失败必须在这段破坏/重启动作之前返回。
- E `mtk_eth_reset.c:48–69`当前以全局计数接ACK，ONLINE/OFFLINE也重写计数；`:71`可排FE恢复工作。D全树本轮未找到发送START_RESET_INIT的调用者，不能凭空给D增加一个发起协议。
- H `hnat_nf_hook.c:515`当前NAT_DONE调旧warm_init；旧warm会清表并重新开启GMAC PPE。固定profile不注册这条HNAT netdevice消费者，NAT_DONE不再有恢复profile/READY的权限。

## 2. 仅需一个永久 owner callback

在builtin E的每个真实hardware owner记录：固定profile restore函数指针、私有context、`profile_ready`、sticky `profile_error`、`ser_active/stop_waiting`。不是可扩展backend链表，没有拓扑epoch或嵌套事务。注册/读取/调用均ASSERT_RTNL；除数据面只读状态外，不从softirq调用restore。

H在分配和初始化全部状态、确认常驻维护限制后一次注册`restore_cpu_profile(owner, context)`；拒第二个不同注册者。H模块无module_exit、H/E解绑入口禁用，已暴露DMA资源永久保留，故函数和context寿命覆盖E所有调用；不提供运行时注销。一次取局部callback并在RTNL进程上下文调用即可，不需要RCU/SRCU。模块引用不能代替资源约束，常驻表/设备限制仍按CPU-profile生命周期合同执行。

callback只恢复同一分配的有效表地址/大小及固定SMA、默认CPU出口、禁KA/scan/cache/mcast/sampling/HQOS等profile，并逐层返回cache/请求/读回错误；无BIND、pending/MIB账户、异步producer和重新发布hooks。先完成全部检查再由E标profile_ready。不能先调用旧`hnat_hw_init`启用通用profile，再在流量已恢复后补关私有模式。

初次probe暴露DMA后的失败保留FAILED绑定/资源；callback缺失、context未准备、sticky错误均使E open/普通AP activation明确失败。不能把NULL callback当“无需后端，可以开始”。冷初始化和SER共用同一个checked固定profile helper，避免两套寄存器配置漂移。

## 3. 最小正常顺序

1. E worker取得RTNL，先拒sticky错误/重入；置ser_active、profile_ready=false，**之后**才dump/save/prepare或发通知。固定CPU的数据面从不建立/消费flow cache，另一个bridge link无需HNAT缓存屏障；它不必被误判成“内层事务”，本设计根本没有跨unlock的transaction depth。
2. 在发送START前重置本轮stop completion和计数，清stop失败状态。目标所有在线Wi-Fi listener发一次START_RESET；实际事件广播仍沿netdevice notifier，使用MT7986支持的事件。结束本段RTNL操作，unlock等待STOP ACK，使用有界timeout；初始产品deadline可取5秒，最终值由真机验证。无Wi-Fi participant时不等。
3. E重取RTNL并核本轮没有失败/成员变化，才做现有第二次prepare、停止原running端口及FE reset。保留原必要硬件顺序；检查pinctrl/`mtk_hw_init(true)`的可返回错误。不是把软件profile标志当物理stop证明。
4. **`mtk_hw_init`成功后，第一次`mtk_open`及任何Wi-Fi RESET_DONE之前**，同步调用registered restore。此时FOE仍为同一分配；全部检查成功后profile_ready=true，但ser_active仍保持。没有BIND资格需要另外“最终放行”。
5. 检查每个需restart端口的`mtk_open`；它无条件选PDMA，禁止由offload_version/l4s等分支恢复PPE/私有旁路。任何一个端口失败都不得继续走成功通知。保持原有必要MAC/reset/QDMA收尾顺序，并核可检测错误。
6. 只有所有必需端口和profile成功，才发一次FE RESET_DONE解开D等待；原NAT_DONE可保留为无副作用观测通知，不能调用旧warm。完成E必要收尾后清ser_active/MTK_RESETTING。D继续现有HIF/N9恢复；本固定profile不等待最终N9 ACK才恢复一项并不存在的BIND许可。

这里profile_ready只表示固定CPU硬件配置可用，**不表示全部无线终端或N9已经恢复**。D原有Stage RESET_PDMA0→WAIT_N9_NORMAL及ErrRecoveryEndDriverRestore继续保留，不新增另一套最终ACK作为缓存准入前提。

## 4. 最小ACK约束，不沿用裸计数的过度承诺

不用reset-id仍需要这些有边界的约束：

- E `pending_work`同一work对象串行执行；额外用ser_active拒其运行中/排队的重入请求。第二个cycle不能在前一个cycle失败后继续，sticky错误直到重启；因此失败后到来的旧ACK永远不可能放开下一轮。
- D START只在没有FE等待、没有sticky失败且现有恢复stage为STOP_IDLE时接受一次：初始化completion和FE结果，置waiting/pending，再发CmdSER。重复START不能重置正在等的completion，也不能重复触发L1。前一轮FE_DONE已发出但D仍在HIF/N9恢复时，用这个**本地已有stage**拒绝下一轮START；不需要为此增加最终N9 ACK。E检查notifier的明确错误，转本轮ABORT，不能忽略busy后继续物理reset。
- D在`:1223`使用一次性的`atomic_xchg(notify_fe,0)`决定是否回STOP ACK，最多回一次，再等待FE结果；旧代码read→set改为一次性交换。E仅在stop_waiting时接收目标ACK，完成后关stop_waiting，多余ACK不积累到下一轮。
- STOP ACK同步在D的notifier调用中交付E；E每成功cycle必须先收到全部STOP ACK才开始FE reset，故正常成功cycle的这份ACK不存在仍滞留未交付的异步消息。重置completion必须先于发START。没有据此声称可以接受任意外部provider伪造/重复ACK。
- wait期间ONLINE/OFFLINE不能重写本轮剩余计数。E在已有这两个notifier分支检测ser_active，标本轮失败并唤醒等待，停止成功路径；动态数量仍可更新供以后诊断，但不得让一台离线等价于“它已经STOP”。本目标不为恢复期间新加入adapter提供继续恢复能力，报错后重启。

若实现仍保留任意重复START、成员变化重写计数或失败后重试，则上述证明失效，必须修正；不能用零BIND替它背书。这个有限合同不引入participant世代/reset-id/最终N9协调框架。

## 5. 最小ABORT与实际失败出口

新增一个FE ABORT通知/结果，**不复用成功RESET_DONE**。D记录独立`fe_result`与sticky `fe_failed`，START设PENDING，DONE仅在匹配waiting/PENDING时设0并complete，ABORT设负错并complete。失败之后的DONE不能覆盖错误。ABORT处理应位于现有`!fRTMP_ADAPTER_START_UP`早退之前，使正在退出/清flag的等待者也能被唤醒。

D在`:1229`改为有界wait（大于E停止/恢复deadline），醒来后先查结果；超时或ABORT：置fe_failed，保持StopRxDMA/原停止状态，停止后续普通数据调度，返回失败；**不执行后面的tm/qm/hif reset/free/reinit、IRQ/DMA enable或向N9报告成功**。后续MCU恢复事件也先检查fe_failed，不能从另一个case重新开启。这里不为了失败退出而free仍可能被硬件触及的资源。

D notifier exit在unregister之前，若仍FE-waiting，自己置ABORT并complete，再按既有adapter退出合同drain工作；不能先移除通知者后把它留在无限FE等待。E若在RTNL外等STOP时观察participant退出，同样走sticky失败。该调整处理已有等待者寿命，不是新恢复框架。

| 失败位置 | E/D明确行为 |
| --- | --- |
| callback缺失、已有sticky错误、START重入 | 不触硬件、不开始新cycle，返回/记录错误 |
| STOP ACK timeout/失败、等待时成员变化 | E不继续FE reset或表重建，profile notready，发ABORT；保留资源，不伪发成功DONE |
| pinctrl/FE init/restore/cache/readback失败 | E保持notready/sticky错误，不open DMA、不发成功DONE；ABORT唤醒D |
| 某个mtk_open失败、成功通知前收尾失败 | 整轮失败，不“dev_close后继续成功”；已分配资源按各owner安全stop合同保留，不能未经停机证明追加free路径 |
| D自身等待超时或ABORT | 不恢复HIF/DMA，sticky失败；下一条DONE/MCU消息不能复活 |
| 重复NAT_DONE | 固定profile没有旧HNATwarm notifier，完全不改变ready/error/寄存器 |

E失败路径不清错误后重排一轮；MTK_RESETTING/ser_active如何收尾须保持“不可重启”的错误字段可见，不能仅留下一个日志。当前源码`mtk_open`失败及其内部DMA allocation unwind还需实现审查，不能靠本文证明任意既有DMA错误路径已安全；本修复不得让HNAT FOE进入该free链。

## 6. 修改边界与放行检查

只需E owner字段/一次注册、pending worker顺序/有界STOP等待/错误分支、mtk_open固定PDMA、现有reset notifier的计数与成员变化规则；H固定profile restore、取消NAT_DONE旧warm消费者；D START/DONE/ABORT结果、一次STOP报告、FE等待检查及exit唤醒。**无net/core通用backend框架，无拓扑epoch，无新的最终N9 ACK。**

实现/真机必须验证：profile restore严格早于任意DMA/RESET_DONE；未READY普通open失败；STOP缺失/重复、ONLINE/OFFLINE、restore失败、首个/后续open失败、ABORT早到/晚到、迟到DONE、旧NAT_DONE均无假成功、无再绑定、无UAF/死锁。正常L1 SER后，普通WED RX/TX及全部软件业务恢复，无BIND/KA/private producer出现。

FE/PPE复位完成、WED停流与slow-path寄存器实际表达仍属硬件验收；有常驻表和软件标志不替代该证明。本轮找到的是可执行的最小控制流，没有证据要求固定CPU profile必须增加r3整套reset-id/最终ACK框架，也没有声称现有L1恢复已验收。
