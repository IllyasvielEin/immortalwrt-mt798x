# 普通 AP 通用交桥最终版：RX、隔离与 VLAN 设计

状态：设计与源码核对，供 Astra 重新审核；未实现、编译、联网或操作设备。基线为 `sources/vendor-mt79xx-7.6.7.3/review-baseline/mt_wifi`，已应用当前生产 42 个补丁，含041端口安全补丁。除仓库集成文件外，以下相对源码路径均位于该树。旧双 MAC 方案已被用户否决，不作为实施前置阶段。

## 1. 推荐与撤回的旧建议

推荐把**加入 Linux bridge 的普通三地址 AP BSS**变成通用 Linux L2 入口：现有 RX 成功接收且授权的普通数据全部进入 bridge，不再根据目的 MAC 在 vendor 站点表中是否存在而选择内部回发。覆盖该能力范围内所有客户端、所有频段、所有 BSS、IPv4/IPv6、ARP、普通非 IP L2、广播和组播，没有 IP/MAC/BSSID/端口白名单。

`netif_is_bridge_port(wdev->if_dev)` 只是实际拓扑的一个输入，不能单独作为放行或恢复 legacy 的条件。普通数据必须先通过整 BSS 激活门，再与本次已提交网络域意图匹配；VLAN upper 接桥时还需逐帧解析真实入口。不新增“目标设备”配置或用户诊断开关。完整阻流合同见 `bridge-r3-activation-contract.md`，固定 CPU 候选的收敛条件见 `bridge-r3-cpu-profile-activation.md`。

**不建议把旧 pair guard 放大成全网许可，不建议引入全套 WCID/TA ctx 或关联 generation。** 旧 guard 要证明一个针对选中身份的例外；通用模式要实现普通已接收数据的标准 AP 入口语义，应在已有源 RX 安全边界完成授权与控制流分离，随后让 Linux 决定出口。BA 不需保存站点指针、接收身份或关联代际；不增加 adapter 上的共享 current-packet 状态，不改 `wifi_dev_ops` ABI。

但“已有 RX 已经绝对完整，所以在 AP 函数第一行直接 TRUE”也未经证明：原函数末端仍有一次源端口检查，且041保留了握手例外。通用版需将普通数据的源授权约束明确落在实际 RX peer 仍可用的位置，保留认证控制流，才能移除依赖目的表的转发决策。下文给出最小改动与必须核实的解析问题。

撤回旧建议：

- 不再要求精确双 MAC、固定 BSSID、SA与选中TA的一次特殊匹配；不需要为了它们扩展 BA ctx。
- 不再只把原同 BSS `0/1` 改成 `1/0`。原跨 BSS `0/0` 也可能来自陈旧目的条目与隔离检查，继续保留这类目的表拒绝仍会制造漫游黑洞。
- 不再让不确定的 ctx/锁竞争回退旧内部转发。对已经承诺通用桥模式的 BSS，退回旧路径会重新引入故障。
- NoForwarding 是隔离策略，不是交桥模式；必须用真正的桥出口策略表达隔离意图。

## 2. 已确认的调用链与安全边界

`embedded/common/cmm_data.c:3608–3609` 仍是唯一 `ops->rx_pkt_foward()` 调用点。已检索 `embedded/`、`os/`、`include/` 全部 C/H。

注册点：AP为 `ap_data.c:5219`，另有 cfg80211 GO表 `os/linux/cfg80211/cfg80211_inf.c:1092`；STA为 `embedded/sta/sta_data.c:2718`、`sta.c:2919`、cfg80211 `:1058/:1674`；WDS为 `ap_wds.c:1624`。STA/WDS 回调直接 TRUE。当前 Build19 未启用 `CONFIG_MTK_CFG80211_SUPPORT`，但通用代码不能让 GO 自动因为复用了 AP 函数而获得普通基础设施 AP 资格。

实际源 peer 从 FMAC descriptor → `RX_BLK.wcid`（`mac/mt_fmac.c:2742`）→ `check_rx_pkt_allowed()`（`cmm_data.c:6810`附近）解析，随后 `dev_rx_802_3_data_frm()` / `dev_rx_802_11_data_frm()` 调对应 wdev RX ops。AP普通路径在 `ap_rx_pkt_allowed()` 检查 Class2/3、ToDS=1/FromDS=0、BANClass3和可选 Radius MAC auth，设置 `fRX_STA`（`ap_data.c:3935–3990`）；WDS/A4分支是不同能力，不能因为同用AP函数就包含进来。

