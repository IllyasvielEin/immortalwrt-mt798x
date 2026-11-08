# 全量桥转发：加速一致性与生产默认设计

日期：2026-11-08 调研，2026-11-09 收口。状态：源码设计输入，未实施。本设计替代固定双 MAC 候选；不存在测试终端、IP、MAC、BSSID 白名单。

## 推荐结论

生产默认应当是：**普通 AP 的允许数据由 Linux 网络栈/桥统一转发；凡依赖 Linux bridge 域的双向 flow，恒定使用软件路径。** 拒绝的对象还包括桥客户端的 routed WAN 流，不能只禁同网段 L2，也不能只禁硬件 `L2_BRIDGE` 类型。对能够证明不经过 bridge/移动无线端点的独立有线 routed/NAT flow，保留 PPE 加速；WED 基础 DMA、token、硬件收发、加密和重排序保持原能力。

这是按设备拓扑与路径能力选择的完整通用策略。所有普通 AP 终端均修正旧 STA 表优先截包的问题，没有“只有指定测试设备有效”的阶段。无法证明路径安全时拒绝缓存绑定，继续软件转发；不得回退到旧 STA 内转，也不能把拒绝加速变成丢包。

需要诚实说明代价：当前 AX6000 的普通客户端和 GS7 上联都在 `br-lan`，因此其 LAN 互访以及多数客户端 WAN 流的 PPE 加速都可能退到软件。保留独立有线/NAT 能力，不等于保证本机无线 WAN 的 HNAT 仍启用。吞吐、CPU 和延迟需要实际测量，不能按 PHY 速率推算。

“交桥并保留桥 flow 动态绑定，补 FDB notifier”在技术上可以进一步开发，但当前没有闭环证明，不能作为生产默认。下文给出具体反例和缺口。

## 已核验基线

- 驱动：`sources/vendor-mt79xx-7.6.7.3/review-baseline/mt_wifi`，生产仓 c196312d 的 42 个驱动补丁 postimage。
- HNAT：`sources/hnat-6.12.112-review-baseline/drivers/net/ethernet/mediatek/mtk_hnat`，overlay 加 14 个相关 kernel patch postimage，按 `PATCHES.json` 记录核验。尤其包含 5111 的 ingress magic 重建、5114 的 FDB 检查范围和 5118 的 NETSYSv2 Wi-Fi TX HQOS。
- WARP：实际外部 `warp_20250408-5f71ec`，SHA-256 `0954055e165dcb71cd2703f07b0c2d521ca3e9c8968d2b17b68e8c9a7853f9b2`，加仓库 WARP 补丁语义核验。
- Linux stable 6.12.112 原始参考：`sources/linux-6.12.112-roam-refs`。最终桥/netfilter/dev 控制流按 `sources/linux-6.12.112-bridge-review-postimage` 核验：21 个选定 C/H 文件，49 个相关 kernel patch；准备使用生产 GNU patch 默认 fuzz 规则，现有 5 个补丁使用 fuzz，详见其 `PATCHES.json`。这是相关文件子集 postimage，不是完整内核编译证明。

本次没有联网、改实现、编译、操作路由器或启动其他代理。

## 现有机制不能直接证明已解决加速一致性

### 现有 roaming patch 的真实覆盖范围

`999-ppe-08-mtk_ppe-add-roaming-handler.patch` 实际改的是 `net/netfilter/nf_flow_table_core.c`：注册 switchdev notifier，收到 FDB DEL 后异步遍历 nf_flowtable，以 DIRECT flow 的目的 MAC 标记 teardown，运行 GC/等待 offload flush。它不是 vendor HNAT 的 FDB notifier。

当前 vendor HNAT 通过 `skb_to_hnat_info()` 和 WARP `mtk_sw_nat_hook_tx()` 自行提交 FOE。HNAT postimage 没有注册 switchdev FDB notifier；只注册 NETDEV 与 NETEVENT notifier。因此 nf_flowtable 清理不能被当作其全部 FOE 条目的管理者。

