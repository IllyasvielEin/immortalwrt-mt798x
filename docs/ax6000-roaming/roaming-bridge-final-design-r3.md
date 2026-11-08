# AX6000 通用桥转发最终设计 r3.1：固定软件转发

2026-11-09，**r3.1 设计修订稿，尚未获准实施**。工作区为 `ax6000/worktrees/bridge-forwarding`，分支 `fix/ax6000-bridge-forwarding`，实现基线 `c196312d3916fad77fa0bb47db57e4b685c15f03`。原 r3 文档快照是 `0de772dce7dbbe27f73a793e72f6ed0927abf1df`，其独立审查关闭三项原 P1，仅剩 WARP STOP 真实性，见 [r3 审查](roaming-bridge-final-astra-review-r3.md)。本次补齐该有限依赖及 P2 实施要求，不恢复已裁剪的框架。当前只有文档和只读源码准备，未修改固件实现、推送、编译、操作或刷写路由器。

## 1. 目标、证据与本次范围选择

普通三地址 AP 的合法数据在所属网络域接入 Linux bridge 时，统一交给 Linux 桥选择出口。旧无线关联表只承担关联、授权、密钥和桥已选无线端口的发送职责，不再抢在桥前按目的 STA 内转。所有符合该能力合同的终端适用，没有 IP/MAC/BSSID/SSID/接口名/业务端口白名单，不重新引入双设备候选。

两次现场证据已确认：移动终端到 GS7 后，桥 FDB 已指向有线上联，MTK 仍按旧同 wdev 条目发送回包；自然老化删除后恢复。关闭 k/v/r 后仍能复现。旧关联未及时删除的原因尚未确定；本修复消除它绕过正确桥出口的能力，不将全部旧关联问题冒称已经查清。

用户指出改动规模超出预期，并要求隔离分支。范围复核发现：r2 为保留少数独立有线路径 PPE BIND，引出了全局拓扑 epoch、动态后端、pending 代际、账户维护及较大的 SER 协调框架。它们不是桥根本修复无条件需要的部分。

**r3 拟采用固定软件转发能力：所有 PPE BIND 与 nf_flow 软件/硬件流缓存从本镜像启动起都关闭，保留 WED 的基础 DMA/token、收发、无线加密及已支持重排序。保留 MT7986 在线 L1 SER，只修固定慢路径的恢复顺序与错误出口。** 不采用范围比较稿中额外关闭在线 FE 恢复的候选。

**r3.1 的新增失败策略拟选 WARP 合同终态 B：无法确认 DMA 停止/完成时，同步进入整机致命故障入口并尝试紧急硬重启；复位失败则不返回正常执行，可能需断电。** 发生 SER 与不可安全拒绝的销毁交叉时也采用该终态。正常成功 SER 继续在线恢复，普通包错和配置错误不触发此策略。代价是上述不可恢复故障会中断全机并可能丢未落盘状态；该取舍已向用户说明，尚非性能/刷机或此故障体验的验收。它替代“未知DMA仍活动、系统保持FAILED运行态”的庞大retain/热移除合同，不新增PCI核心框架。

这项取舍已向用户说明并提供可选偏好；截至成稿尚未收到额外选择，依据“收敛改动”方向形成送审稿，不把未回复当成产品性能验收或刷机许可。若用户要求保留独立有线 PPE，必须另行回到完整能力合同复审，不能在本实现中悄然恢复绑定。

公开代价：桥客户端 LAN/WAN 在 r2 中已经走软件；r3 额外取消独立非桥有线连接的 PPE/NF-flow 加速及相关硬件 QoS offload。CPU、吞吐、时延必须实测，不能声称维持原峰值，也不填写未测损失比例。普通 Linux 防火墙、NAT、IPv6、软件 qdisc 和现有代理的普通软件功能保留。

现有产品还选择了 `mtkhqos_util` 和 MTK eQoS：前者从镜像取消，不执行会先改 bridge sysctl 再写 HNAT 的启动脚本；后者复用已有 tc/HTB/IFB 软件实现，CPU profile 下所有规则走软件，不保留无效硬件 UI。准确源码的旧分界是1–31硬件、32起软件，不是64。软件总带宽父类与默认叶类分别预留 `1:fffe/1:ffff`，规则编号统一1–65533，避免规则1和旧父类1:1冲突；配置/CLI/UI一起校验。HTB增加默认叶类，未匹配规则和单设备零限速仍受总带宽限制。start/stop/enable 不写HNAT/QDMA/mark/turboacc，已有u32/mirred软件filter显式skip_hw。这个适配只使用既有QoS代码，不新增引擎；不承诺br-lan qdisc覆盖所有纯桥内部转发。一般 Linux tc 功能不因此移除。

