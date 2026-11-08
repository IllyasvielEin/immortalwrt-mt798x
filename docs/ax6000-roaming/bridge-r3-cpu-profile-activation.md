# 固定 CPU 转发候选：BSS 激活合同如何缩小

2026-11-09。本文仅为 `bridge-r3-scope-comparison.md` 提供一个可实施的控制面比较候选，不表示已经决定切换方案，也不是设计放行。仅新增本文；未联网、操作路由器、修改实现或启动子代理。准确 D/N/K/V 源码范围与 `bridge-r3-activation-contract.md` 相同。

## 1. 判断与适用前提

**有条件可以删除 BSS 激活对全局 topology epoch、backend 注册/嵌套事务、动态 cache prepare/恢复框架的依赖。** 替代方式是每 BSS 的 prepare/drain、已提交的网络域意图快照，以及 COMMIT 内持 RTNL 对实际全部端口的一次完整核验。

必要前提是比较稿中的 CPU profile 真正落实为本目标固件的固定产品合同：从启动起不生产 PPE BIND，不启用 nf_flow 软件/硬件缓存，不发布资源型 HNAT 外部 hooks；WED 的必要慢路径与常驻空表继续有其受控初始化/复位顺序。独立有线路径也不保留 BIND；没有“仅特定桥流拒绝，其他流仍可能引用旧表”的资格例外。

因此不能只把 UCI 的 `flow_offloading`/`flow_offloading_hw` 设 0 就宣称满足前提。必须由 fastpath 比较稿覆盖真正的 FOE 写入口、私有 fast forwarding、WARP 调用、SER 恢复和表/DMA 生命周期。若该前提不成立，本文件的删减结论不适用；本控制合同不会用 CPU gate 冒充硬件已停止。

## 2. 能删与必须保留的部分

| 控制项 | 固定 CPU 候选的处理 |
| --- | --- |
| 全局奇偶 topology epoch、路径资格 epoch/pending 证明 | 从 activation 删除；不再为了保留少量独立有线 BIND 维护这些状态 |
| per-backend register/unregister、跨拓扑 cache barrier、SER backend READY/RESETTING 状态机 | activation 不依赖；固定 CPU/WED profile 自身的硬件安全初始化与复位仍须成立 |
| COMMIT 之前用户态多次读回依赖同一 epoch | 改成内核 COMMIT 内持 RTNL 直接核验真实配置；用户态读回保留为错误定位 |
| 整 BSS 默认 BLOCKED、prepare 确认、失败先阻流后 delif/release | 必须保留；external=2、多 claimant 与无条件 mark_up 的事实没有改变 |
| 驱动内旧 BA/TX/ACK/PS/BMC 队列的配置 lease | 必须保留；CPU 转发不会自动消除旧队列在重配置后被重新许可的问题 |
| skb extension 的 clone/COW/新 skb 继承 | 必须保留租约所需部分；ownership 若由 CPU profile 从源头消除，可独立简化，不能混写租约 |
| 固定 netns、静态 VLAN、hairpin、精确 NoForwarding/BTNBSSID | 必须保留；与有没有 PPE BIND 无关 |
| 全 Linux backlog/qdisc/socket 撤销与 xmit_one 租约 registry | 不增加；沿已认可的正常在途边界 |

owner/transaction/lease 仍需避免模块重载后的 ABA。可在已经要修改的 skb-extension 内核支持中保留一个非常小的、普通导出的全局 u64 单调分配 helper；它只分配不可复用的 BSS cookie，不记录拓扑、不遍历 flow、不注册 backend，也不是 topology epoch 框架。不能把同名“token”都误认为同一种全栈机制。

## 3. 更小的实际 prepare/commit 顺序

继续使用上一份合同已经核实的 `SIOCDEVPRIVATE+15` / `ndo_siocdevprivate` 二进制 ABI、netifd C binding 与 SET_DATA 内部通知，不依赖 iwpriv。准确 6.12.112 `net/core/dev_ioctl.c` 的 private 分支已经在 RTNL 下进入 callback；新 handler 自行检查 CAP_NET_ADMIN、版本、owner 与 adapter netns，不能重复取 RTNL。

一次配置按以下顺序执行：

