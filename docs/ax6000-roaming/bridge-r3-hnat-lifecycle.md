# 通用桥转发 r3：HNAT 生命周期与共享后端状态修订

2026-11-09。本文是对 Astra r2 审查 P1-3、共享 P1-2 状态界面及相关 P2/P3 的设计修订输入，不是实现通过记录。仅只读源码并新增本文；没有修改生产源码、构建、联网、操作设备或启动子代理。Ethernet SER 实际 caller 的改序由主设计单独给出，本文定义其必须遵守的后端合同。

本文输出位于独立 `fix/ax6000-bridge-forwarding` worktree，来源基线保留只读。为回应改动范围顾虑，下面明确区分依赖与独立加固：裸 hook 生命周期、正在运行的 HNAT work 停止、SER 状态和失败不 free DMA，属于本次新增统一 gate/重建操作直接需要的合同；不能省略。**不在本修复中重写所有平台的 multicast/NPU、第三方 tunnel/crypto、WARP 自有 DMA 热退出或实现安全热卸载。** 对原先未获本版支持的加速 provider 只在实际注册/激活入口拒绝，而非开展其内部重构；普通软件功能和无线基础加密不因此删除。支持热卸载所需的完整物理停机证据仅作为未来能力的放行条件。

## 1. 裁决与首版收敛选择

保留“普通 AP 由 Linux bridge 决定转发；桥依赖的双向流恒定软件；仅正向证明的独立有线路径可 BIND”的最终策略。**首版不支持运行中破坏性 HNAT 卸载、设备解绑及手动 enable/disable，且必须从实际发起入口禁止。** 不实现一个缺少硬件停机证据的热卸载功能，不将 cache 清理、CPU grace 或模块引用当成 DMA 已停止的证明。

支持的重建入口仅为受控 Ethernet SER：资源保持分配，先进入持久 RESETTING、停止新提交并排空 CPU/异步使用者；实际 Ethernet caller 完成物理 reset 后，再重建并核验原表，最后由统一 commit 恢复 READY。任何失败进入 sticky FAILED，保留资源与 core 阻断记录，重启恢复。禁止 debugfs 写 enable 清除错误。

外部裸 hook 变量改为**永久 dispatcher 函数**，随本固件一起修改实际 mt_wifi/WARP 调用者。dispatcher 内一次读取活动 ops、在完整调用期间持有明确读侧。packet ops 用 normal RCU；可睡眠的设备注册控制使用 SRCU，不能把 `GFP_KERNEL` 注册操作放入 normal RCU。无活动 packet ops 时仍执行无需 FOE 表的可信 ownership/metadata 处理，不能把所有包一律 `return 1`。具体硬件 reason 分类以 P1-4 修订为共同接口。

硬件已获知 DMA 地址后的 probe/init 失败采用 **bound FAILED + retained resources**：probe 返回已绑定，但状态、日志、core gate 与 AP activation 明确报失败，绝不谎称后端 READY。不能返回 probe 错误后让 devres 回收硬件仍可能使用的内存。硬件暴露前的失败可以正常报错并回收。

## 2. 准确证据范围

| 代号 | 路径/范围 |
| --- | --- |
| H | `sources/hnat-6.12.112-review-baseline/drivers/net/ethernet/mediatek/mtk_hnat`，overlay 加全部 14 个相关 kernel patches 的 postimage |
| D | `sources/vendor-mt79xx-7.6.7.3/review-baseline/mt_wifi`，42 个生产补丁后的 postimage |
| W | `sources/vendor-warp-20250408-5f71ec/source/warp`，实际外部归档；另核 `F/package/mtk/drivers/warp/patches`，不是驱动 bundled WARP |
| E | `sources/linux-6.12.112-ethernet-review-postimage/drivers/net/ethernet/mediatek`，82 个相关补丁后的四个选定文件 |
| K | `sources/linux-6.12.112-bridge-review-postimage`，选定文件加 49 个相关 kernel patches |
| L | `sources/linux-6.12.112-roam-refs` 中 `kernel/module/main.c`、`drivers/base/platform.c`、`drivers/of/{overlay,dynamic}.c`，准确 stable 6.12.112 原文，**不是完整 kernel postimage** |
| B | 本 worktree `.analysis/linux-6.12.112-refs/drivers/base/{bus,driver}.c`，同一 stable 原文；首次读取为 `/tmp/ax6000-linux-driver-{bus,core}-v6.12.112.c` 的同内容副本 |
| F | `sources/immortalwrt-mt798x`，生产 HEAD `c196312d3916fad77fa0bb47db57e4b685c15f03` |