## 2. 权威材料与源码边界

本文件为 r3 总合同。以下材料提供具体接口与调用图；冲突时以本文件明确裁剪后的合同为准，不拼接成另一套更大的实现：

- [BSS 阻流与激活](bridge-r3-activation-contract.md)：状态、lease、ioctl、netifd/ucode 错误传播；其第8节对动态 backend 的依赖由固定 CPU 合同替代。
- [固定 CPU 激活收敛](bridge-r3-cpu-profile-activation.md)：不用全局 epoch，以配置快照和 COMMIT 时 RTNL 内实际核验替代。
- [固定 CPU 最小 SER](bridge-r3-cpu-profile-ser.md)：保留现有 L1 恢复，永久 owner callback、checked restore、有界等待和 ABORT。
- [WARP STOP 合同](bridge-r3-warp-stop-contract.md)：实际 D→proxy/WARP 的 checked 返回、停流确认、资源清理及失败 ACK 边界；补齐原 r3 的唯一 P1。
- [范围比较](bridge-r3-scope-comparison.md)：源码依据及取舍历史。其中“禁止在线 FE 恢复”不被 r3 采用。
- [packet ownership 依据](bridge-r3-packet-ownership.md)：只采用可信 native reason、释放责任和禁私有 producer 部分；保留有线 BIND 的账户/CT owner/epoch 方案不被 r3 采用。

`roaming-bridge-final-design-r2-reviewed.md` 与 `roaming-bridge-final-astra-review.md` 保持原送审和 NO-GO 结论。`bridge-universal-{rx,policy,offload}-design.md` 保留源码依据；其 r2 动态加速、旧配置失败处理或自动 legacy 建议不能覆盖本文件。完整 HNAT 生命周期/通用 SER 草案是未选能力的研究，不是本版附加任务。

只读准确基线位于 `/home/illya/workspace/router/ax6000/sources`：D 为42补丁 mt_wifi，H 为14补丁 HNAT，E 为82相关补丁的4个 Ethernet 文件，K 为49相关补丁选定 bridge/netfilter 文件，V 为5补丁 VLAN 文件，N 为锁定 netifd cbb83a185740，W 为实际外部 WARP 20250408-5f71ec。各目录 PATCHES.json 记录准备范围；部分既有内核补丁需要 GNU patch fuzz，不能称完整内核 prepare 或全部 fuzz=0。

另在当前 worktree `.analysis/linux-6.12.112-tc-postimage` 准备了 v6.12.112 `mtk_ppe_offload.c` 和14相关补丁，记录原始/补丁/postimage SHA。其 `mtk_eth_setup_tc():1053` 仅允许 BLOCK/FT，r3 在安装的实际 ndo 入口固定拒绝这两类硬件请求，普通不依赖 ndo_setup_tc 的软件调度继续可用。所选单文件准备不等于完整构建。

## 3. 普通 AP RX：源授权完成后交桥一次

保留实际 RX peer 的 Class2/3、关联上下文、ToDS/FromDS、PrivacyFilter、PortSecured、Radius、PN/MIC、解密、分片及重复包检查。在两个 `rx_*data_frm_announce()` 的普通数据准入处统一授权，不能等目的表命中才按 Ethernet SA 二次猜源。BA 正常在途接收语义沿用 Astra r2 已接受的边界，不新增站点指针/关联 generation/trylock 许可。

配置 reader 的起点早于 announce：在两个 `dev_rx_*()`/`check_rx_pkt_allowed()` 已解析真实 `pEntry->wdev` 后、调用 `ops->rx_pkt_allowed` 读取配置之前，为规范 parser 判定的普通 AP 数据进入 READY/profile reader。覆盖同步 VLAN/隔离等配置读取，至队列接管或实际 handoff 才退出，所有提前返回配对释放。安全授权仍照常执行，通过后才 stamp RX lease；BLOCKED 时普通数据不能先进入这些配置读取。控制流使用独立分支。BA 出队在 common announce 读取 VLAN policy 前重新取得 reader，不补当前 lease。