HNAT `nf_hnat_netevent_handler():681` 只处理 NEIGH_UPDATE，`foe_clear_entry()` 只在目的 IP 对应 MAC 变化或 NUD_FAILED 时删除。客户端保持 IP/MAC，只从一个桥口移到另一个桥口，不满足“目的 MAC 改变”。NETDEV 处理主要覆盖 ifdown、设备注册/注销、部分 bond carrier 和 SER，不能替代每 MAC 漫游通知。

驱动现有 `ppe_del_entry_by_mac` 调用主要位于 A4/MWDS、关联时 RoutingTab 命中等场景。普通三地址客户端离开本机到其他厂商 AP，不保证触发这些调用；也不能假定 GS7 必然给 AX6000 发可信离开事件。

### 最关键的未观测移动反例

双向流已经 PPE BIND 后，终端从本机 AP 移到有线上联后的其他 AP。新入口的包如果也命中硬件 flow，可能在 Linux 源学习之前被转发。此时桥没有收到源帧，FDB 不会改变，也就没有 FDB notifier。再完善的软件 epoch 状态机也不能响应一个未发生的通知。

源码有两个相关事实，但不足以排除此反例：

- `hnat.c:1223` 对 NETSYSv2/v3 设置 `SP_CMP_EN=1`；`skb_to_hnat_info()` 复制原 UNBIND 的 source-port 字段。这只证明配置了源端口比较，已查源码没有给出“所有不符均必 trap 并先运行 Linux 源学习”的硬件行为契约，更未证明 BSS/WCID 是比较键。
- `BIT_ALERT_TCP_FIN_RST_SYN` 会使新 TCP 握手包成为 CPU 观察机会，但不能覆盖已建立 TCP 数据、UDP、同 WED port 的跨 BSS、同 GMAC/DSA 域内的下游 AP 移动。

所以不能依据寄存器名字推断完整观测机制。此前新建 TCP 连接时 Linux FDB 已正确，只能证明那次流量进入了桥，不能外推到所有已绑定流。

### FDB 事件与提交竞态也尚未闭合

Linux `br_fdb_update():901` 在普通移动时依次执行：旧出口 DEL 通知 → `WRITE_ONCE(fdb->dst,new_port)` → ADD 通知。只在 DEL 时清一次 FOE，另一个 CPU 仍可能在写新出口之前重新绑定旧出口。

WARP TX 还存在 `hnat_flow_entry` 暂存：post hook 先准备 entry，TX callback 在约 3 秒有效期内再实际提交。删除已 BIND FOE 而不处理暂存和在途值副本，可能随后复活旧 WCID/出口。

`br_switchdev_fdb_notify()` 对 LOCKED，以及特定用户添加的动态 FDB 条目直接不发事件；静态更新/外部学习的次序也与普通动态移动不同。仅根据“DEL 与 ADD 总配对”构造状态机不充分。VLAN-aware key 还需要 netns、bridge identity、VID、MAC，不能按 MAC 跨域混用。

### 两类容易误判的源码条件

1. `IS_L2_BRIDGE(entry)` 不是所有桥转发。HNAT debugfs 明示 L2_BRIDGE 模式只支持 MT7987，默认 `l2br_toggle=0`；MT7986 的普通 IPv4/IPv6 桥流仍可使用 IPv4 HNAPT/HNAT、IPv6 5T/3T 等硬件类型。关闭 l2br_toggle 不解决本问题。
2. `hnat_is_hw_path_bridging():1953` 比较 `hw_path->eth_src` 与桥/端口自身 MAC，供 TTL 处理使用。它不是完整的 ingress/egress/策略/移动依赖证明，不能独立成为安全卸载许可。

## 通用默认的具体控制流

### 统一路径资格

新增集中路径检查，语义至少为 `NONBRIDGE_VERIFIED / BRIDGE_DEPENDENT / UNKNOWN`，而非一个“没找到 bridge 就当安全”的布尔值。

输入应包括原始 ingress、当前 skb 设备、真实 egress、双向路由/forward-path 中出现的设备。检查 `netif_is_bridge_master()` / `netif_is_bridge_port()`，并沿已选择的 VLAN、bond、macvlan 等下层/主设备关系识别依赖。原始逻辑 bridge 不能在解析成物理出口后被遗忘。若路径含 bridge、已知 WED/AP 移动端点、不能解析的封装/隧道或不完整元数据，则拒绝 flow 缓存。