以上相对目录均以 `/home/illya/workspace/router/ax6000` 为根。E/K 准备使用生产 GNU patch 的默认 fuzz 上限，不能称全 fuzz=0。实际 WARP 归档 SHA-256 为 `0954055e165dcb71cd2703f07b0c2d521ca3e9c8968d2b17b68e8c9a7853f9b2`；本轮未重新下载或重算归档。WARP 的兼容补丁不提供下述缺失的裸 HNAT hook 生命周期协议。

## 3. 已核实的问题与全部外部接口面

### 3.1 裸 hook 的读者、写者与上下文

`H/hnat.c:35–68` 导出下列变量。不能只改 RX/TX：

| 现接口 | 已读到的调用者/方向 | 修订合同 |
| --- | --- | --- |
| `ra_sw_nat_hook_rx` | `D/os/linux/rt_profile.c:1823,1839,1900`；接收/softirq，FAST_NAT 与 BB_SOC 分支 | packet dispatcher；RCU 取 ops 一次，完整调用在读侧内；无 ops 走 table-independent 正常化/ownership 路径 |
| `ra_sw_nat_hook_tx` | `W/warp_wifi.c:130,157,164`，由 `W/warp_main.c:1184` TX 入口调用；另 `D/embedded/common/cmm_data.c:7655,7670` 两条宏分支 | 同上；普通用户包拒缓存返回继续 TX，维护副本不能按普通包放行 |
| `ppe_del_entry_by_mac` | `D/embedded/ap/ap_data.c:3761`、`common/a4_conn.c:402`、`fsm/ap_mgmt_assoc.c:3097`、`fsm/sta_mgmt_cntl.c:4090` | 有界、不可睡眠删除 dispatcher；无 READY 时不访问表；返回明确不可用/无需操作，不恢复 BIND |
| `ppe_del_entry_by_ip`、`ppe_del_entry_by_bssid_wcid` | H 导出；在本轮检查 D/W 中未找到额外调用者 | 同样作为受 gate 的删除接口，不能因此保留裸指针 ABI |
| `ra_sw_nat_clear_bind_entries` | H 发布/清除；D/W 本轮未见直接消费者 | 仅受控控制入口，不能作为绕过事务的 void 清表/重启学习能力 |
| `ppe_dev_register_hook`、`ppe_dev_unregister_hook` | `D/include/os/rt_linux.h:280–286` open/close 宏；`D/embedded/common/cmm_cfg.c:10921–10926` ioctl 设置 | process/SRCU 控制 dispatcher，内部允许 GFP_KERNEL；不假定所有调用者已持 RTNL |
| `hnat_get_wdma_{tx,rx}_port` | H 导出，实际 D/W 本轮未找到调用者 | 保留必要映射时用受状态检查的函数；未就绪返回负错误，不返回可误用的出口 |
| `hnat_set_wdma_pse_port_state` | H 导出，实际 D/W 本轮未找到调用者 | 不开放任意 runtime 硬件切换；仅受控内部 SER/冷初始化调用带返回值的私有 helper |
| `mtk_tnl_encap_offload`、`mtk_tnl_decap_offload`、`mtk_tnl_decap_offloadable`、`mtk_crypto_offloadable` | 方向相反：H 导出槽位供其他 provider 写；H NF 路径消费，见 `hnat_nf_hook.c:1272,1529,1701,2230,3792,4246` | 首版 NONBRIDGE_VERIFIED 不授予 tunnel/crypto provider 路径；移除可直接写的槽位，注册返回不支持，普通软件协议栈不受影响。不是关闭无线 WPA 硬件加密 |

一个容易遗漏的**非 HNAT 写者**：`D/embedded/fsm/sta_mgmt_cntl.c:2537–2540` 在 APCLI cover/WIFI_PKT_FWD 分支保存 RX hook 后写 NULL，`:3774–3776` 再恢复旧指针；备份定义位于 `D/os/linux/rt_profile.c:74–75`，`D/embedded/sta/sta.c:603,680` 清备份。它能将早已撤销的地址重新发布。必须删除这种全局指针读写/备份能力；该未支持特殊角色在激活入口拒绝，或仅用其自身受控的局部 bypass 状态，不能恢复 HNAT-owned endpoint。正常三地址 AP 不能因编译了这段条件宏而被排除。

实际外部 WARP 仅发现 TX 裸 hook 消费；驱动树 `whnat/woe_client_jedi.c:155,182` 还有 bundled 旧实现，实际配方选 external WARP，不把它当现行入口。构建验收仍需保证不意外切到该未修分支。上表“未找到”限于本轮 D/W/H/F 可读源码，不是任意第三方模块不存在的证明；本版不兼容继续导入旧裸变量 ABI 的外部二进制模块。