认证/握手保留现有 MLME/daemon 通道。规范纯解析 helper 有界区分 Ethernet II、VLAN、LLC/SNAP 与 EAPOL payload，覆盖前置 `rx_packet_process()` 的读取和两个 announce；不能只把 `RTMPCheckWPAframe(...,FALSE)` 改 TRUE。EtherType 在 Ethernet +12，跳14之后不是 EtherType。802.11 可变头/QoS 依实际长度，所有前置读取先验证边界；纯解析不执行 WPA 状态机副作用。

未授权 SW A-MSDU/Ralink 聚合整体拒绝，或逐 MSDU 仅分离合法控制流，不能借首个 EAPOL 放行其它普通数据。HW 拆分后每 MSDU 均检查。

普通已获准数据在共同 AP callback 任何 MacTab.Size 快捷分支、group clone 和 DA lookup 前选择交桥；广播/组播也只上送一次。覆盖普通、头转换、BA、SW/HW A-MSDU、Ralink 第二 MSDU。`to_os=true` 后还必须明确跳过不满足合同的 FAST_NAT/WIFI_PKT_FWD/私有回注，保持一次 protocol assign 和一次真实 Linux handoff，不以布尔值作到桥证明。

独立 AP 域只有“当前实际 standalone + committed snapshot 明确允许 standalone”同时成立才可使用原本地转发。已承诺 bridge 的域即使 child/master 消失也不变 legacy；未知、损坏、DOWN upper 或配置失败不走旧内转。

## 4. TX：始终服从 Linux 已选端口

普通 AP TX 的 DA 查找限定本 wdev，核验 DA、peer/TR、PortSecured 和 VLAN。若另一个 BSS 有同 DA 的旧条目，拒绝本无线副本；本版统一选择**拒绝并计数**，不在队列末尾改道另一个 wdev或换 WCID/密钥重试。

共同最终校验覆盖 qm、fp_qm、fp_fair_qm、ACK 优化、PS/UAPSD/BMC 和聚合中的每个普通 MSDU，位置须仍能知道原 Ethernet DA 与网络域，并在其后不再发生 wdev/WCID 重写。删除普通桥路径中“根据旧 TR 自动改 wdev”的行为；group 用桥已选端口的 BMC。保留 sta_rec、安全与 TX 加密检查，不能为修复出口而跳过它们。

配置 lease 在第一次进入 driver TX 时取得；driver 自身重新生成/缓存的包继承旧 lease，不能伪装新入队。来自 Linux 的正常 qdisc 在途包首次入 driver 时依当时实际已就绪出口取得许可，不新增全 Linux 逐包撤销机制。

## 5. P1-1：整 BSS 默认阻流，配置确认后才激活

每个可承担 AP 的 netdev 注册前建立 gate，默认 BLOCKED；状态为 BLOCKED/PREPARING/READY/FAILED_BLOCKED/STOPPING。ifup/open/claim、carrier 或 netifd mark_up 不隐含 READY。PREPARE 先撤销旧配置 lease，关闭普通数据准入，等待覆盖实际 driver CPU 决策的 active reader 退出；队列内旧 lease 永久无效。COMMIT 只能发布全新许可。

使用正式 skb extension 保存 RX/TX 的 owner+lease 值，不放 STA/wdev/adapter 指针，不占未知 cb/mark。标准 clone/copy 继承，写用 skb_ext_add 的 COW；厂商新建子帧、第二 MSDU、ACK/聚合/分片重建显式继承。配置 lease 不是站点授权代际；不撤回已入 Linux 或已交硬件的数据。

只为 lease 增加一个小型 built-in 单调 cookie 分配 helper，生命周期覆盖模块重载、ifindex 复用；值不复用，耗尽拒激活。它不保存拓扑、不注册 backend。extension ID/长度和 CONFIG_SKB_EXTENSIONS 在真实内核配置下检查，不取消原内核边界检查。P1-4 源头消费不需要 ownership extension，删除原方案账户/producer epoch 字段。