`ap_ieee_802_3_data_rx()`、`ap_ieee_802_11_data_rx()` 保留重复包、A-MSDU检查、NULL拒绝、分片/解密/MIC等原处理。`rx_data_frm_announce():6210` 在普通非EAP分支检查实际 peer 的 PrivacyFilter（`:6314–6332`）；`rx_802_3_data_frm_announce():6752` 经041读取 `tr_entry[pEntry->wcid].PortSecured`，未授权仅保留 `RTMPCheckWPAframe()` 认可的握手例外。

原 `ap_rx_pkt_foward():4117–4128` 的最后一次 PortSecured 检查，是在**目的表命中关联STA**后才按 Ethernet SA二次 hash查源。目的未知、总表Size≤1等原路径本来就上送；因此它既不是统一的源授权边界，也不应继续承担 bridge mode 的入口授权。通用版应使用上游实际 peer 做普通数据授权，不在出口重新猜RX身份。

### 2.1 必须补实的最小源授权分流

推荐在两个 `rx_*data_frm_announce()` 内、原安全检查之后、进入普通/AMPDU指示之前，统一普通基础设施 AP 的规则：

1. 实际 peer存在、关联且属于当前普通AP接收上下文；保留现有Class2/3、PrivacyFilter、Radius及密码学检查。
2. 普通数据必须实际peer的有效TR端口已授权；不依赖Ethernet DA是否命中站点表。拒绝就在这个源入口完成，不延后到目的选择。
3. EAPOL/握手仍走现有MLME/daemon安全流程，不作为未授权普通数据交桥。管理/控制/NULL仍由原接收分支处理。
4. 未授权聚合不能因为其中一个子帧看起来像EAP就把所有子帧当普通数据放行；检查SW A-MSDU、HW拆分A-MSDU与Ralink双MSDU的实际入口。合法握手聚合形态如存在，需要逐帧控制流分离，不能用第一子帧建立整个聚合的授权。
5. 已授权后进入普通数据通道的帧不再因为日志、TCP解析失败或某个目的表值改变出口；也不把授权快照保存成长期许可。

这项改动是把当前依赖目的命中的源端口约束移回源入口，不是要求重写WPA状态机、统一所有auth writer的锁或为每次密钥更新建立世代跟踪。接口和桥策略改变仍按控制面停流/重载处理。

**直接代码证据要求先核验EAP解析：** `rx_802_3_data_frm_announce()` 保留 `eth_frame=FALSE`；头转换普通ToDS帧的 `Addr2` 是重建Ethernet SA（`mt_fmac.c:2894`），041只修端口绑定，未修解析格式。`RTMPCheckWPAframe()` 在 `embedded/security/cmm_wpa.c:1476–1570`：eth_frame=TRUE时先跳 `LENGTH_802_3`，随后又检查两字节EAPOL值；若输入为标准Ethernet header，EtherType在+12，跳14后是EAPOL payload，不能简单将FALSE改TRUE就宣称正确。应以真实输入布局和有界测试确认，并用一个规范解析 helper 区分Ethernet/LLC-SNAP与EAPOL payload，再调用/复用已有状态机处理。此处尚未证明现场存在可利用漏洞，也不能把未做这项核验的早返回补丁视为最终方案。

若实现需要跨函数传递“普通数据已通过入口”分类，最小只是内部enum/一位来源标签，不是旧WCID/TA/关联ctx。优先通过整个普通BSS能力契约和控制流分离避免该标签；只有某些共享控制路径确实无法区分时才添加，并明确初始化/clone/BA继承位置。不得从原有、未统一初始化的skb WCID自行推导“已授权”。

## 3. AP 回调与 BA/A-MSDU 的具体实现点

`ap_rx_pkt_foward():4035` 在验证wdev/MBSS有效后、原 `MacTab.Size<=1` 和任何 `MacTableLookup(DA)` 之前选择普通bridge策略。就绪且能力支持时：普通数据只返回上送决定，不作driver内部clone；原 `WH_EVENT_NOTIFIER` 内部转发计数不再增加。`NoForwarding`/BTNBSSID在此不再通过目的表拒绝包，其隔离由桥策略实现。