只在已验证非 bridge 的完整路径上允许绑定。复杂封装暂无法证明可安全卸载时，软件转发是最终功能的确定行为，不是只给两 MAC 加临时限制。相关用户数据仍经正常协议/隔离处理。

不能在 RX 中调用要求 RTNL 的 `br_fdb_find_port()`。此次默认根本不需要做逐包 FDB 查询；Linux 桥本身负责出口选择。HNAT 模块是 GPL v2，未来若做内部 bridge helper 应留在 kernel GPL 层，不让专有 mt_wifi 模块引用 GPL-only/RTNL 错误 API。

### HNAT 的拦截位置与返回值

| 入口 | 必须执行的规则 | 继续软件转发的返回值 |
| --- | --- | --- |
| IPv4/IPv6 pre、`mtk_hnat_br_nf_local_in` | 在 5111 magic 重建和任何 `do_hnat_*` 私有快转之前检查 ingress/原路径；bridge/unknown 不进入私有消费/学习 | `NF_ACCEPT=1` |
| `mtk_pong_hqos_handler`、HQOS ptype callback | 私有 ping-pong 是已有快路径；只能处理带有已验证非 bridge 所有权的合法条目。未知或 bridge 依赖不能按旧 act_dp/WCID继续发包 | 按该函数 ownership 契约处理，不能套用同一个整数返回值 |
| `mtk_hnat_nf_post_routing` | **在 464xlat 特殊处理之前**复核原 ingress 与实际/解析前 egress；bridge/unknown 不准备任何 entry | **0**，其 wrapper 才返回 NF_ACCEPT；错返 1 可能导致 DROP |
| `skb_to_hnat_info` | 在准备 FOE/WARP pending 前再作完整路径许可；记录策略/拓扑 epoch 与确定的来源证据 | 放弃绑定，不丢用户包 |
| `mtk_sw_nat_hook_tx` | WARP 最终提交前，复核 pending 的许可、epoch、当前 egress/WED 依赖和全局 gate；unknown 一律不绑定 | `NF_ACCEPT=1`，避免 WARP 设置 DropPkt |
| `hnat_bind_crypto_entry` / xlat / 其他直接提交点 | 只允许已验证不依赖 bridge/移动端点的完整路径；不能因为走特殊 engine 就绕过生产策略 | 放弃绑定，走该 engine 现有软件路径 |

原 ingress 不能只用 post-routing 的 `state->in`，该位置可能为 NULL；需在可用时验证 `skb_iif` 对应设备与路径上下文，并将已验证来源按值带入 pending。找不到来源、封装改变来源且未保留证据时拒绑定。不要为传上下文随意占用 skb cb/mark，也不要保存临时 `nf_hook_state` 或 wdev 指针。

`skb_hnat_alg=1` 可作为现有阻绑定提示，但不是唯一安全依据：5111、RX normalization 或 headroom 变化可能重建元数据。pre 中还要直接跳过私有 fastpath，post/final commit 重新验证拓扑；必要时按现有 head/frag_list 规则传播 alg。缺少可写 headroom 时不写越界，也不能因此放行私有消费。

WED RX/TX tuple wrapper 本身无需针对某个客户端修改。WARP `wifi_rx_tuple_add()` 只注入 PPE descriptor 信息；`wifi_tx_tuple_add()` 最终仍调用受新生产策略控制的 HNAT TX hook。WED 基础硬件能力与建立可绕过 CPU 的 PPE flow 分开处理。

### Linux nf_flowtable 同样必须受控

仓库的 999-ppe-01/15 为 xt_FLOWOFFLOAD/nft_flow_offload 加入桥 DIRECT forwarding，软件 flowtable 也会绕过桥源学习。当前现场没有 flowtable 定义，不意味着用户以后不能启用它。

只让 bridge 的 `ndo_flow_offload_check` 返回不支持不够：该回调属于旧 vendor HW path API；nft/xt 还用标准 `dev_fill_forward_path`。必须以 postimage 为准：当前 `nft_dev_forward_path():336` 已改成 int，解析/设备匹配失败会返回 -ENOENT，routing/bridging route 构造会释放引用并失败；不能把 raw stable 的 void/fallback NEIGH 行为套给本固件。尽管如此，旧 ndo 回调本就不是该标准路径入口，单改旧 ndo 仍拦不住它。NEIGH flow 也不能放行：它可以在 egress 调用 bridge，但已经跳过 ingress 的源学习。