1. parent netifd 对所有受影响现存 BSS/sibling 同步 PREPARE。driver 先撤销普通数据许可，再等待已获准的驱动 CPU reader 退出；队列中旧 lease 永久失效。CPU profile 成立时，这里没有一次按拓扑扫描/清空 FOE 的 backend 工作。
2. driver 已经默认阻止新建 BSS 的普通数据；执行 DAT/ifdown/reload/ifup，收集当前实际 owner。新 adapter 不能使用旧 token。ifup 只建立控制面，不自行 READY。
3. 配置全部实际 bridge/VLAN port、隔离与 hairpin。每个失败真实回传，整 BSS 仍关闭；多个 claimant/child 共享一个 owner，不能分别提前放行。
4. STAGE_DOMAIN 暂存完整的**预期配置**，不把用户态已经观察到的值直接当许可。记录真实设备对象及域意图，而不是接口名称或“现在似乎没有 bridge”。
5. COMMIT callback 在同一个 RTNL 临界区内解析/核对完整 staged snapshot，检查 driver/profile 就绪和所有真实端口；全部成功后才发布新 lease 与 READY。任何不一致返回具体错误并保持 BLOCKED。只有发布之前允许分配/拷贝/失败；READY 发布后不再调用可能失败的操作。
6. netifd 收到 COMMIT 成功才 mark_up。失败清理仍先取得已确认阻流，再移桥/释放。ifdown 失败不解除门，无法取得阻流确认则不先拆现有拓扑。

不再要求 GET 一个全局 epoch。设备删除、master 改变或 ifindex 复用发生在 STAGE 与 COMMIT 之间，均由 COMMIT 的实际对象/关系核验发现；修改线程在 COMMIT 持 RTNL 期间不能发布另一套 bridge/VLAN 关系。发布许可与核验处于同一临界区，避免用户态读回后再次竞态。

## 4. COMMIT 的小型内核核验面

用户态 `RTM_GETLINK`/sysfs 读回不能跨多次调用构成一次原子核验。因此需要一个**无状态、无 callback registry 的 bridge policy 检查 helper**，而不是用“netifd 会很快读回”代替一致性。

实现点可在 `net/bridge/br_if.c` 及 `include/linux/if_bridge.h` 添加值型检查 API，例如 `br_mtk_ap_port_check_rtnl(port, expected_bridge, expected_policy)`。内部 ASSERT_RTNL，使用准确 `br_private.h:459 br_port_get_rtnl()`，比较实际 master/port flags，并通过 bridge 自己的 VLAN 数据读取所需成员关系；只返回 errno/规范化值，不把 `struct net_bridge_port *` 等私有裸指针交给专有模块。它是一个有明确参数和返回值的小型静态检查，不持有全局网络状态。

该新公开 helper 需要普通 EXPORT_SYMBOL，并在完整 MT7986 CI 核实模块许可与配置链接。现有 `br_vlan_enabled()`、`br_vlan_get_pvid()` 等接口是 GPL-only，不能直接假定 mt_wifi 能调用；公开检查 API 在 bridge 实现内部读取自己的数据，避免复制一套 bridge 私有结构给 driver。

一次 COMMIT 至少核对：

- base、每层 VLAN、实际 port、bridge 都仍 registered，处于同一固定 netns；引用的是 staged 的同一个设备对象，不能只比 ifindex 数字。
- VLAN 的直接 real_dev、proto/VID 与层次顺序一致；不能把折叠 real_dev 当直接父设备。
- 所有预期 bridge 域仍进入预期 bridge；显式 standalone 域当前确实无 bridge/未知 RX handler，且不是缺少一个本应存在的 VLAN child 才“变独立”。
- hairpin、BR_ISOLATED、必要 mcast/proxyarp 设置及 driver 的 NoForwarding/BTNBSSID 与同一预期配置相符；false 与 true 都核对。
- VLAN filtering/proto、实际 port 的 membership、PVID、egress untagged 与预期匹配；不能只查几个正向 VID 而忽略意外多出的许可。桥与端口的标准 VLAN 处理仍是权威，不另建 FDB/MDB 副本。

STP forwarding state、FDB/MDB/邻居动态学习不成为固定 snapshot 的逐项镜像；它们可按 Linux 正常运行。受支持的隔离/VLAN/profile 写路径依然先 prepare，legacy 在线 setter 在 READY 时拒绝不受控改写。COMMIT 与这些 writer 的锁顺序必须一致，不能检查完 driver 隔离字段后允许另一个 setter 在发布 READY 前改掉它。

## 5. 已提交域意图不能随外部移桥自动变宽

删除全局 epoch 不等于每包重新按 `IFF_BRIDGE_PORT` 决定“有桥就交桥、无桥就 legacy”。READY 持有不可变 committed snapshot，记录本次允许的域及其所属真实 port/bridge。RX late handoff 与 TX 绑定继续核验实际选中域和这份意图；driver 内排队包还要核验 lease。

最重要的单向规则是：**已承诺 bridge 的域不会因为 port 被外部移走、VLAN upper 删除、master 暂时为 NULL 而获得 standalone 许可。** 它只能拒绝/kernel-only，或关闭该 BSS 后等 parent 重做配置。只有正式 PREPARE→新 snapshot→COMMIT，才可把某个域重新声明为 standalone。

snapshot 在控制面持 netdev 引用，删除的旧对象即使尚未释放，也会因 reg_state/关系不符而不再有效；新设备复用名字/ifindex 不会继承许可。发现域失配时可立即闭锁软件 gate，控制线程随后进行同步确认；不能在 RX 中等待、拿 RTNL 或做用户态工作。

