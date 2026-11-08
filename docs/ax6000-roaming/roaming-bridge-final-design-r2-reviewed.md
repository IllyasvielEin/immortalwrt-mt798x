# AX6000 通用桥转发最终版设计

2026-11-09，r2 送审稿。用户已否决固定两台终端的实验候选，要求直接设计全量最终版。本设计替代已撤回的 r1；没有 IP、MAC、BSSID、SSID、接口名称或 TCP 端口白名单，也不以双 MAC 试验作为实施前置步骤。当前尚未修改固件实现、推送、构建或操作路由器配置。

## 1. 最终行为与明确边界

所有普通三地址 AP 客户端，只要其当前报文所属网络域接入 Linux bridge，合法接收的数据就统一交给 Linux。驱动不再通过目的 STA 表选择内部直转，也不通过陈旧目的条目作跨 BSS 隔离判断。Linux bridge 负责源学习、FDB/VID、出口、未知单播泛洪与组播处理；驱动站点表保留无线关联、授权、加密以及桥选定 AP 出口后的实际无线发送职责。

同 BSS、跨 BSS、跨频段、外部有线 AP 和返回本 AP 使用相同规则；支持 IPv4/IPv6、ARP/ND、TCP/UDP、普通非 IP L2、广播和组播，不解析业务端口来决定是否修复。返回本 AP 的正常源帧也必须进入桥学习。尚未收到可学习源帧前，不承诺系统凭空知道终端新位置。

独立、没有对应 bridge 的 AP 网络域仍保持该拓扑所需的本地转发，避免破坏同 AP ARP；这是按真实网络域选择，不是按终端筛选。WDS/APCLI、四地址/MWDS/MAP backhaul、P2P GO 等特殊角色使用明确的既有专用能力，不能因为复用了 AP 函数就自动获准普通三地址模式，也不能仅因为编译了 MAP/A4 就排除正常 fronthaul。混合角色无法兑现普通桥合同的配置须拒绝激活并报告，不对普通流量静默回退旧直转。

本次不顺带宣称补齐原来未实现的 Radius/动态 VLAN 控制面；当前未启用的可选功能保持其真实支持状态。已启用普通 AP 的标准 VLAN 数据路径必须纳入实现与验收，不能用排除所有 VLAN 包来称通用版。

## 2. 两层问题一起解决

已证实的直接阻断是：桥已把移动终端学到有线出口，mt_wifi 仍按旧 STA 同 wdev 将回包 `to_os=0,to_air=1`。自然老化有效，旧关联为何没有更早清理仍未确定。本修复无需等待这个问题全部查明。

同时，当前厂商 HNAT 自行提交 FOE，不受现有 nf_flowtable 漫游清理补丁完整管理。源码也没有证明已 BIND 流的每次入口移动都会在消费旧 WCID/出口前通知 Linux。只加 FDB notifier 不能处理未被观察的移动；`SP_CMP_EN` 的寄存器名字不能当作完整硬件学习证明。

因此最终默认是：**桥依赖的双向连接使用软件路径，不建立绕过桥学习的 HNAT/PPE 或软件 flowtable 缓存。** 包括这些客户端的 routed WAN，不能只禁硬件 `L2_BRIDGE` 枚举。只有能够证明不依赖 bridge/移动无线端点的独立有线路径可继续 PPE；未知能力拒缓存而不丢包。WED 的基础 DMA、token、硬件收发/加密/重排序继续保留。

性能范围公开：本机 AP 与下游 AP 共用 `br-lan`，其 LAN 互访与多数客户端 WAN 可能都会增加 CPU 开销，不能笼统声称无线 WAN HNAT 保持不变。吞吐、CPU、时延必须实测。今后要重新开放桥依赖加速，先证明移动可见性，再实现完整缓存一致性；本版不默认开放一个缺乏该证明的“优化模式”。

## 3. RX 入口、控制流与聚合

详细源码依据见 [通用 RX/TX 设计](bridge-universal-rx-design.md)。在实际 RX peer 仍有效的两个 `rx_*data_frm_announce()` 入口，统一普通数据的 PortSecured、PrivacyFilter、Radius 等约束，保留原 Class2/3、PN、MIC、解密、分片和重复包检查。使用 descriptor 解析的实际 peer，不再按 Ethernet SA 二次猜源授权，也不依赖目的表命中才检查源。

认证/握手明确保留现有 MLME/daemon 控制通道。先用格式明确、有界的 helper 分清 Ethernet II、LLC/SNAP 与 EAPOL payload：Ethernet EtherType 在 +12，不能把现有 `eth_frame=FALSE` 简单改 TRUE，因为现有 helper 跳14字节后的解释与它并不自动一致。helper 只解释封装，不放宽 WPA 状态机、重放或长度验证。未授权聚合不得因一个 EAPOL 子帧而授权其余普通子帧。