需要实施：

1. NFT 和 xt 构造双向 route 时保留“路径曾包含 bridge/WED/未知层”的事实。任一方向依赖 bridge，都让整条 flow route 拒入表，而非只清硬件 offload flag；包括 NEIGH、DIRECT、XFRM。
2. `flow_offload_add():306` 作为所有 provider 的共同最后门，在发布任一方向 tuple 前核验许可与拓扑 epoch，并检查双向 iif/out/dst_cache 设备。不能只修改 nft 表达式而留下 xt 或其他 provider 绕过。未提供完整路径证明的 provider 安全拒加速。
3. `flow_offload_lookup()` 或 IP/IPv6 fast hook 在消费前检查全局拓扑过渡 gate 与当前 tuple 依赖；遇到已不安全的旧软件 flow，标记 teardown 并返回 slow path，不能在本包继续 DIRECT xmit。
4. 软件 flow 保存创建 route 的 epoch，查找与 refresh 在 epoch 变化后立即拒旧项并按正常 GC 回收，不要求在 RTNL 内同步 flush workqueue。原 `nf_flow_table_cleanup()` 只按两个 iif 匹配单设备，不是完整 egress/封装依赖失效器。

精确修改面包括 nft postimage `nft_flow_offload_eval():551`、`nft_flow_route_routing():404`、`nft_flow_route_bridging():459`、`nft_dev_path_info()` 的 DEV_PATH_BRIDGE/MTK_WDMA 分支；xt 对应 `flowoffload_tg():683`、`xt_flowoffload_route_{routing,bridging}():572/617` 和 `nf_dev_forward_path()`。当前 central add/lookup 未作 topology 检查。两条 route 的任一方向出现 bridge/WED，拒绝整个缓存连接；不能只拒当前包的单方向。

对于 generic nf-flowtable 的硬件 provider，只有证明最终硬件提交同样遵守 gate/epoch、且配置屏障可同步撤销已绑定条目时才可开放。当前生产安全模式可明确拒绝这种未验证硬件 capability，保留已单独受控的 vendor HNAT 独立有线加速；不要一边声称没有 RTNL 等待，一边让异步 FLOW_CLS_REPLACE 在配置发布后继续提交旧 route。XDP/BPF flowtable 消费若编入该配置也必须走相同 epoch/gate 检查；不能证明这一点的 hook 安装返回不支持。这里拒绝的是未具备同步契约的 provider 能力，普通包仍走协议栈。

## 启动、配置变化和回滚

### 启动策略

策略从新固件启动起确定，无“仅开关 ON 后才清表”的试验行为，也不提供假称可无缝切换的 runtime legacy 模式。mt_wifi 普通 AP 统一桥语义与 HNAT/nf-flowtable 的 bridge 拒绑定策略一同生效；某层策略依赖不可用时不能开放该层加速。

HNAT `hnat_start():1260` 为各 PPE 新分配 DMA FOE 表、写新 `PPE_TB_BASE`、memset 清零、初始化新 `foe_flow`，`hnat_hw_init()` 启用 cache 前先清 cache；probe 在这些完成后才启用 hooks。新固件冷启动可作为确定基线，不带入旧固件 BIND。

### 动态 topology 需要真正的配置屏障

在桥 attach/detach 或上下层关系变化的控制事务里，可保守全量清 flow。这是网络配置生效过程，允许短暂退软件；与已取消的双 MAC A/B 试验不同，不存在 ON 时清缓存造成的因果混淆。

应在实际 kernel 控制路径实现带 begin/end/abort 的同步事务，而不是只增加 PRE/POST notifier。已核实的具体约束是：