### 3.2 其它直接资源/API 与回调

还必须统一处理下列入口，不能由“ops 只有两个”遗漏：

- `hnat_get_foe_entry()`（`H/hnat.c:76`）返回逃逸的原表指针，普通 RCU 包住函数后返回不解决调用者之后使用。首版停用该原始借用 ABI，提供复制快照；旧导出如必须保留仅返回 NULL/不支持，核验没有生产消费者。不能返回锁释放后的硬件表指针。
- `entry_delete`、`get_ppe_mib`、`is_entry_binding`、`hnat_get_count`、`foe_clear_crypto_entry`、`hnat_bind_crypto_entry` 等直接导出要经过同一资源 lease/状态检查，**在任何 `hnat_priv`/`CFG_PPE_NUM` 解引用前**检查。`CFG_PPE_NUM` 本身是 `hnat_priv->ppe_num`（`H/hnat.h:250`），不能当无状态常量用。
- `hnat_api.c` 的 query/count/hash/delete、FIN-age MMIO API 也要受 lease。查询复制值、删除不得重建 BIND；FAILED/RESETTING 不访问被重建的表/MMIO。按 index 修改不能证明 index 尚属同一连接，不能以 magic/index 代替 owner/token。
- `hnat_update_entry_generic():373` 复制整项再写回，虽不显式写“=BIND”，仍可保留 BIND。首版生产拒绝未携带正向路径证明与 entry owner 的所有 index/skb 修改 API（HQOS、DSCP、VLAN、PPPoE、is_prior、tid）；已有软件报文仍正常转发。不能仅修 VLAN/PPPoE 而让其余 whole-entry 写回越门。
- `hnat_bind_callback`、`hnat_fin_callback` 由 `hnat_api.c:775,788` 无锁赋值，现有 register 不接受 NULL，也没有可靠注销排空协议；调用端 `hnat_trigger_callback():867` 不提供 provider owner/lifetime。首版不开放这两项可选回调注册（返回不支持，禁 debugfs callback test 写），不是假定任意 callback 不睡眠而塞进 normal RCU。
- multicast 的外发 `mtk_hnat_mcast_blist`、`mtk_hnat_mcast_offload`，入向 `hnat_mcast_eth_ext_link_handle`、`mtk_wifi_mcast_get_group_info`、`hnat_mc_ops` 全列入可选 provider 能力；`hnat_mcast.c:1550,1646,1662` 的注册/注销目前无 drain。AX6000 v4 数据为 `whnat=true,mcast=false,version=V2`（`H/hnat.c:1922`）；首版保持这项硬件 mcast offload 未开放，并在其注册、enable、debugfs/proc 入口实际返回不支持。不能仅靠当前未调用而保留未来可热启的坏生命周期。普通 Linux/无线组播不关闭。

## 4. dispatcher、RCU 与控制注册的执行合同

新增公共非 GPL-only 内部头/函数导出，供 D/W 同固件重新构建；不让专有模块直接访问 H 私有结构。旧裸变量及所有写者一起替换；不是只给 H 自己的赋值加 RCU 宏。