所有正常 netifd bridge/VLAN/隔离变更都先 prepare。对绕过 netifd 的普通管理员移桥，需要保持以下最低防线，不能静默自动提交新关系：

- driver 的“是否允许 legacy”必须同时满足实际解析与 committed standalone 意图；known bridge 域失配永不 legacy。
- Linux 接管的 AP 数据在真实 bridge ingress 再核验其 BSS/domain 意图。可复用现有 adapter NF policy 生命周期，增加一个很小的 `NF_BR_PRE_ROUTING` admission 检查，并保留 `NF_BR_FORWARD` 的实际 in/out 隔离；不注册全局 topology listener 或 backend。
- 对 PRE_ROUTING 检查已经通过或已经交 Linux 的包，保留正常在途语义；不承诺撤回所有排队帧。新进入错误 bridge 域的 AP 数据应拒绝，不能因此恢复 vendor 内转。
- AP TX 的最终目标域必须仍符合 committed port/bridge/VLAN 意图；不能凭新 bridge 成员身份给旧缓存续租。

入桥/出口核验可在同一小型 bridge API 中提供 RCU 只读版本，比较当前实际 port/master、相关策略位及本帧选择的域；不在每包路径复制/遍历全部 VLAN membership，不把 RTNL checker 直接放进 softirq，也不向专有模块泄漏 bridge 私有指针。全部配置的完整比较只在 COMMIT 进行。

准确 K `br_input.c:76` 在 finish 中按实际 port 取 bridge，`:161` 设置本包的 brdev；`:70` 的 local-in 与 `br_forward.c:115` 的 forward 是不同出口。因此仅在 FORWARD 检查两端隔离并不能代替普通 AP 的入桥域校验——本机/路由接收也需要正确 ingress。上述 ingress 检查只保护 AP 受控域，不扩展成全 Linux 数据撤回框架。

## 6. mixed VLAN 与隔离的最低能力

| 情形 | CPU 候选必须保留的行为 |
| --- | --- |
| untagged standalone，tagged VLAN upper 加桥 | 逐帧按 metadata→in-frame tags 的 Linux demux 顺序选域；不能因“有任意 bridge upper”把整个 BSS 一律交桥，也不能因 base 没桥一律 legacy |
| bridge child 移走/删除 | 原 tagged bridge prefix 不变成 standalone；整 BSS 失败闭锁，重新配置才可恢复 |
| VID0、802.1Q/AD、双层/metadata+帧内 tag | 保留已审 bounded parser/直接父设备规则；未知、截断、DOWN upper 不回 legacy |
| 非隔离同 AP 互访 | 实际 bridge port hairpin=1，独立于 multicast_to_unicast；VLAN upper 继承有效无线策略 |
| NoForwarding | 实际同 BSS AP↔AP 在 FORWARD 拒绝，包括 group；不阻止正常网关本机/有线业务 |
| NoForwardingBTNBSSID | 同 adapter 的不同实际 BSS 之间拒绝；不能用所有 port 的 BR_ISOLATED 代替而误伤同 BSS |
| bridge_isolate | 与上述两个 driver 标志分开，保留实际 BR_ISOLATED 设置/读回，不互相替代 |
| child 或一项策略安装失败 | 整 BSS 继续 BLOCKED，先确认阻流再撤销关系；没有“其余 VLAN/claimant 已成功就开门” |
| per-STA 动态 VLAN、WDS/A4/未核角色 | 维持明确未支持/拒激活范围；不借 CPU profile 宣称新支持 |

固定 netns 约束仍是 netdev 生命周期不可撤销的 `netns_local` bitfield，VLAN 后代在注册前递归继承；AP→STA 或能力 false 不清除。此项是小型确定性支持边界，不是可随 CPU profile 删除的加速机制。

## 7. 比较结论与尚需核实的门槛

该候选能把 activation 从“全网拓扑/加速后端协同事务”收缩为“每 BSS 的端口配置提交”，保留一个有限的 RTNL bridge 检查 API、配置快照及驱动队列 lease。netifd/ucode 的失败传播、VLAN child 继承和真正阻流仍有实质工作；不能描述为仅改 AP callback 的几行补丁。

进入实施前需要比较稿与复审共同确认两件事：固定 CPU/WED profile 确实不存在可绕过这些门的缓存/资源 hook 路径；小型 COMMIT helper 能在准确 postimage 对所列全部必要配置做 RTNL 内核验。若两者成立，activation 本身没有继续保留全局 topology epoch/backend 框架的必要。

实现验收应覆盖 prepare/ifdown/readback/commit 的失败顺序、外部移桥不转 standalone、mixed VLAN、同 AP hairpin、所有隔离组合及旧 BA/TX 缓存；性能损失与固定 CPU profile 的 reset/DMA 安全由独立证据验收。本文没有给出性能百分比，也不把缩小修改面当作正确性已通过。