控制 ABI 为已核 `SIOCDEVPRIVATE+15`/`ndo_siocdevprivate`，定长 version/size/reserved、owner/transaction、明确 errno；所有操作显式检查 adapter netns 与 CAP_NET_ADMIN。6.12 private callback 已在 RTNL 下，不重复加锁，不依赖 iwpriv/WEXT 共享 cookie。C→ucode 的 u64 用固定十六进制字符串，32/64位兼容，不暴露内核指针。

parent netifd 为整个 BSS 及 VLAN descendants 持一个 activation owner：PREPARE 确认 → 配置 DAT/接口与全部真实 bridge/VLAN port → 写入并读回 hairpin/isolation/VLAN → STAGE_DOMAIN → COMMIT → mark_up。already-up、多个 claimant、任一 child 失败都不能部分放行。ifdown 失败但已确认阻流可清理本事务；不能取得 blocked 确认时不先 delif/release，保留拓扑并报错。

COMMIT 在同一个 RTNL 临界区核验实际 netdev 对象、注册状态、直接 VLAN ancestry、bridge master、端口策略的0/1两方向及完整 VLAN membership/PVID/untagged；不以多次用户态读回冒充原子事实。小型 bridge 内部检查 API 普通导出，仅回 errno/值，不泄露桥私有指针。handler 内的分配、核验和自身 copyout 完成后，仍在既定串行规则下复核 owner/token、状态和 sticky profile fault，才发布 READY；不在其后执行 handler 内可能失败的动作，没有全局 topology token。

这不保证整个 native/compat ioctl syscall 在 handler 返回后绝无外层 copyout 失败。控制面将负返回/超时视作结果不确定，不据此推断 blocked 或继续 mark_up：先 GET，再重新 PREPARE，获得新的成功 blocked ACK 后才清理关系/重做配置。即使 handler 已短暂 COMMIT而外层返回失败，也走明确重新阻流流程；完整源码和故障注入核验外层行为，不为此改造通用 ioctl 框架。

netifd C、system-linux、bridge/VLAN helper、device_set/interface_link、MTK notify/config.uc 和 wireless-device.uc 全链实际传播失败；不能 external release、日志、无条件 mark_up 代替阻流。所有受支持配置入口先 prepare；legacy 在线隔离/域 setter 在 READY 时拒改并要求正常 reload。RX/TX 和桥 ingress admission 发现 committed bridge 域失配时拒绝/闭锁，不自动扩大 standalone。

## 6. hairpin、隔离、VLAN 与 namespace

内部 `wireless_bridge_forwarding` 能力经 MTK→wireless-device.uc→netifd，true/false 与 VLAN child 继承均落实。非隔离普通 AP hairpin=1，独立于 multicast_to_unicast/proxyarp；isolate 强制关闭并读回。NoForwarding 与 BTNBSSID 在 adapter 注册的 NF_BR_FORWARD 按实际 in/out AP/VLAN 身份执行，不按 DA STA、不用 skb_iif 猜桥入口。NoForwarding 拒同 BSS，BTNBSSID 拒同 adapter 不同 BSS；不以全端口 BR_ISOLATED 代替。隔离扩展至 group 是本版明确行为，网关与有线服务沿原网络策略。

角色/capability 变 false 时也发送完整显式布尔组，netifd 清理旧派生 hairpin/isolate/proxyarp/bpdu_filter；不能只因“当前已不是 AP”跳过 active port 清零。两类 VLAN 在 ADD 前、parent policy 变化和 REMOVE 时递归重算/通知有效值，保留本地显式设置与继承值的区别。实际写回和读回失败逐层传播；上述行为在 prepare 阻流下进行，netns_local 生命周期约束不随之清除。

增加有限 NF_BR_PRE_ROUTING admission 保护 committed AP 网络域；不能只在 FORWARD 核验，因为本机投递/路由同样需要正确 ingress。hook 只用 WdevListLock 复制注册映射/标志，不取 MacTabLock/RxReRingLock，不分配/等待。退出先停止新数据，再 unregister + 显式 synchronize_net，后释放 adapter/snapshot。hook 不能先拆掉却允许数据继续转发。

保留已有 VLAN_RX policy、VID/PCP、metadata 与帧内标签。按 Linux 顺序逐层匹配直接 802.1Q/AD upper，含 VID0、双 tag、metadata+in-frame；同 MAC 不同 VID 独立。base 在 VLAN-aware bridge、VLAN upper 接桥、untagged standalone+tagged bridge 都支持。未知域不能回 legacy，不把“有任意 upper 在桥”当全 BSS 判据。