1. H 模块提供永久的 `hnat_dispatch_rx/tx/delete_*` 等函数。D/W 实际调用点改为调用 dispatcher，不再 `if (global) global(...)`。dispatcher 开始 normal `rcu_read_lock()`，**一次** `ops = rcu_dereference(active_packet_ops)`，仅使用这个局部指针，直到对应 op 完成才 unlock；调用后不从共享指针重读。op 不睡眠，不保存 ops/h/表地址到异步对象。
2. 永久 dispatcher 的空路径不依赖 hnat_priv、FOE、MIB、待重建 registry。先调用可信 descriptor 的 ownership 分类与有界 metadata 正常化：正常用户包继续软件/正常 TX；硬件重复/私有回注按 P1-4 表处理。不能靠重新查可能已复用的 FOE index 区分旧副本。若该分类尚未实现，本项仍是 NO-GO。
3. state/token 不能只在 dispatcher 入口检查。实际最终 FOE 写入仍在 entry_lock 内检查 READY、稳定且匹配的 core token、backend generation、正向资格。撤活动 ops 前进入 RESETTING/QUIESCING；最后提交门与状态变化使用同一 entry_lock 串行，已进入旧回调也不能晚提交。
4. 设备注册/注销单独为 SRCU dispatcher，完整 callback 在 `srcu_read_lock/unlock()` 内，可分配/睡眠。实际 D 的 open/close/ioctl 调用端改用新接口；不能持 MacTabLock、RxReRingLock 或自旋锁调用它。若发现已有原子上下文调用，改为值型、持 dev 引用的控制请求队列或拒绝该不支持入口，不能让 GFP_KERNEL 在该上下文继续执行。
5. **设备 registry 与活动硬件 ops 分离**：registry 的对象/锁在 INIT 时建立，寿命持续到安全 DEAD/重启。注册/注销只处理持引用的 netdev 记录，不访问 FOE/PPE；RESETTING 期间特别允许注销，避免 close 变成 no-op而留下陈旧设备。控制写由 mutex 串行，packet 只 RCU 读快照；删除先摘除 RCU 记录，grace 后释放记录和 dev 引用。新注册在非 READY 可明确暂不可用，不能编造已注册成功。SER 不销毁 registry；READY 前重新核对现存 netdev 状态。
6. SRCU 活动指针的最后撤销与 `synchronize_srcu()` 用于真正销毁控制对象；**不能在持 RTNL 时等待可能取 RTNL 的控制读者**。SER 若只重建硬件而 registry callback 已被证明完全不碰硬件，可不为 registry 做无意义销毁；任何碰 HW 的控制 API 单独关闭并在 RTNL 外排空。
7. NF hooks、HQOS packet_type、netevent/netdevice notifier 也必须是到上述受状态保护入口的 dispatcher。NF 注册本身不等于裸 RX/TX 排空；仅 RTNL 移除 core backend 链表更不等于退出完成。

首版 H 模块不卸载，dispatcher 的代码地址因此稳定，但**这不免除活动 device 对象/表的 lease 和 grace**。外部 WARP 对导出变量产生的常规模块依赖也可能已阻止 rmmod；没有声称现有普通 rmmod 必然 UAF。模块引用与 HNAT platform 私有资源有效性是两回事。

## 5. 全部异步生产者与停止顺序

| 入口/工作项 | 当前重排/生命周期问题 | 首版处理 |
| --- | --- | --- |
| `hnat_sma_build_entry_timer`，`hnat.c:70` | `hnat_disable_hook():1531` 和 `hnat_nf_hook.c:418` 都可 mod_timer；回调重新打开学习 | 不保留手动 disable 的自动重开逻辑；所有 arm 点受 stopping/state 检查，失败状态不能 rearm |
| `hnat_reset_timestamp_timer`，`:91` | `:110` 自 mod_timer；当前 remove 在 FOE free 后才 del_timer_sync | V1.3 专用，AX V2 不启动；通用代码仍按同一停止合同，不能先 free 后 cancel |
| `_hnat_roam_work`，`:532` | 阻塞 kernel_recvmsg，`:571` 无条件自 schedule；disable 只 sock_release 无同步 cancel | **首版删除这条旧 FDB netlink polling worker 的启动/支持路径**。桥依赖流本来不 BIND，无需以此补 per-MAC 清表；不把关闭 socket 当已排空。若日后保留必须非阻塞接收/可靠唤醒及 stopping+sync drain，另审 |
| `_hnat_flow_entry_teardown_work`，`:630` | `:658` 自重排；`:669` 仅 cancel_delayed_work；`:623,642` 错用 DEF_ETRY_NUM/4 | 用实际 foe_etry_num 的 pending 桶数；stopping 禁重排，始终 cancel_delayed_work_sync，不按 pending 位判断 |
| `neigh_update.work`，`hnat_nf_hook.c:613` | netevent `:723` 入队；handler `:676` 自重排；cleanup `:631–643` 先清共享列表，再按 pending 决定 sync cancel，漏 running/local_list | 停生产者/禁止入队后**无条件** cancel_delayed_work_sync，再清列表；handler 自己的 local_list 在返回前释放，不能由清共享列表推定已排空 |
| `hnat_mcast_nlmsg_handler`、`sk_data_ready` | `hnat_mcast.c:1164` socket callback queue_work；当前 disable 先 flush/destroy 队列，后 sock_release（`:1504` 后） | AX 首版硬件 mcast enable/register 实际拒绝，无队列/套接字可热起；未来支持须先撤 socket 生产者/排 callback，再 drain/destroy queue |
| `hnat_mcast_check_timer` | `:1218` 自 mod_timer，V1.3 | 首版不启动；不能把未启路径概括为已修所有平台 |
| NETDEV/NETEVENT callbacks | NETDEV 可 mod_timer、处理 extdev；NETEVENT 分配/入队；当前 probe 在 neigh 初始化前就注册 notifier | 所有锁/list/work/timer先初始化，producer 后发布；回调先检查 state，硬件动作只 READY；persistent notifier 不在 SER 内反复 unregister/register |
| NETSYSv3 FE irq2 | `hnat.c:940` 读 PPE/cache/entry；probe 先 request_irq 后初始化表/锁 | AX V2 不注册该 IRQ；共享代码中注册必须最后，停止时先 mask 源、synchronize_irq，再动表。不得在硬 IRQ 中共用仅 BH-disable 的锁 |
| HQOS `packet_type` | probe `dev_add_pack`，remove 在表 free 以后才 dev_remove_pack | 若保留由永久 dispatcher + ownership 接口管理；任何安全销毁需先撤入口并 synchronize_net。不能将一般 NF_ACCEPT 当私有封装恢复合同 |