- `dev.c __netdev_upper_dev_link():8050` 持 RTNL，PRE:8090 的错误可 veto；但邻接分配失败直接返回，CHANGEUPPER 失败的 rollback 也不发配对结束通知。只等 POST 的 gate 会遗留关闭状态。
- `__netdev_upper_dev_unlink():8176` 为 void，PRE:8192 和 POST 返回值都忽略，NOTIFY_BAD 不能 veto。
- `br_add_if():579` 在调用 upper link 之前已注册 RX handler 并置 IFF_BRIDGE_PORT；仅在 upper PRE 开始事务太晚。桥级外层事务应从这些发布之前开始，覆盖所有 err 标签和成功返回。删除侧应覆盖 `del_nbp` 到 flags/RX handler 清理完成，而不止邻接表 unlink。

最低顺序为：

1. **发布拓扑前**阻止新 HNAT/nf-flowtable 发布及旧软件 fastpath 消费；增加 epoch，使已在 CPU 上准备的 pending 不能随后提交。随后在 RTNL 控制面调用 `synchronize_net()` 排空已通过旧 gate 的接收处理；postimage `dev.c:11455` 明确在持 RTNL 时使用 expedited RCU，现有 `netdev_rx_handler_unregister():5564` 也如此使用。它不阻止新普通包进入软件路径，所以必须先发布 gate。已在 qdisc/硬件队列内的普通数据属于配置切换的在途包，不承诺通过一个 epoch 消除一切配置瞬态。
2. 对会把原可加速设备引入 bridge/不安全上层域的 **link**，同步清 vendor FOE 与 WARP `foe_flow` pending，确认硬件 cache 失效。软件 nf-flowtable 由 gate/epoch 立即阻断旧消费，再由正常 GC 回收；不在 RTNL 等待可能反向获取 RTNL 的 offload workqueue。未具备同步撤销契约的 generic 硬件 provider 从启动起不开放。
3. 只有屏障完成，才发布桥/upper 关系。
4. 按新拓扑恢复仅 NONBRIDGE_VERIFIED 类别。bridge 与未知类别始终走软件。配置失败时不能默默恢复旧高速状态；可保持软件运行并明确状态，或按已验证事务恢复。

允许 nested begin/end 以覆盖 bridge 外层与 generic upper 内层，所有返回/rollback 必须完整结束；全局过渡 gate 和 epoch 在外层最后退出后才发布为稳定态。这样所有 netlink/ip-link/netifd 入口都受控。

**unlink 可以有独立的安全证明**：在本默认下，依赖被移除 bridge 域的 flow 原本就不可能 BIND，因此解除该依赖不需要成功清一个实际上不存在的 bridge BIND，也不必伪造 void unlink 的 veto。仍以 gate/epoch 阻断在途旧 route，在 detach 全部结束后才允许新发现的 NONBRIDGE_VERIFIED 路径。为使这个证明成立，未经验证的非 bridge 虚拟/叠加栈也应归 UNKNOWN；第一实现只开放能够证明独立且拓扑稳定的有线路径，不能任意允许 bond/tunnel 后再借用此单调性证明。若以后扩大 capability，就必须为相应删除路径增加可验证的硬件撤销契约。

### 最终提交与锁边界

控制面 gate/epoch 与最终 FOE BIND 检查必须有同一发布屏障。控制面置 disabled 后，任何早先准备的 entry 都须在最后 entry_lock 内因 epoch 不符而拒绝。WARP pending 按值复制 epoch；清 pending 与 TX 取项使用 flow_entry_lock，最终 entry commit 使用 entry_lock，不在持 driver MacTab/RX reorder 锁时阻塞等待控制事务。

新失效扫描必须按实际 `hnat_priv->foe_etry_num` 遍历，且在 entry_lock 内重新检查并修改条目。不能直接复制旧 `entry_delete_by_mac()` 的 DEF_ETRY_NUM 扫描和锁外匹配模式；`hnat_start()` 允许分配不足时缩小实际表。默认不再需要为每 MAC 漫游反复扫描。

不要把大表扫描、flowtable mutex 或 work flush 放进 RX/switchdev atomic 回调；本默认不需要每次漫游做该扫描。实际配置前置屏障若可睡眠，应在 RTNL 控制面完成；否则修改 caller 建立同步事务，而非在 atomic 区睡眠。