所有可能担任 AP 的 netdev 在首次注册前固定 adapter namespace 并设置整个生命周期不可撤销的 `dev->netns_local` bitfield；不是 features 位，false 能力/AP→STA 不清除。公共 register_vlan_dev 在 vlan_vid_add/register_netdevice 前递归继承并拒跨 ns。存在未受约束旧 upper 的原地角色转换拒绝，要求重建。WDS/APCLI/A4/MWDS/backhaul/GO、动态 perSTA VLAN 等未闭合能力明确拒激活，不因编译 MAP 就拒普通 fronthaul。

## 7. 固定 CPU profile：没有可重新开放的加速路径

新产品编译能力同步约束 kernel/mt_wifi/WARP/netifd，选中 AX6000 镜像默认启用，缺失或不匹配就不激活 AP。不提供运行时重开 BIND 的键。生成内核/包配置和真实模块符号必须核验，不能用 UCI offloading=0 冒充不变量。

H probe 产品分支只建立 WED 需要的有效慢路径 FOE 资源：全部表先分配/清零/DMA可见，再暴露地址；固定 SMA_ONLY_FWD_CPU、正确 CPU 默认出口，禁止学习/BIND、KA、scan、cache、MIB、硬件 mcast/sampling/HQOS。每个可检查请求/读回失败传播；不能先启完整 hnat_hw_init 后再补关。有效空表不表示硬件不再 DMA，表仍需常驻。

不调用旧 enable/NF注册、HNAT netdevice/netevent、邻居/roam/pending/MIB work/timer、HQOS ptype、可写 debugfs。D/W 对 HNAT 资源型 hook 的调用/保存/恢复从产品路径移除，H 导出槽位始终不发布；所有直接 API 在解引用 hnat_priv/CFG_PPE_NUM 前返回不支持，不返回原始表指针。最终 BIND/整项写入口、crypto/xlat/static/cache/HQOS/setter 都不能绕过固定限制。只保留有界只读状态，不暴露“写回完整条目”的诊断接口。

WARP 包和基础操作仍保留；当前所核 WARP/D 未见直接访问 HNAT 表资源，需最终 undefined-symbol 表再核。若真实编译发现某个基础 WED 操作必须保留 resource callback，先收敛其寿命/锁并复审，不能用 NULL 强行替代后宣称功能保留。

nf_flow 的共同 add 在发布 tuple、改变 ct timeout、排硬件 work 前固定拒绝；lookup 不返回 fast hit，refresh 不重新排加速。nft/xt 前端更早跳过尝试，保留正确 free/clear IPS_OFFLOAD 错误路径，普通协议栈继续。保留 firewall4 依赖包；以后改规则也无法重开缓存。Ethernet 实际 ndo_setup_tc 拒绝 BLOCK/FT 硬件请求；普通软件防火墙/qdisc 不由此关闭。保留 HNAT 构建分支，避免因取消它反而启动 generic PPE。

E 的所有正常 open 固定 GMAC→PDMA，不按 offload_version/L4S/TDMA 重新选 PPE。WED 若仍经 PPE 传递，依赖上面的有效 CPU 慢路径，不能由 GMAC 已选 PDMA 推定 WED 也绕开 PPE。此硬件表达必须后续验证。

## 8. P1-3：常驻资源与明确失败归属

本版无需动态 packetops 的 RCU/SRCU registry：实际资源型 hooks 从未发布、软件维护 producer 从未启动。不是保留裸指针热切换后赌无并发。仍需以下最小资源合同：

- HNAT 产品模块有 module_init、无 module_exit；CONFIG_MODULE_FORCE_UNLOAD 实际为 n。H 与 builtin E 设置 suppress_bind_attrs，禁 sysfs 解绑；本产品运行时 CONFIG_OF_OVERLAY=n，bootloader 合成 FDT 保留。
- 冷 probe 在 DMA 未暴露且尚未发布任何永久引用时失败，才可按实际分配位回收并返回错误。DMA 暴露或永久 callback/context 已发布后失败，必须保持绑定 FAILED、资源/MMIO/引用，不走 devres/dma_free；返回0只用于保留 owner，状态查询和 activation 必须明确报失败，不能当功能成功。永久 callback 在可失败冷准备之后、RTNL下最后发布，避免未写 BASE 但已发布 callback 的悬空 context。
- 表不在正常配置、Wi-Fi reload 或 SER 换所有权/释放。原 destructive stop、debugfs enable/raw/cache 写等没有产品入口。仅已证明未暴露的资源可回收；不在 void remove 中打印错误后继续释放。
- 无 cache/BIND 不意味着可忽略 WED、FE 或硬件 DMA。资源保留与关闭实际销毁入口仍是必要维护限制；不顺带重写 WARP 自有 ring 退出。