具体停止协议（SER 开始使用前半段，安全冷回收使用全部适用段）：

1. 持 RTNL 的**短** core transaction 发布过渡 token；设置 `stopping=true`，关闭所有重新排队点，再在 entry_lock 下进入 RESETTING/QUIESCING、推进 backend generation；撤 `active_packet_ops` 及硬件控制 ops。结束短 transaction，保留常驻不可用状态，不能把 nesting depth 留到 RTNL unlock 后。
2. 不持 entry/flow/registry/queue 自旋锁等待 grace。normal RCU 的排空用对应的 `synchronize_net()`；SRCU 读者用 `synchronize_srcu()`，后者在 RTNL 外。实际控制调用可能持/取 RTNL，必须让出该锁后再等。
3. 在 RTNL 外无条件同步取消 retained work/delayed_work；timer 使用同步停止，永久销毁用 `timer_shutdown_sync()` 防重启，SER 可重启 timer 用 state+enqueue 锁保证 del_timer_sync 后无重排。禁止“pending 位未置就不等”。
4. 所有 schedule/mod_timer 受统一的 async lock 与 stopping 检查。handler 在开始和再次 enqueue 前检查；新生产者在同锁下检查后入队。邻居队列规定 async→neigh 锁序，不能反向；pending/entry 锁各自释放后才重新排队。状态转移不能持 entry_lock 再等 worker；队列锁内不做 cache 等待。
5. 排空后再按实际分配容量清 pending/list，entry commit 屏障与 FOE 清理分别取现有锁，flow_entry_lock 与 entry_lock 不嵌套。SER 此时才允许物理 reset/清表；恢复 state 与 rearm 必须由最终 commit，不能由旧定时器自行恢复。

当前 retained 邻居、pending teardown worker 内没发现 RTNL 获取；仍采用 RTNL 外 drain 的统一合同，避免后续增加控制调用后形成锁反转。不能用这一局部事实为任意 provider callback 或 multicast/NPU callback 作不睡眠保证。

## 6. core 与 HNAT 的共享持久状态

状态记录放在始终存在的 core slot，不能因移除 HNAT 回调而消失。它与短 RTNL transaction 的 nesting depth 是两个对象。slot 至少记录 state、generation、last_error、resources_exposed/retained；短事务保留统一 topology token。数据面只读取值，不遍历控制回调链表。

| 状态 | 新 BIND / 硬件快消费资格 | topology prepare | 允许的后续 |
| --- | --- | --- | --- |
| INIT | 禁止；未发布 packet ops | 返回明确 not-ready/busy，不能按“无后端”成功 | 冷初始化全通过→READY；暴露前安全失败→DEAD；暴露后失败→FAILED |
| READY | 仍须 NONBRIDGE_VERIFIED、稳定 token、entry_lock 最后检查 | 正常同步 prepare | 受控配置→短 gate；SER→RESETTING；不开放用户 disable |
| QUIESCING | 禁止；停止异步重排，撤活动硬件 ops | 返回 busy/error | 仅被支持的内部生命周期；首版不开放 destructive remove |
| RESETTING | 禁止；跨 RTNL unlock 仍常驻；空 dispatcher不访问表 | 返回 busy/error，不能嵌套进另一人的事务 | 对应序号 reset 的完整 prepare/check/commit→READY；任何失败→FAILED |
| FAILED | 禁止；sticky，错误不能被 enable/debugfs/后来的 NAT_DONE 清除 | 维持错误并拒需要硬件屏障的拓扑发布 | 重启；只有另经审查的完整物理恢复协议才可新增恢复边 |
| DEAD | 仅代表**从未暴露或已证明物理停止且已安全回收** | 无该后端时才可走安全软件合同 | 新的正常冷 probe；失败保留硬件时绝不能标 DEAD/注销 tombstone |