SER warm init、HNAT enable/disable、module unload/reload 都必须同步重置 gate/epoch/pending：当前 `hnat_warm_init()` 会清 FOE，但未见清 `foe_flow`，新实现不能继承旧 pending。回滚使用固件/模块重启及原布局镜像；不宣称写一个 flag 能撤销 FDB、邻居或已建立连接的变化。

### cache 清理失败是必须解决的门

当前 `__hnat_cache_clr():978` 忙等超时后只打印日志，返回 void 并恢复 learning。若配置屏障依赖它，必须改为可检验的成功/失败结果。失败时不得宣布新拓扑已安全启用，也不能恢复新 binding。

具体处理应是：进入不安全域的 link 在 cache 清理失败时返回错误，**保持旧拓扑，不发布新关系**，维持阻新绑定状态并报告配置失败。已存在的硬件项可能仍按旧拓扑转发，不能说此时已全退 CPU。可在受控恢复/重启后重试，不能仅关 learning 就继续 link。unlink 按上面的“该依赖从来无 BIND”证明，不依赖这个失败时不可 veto 的清表动作。

不能仅把 PPE port 置 down 然后假定包会自动改送 CPU；本设计不使用该兜底。若实现没有在所有实际 link 发布路径前建立可失败的屏障，或已经放行了无法同步撤销的未知 hardware provider，则为 **STOP / no-go**，不得交付为通用安全默认。

## 两个方案的取舍

| 项目 | 交桥 + 完整动态卸载一致性 | 通用 bridge 依赖软件默认（推荐） |
| --- | --- | --- |
| 去掉旧 STA 优先级 | 需要统一桥 RX 修复 | 同样统一修复，始终以桥为准 |
| 未进入 CPU 的移动可见性 | 当前尚无完整证明，是实施阻断 | 受影响流不建立绕过学习的缓存，从机制上排除 |
| FDB/VID/STA/WCID/暂存代际 | 必须完整维护，并处理丢事件/资源失败/超时 | 不依赖逐漫游通知；只需配置事务 gate 和 pending epoch |
| 正常漫游需要清表 | 通知驱动的可靠定向清理不可缺 | bridge 依赖没有 BIND，无需按终端清表 |
| 加速能力 | 理论可保更多，但不能把理论当验收 | 可证明独立的有线 routed/NAT 与 WED 基础能力保留 |
| 代价 | 实现/竞态/硬件观察证明成本大 | bridge 客户端 LAN/WAN 更多 CPU 开销，需实测 |

未来若要开启桥依赖加速，必须先取得并验证“每次入口移动均可在旧 flow 消费前可靠观察”的硬件/驱动机制，再实现 VID/bridge/STA generation 和 cache 一致性；不能只加一个 FDB listener 就默认开启。该优化不是本通用最终功能的前提，也不在本次默认中开放不可靠模式。

## 交 Astra 前应完成的设计验收点

- 通用 RX 方案确实取消所有普通 AP 客户端旧 STA 内转优先级；授权、隔离、控制帧、VLAN/动态 VLAN 与特殊 A4/WDS 由各自策略维持，不能用退回旧内转来隐藏未支持路径。
- HNAT 和 nft/xt 共用明确的“拒缓存但继续软件”的规则；当前无 flowtable 不是省略此工作的理由。
- bridge 依赖包括 routed WAN 两个方向；no bridge/unknown/移动 Wi-Fi 路径的能力边界明确，无任何写死终端身份。
- 动态 attach/detach、失败回滚、SER、cache timeout 的同步控制路径已落实；若 PRE 通知不足，就修改实际 kernel 控制路径。
- 非 bridge 可加速类别有正向证明，不把 unknown 视为可加速。cache清理失败不继续冒险发布新拓扑。
- 新增 patch 针对完整 kernel/driver postimage，放在相关现有补丁之后，避免改 overlay 破坏中间 patch 前像。

实现后验收应覆盖全部普通 AP 终端角色：同 BSS、跨 BSS、跨 AP/有线、返回 AP、热 TCP/UDP、IPv4/IPv6、VLAN/PVID 与 source learning；同时验证无线/有线实际 outlet、隔离与非授权拒绝、无重复/环路。另用独立非 bridge 路径验证保留的 PPE 能力，实际报告 CPU/吞吐/延迟与配置事务中断时间。