实现可将旧函数主体保留为显式 legacy helper，普通bridge分支不进入它；函数原签名和所有ops表保持。控制/握手流如需原专用处理应走明确控制流，不能靠一个“无法判断，调用legacy”覆盖普通数据。短包/损坏头应拒绝并计数，不能因为解析失败回退旧无线出口。

所有普通Ethernet指示均汇合于common announce：`cmm_data.c:3462`（SW A-MSDU clone）、`:3723`（802.3）、`:4033`（802.11转换）、`:4099/:4102`（Ralink两个MSDU）、`ba_action.c:179`（BA出队）。因此通用BSS决策在共同回调处自然覆盖队列，不必存接收WCID/TA/entry/wdev指针。

BA enqueue保留自己的WDEV记录与原队列生命周期；SW A-MSDU 每个子帧依旧转换后进入共同announce；不额外clone两次。对正常已授权包，广播/组播也只上送一次，不保留driver先发一份、bridge hairpin再发一份的双重反射。Ralink第二包、HW A-MSDU、普通及头转换均须验证，而不是只测试TCP SYN。

无需读取桥FDB或引入RTNL/GPL-only桥私有接口，不在RX新增MacTabLock/trylock，旧pair方案发现的 `RxReRingLock→MacTabLock` 反序也随之避免。`to_os=TRUE`之后如何实际抵达bridge、避免WARP/NAT/WIFI_PKT_FWD旁路，仍须与快路径子方案结合；单独本节不能证明交桥完成。

## 4. 行为矩阵

| 接收/目标情形 | 通用行为 | 必须满足的约束 |
| --- | --- | --- |
| 普通已授权AP客户端，同BSS目的仍在本AP | 先交桥；桥FDB选回同端口，再通过该AP TX正常发无线 | 非隔离时hairpin=1；TX仍做目的授权/sta_rec及本wdev校验 |
| 同BSS旧STA条目仍在，目的实际在另一AP/有线 | 先交桥；桥按当前FDB输出外部端口 | 无目的站点表前置拒绝、无内部clone、不自动回legacy |
| 目的表指向另一BSS但实际已漫游 | 同样只由bridge选择出口 | 不保留原BTNBSSID基于stale表的0/0 |
| 目的真实在另一BSS | 桥选择该BSS端口，按跨BSS隔离/VLAN策略决定 | AP TX不得根据全局DA查到另一wdev后越过所选端口 |
| 从外部AP返回的普通无线源数据 | 全部进入桥触发源学习 | 不再因本机另一个目的条目命中而内部直转，避免FDB留在外部口 |
| IPv4、IPv6、ARP、普通非IP L2、LLC/SNAP数据 | 同一L2策略，不检查IP/传输层端口 | 仅安全入口和标准桥策略负责许可/协议消费 |
| 广播、组播、未知单播 | 交桥一次；由桥学习/泛洪/MDB及AP TX实现 | hairpin、IGMP/MLD snooping、multicast-to-unicast与无线组播行为一致验证 |
| 本机MAC或本机IP业务 | 沿bridge本机投递/路由接收 | 不强制把本机协议也发回无线 |
| 未授权普通数据、原RX密码学/PrivacyFilter拒绝 | 源入口拒绝 | 不能通过早返回绕过源检查 |
| EAPOL、认证管理流 | 原MLME/daemon/本机控制处理 | 不给未授权普通数据创建例外；不可借组播反射泄漏控制流 |
| 静态perBSS VLAN、trunk/tagged或skb VLAN metadata | 维持来源VID/PCP与既有RX政策，交bridge VLAN处理 | tag不能丢失/双插；同MAC不同VID不能混学 |
| 动态perSTA VLAN | 在实际RX peer可用处确定VLAN归属，传到最终Ethernet/标准skb VLAN | 不能继续只凭晚期不可靠skb WCID查当前站点来决定包的旧VLAN |
| 独立WDS/APCLI/STA wdev | 保留其专用ops，不自动套普通AP能力声明 | 不宣称特殊拓扑已获得本修复的完整保证 |
| 同BSS的A4/MWDS/CLIWDS/mesh/backhaul角色 | 属于不同能力，必须显式支持或拒绝该BSS配置/角色激活 | 不能逐包悄悄回legacy，同时声称此BSS的所有普通数据都遵桥 |
| 非bridge成员的AP接口 | 保留既有独立/路由AP语义 | 自动选择条件的明确范围，不以接口名/设备名判断 |