使用 `entry_lock` 串行最终提交与状态 gate；控制 state 转移受 RTNL 串行，stopping/enqueue 使用 async lock。packet proof 在路径证明之前取得 core token+backend generation，pending 按值携带；最终提交比较原值，禁止到最后临时读取当前 token给旧 route 换身份证。旧 pending 在 reset 清理后不能重新导入新表。

SER 通知必须由实际 caller 直接传播返回状态，不能寄望普通 notifier 的日志就阻止恢复：START 阶段先使 RESETTING并撤活动 ops，结束短事务才 unlock；RTNL 外 drain；物理 reset 后 warm_prepare **不 READY、不恢复 GMAC PPE forwarding**；恢复 Wi-Fi/收 ACK 的窗口仍无新 BIND；最终新短事务确认 reset-id、表/cache、双向 gate 与硬件模式后才 READY/发布 ops/恢复 PPE。主线程已在准确 E postimage核该序列，本文不把原 NAT_DONE 处理宣称为已满足。

重复 START：同 reset-id 幂等，不重复初始化已 running work；不同 id 在未完成时拒绝或进入 FAILED。无 START 的 DONE、过期 DONE、缺失 ACK/DONE、warm/cache 超时、reprobe 发现 retained FAILED 均不能恢复 READY。RESETTING期间其他 RTNL link 见持久状态返回 busy；它们不能被全局 nesting depth误认为同一事务的内层。中途不注销 slot，重新注册不得覆盖旧 FAILED 记录。

## 7. 不安全 runtime remove 的可执行禁止与 probe 错误所有权

### 7.1 实际入口限制

- H 现 `module_platform_driver(hnat_driver)`（`:1958`）同时生成 init/exit。改成有 module_init、**没有 module_exit** 的首版常驻模块，继续使用现有 OpenWrt kmod配方；无需无根据改成 builtin。`L/kernel/module/main.c:743–749` 在有 init无 exit且不能 force 时拒绝 delete_module，`:639–650` 证明 `CONFIG_MODULE_FORCE_UNLOAD=n` 时 O_TRUNC也不能强制。实际 generic config已为 n，CI还必须断言**生成内核配置**为 n。
- H `.driver.suppress_bind_attrs=true`，取消用户通过 platform driver sysfs bind/unbind发起 remove。实际 `B/bus.c:235–246` 的 unbind_store直接调用device_driver_detach，`:746`只在该flag为false时创建bind/unbind文件；不是在void remove中尝试veto。E 的 `CONFIG_NET_MEDIATEK_SOC=y` 已是 builtin（filogic config:340），不用额外取消模块退出；但也设置其 `.driver.suppress_bind_attrs=true`，避免先解绑 `hnat_priv->eth` 的 supplier。只保护 H自身不保护这个外部资源是不够的。`B/driver.c:267` 的driver_unregister为void且调用bus_remove_driver，因此本版不开放会发起它的HNAT退出入口。
- AX6000桥修复产品配置中强制 `CONFIG_OF_OVERLAY=n`，核生成配置而非只加一个不会映射的用户 defconfig行；当前 target为 y，不能假定无这条发起路径。可由选中该产品特性的 HNAT KCONFIG条件加入覆盖，须检查 OpenWrt kernel配置合并结果。限制的是**内核运行时** DT overlay；不影响 bootloader在启动前合成的FDT，也不全局修改其它filogic profile。
- 仅加 of_reconfig notifier不足以 veto设备销毁：`L/drivers/of/dynamic.c:315` 的直接 detach忽略通知返回；overlay PRE_REMOVE虽在`:1223`可拒，普通changeset通知并非全都有发布前veto。首版不增加一个不完整的设备树 veto状态机，采用上述产品功能边界。
- debugfs `hook_toggle` 去除 write行为或实际返回 `-EOPNOTSUPP`；现文件mode0444但存在write实现（`H/hnat_debugfs.c:2874`），只读mode不等于不可调用。其它debugfs/proc的 enable、cache/PSE控制、静态写表入口同样拒绝，不能 chmod后恢复。旧 `hnat_enable_hook/disable_hook` 不再是可绕过状态的公开操作，只保留受控内部实现或拒绝旧维护调用。
- 这不禁止普通 AP down/up、netifd reload 或 WARP client的正常内部 ring重建。W 的 `warp_register_client():1000`创建platform driver，`warp_probe():340`初始化WED/WDMA/WO，`warp_ring_exit():1125`停自有DMA/IRQ并reset ring，`warp_client_remove():1040`内部unregister触发`warp_remove():457`，另有`warp_module_exit():1492`；SER/suspend/resume是同文件`warp_{ser,suspend,resume}_handler`。生产补丁008已将warp_remove改为void，不能用其旧int返回值作veto。这些入口没有回收HNAT coherent表的权限。本修复改变它们到HNAT的dispatcher ABI，不重写WARP自有DMA退出，也不因此额外禁用所有Wi-Fi reload/模块维护；若实现证明其独立退出另有旧缺陷，单独列项。不能用“HNAT常驻”给WARP自身安全性背书，亦不能用旧WARP hook缓存重新发布H端点。独立wired BIND不携带WARP/WCID依赖，bridge/wireless项本来不BIND。