仅一个每 E 硬件 owner、RTNL 下永久注册的 CPU-profile restore callback；H 代码和 context 常驻，不支持运行时注销或替换。不是通用 backend 列表，不保存拓扑，不从 RX 调用。profile ready/error 是固定硬件能力状态，不是路径许可或每终端状态。

kernel 提供普通导出的只读 status helper，绑定本目标真实 E owner；D 在创建 adapter 时取得/验证这一硬件关联，不能按客户端或 ifname 选择。未注册 owner/未初始化/RESETTING/FAILED 一律不就绪。COMMIT 及每次普通 RX/TX gate 准入均核该状态，已 READY 的 BSS 不能在 profile fault 后继续新准入。RX producer 发现禁止的私有实例只原子置 sticky fault/撤 ready，不在 softirq 调 restore 或释放资源；RTNL 控制路径发布 ready 时必须与 fault 闩串行核验，错误不可被随后一次 ready 写覆盖。正常已交 Linux/DMA 的在途边界保留。

## 9. P1-2：保留 L1 SER，恢复固定 profile 先于数据恢复

准确顺序采用 [最小 SER 合同](bridge-r3-cpu-profile-ser.md) 与 [checked WARP STOP](bridge-r3-warp-stop-contract.md)：共同 pending worker 首次硬件 touch 前置 notready/ser_active；MT7986 用 D 已实现的 START_RESET；RTNL 外有界等 STOP ACK；重取 RTNL 检查本轮状态，执行物理 stop/reset并检查 hw_init；**首次 mtk_open/Wi-Fi RESET_DONE之前**同步 checked restore 同一资源的 CPU profile；始终 PDMA，所有必需端口成功后才发 RESET_DONE。NAT_DONE 没有 warm/READY 权限。不存在跨 RTNL 的全局 nesting depth。

STOP 成功须包含实际 WARP 必要 engine 的 checked 停流，不能用现有固定返回0的 hook dispatcher、最后一个成功子步骤或 void WO 包装作凭据。D预置值型未完成结果，proxy的SER分支先于whnat_en早退调用checked helper并回写，缺listener/实际资源状态不明不成功。STOP只禁生产者、checked WO/engine idle，首错锁存和总deadline；保留ring/buffer，WO保持SER_RESET，不自动module fallback，不做软件ring/index重写。FE_DONE成功且未ABORT后才一次清理旧ring，再checked HIF/BM准备和WO/IRQ/DMA恢复。WO正的非零错误也失败，busy用真实BIT掩码，不能沿用RX_DMA_BUSY位号3的错误按位与。

ABORT/sticky检查早于SER hook、阻塞动作返回后、清理/ACK前；自主Wi-Fi SER也遵守。失败且可能仍DMA则调用下述noreturn终态，不报告成功ACK、不返回普通err unwind。stop-fail即使E不在stop_waiting也锁存故障，不能触发旧fallback新一轮。正常确认停流才复用原ring清理；不关闭WED或在线SER绕过本项。

D START 仅在现有 STOP_IDLE、无等待/失败时接受一次；STOP 用 atomic_xchg 只报告一次，E 只在 stop_waiting 接受本轮通知。成功周期的 STOP 同步交付，不残留到下一轮；ONLINE/OFFLINE 在等待期间使本轮失败而非改写剩余计数。D 前轮尚在 N9 恢复时拒第二 START，E 处理 notifier busy，不新增最终 N9 ACK 或 reset-id 框架。