普通数据在共同 AP callback 的任何 DA 查表、Size 快捷分支与 clone 之前选择 bridge-owned 路径，只上送一次。广播/组播一并停止驱动内部复制，避免桥 hairpin 再发一份。BA、SW/HW A-MSDU、Ralink 第二 MSDU、普通及头转换都经过同一最终规则；不新增旧双 MAC 方案的 WCID/TA 身份 ctx、trylock 许可或关联 generation。

源授权采用正常接收时授权语义：已经合法接收的在途数据可经 BA/Linux 队列延后，不宣称 deauth 能撤回所有已接收包。若共享控制路径确实需要最小来源分类，只携带显式普通/控制/专用类别并完整初始化，不从未统一写入的 skb WCID推定授权。普通桥流的错误不得回退旧 DA-STA 内转。

## 4. TX 必须服从桥选出的端口

修复 `ap_tx_pkt_allowed`、`ap_fp_tx_pkt_allowed` 的全局 DA lookup 使用：普通桥出口必须匹配当前 wdev、目的 MAC、peer/TR 绑定、授权与 VLAN。找到另一 BSS 的旧条目时拒绝该候选，不能把包改道该 wdev。

覆盖 qm、fp_qm、fp_fair_qm 和最终提交：队列不能再依据一个旧 TR/WCID 把 packet wdev 重写成另一 BSS。ACK 优化等条件分支的二次 DA 查表也遵守相同约束。保留 sta_rec、端口安全和 TX 加密检查；广播/组播使用桥选定 wdev 的 BMC，不复用旧 RX WCID 强制回发。

## 5. hairpin 与隔离按实际出口执行

详细集成见 [通用配置与生命周期设计](bridge-universal-policy-design.md)。新增内部能力通知 `wireless_bridge_forwarding`，经 mtwifi 通知、wireless-device.uc、netifd device/ucode 状态传到 bridge 端口配置；不是用户开关，不依赖缺失的 iwpriv。角色变更显式清 true/false。非隔离 AP 的 hairpin 不再依赖 multicast_to_unicast/proxy_arp 是否开启；isolate 仍强制关闭。新能力端口写后读回，配置失败不 mark_up。

mt_wifi 每 adapter 注册 `NFPROTO_BRIDGE/NF_BR_FORWARD`，按真实 in/out AP 端口实施同 BSS NoForwarding 和不同 BSS BTNBSSID；不读目的 STA，不使用 skb_iif 代替桥入口。保留 Linux VLAN、STP、bridge_isolate 与路由/防火墙语义。BTNBSSID 不能用所有端口 BR_ISOLATED 替代，否则会误杀允许的同端口 hairpin。

隔离覆盖 AP↔AP 的全部 L2，包括广播/组播。这明确收紧旧驱动组播未查隔离的行为；本机控制流、网关及有线服务端业务按原网络策略处理。默认未隔离网络不因这一修正额外隔离。

hook 只持 WdevListLock 复制当前注册 AP 映射/标志，不取 MacTabLock/RxReRingLock、不分配/发包。注册失败则桥能力启用失败。释放 adapter 前 unregister，随后在不持回调依赖锁的进程上下文显式 `synchronize_net()`；注销本身不是同步排空。

namespace采用固定adapter创建时的namespace并持有引用，hook在该namespace注册。普通AP base设置netns_local，派生VLAN在注册前继承该约束并拒绝跨namespace创建；不能让upper单独移动绕过隔离。具体落在公共 `register_vlan_dev()` 的 `vlan_vid_add/register_netdevice` 之前做namespace检查与能力继承，VLAN feature重计算也保留该限制。该公共入口覆盖netlink与旧ioctl创建，不只靠netifd；不宣称支持任意AP跨namespace迁移。新VLAN分析postimage已包含实际5个相关平台补丁。

## 6. VLAN 域与接口拓扑

保留 common announce 已有 VLAN_RX_DROP/REPLACE/ALLOW、VID/PCP、帧内及 skb VLAN metadata；桥 FDB 以正确 VID 分域，不能剥标签后混学。同 MAC 不同 VID 必须独立。MT7986 的硬件 VLAN 输入表示须由真实路径与抓包证实，不能只看编译宏。

覆盖 AP base 直接作为 VLAN-aware bridge port，以及 VLAN upper 接桥两种标准形态。后一种不能只检查 AP base 的 IFF_BRIDGE_PORT，也不能因为“有任意 bridge upper”就把其它独立域一概改行为。按本帧规范 VLAN 表达解析实际 ingress，跟随匹配的 802.1Q/802.1ad upper 再判桥归属；VID0、双标签和 metadata/in-frame 组合要与 Linux demux 一致。