未知单播在无线TX当前实现中未必能向所有未关联终端发送，这是正常无线介质限制；目标保证是vendor stale站点不能抢走bridge的唯一出口。桥FDB本身错误/陈旧、网络环路或对端AP不转发不由这项改动自动修复，需区分因果证据。

## 5. 隔离与同 AP hairpin

普通非隔离BSS必须hairpin=1，否则bridge选择相同无线口的帧会因普通split-horizon被挡住，造成同AP客户端互访回归。仅“bridge已挂载”不足以验收。

perBSS `NoForwarding` 的用户意图是同BSS客户端隔离。bridge mode下对应hairpin=0或等价的精确forward层拒绝；流量仍应上送以到达路由器/有线网络。它不是设置to_air=0并保留to_os=0的同义词。广播/组播从无线进入也不得绕过隔离反射回该无线口，但本机/有线入站广播是否允许遵既有网络隔离定义。

全局 `NoForwardingBTNBSSID` 要拒绝**不同AP BSS端口之间**的转发，同时允许同BSS非隔离hairpin。简单把所有AP端口设 `BR_ISOLATED` 不等价：hairpin时入口和出口仍是同一isolated端口，可能连同本应允许的同BSS通信也拒绝。建议控制面根据动态AP端口集合安装bridge-forward规则，明确同端口由perBSS策略处理、不同AP端口间按BTNBSSID拒绝；不要静态写死ra/rax名称。若用nft bridge，包选择需加入 `kmod-nft-bridge`（Build19未选择），规则动态随netifd端口生命周期维护。

`NoForwarding` setter目前只改 `IsolateInterStaTraffic`（`ap_cfg.c:14135–14163`）；BTNBSSID setter只改全局字段（`:14170–14188`），不能假定改driver字段会自动修改Linux桥。UCI converter当前仍将 `isolate` 映射成DAT NoForwarding：生产仓库 `package/mtk/applications/mtwifi-cfg-ucode/files/usr/share/ucode/mtwifi/converter.uc:311`。最终版需让配置更新桥策略，或在运行时setter拒绝无法同步的热修改，要求经配置reload完成，不允许两份策略悄悄分叉。

## 6. VLAN：标准标签与可选动态能力

普通交桥不应排除所有VLAN包。common announce先执行现有MAP TS与VLAN RX政策（`cmm_data.c:3545–3606`），最终bridge分支位于其后，保留政策drop/rewrite。标准bridge根据VID学习/转发，桥VLAN成员、PVID、egress untagged等为最终L2域权威。

保留 in-frame 802.1Q/802.1AD/QinQ 和 `skb_vlan_tag_present()` 表达，不能只看EtherType判断“无VLAN”。不在bridge模式盲目clear VLAN metadata、剥标签或把VID=0视为没有tag。硬件policy若剥掉tag必须提供正确的标准skb/帧表达，不能让Linux看见无VID后跨域学习。

MT7986明确使用 `VLAN_RX_TAG_HW_MODE`（`chips/mt7986.c:12460`），wdev VLAN setter会更新ASIC（`ap_cfg.c:14313`）。在已读FMAC代码仅看到 `RXD_HTF→rx_blk->is_htf`（`mt_fmac.c:2747`），未找到统一建立Linux `skb_vlan_*` metadata的代码。因此静态VLAN实际输入/输出表达必须用源码与实机抓包共同核对，不能仅凭“编译了VLAN_SUPPORT”放行。

可选 `DYNAMIC_VLAN_SUPPORT` 现有 `os/linux/rt_profile.c:1786–1820` 在很晚的上送阶段用skb WCID再次查当前peer->vlan_id，再插标签。旧调研已证明WCID并非每条普通/HDR_TRANS/BA/新skb路径统一初始化；即使补齐WCID，同槽复用/改VLAN也可能把已接收包归入新域。通用最终版如启用动态VLAN，建议在实际peer可用的入站安全边界复制**VLAN值**，转换为Ethernet时建立标准tag/metadata；BA与子帧只继承VLAN元数据，不携带站点指针/授权generation。取消此类包的晚期二次VLAN决定，避免重复插tag；TX同时按目标STA VLAN验证。该小型VLAN归属值是网络域元数据，不是旧pair身份ctx。