任何等待超时、成员变化、hw_init/restore/cache/读回/open失败先锁存sticky错误，不发成功DONE，不重试下一轮。若尚不能确认活动DMA已停止（包括E等不到STOP ACK），进入noreturn故障终态；若已经有有效停止证明且尚未重新暴露/启动DMA，则可保持blocked、保留资源并报告错误。独立ABORT只置错/唤醒，D在HIF free/reinit/IRQ-DMA enable前复核；迟到DONE/MCU事件不能复活。重开DMA/新资源暴露前撤销旧停止证明，不能据旧proof回收新资源。

故障终态由builtin E的普通导出硬件故障API实现：原子发布同一fatal闩并关闭准入，由首个CPU选为复位执行者，设置REBOOT_HARD并直接进入arm64 machine_restart；其它进入者关本地中断并停留，不竞争锁或继续清理。不得排异步重启任务后返回，不经normal kernel_restart/device_shutdown、panic/kexec或用户态。API属于真实硬件owner故障处理，专有D/proxy不直接调用GPL-only emergency_restart。ARM64、E builtin、EFI=n及实际restart provider进入构建核验。

SMP stop是best effort，故不能仅靠调用CPU不返回。WARP合同所列D close/HIF/adapterfree、真实rbus remove/module exit、proxy hook退出、W ring/client/remove/module exit及E DMA销毁入口共享本轮SER/teardown短状态准入，并先核fatal；SER开始与已有破坏性操作互斥，已进入清理则不开始SER，SER中不能可靠busy/defer的销毁交叉进入同一noreturn终态。匹配的一条同步SER控制栈仍可在已证明停止后执行计划内cleanup；普通外部close不能冒用此许可。不在RTNL close里等会取RTNL的HwCtrl，不增加PCI核心veto或运行态retain。proxy退出也必须在unregister前持这项准入，避免checked同步回调途中释放其代码。命中故障的void remove不返回，drivercore才不会继续devres释放。

PSCI HARD system reset与MTK watchdog是实际源码候选；本CPU最终循环不代表硬件一定复位，其范围/完成、未停CPU、并发销毁guard和断电恢复均须实机验收。不能声称这种严重硬件故障仍安全继续正常运行。

实施必须把 checked reset 逐层做到可检测错误源：现有 warm-reset 的 void/fallback/仅打印 poll 不能被顶层 int 返回0掩盖。SER worker 的内部 restart 使用受限共同 open helper，仅该控制栈在已 restore 后允许；ser_active 时普通 ndo_open/COMMIT 仍拒绝。第一个端口已开而后续失败，先撤 ready/sticky fail，再只用经确认的停止路径处理已开端口；未确认停机的DMA不走无条件dev_close/free unwind。

参与者按真实硬件活动能力定义，不把probe ONLINE计数或 !START_UP 当停流证明。未启动对象必须在串行生命周期内确认无DMA/初始化活动，或完成checked静止确认再回无需停流；活动对象履行完整STOP。up/down/注册失败与成员变化均有明确出口，不给“模块已加载、无线未启动”制造必然超时，也不假报成功。子步骤的有界等待共同计入STOP总预算，D等待FE结果留出该预算，5秒只是待核初值。

callback 是固定资源的同步恢复，不开放 BIND；没有必要为不存在的动态流创建全局 epoch/路径 proof、pending generation或最后 N9 加速放行。真实 FE/PPE reset 完成和 WED 停流语义仍是实现/硬件验收，不以软件状态取代。

## 10. P1-4：先确定包所有权，副本在源头终结

E 在合法 DDONE/SoC RX descriptor 解码后、任何 head magic 重建/PPD redirect/XDP/GRO 之前；D 在 HIF gen_tuple 确认 PPE_VLD 后、WARP callback 之前分类，不能被旧 whnat_en 等运行时开关绕过。只信真实 descriptor，不信负载或普通 EtherType 0x5678。D 两个 HIF caller 使用既有 drop_pkt 释放和补环；不能在 void WARP helper free 后继续用。建skb前消费与建skb后skip/refill使用各自正确ownership，不复用已free buffer；D helper只回drop结果并合并原错误位，不另行free。