任意恶意内核代码直接调用 device_unregister不属于内置维护接口合同，但不能把正常受支持的sysfs/overlay/rmmod入口当成“任意代码”忽略。`L/drivers/base/platform.c:1390` 的remove为void，无可靠返回错误通道；**不采用在remove里return errno或打印unsupported后继续devres free的方案**。

### 7.2 初始化阶段与失败回收

当前 probe是危险发布顺序：`H/hnat.c:1818` 先 enable hooks，`:1822–1824` 注册notifier后才初始化邻居work，`:1831`后才setup timer，`:1847`才初始化xlat/mcast list；`hnat_enable_hook():1473`在NF注册可能失败之前已发布多个裸hook。全部改成以下阶段：

1. 建立持久 INIT slot，分配状态、所有锁/registry/list/work/timer，且没有外部活动ops、未启动worker。获取supplier引用并验证其可用；先完成所有会失败的table/MIB/pending/registry/回调注册准备，不能一边启动第0 PPE一边为第1 PPE分配资源。
2. 分配所有PPE表，记录每个实际容量与 initialized/allocated 位。清零及DMA可见性在写表地址、启动硬件之前；不能先写 `PPE_TB_BASE` 后才memset。逐阶段记录硬件是否可能获知DMA地址，后续错误分类不能靠“probe没成功”推断。
3. 第一次可能暴露DMA之前失败：不进入原统一hnat_stop扫描所有PPE的错误路径，按实际初始化位逆序撤回producer并回收；全局指针/slot不再指向devm即将回收的对象。可返回 `-EPROBE_DEFER/-ENOMEM/...`。
4. 一旦硬件已可能消费DMA地址，任意cache/init/check失败：保持绑定，进入FAILED、不发布packetops/READY、保留所有已暴露DMA、MMIO、状态及必要device引用；**不走 dma_free_coherent/devres错误退出**。返回0只表达drivercore保留绑定，不表达功能成功。只读status/core slot必须显示失败原因；activation读取失败且停流，不以“probe返回0”放开AP。未暴露资源是否回收由allocated位安全决定，不触动retained bundle。
5. 全部hardware/cache检查、NF入口、必要producer注册成功后，在新的短RTNL事务中最后发布READY与活动ops；不在probe中启动旧roam polling，不把debugfs可写接口放在锁/状态之前。失败清理若要停止已发布异步producer，按第5节无条件同步协议；本设计安排发布最后，减少这个错误面。

首版运行中不调用 destructive `hnat_stop()`释放DMA：配置变化只invalidate/cache同步，SER复用同一分配，失败保留。正常重启由系统重新启动恢复。未来若要支持热remove，必须补齐以下**物理停止证据后再另审**：阻断全部GMAC、WED/WDMA等输入，关学习/keepalive/scan，mask并drain IRQ，检查所有cache命令完成，关闭PPE并确认busy/总线DMA停止，读回完成条件，最后清表地址再free；任何一步失败都不free、不撤tombstone。当前 `hnat_stop():1390` 忽略busy timeout后`:1421` free，不能复用作成功证明。

## 8. P2：所有硬件提交与 cache 错误统一门

最终commit helper必须是**唯一能将有资格条目发布给硬件的路径**，不只搜索字面BIND：

| 现写点 | 必改事项 |
| --- | --- |
| `H/hnat_nf_hook.c:3301` 正常流 | proof在路径证明前取得；最终entry_lock内验证READY/token/generation/双向NONBRIDGE_VERIFIED |
| `:3660` WARP | pending `:3270`保存proof值，`:3375`复制后释放；不能取“当前token”补旧条目；wireless依赖拒BIND |
| `:2449` crypto导出 | 同一helper；统一锁上下文（现spin_lock与其它BH调用不一致），不因可选provider当前不启就漏门 |
| `:3008` tunnel/crypto准备提交 | state即使UNBIND也不能未经lease写到正在重建/失效的表；特殊path首版不授予加速 |
| `:4125` 464xlat直接memcpy | 移入共同checked commit并加entry_lock，不能保留无锁整项写；proof必须覆盖转换前逻辑路径，无法证明则软件 |
| `H/hnat_debugfs.c:3808,4013` static_entry | 生产拒无proof的静态BIND/原始条目及cache写；保留只读snapshot |
| `hnat_api.c:373` generic update及所有具体update | 拒旧index/skb维护API改变/保留BIND；以后有owner+proof的新API才允许走共同helper |