已核实 stable6.12.112 的 `__vlan_find_dev_deep_rcu`、`vlan_dev_real_dev`、VID/proto helper 为普通 EXPORT_SYMBOL，可在 RCU 下使用；不需要专有模块调用 GPL-only 的全 upper walk。隔离 hook 对已识别的 VLAN upper 还原 AP 身份，保留实际桥端口作 VLAN 判断；netifd 的内部无线能力与隔离属性也必须传播到实际 VLAN bridge port。

若启用现有可选 perSTA VLAN，必须在实际 RX peer可用处捕获网络域值，随转换/子帧/BA保留，再建立标准标签，取消晚期按当前 skb WCID重查并插标签；TX按目标域验证。这个 VLAN 值不是关联身份许可。没有闭环的可选控制面不宣称已支持，能力/配置失败须显式拒绝，不能静默走旧内转。

## 7. HNAT 与 nf_flowtable 的共同规则

详细拦截面、证据及返回值见 [通用卸载设计](bridge-universal-offload-design.md)。路径资格为 NONBRIDGE_VERIFIED / BRIDGE_DEPENDENT / UNKNOWN，检查原 ingress、当前设备、解析前和实际 egress、双向 forward-path，并保留路径经过 bridge 的事实。post-routing 的 state->in可能为空；缺证据不能默认安全。不占用未知 skb cb/mark，不保存临时 hook/wdev 指针。

HNAT pre/bridge pre 在5111 magic重建与任何私有快转前拒 bridge/unknown 缓存，继续 NF_ACCEPT；common post 在 xlat/准备条目前拒缓存返回0；WARP TX最终提交拒缓存返回1，避免被当成丢包。crypto/xlat/HQOS/pong等不能绕过相同资格，按各函数 ownership处理。所有真正 BIND 写入在 entry_lock 内复核资格、拓扑 epoch与全局 gate，WARP pending 按值记录 epoch。

NFT 与 xt 的双向 route 构造保留 bridge/WED/unknown 依赖；共同 `flow_offload_add` 在发布 tuple前最后核验，任一方向不安全拒整条连接入表，包括 NEIGH/DIRECT/XFRM。lookup/refresh 消费前也检查当前 gate/epoch，旧项 teardown 后本包走慢路径。不能只拒硬件 offload flag或旧 ndo_flow_offload_check。

generic flowtable 硬件 provider/特殊消费 API 只有具备同样最终提交和同步撤销合同才允许；当前不开放未验证能力。它们拒加速不影响普通协议栈转发。当前没有 nft flowtable 的现场状态不能替代这项机制。

## 8. 启动与配置屏障

策略随新固件启动确定，无双MAC或热实验开关。HNAT正常启动先新建/清零 FOE、设置表地址并清 cache，最后启用 hooks。普通桥能力和拒缓存策略必须一起发布，不能一半生效。

控制面采用核心内核的全局拓扑 gate/epoch（不按终端建状态），各后端共同遵守。桥 add必须在 RX handler/IFF_BRIDGE_PORT 等发布**之前**开始；generic upper 变更有配套 begin/end/abort，覆盖所有返回和 rollback，支持嵌套。仅 PRE/POST notifier 不完整：link失败可能无POST，unlink为void且忽略PRE错误。

外层 begin先阻旧 fast lookup与新提交、推进 epoch，再 `synchronize_net()` 排空通过旧 gate 的CPU处理。进入不安全域的 link同步清 FOE、foe_flow pending及硬件cache，成功才发布新拓扑；软件 flowtable按 epoch立即拒旧项，随后正常GC，不在RTNL等待可能反拿RTNL的offload工作队列。最终提交锁与 begin发布建立同一屏障，不能让旧 pending在清理后复活。

核心 gate/epoch的存储应位于始终可用的 net/core层，避免 builtin dev.c依赖可卸载bridge/HNAT模块；模块后端注册受RTNL保护的准备/完成回调。RX只读稳定epoch，不遍历控制回调。完整API、编译依赖和锁图在实现中作为一组提交，不增加每MAC临时特例。

具体实现合同：新增 `net/core/bridge_offload_guard.c` 与公共内部头，提供 begin/end/abort、稳定epoch读取及后端register/unregister。控制事务只在RTNL进程上下文运行，嵌套深度由RTNL串行保护；全局原子序号用“奇数过渡、偶数稳定”表达。外层begin原子发布过渡并推进世代，最后end/abort发布新的稳定世代；普通失败也结束事务，硬件清理失败由后端保留禁止新绑定的错误状态。后端退出与注册同样由RTNL保护，RX不持后端指针。