若本轮构建未启用动态VLAN/每VLAN GTK等可选能力，可以明确保持其未启用；以后打开必须完成这项tag传递实现与测试，不能自动宣称全功能已通过。若运行配置请求的VLAN表达尚不能证明安全，BSS设置应失败并给原因，不能去标签后继续，也不能落回内部转发掩盖缺口。

动态STA VLAN的最小可执行交付界限：当前未编译该功能的普通构建，不添加身份ctx，不启用不完整的动态VLAN分支；netifd配置如果声明STA VID/动态VLAN而目标没有该能力，`setup_failed`并保持BSS未启动，不能仅忽略该键。若未来构建打开 `DYNAMIC_VLAN_SUPPORT` 但未包含完整实现，同样拒绝 `bDynamicVlanDisabled==FALSE` 的bridge AP配置及运行时启用。若决定本次同时交付，则必须一并实现以下闭环：

1. 实际RX peer通过授权时复制该包的VID/PCP/是否分配标记；捕获在BA和后续VLAN更改之前。受控VLAN变化停流/清队列后生效，不把旧包改归新VID。
2. 通过 `RX_BLK` 内部域元数据以及必要的 `reordering_mpdu` 值型字段，或已证明初始化/复制规则的packet元数据保存该VLAN值；SW A-MSDU/Ralink第二MSDU逐包继承。只在可选能力编译下需要，不保存peer/wdev指针或关联token。
3. 最终Ethernet帧建立标准tag/metadata；入口已有tag与分配VID冲突按明确定义的策略拒绝/规范化，不能信任客户端任意tag选择网络域；QinQ需区分控制的外层域和允许的内层，禁止一律覆盖任一层。
4. 取消这些已归域包在 `rt_profile.c` 的晚期WCID→当前vlan_id二次决定，防双tag；未分配动态域的普通包保持静态/桥策略。
5. AP TX在任何tag剥除/改写前获取桥选择的VID，验证它与目标STA允许域一致；tag可能由hardware helper在 `send_data_pkt():1566` 前处理，须保留可验证的域值到最终TX提交，不以“包现在没tag”免检。
6. 组播必须按VID选择适当BMC/GTK或逐STA复制并只向该VID站点发送；同一BSS用一个共享GTK把不同VID的广播发给全部STA无法满足VLAN隔离。没有每VID GTK或等价可靠发送能力时，拒绝此类动态VLAN配置，而不是宣称只修单播就支持了动态分域。

这给出完整可实现路线和明确的启用拒绝规则；当前基线仅声明STA schema有vid或编译存在相关字段，不是这个闭环已完成的证据。

## 7. TX 端口边界与特殊拓扑

交桥后的AP TX仍需要MacTable把已选择的无线端口映射到本地接收STA，这是正常用途；要禁止的是vendor把目的表作为绕过bridge的唯一出口或覆盖bridge选定端口。

`ap_fp_tx_pkt_allowed():346` 和 `ap_tx_pkt_allowed():587` 当前按DA作全局 `MacTableLookup()`，拿到关联条目后设置其WCID，已读相应代码未见在这之前要求 `entry->wdev == wdev`。**两条路径必须统一使用当前出口wdev限定的查找和校验，作为最终版必要修改。** 现有 `embedded/mgmt/mgmt_entrytb.c:568–610` 的 `MacTableLookup2(pAd, DA, wdev)` 已有地址+wdev限定语义，可以直接复用，避免另造查找结构；`mac_entry_lookup()`虽接收wdev参数，`:2742`实现实际忽略它，不能以参数名字为凭据。

允许发送前校验普通目标peer和TR：有效index、peer Addr等于最终Ethernet DA、peer wdev与TR wdev均等于bridge选择的wdev、peer关联/普通站点类型、有效sta_rec、端口授权、当前允许VLAN及Radius等原约束。未找到本站点或只在另一wdev找到时拒绝本次无线副本并计数，不能改道那个wdev。未知单播的其它bridge泛洪副本已由bridge负责，这里不得把本端口的miss变成跨BSS“帮助转发”。广播/组播使用当前wdev的BMC/适当VLAN域，不拿旧RX WCID强制回发。