entry_lock与flow_entry_lock不嵌套，不能等grace/work时持spinlock。AX支持的CPU写者统一BH-safe锁；若扩展NETSYSv3 hard IRQ写者必须重新定义irqsave锁域，不把AX局部证明外推所有平台。delete也须检查lease与范围，不允许先按不可信index取地址再做状态检查。

cache链路改成可返回错误的内部API，覆盖 `hnat_cache_clr` 的idle等待（`H/hnat.c:1015`）、`__hnat_write_cache_line`每个CAH_REQ等待（`:762`等）、cache enable（`:1064`）及所有冷/热启动、topology prepare、reset/stop caller。任何失败不能执行后续“恢复旧cache/flowcfg就当成功”发布；锁内只记录必须的局部错误，统一错误闩使stateFAILED并阻后续提交。保留旧void导出时仅作为拒操作/记录失败的兼容壳，不能越过新API自动enable。寄存器请求完成语义需实现阶段核验，写寄存器成功不是DMA idle证明。

扫描实际 `foe_etry_num` 与按其计算的pending桶数，不继续使用DEF_ETRY_NUM/4，涵盖teardown、probe unwind、reset和API bounds；每PPE初始化/分配位防止半初始化清理误访问。

## 9. 必须合入主设计的文字修正与实现放行条件

旧主设计“SER/enable/disable/卸载同样重置epoch”改为本状态表与首版禁止入口；“backend unregister受RTNL保护即可退出”撤回。保留FAILED tombstone，不以回调NULL表示硬件不存在。

旧RX稿的legacy运行开关删除；policy正文过期“缺VLAN源码”删除。VLAN demux用K postimage `net/core/dev.c:5705`，netns拒移动用`:11693`；netns_local是netdev整个寿命不可撤销约束，不随角色/能力false清除。“全量”仍指所有符合普通三地址AP/受支持网络域合同的终端，不新增WDS/A4/APCLIcover/动态STA VLAN/任意虚拟provider支持。旧双MAC/运行时诊断候选不得混入实现。

实施面为末尾新增kernelpatch（H/E/core/调度与cacheAPI）、D生产42patch后新增driverpatch、实际externalWARP补丁、AX6000特性配置与生产guard。不能直接修改overlay前像导致现有14个HNAT patch冲突；所有生成postimage重新验证。

设计关闭P1-3的检查是以下执行路径完整，而不是“增加几个RCU宏”：

- 所有D/W裸调用与APCLI保存/恢复writer已替换；活动packetops一次取值且完整读侧，控制GFP_KERNEL路径用SRCU；无ops仍符合ownership，不泄私有封装、不重复发包。
- running work自重排、邻居local_list、timer与进入RESETTING并发均可可靠drain；RTNL释放窗口无nestingdepth，另一个link明确busy；queue/entry/flow/registry锁图无反转。
- sysfsunbind属性不存在；delete_module在有无WARP引用时均明确拒绝；force配置实际为n；overlay运行能力实际关闭且bootFDT不变；debugfs写enable/rawBIND/cache不可复活FAILED。
- probe各分配失败点、首次DMA暴露后的cache失败、busy timeout，证明没有dma_free/devres回收已暴露资源；boundFAILED状态可见且activation不放行。安全暴露前失败可正常回收，不把所有ENOMEM都泄漏。
- SER的旧proof/pending/延迟descriptor不进入新表，FAILED不能被过期DONE或enable恢复；clear/commit全部真实调用helper，不用测试复制算法替代实现。
- 完整MT7986构建验证D/W的新函数ABI与符号许可、HNAT无module_exit、generated kernel config、全补丁应用及production guard；真机性能与硬件完成条件另验，不能由本只读设计承诺百分比或完整硬件安全覆盖。

若实现仍让一个受支持入口到达会free DMA的void remove，或硬件已暴露后probe错误仍触发devres回收，P1-3保持NO-GO。首版的明确答案是常驻、拒破坏性入口、失败留资源与错误状态，受控SER不更换DMA所有权；不把不可证明的热销毁留作上线后补充。