后端准备可保守清全表，不必发明终端映射；只有实际网络配置变化才执行。entry最终提交在现有entry_lock内检查完整序号一致且稳定；begin发布过渡后也经过该锁/屏障再清表，使“已在锁中完成的旧提交被清除，之后的旧提交被拒”可证明。NF软件flow在创建时存序号，lookup/refresh不匹配即慢路径及teardown，异步GC不承担安全屏障。epoch不是站点授权世代，两者不可混称。

cache清理超时从void日志改为可检查错误：link失败保留旧拓扑并阻新硬件绑定，不宣称此时旧cache已全退CPU。受控恢复后才能重试。unlink采用更窄的单调性证明：受移除bridge域影响的flow原本从未BIND，故不假造void unlink的veto；gate/epoch一直覆盖detach结束，之后仅允许新验证的独立路径。任何破坏这个前提的虚拟/未知路径仍拒绑定。

SER warm init、HNAT enable/disable和卸载同步重置 pending/epoch；表扫描使用实际 foe_etry_num，入锁后重核条目。清理或策略应用失败不 mark成功、不回旧STA直转，也不把“关PPE端口”当成已证明的CPU回退。

## 9. 生产与诊断分离

从生产 c196312d建立独立通用修复开发分支。生产实现不依赖043诊断、不含roam_diag参数或固定终端信息，原生产guard保持有效；不改变uBoot/MTK U-Boot布局和分区。保留已有生产与Build19诊断引用，不合并已撤回r1。

driver、kernel、netifd与mtwifi用户态修改分别成可审提交；完整方案Astra设计通过后实施，完成真实helper/生命周期测试和精确postimage应用检查，再交Astra实际实现审查，通过后推送CI编译。沿用锁feeds、外部Go bootstrap与缓存核验、RRM/WNM/FT及HNAT/WARP模块检查。测试所需增强观测只放独立验证分支/overlay，其结果不能成为生产转发条件。

不自动刷机或发布正式Release。不持续查询CI，沿用用户通知构建完成的约定。刷机前仍交付匹配布局的镜像、SHA和回退说明。

## 10. 验收与停止条件

静态/构建：无任何固定身份或业务端口；无普通桥流 legacy fallback；原RX安全、桥hook生命周期、TX端口约束、VLAN分类、gate/epoch和所有最终提交点逐项审阅。真实MT7986宏组合、netifd/ucode与内核全构建，模块配置/符号、patch系列和git diff检查通过。

有意义的helper测试：格式明确EAP解析/短包/恶意封装/未授权聚合，VLAN域解析与同MAC多VID，真实桥in/out隔离矩阵、TX错wdev/TR/WCID、路径资格和双向依赖；控制事务故障注入覆盖每个失败/abort、cache超时、旧pending跨epoch、SER和模块退出。测试直接调用实际helper，不能只搜补丁字符串。

真机：任意客户端重复同AP/跨BSS/跨频段/跨AP并返回；故意观察旧STA仍在时桥能选正确出口，无需等ageout；IPv4/6 TCP/UDP、ARP/ND、非IP L2、广播/组播、冷/热流，BA与聚合；关闭mcast_to_ucast也可同AP互访；隔离、VLAN、网关/DHCP、动态接口和重载并发。匹配真实请求/响应、桥/FDB/物理出口与实质数据传输，不只看to_os/submit_rc。

性能单独报告各拓扑的吞吐、CPU、时延及重配置中断；没有测量不填写损失百分比。出现越权/跨VLAN、重复/环路、桥选端口被重定向、旧流绕源学习、cache错误仍发布拓扑、UAF/锁死或无法安全退出，即停止放行并修正。设计认可不等于编译、真机或性能验收。

## 11. 实施修改面与检查顺序

- 驱动补丁：生产42patch后新增通用AP桥入口/源授权解析、TX wdev绑定、桥出口隔离与VLAN域处理；不包含043诊断源码。保留ops ABI，不向RX增加MacTabLock等待。
- 内核补丁：当前平台队列之后新增net/core guard、br_add_if/del_nbp及upper link/unlink事务，HNAT路径资格/最终提交/pending/cache失败处理，nft/xt双向route与共同add/lookup epoch检查。源文件基于相关现有patch postimage生成，不直接破坏overlay前像。
- netifd补丁：配方锁定的cbb83a185740源码新增内部能力、AP/VLANupper传播和hairpin配置确认；MTK ucode及共用wireless-device通知链同时修改，必要up/down/reload错误向上返回。
- 按模块分提交并逐组核对字段/接口，再做全组合构建。VLAN/隔离/授权/拓扑屏障属于最终方案必要部分，不作为推送后才发现的可选增强；未完成就不宣称整套实现通过。