仅在TX入队时约束还不够：`embedded/common/qm.c:1544–1551` 的 `ge_deq_data_pkt_v2_process()` 明确在packet wdev与TR wdev不一致时把 `pTxBlk->wdev` 改成TR wdev，注释称“STA换频段时纠正wdev”。**普通bridge AP路径必须禁止这一重定向，改为按原bridge出口wdev重新解析DA并核验，或拒绝/计数；不得执行原赋值。** 对同一wdev内WCID被另一MAC复用，最终DA核验同样必要；只有wdev相等还会把包送给错误站点。

`fp_qm.c:434–461`、`fp_fair_qm.c:306–343` 从packet WDEV/WCID构造TX_BLK，及普通qm其它dequeue路径，应调用同一项最终绑定校验；最靠近统一数据TX提交的可用点也需保证 peer/TR/WDEV/DA/VLAN 一致，覆盖硬件offload与非offload，而非只补其中一个queue。使用当前查找/校验重新决定本端口STA，不缓存entry指针作为跨队列许可；不在此获取RTNL。实现时仍沿既有表槽并发/在途语义，不把单次校验夸大成能撤回已交硬件的帧。

额外条件分支：`cmm_data.c:7685–7691` 的 `REDUCE_TCP_ACK_SUPPORT` 会再次全局DA查表并覆盖skb WCID；普通bridge AP也必须改为本wdev限定或复用已核验结果，不能在主入口修完后被该功能重新覆盖。该构建可未启用，但编译分支仍需正确。保留原PortSecured、sta_rec及TX密码学检查，不以绕过它们来保证吞吐。

独立WDS/APCLI专用接口可保留其原ops；多链路、A4、MWDS、CLIWDS、MAP backhaul、P2P GO等未审计能力不与普通三地址AP混为一谈。不要根据“编译了A4”就禁止所有普通AP：MAP宏会编入A4，普通fronthaul仍可工作。应检查实际角色/配置；对于进入自动bridge模式的普通BSS，阻止未支持的四地址/角色激活，或在控制面拒绝该配置，避免运行中出现半个BSS走bridge、半个走legacy且无法兑现承诺。若专门支持这些能力，需另审地址代理、环路、VLAN及快路径，不以“软件返回TRUE”当成完成。

## 8. 并发、失败与默认策略

推荐默认受控激活：每个动态创建的普通 AP BSS 先默认阻流，配置、读回和 commit 完成后按其明确网络域交桥或使用被允许的 standalone 路径；不做设备名/MAC/SSID匹配。已承诺 bridge 的域不因端口移除或配置失败变成 standalone。回滚使用布局兼容的已知可用固件；生产不提供在线 legacy 开关或隐式旧内转 fallback。

bridge attach/detach由Linux既有RX handler/RTNL/网络设备生命周期管理；在RX只读取合法成员状态，不取得RTNL、不读桥私有FDB、不缓存bridge或wdev裸指针。netif_is_bridge_port不是lifetime引用，仍依赖原RX持有当前netdev/wdev的合法生命周期。对桥/VLAN/隔离/无线角色的配置变更，由控制面先停该BSS的数据/接口，再配置并验证bridge端口策略，最后恢复；失败保持BSS down/blocked并报告原因，不恢复旧内部直转。缺hairpin、隔离规则安装失败、VLAN能力不支持或旁路未约束都属于配置失败，不是逐包“尽量回退”。

BA 不新增站点身份或关联 token：已通过实际源授权的数据可作为正常在途数据延后，Astra r2 已接受这一语义，不承诺 deauth 撤回所有已接收数据。另一个独立边界是 BSS 配置变更：驱动内旧 BA/TX/ACK/PS 缓存必须继承并复核配置 lease，PREPARE 撤销旧 lease 后不能在新配置下重新获许可。配置 lease 不是站点授权代际，也不撤回已进入 Linux 或已提交硬件的数据。

`MacTabLock`并不保护全部PortSecured writer（例如 `cmm_wpa.c:5708/:6828`），wdev注册/注销另用WdevListLock（`common/wdev.c:104/:165`）；READ_ONCE也不让多字段整体原子。通用方案不新增一个假称解决这些问题的trylock许可步骤。桥模式出错不可以“identity_busy→legacy”反复引发黑洞。