| 可信 producer 情形 | 动作 |
| --- | --- |
| 0x15 HIT_BIND_KEEPALIVE_DUP_OLD_HDR | 无条件消费一次，不查当前 FOE/index/代际，不进入桥/路由再发送 |
| 0x13/14、0x16、0x18/19、0x1b或真实 HQOS 私有实例 | 固定 profile 禁止的 producer 异常；消费并限速记录、使 profile fault，不把私有/部分已交付包当普通慢路径 |
| 正常 TTL/options/fragment/UN_HIT/UNBIND/MTU 等 native 普通数据 | 保留原 RX 错误检查后继续正常软件路径；不因为拒加速丢用户包 |
| 其它未知 native reason | 不能仅凭未知归副本；保持普通慢路径，记录，不读 FOE 决定出口 |
| PPE_VLD=0 或普通负载伪装 magic/EtherType | 普通安全/协议检查，不获得硬件来源身份 |

无任何合法 BIND 账户，故不需要副本 MIB/conntrack refresh、slot owner 或 producer epoch 扩展；普通软件数据正常维护 conntrack。原保留 wired BIND 的维护闭环研究只作为未选能力记录，不偷偷添加一个 worker。副本消费不依赖 extension 分配成功，也不访问可退出的 HNAT 对象。

## 11. 修改分组、验证与放行顺序

按可独立审查的组提交，全部在当前 worktree：

1. 内核小型 AP 支持：正式 lease extension/cookie、有限 bridge RTNL/RCU 校验 API、VLAN 注册 netns 继承。
2. 固定 CPU/WED profile：H 初始化/常驻/资源 API限制，E PDMA/TC/NF-flow拒缓存与 native ownership，D/W 源头对接。
3. 普通 AP RX/TX：安全解析、统一交桥、最终 wdev/DA/TR/VLAN 校验、配置 gate/lease 与精确 NF policy。
4. 控制面：netifd/MTK/共用 ucode 的 prepare-stage-commit、端口继承/读回、所有失败传播。
5. 最小 SER：上述固定资源恢复顺序、有界等待和失败出口。
6. 产品配置、既有 eQoS 软件模式适配、生产 guard、真实 helper/状态机测试、维护与回滚文档。

只在当前 r3.1 设计通过 Astra 复审后实施。不依赖043诊断，不写终端信息；不改 uBoot/MTK U-Boot布局或分区，保留生产/诊断引用。新内核/驱动 patch 基于准备好的实际 postimage 生成，再验证整系列，不直接破坏 overlay 前像。

真实 helper 与故障注入必须覆盖：EAP封装/短包/伪装/混合聚合；VLAN demux/同MAC多VID；TX错wdev/WCID/DA和每聚合MSDU；lease撤销/clone-COW/新skb继承；default/already-up/child/multi-claimant/addif/hairpin/VLAN/down/commit/外层copyout失败；意外移桥不legacy；可信副本/私有异常与负载伪装；probe半初始化/永久callback发布后失败、静态API无法BIND、cache/restore/open/ACK/ABORT失败。WARP首项失败末项成功、WO正错误/timeout、busy位、无listener、whnat_en关闭但资源活、自主SER、ACK前ABORT、timer/tasklet自重排与SER/close/remove/proxy退出竞争，均须零次成功ACK、零次未确认停流的ring reset/free/unmap、零次HIF/IRQ/DMA恢复，并证明fatal入口不返回。测试调用实际helper，不能复制算法或仅搜字符串替代；真实硬重启不得无授权在路由器触发。

完整 MT7986 kernel/driver/WARP/netifd/ucode 构建，核生成配置、模块 license/普通导出/undefined符号、extension边界、RRM/WNM/FT与现有生产 guard，锁定 feeds/Go/cache流程沿用。完成实际实现 Astra 审查后才推送 CI。用户通知编译完成，不持续跟踪，不自动刷机或发布正式 Release。

真机需在用户授权刷机后：任意客户端同/跨BSS、跨频段、AX→外部AP→返回，在旧STA仍在时即按正确桥出口工作；IPv4/6 TCP/UDP、ARP/ND、非IP L2、广播/组播、冷/热流、BA/聚合、mcast_to_ucast=0、所有隔离/静态VLAN/动态重载、正常与失败SER。匹配真实请求/回包、FDB/物理出口和有效业务传输，不只看 submit_rc。

性能独立测试 LAN/WAN/有线/无线吞吐、CPU、时延与配置中断。WED慢路径不成立、仍有BIND或缓存绕桥、越权/跨VLAN、重复/环路、旧wdev重定向、私有封装外泄、资源误释放/UAF/死锁、失败仍放行均停止验收。设计 GO 只允许实施，不等于构建、硬件、性能或生产验收。