故障计数至少区分：正常交桥、源授权拒绝、控制流消费、损坏包、拓扑不支持/配置未就绪、bridge TX跨wdev拒绝、VLAN归属错误；计数和日志不决定是否交桥。被bridge STP/VLAN/隔离规则拒绝应区别于driver误用stale STA。OFF/回滚不会撤销已经学习的FDB或conntrack，重载期间结束旧性能/因果比较窗口。

## 9. 精确修改面与验收

| 文件/组件 | 计划修改 |
| --- | --- |
| `embedded/ap/ap_data.c` | AP回调在DA查询前选择普通bridge路径；旧逻辑保留为非bridge/明确专用legacy；normal/fp TX统一调用本wdev限定peer/TR/DA/VLAN校验 |
| `embedded/common/qm.c`、`fp_qm.c`、`fp_fair_qm.c`与公共数据TX提交 | 最终发送前复核packet出口wdev与当前peer/TR/DA/VLAN；bridge AP禁止qm中的TR驱动wdev重定向；检查ACK优化再次改WCID等条件分支 |
| `embedded/common/cmm_data.c` | 两个RX announce统一实际peer普通数据端口边界；控制/握手分流；保留全部普通/头转换/聚合/PN/安全处理与VLAN流程 |
| `embedded/security/cmm_wpa.c`及相关内部声明（若解析核验需要） | 将Ethernet/LLC-SNAP/EAPOL格式解释变为有界、可测试helper；不重写WPA状态机 |
| Linux helper/接收上送实现 | 自动bridge成员判断、实际bridge注入、标准VLAN元数据；由快路径子方案合并最终位置 |
| `mtwifi-cfg-ucode` netifd集成 | 用实际动态AP端口配置hairpin、同BSS/跨BSS隔离和VLAN；失败报告并保持接口未就绪 |
| 可选动态VLAN编译分支 | 若启用，入口捕获VID/PCP并随包转换/BA继承，移除相关晚期当前WCID二次决定；组播必须分VID；能力未闭环时显式拒绝启用，不落回原路径 |
| 包/固件配置 | 精确隔离若采用nft bridge，加入所需内核模块/规则管理；不混入旧pair诊断许可 |

验收必须覆盖以下性质，不以“编译完成”代替：

1. 生产42patch基线上实际MT7986/APCLI/WDS/MAP/HDR_TRANS/VLAN组合编译；cfg80211/GO等共享ops在其编译组合中不会误入普通AP策略。无新的ops ABI/站点裸指针或RX per-packet共享变量。
2. 无任何固定IP/MAC/BSSID/端口；任意STA对重复同BSS→跨AP→返回、两个无线频段/不同BSS、无线↔有线。故意保留旧目的STA，仍由bridge实际出口决定；返回方向源FDB能重新学回无线。
3. 普通/HDR_TRANS、BA直通/排队/timeout flush、SW/HW A-MSDU、Ralink第二MSDU；IPv4/IPv6 TCP/UDP、ARP/ND、非IP L2、广播/组播。组播不重复、IGMP/MLD/MDB及multicast-to-unicast无回归。
4. 同AP非隔离双向可达；perBSS隔离、跨BSS隔离分别开关；证明BTNBSSID不误杀允许的同BSShairpin，旧STA不能造成隔离判断错误。控制面策略安装失败时不进入legacy。
5. 未授权/PrivacyFilter拒绝、认证握手及组密钥更新、有界EAP parser恶意/短包；未授权聚合不能把普通子帧送入桥。EAPOL/管理流不因为模式而被当作普通未授权数据转发。
6. tagged/untagged/VID0/802.1AD、skb VLAN metadata、同MAC不同VID、静态wdev VLAN、硬件tag模式，及实际启用的动态VLAN；入口VID/出口VID与FDB VID域一致，无丢tag/双tag/跨域。
7. 原无线接收授权语义、BA/teardown/桥attach/detach/配置reload并发压测；无新增锁序、无未声明fallback；实际快路径/硬件卸载无法绕过bridge学习和隔离。
8. 吞吐、CPU、时延及功耗影响实测；root fix通过后才能评价是否继续增加安全的卸载优化，不能为保性能悄悄恢复driver内部唯一出口。

当前尚待主方案合并的关键事项是：实际bridge与快路径合同、规范EAP输入格式、静态硬件VLAN表示和动态隔离配置落点。它们是最终实现必须完成的具体工作，不应以扩大旧双MACguard或不断增加身份ctx替代。
