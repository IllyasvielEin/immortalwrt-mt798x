# r3：PPE CPU 包的所有权与私有回注边界

日期：2026-11-09。只读设计，供关闭 Astra r2 P1-4；未修改实现、联网、编译或操作设备。本报告已转入独立分支 `fix/ax6000-bridge-forwarding` 的新worktree文档目录，停止维护旧research副本。仅定义packet ownership必要合同，不为可选硬件模式新增完整功能，不替代父方案的拓扑事务、SER、CPU hook/DMA退出协议。引用的sources路径以 `/home/illya/workspace/router/ax6000` 为根。

引用：H=`sources/hnat-6.12.112-review-baseline/drivers/net/ethernet/mediatek/mtk_hnat`；E=`sources/linux-6.12.112-ethernet-review-postimage/drivers/net/ethernet/mediatek`；D=`sources/vendor-mt79xx-7.6.7.3/review-baseline/mt_wifi`；W=`sources/vendor-warp-20250408-5f71ec/source/warp`。本轮已读准确Ethernet postimage，不再沿用r2“缺完整mtk_eth_soc.c postimage”的旧范围说明。

## 1. 必须改变的顺序

明确撤回r2“资格不符就统一在pre接受、common post返回0”的包级推广。**拒绑定、包的所有权和当前拓扑资格是三件事。** 一份硬件已经发送过的CPU副本，即使gate关闭、路径UNKNOWN、alg置位或epoch失效，仍须消费，不能再走一遍软件网络。

生产推荐：在**硬件接收descriptor被CPU合法取得时**完成所有权解码；能确定的复制/私有维护包在产生者处消费。正常原始数据进入协议栈；后续所有拒加速分支只适用于这类正常数据。该层早于HNAT magic重建、alg清理、拓扑资格、GRO、bridge源学习、nf_conntrack及private `do_hnat_*`快转，也不能依赖HNAT hooks此刻是否已注册。

最小、确定的生产能力约束是：保留普通WED DMA/token/收发/加密/重排序；**从启动和每次恢复起不开放private HQOS ping-pong、PPE multicast复制及未验证的CPU继续发送模式**。独立有线GMAC直达PPE flow也只有在其维护合同完整时才属于可开放能力，不能仅凭“不经过bridge”声称已支持。不能靠包到post时再按当前FOE的act_dp/WCID挽救私有包。这是全局能力限制，既无设备/地址名单，也不是临时试验开关。

KEEPALIVE_DUP_OLD_HDR(0x15)本身可无需查表确定为副本，并始终消费。消费前可在**已经证明资源/生产世代一致时**，沿受保护的现有账户helper做best-effort维护；不确定时只跳过维护，仍消费。旧0x13/0x14 keepalive模式的“是否承担一次原始发送”在已读C源码中没有完整硬件契约；不得只凭名字把两者都当副本。保持r2已选的正向证明独立有线能力，**本报告不擅自将关闭全部PPE binding或新增MIB worker变成生产默认**。账户/conntrack维护的实际闭环及长期有线能力由主线程/Astra裁决，必要修复和能力缺口分开列明。

## 2. 已确认的生产者和现有错误边界

### 2.1 Ethernet DMA 是可信reason的来源，payload magic不是

E/mtk_eth_soc.c `mtk_poll_rx():2835`附近在DDONE descriptor取得后，按SoC格式解析原始sport、reason、FOE index。`:2991–3004`把可信RX descriptor的rxd5/rxd4复制到新skb head，并写HNAT magic；`:3006–3015`对FORCE_TO_CPU+WDMA source尝试改投rx_ppdev；`:3099–3104`也会依to_ppd修改skb->dev。**ownership分类必须先于这些重定向，不能等改成虚拟设备后再猜真实来源。**

source port=QDMA TX在`:2882`附近本来就被当作私有PPD回注；这不是普通物理GMAC入站的同义词。NETSYSv3/MT7988特殊slow-path编码在`:3028–3036`把某些0x1e重建为UN_HIT/RATE_REACH、把MODEM_TO_CPU重建成0x15。新解码必须按确切SoC、模式和descriptor位做同样受控规范化；**不能在MT7986上把任意0x00认作keepalive副本**。

现有HNAT head magic由多处软件重建，H/nf_hnat_mtk.h:160只有16位magic判断；H/hnat_nf_hook.c:1340附近、:1490附近、:1650附近的external ingress可自行写magic。普通Ethernet EtherType=0x5678、普通payload恰好有0x6789，均不证明硬件私有来源。对用户普通包，不得根据这些字节改变所有权。

### 2.2 WED RXDMAD → HIF → WARP 的可信链

D/include/hif/mt_hif_pci.h:290–294定义CSRN[15:11]、PPE_ENTRY[30:16]、PPE_VLD[31]。D/embedded/common/hif_pci.c `gen_tuple():1553–1574` 仅在PPE_VLD置位时从descriptor复制entry/reason到 `rxdmad_info`，调用WLAN_HOOK_RX，并标记RX_PPE_VALID；未置位则invalidate NAT magic。调用点`:1716`和`:2222`两条接收分支都按返回的drop_pkt完成释放/补环。

D/warp_proxy.c `client_rxinfo_wrapper():336–343`只复制pkt/entry/csrn；`client_handle():765`先检查client_whnat_en，`:784`才调WARP。W/warp_main.c:1190–1206再进 `wifi_rx_tuple_add()`；W/warp_wifi.c:93–116只把已传来的entry/reason写到skb head。**WARP wrapper没有单独的PPE_valid证明，可信性来自HIF只在合法descriptor valid时调用它，不能把一个后续head magic当成等价证明。**

因此所有权必须在D/HIF中、runtime whnat gate和WARP hook之前解码。即使client_whnat_en=0、HNAT已disable、WARP hook未发布，旧0x15 descriptor仍须消费。`wifi_rx_tuple_add()`是void，不能在那里free之后让调用者继续使用；`mtk_sw_nat_hook_rx()`在WHNAT分支的返回值又被D/rt_profile.c:1842–1846忽略，单改hook返回0不能消费包。正确位置是gen_tuple/接收调用者的drop_pkt合同。

### 2.3 原post处理确实晚于危险的早返回

H/hnat_nf_hook.c:4129起common post先做xlat、alg/magic、headroom、path检查，随后才到`:4254`的0x15；原代码仅在当前FOE仍BIND时`:4274`返回-1，wrapper`:4418/:4432`把非0变成NF_DROP。`:4282–4287`对0x18/0x19的部分出口拒发GMAC，因为硬件已经发送过。

这既证明“gate关闭统一return0”不安全，也说明“查当前FOE是否BIND”不是副本所有权证明：包在DMA/CPU队列等待期间，epoch/表分配可已变更，甚至同index已经成为另一条新流。r3无论BIND/UNBIND/INVALID或表是否存在，可信0x15都按副本处理，**不查询FOE来决定是否再发送**。

## 3. 分类输入与正式skb extension

与BSS policy lease共用正式skb extension的独立域；不再占skb cb、mark、HNAT head保留位，也不保存临时descriptor/驱动指针。建议的最小值字段如下，名称由主方案统一：

| 字段 | 来源与约束 |
| --- | --- |
| `owner` | HOST_DATA / HW_DUP / PRIVATE_CONTINUATION / PARTIAL_MCAST / INVALID_DESCRIPTOR |
| `origin` | NONE / GMAC_DMA / WED_RXDMAD；仅对应真实生产者能设可信值 |
| `raw_reason`, `effective_reason` | 原descriptor及按确切SoC模式规范化的值，后续magic/alg不得改写这份快照 |
| `descriptor_valid`, `native_frame` | 合法DMA完成、适用格式/valid位与是否为可正常上送的原始L2表达；native不能仅靠“看起来像Ethernet”判断 |
| `source_port`, `ppe_id`, `foe_index` | 有界值；index可用于观测，**不自动授权查当前FOE** |
| `producer_incarnation`, `profile_id` | 所属DMA生产者实例及已从启动生效的所有权能力profile；后端remove后不能复用旧实例 |
| `capture_epoch`, `producer_epoch`, `producer_epoch_valid` | capture只记CPU捕获时epoch；producer_epoch须有真正生产世代证明，没有证明时valid=0，不能用capture代填 |

生产者首先在栈上对descriptor分类，确定要消费的副本/私有异常**不需要成功分配extension**，可直接释放/补环。单就ownership而言，正常包分配extension失败仍是正常包，只拒新绑定；BSS policy已要求lease stamp分配失败时拒绝该BSS数据，遵其独立准入合同，不能把这类拒绝伪称为副本消费。

来源字段只在descriptor取得处初始化一次。BSS lease后续写入必须 `skb_ext_add()`取得COW后的域并保留ownership成员；clone共享只读值，另造skb需显式复制两类域。`skb_ext_find()`返回内容不可直接写。原始6.12.112 `net/core/skbuff.c:7088–7134`提供add/COW语义；导出不是GPL-only。此结构不含ownership资源引用，BSS lease若另外持有引用仍由其独立copy/destructor合同管理。

CPU在新epoch读取一个已经完成的旧descriptor，不能把当前epoch填进capture_epoch后称它是新世代包。硬件没携带generation时，真正DMA世代只能来自已停流/排空后重建ring时的生产者/环槽标记；无法证明时为UNKNOWN。已知副本分类不需要这个世代；只是禁止其触碰当前FOE/账户。extension及源头分类应随Ethernet/HIF生产者存活，不能只放在可能先注销的HNAT NF hooks中。

## 4. 全部CPU reason的归属表

数值取H/nf_hnat_mtk.h:170–199。下表以MT7986及上述安全profile为生产目标；NETSYSv3不同编码必须先按E的SoC分支规范化，不能跨芯片套用。

动作约定：**S**=正常数据走软件，禁止/未获许可时不BIND；**C**=消费，不交协议栈/bridge、不发第二次；**Q**=源头已禁该能力，若收到可信硬件实例，则消费本实例并记录backend/profile违规、阻后续加速并进入故障恢复；Q可能损失一份无法安全归属的硬件继续包，必须明确报错，不能谎称它是普通软件回退。**正常未知路径是S，不是Q。**

| reason | 可信来源下的所有权 | READY正常 | 路径拒加速/UNKNOWN、gate关闭、旧epoch、cache失败 |
| --- | --- | --- | --- |
| 0x00 MODEM_TO_CPU | 本SoC正常native CPU数据；若明确MT7988 KA编码则先规范为0x15 | S，不凭reason猜复制 | S；MT7988已确认副本则C |
| 0x02 TTL_0 | 原始异常数据，需正常协议栈TTL/ICMP处理 | S | S，不能当维护副本丢弃 |
| 0x03 HAS_OPTION_HEADER | 原始数据慢路径 | S | S |
| 0x07 NO_FLOW_IS_ASSIGNED | 原始数据慢路径 | S | S |
| 0x08 IPV4_WITH_FRAGMENT | 原始数据/分片 | S | S |
| 0x09 IPV4_HNAPT_DSLITE_WITH_FRAGMENT | 原始受限格式慢路径 | S | S；不进未获准的私有xlat处理 |
| 0x0a IPV4_HNAPT_DSLITE_WITHOUT_TCP_UDP | 原始数据慢路径 | S | S |
| 0x0b IPV6_5T_6RD_WITHOUT_TCP_UDP | 原始数据慢路径 | S | S |
| 0x0c TCP_FIN_SYN_RST | 原始控制位数据；不是副本标识 | S；必要维护仅走独立有效账户 | S；不读旧index查FIN/BIND决定所有权 |
| 0x0d UN_HIT | 原始数据 | S；完整资格可进入新学习 | S，禁止绑定 |
| 0x0e HIT_UNBIND | 原始数据 | S；满足阈值及完整资格可学习 | S，禁止绑定 |
| 0x0f HIT_UNBIND_RATE_REACH | 原始数据 | S；完整资格才学习 | S，禁止绑定 |
| 0x10 HIT_BIND_TCP_FIN | 需要CPU处理的原始数据 | S | S |
| 0x11 HIT_BIND_TTL_1 | 原始异常数据，需协议栈 | S | S |
| 0x12 HIT_BIND_WITH_VLAN_VIOLATION | 原始VLAN异常数据 | S，交正常VLAN/安全政策 | S；不能绕过VLAN政策，也不因拒加速误丢 |
| 0x13 KEEPALIVE_UC_OLD_HDR | 未证明是复制还是承担原发送的替代KA格式；源头不开放 | Q | Q；不可称0x15同义词 |
| 0x14 KEEPALIVE_MC_NEW_HDR | 已改header的KA格式，原始投递职责不完整；源头不开放 | Q | Q；不可按新header正常泛洪 |
| 0x15 KEEPALIVE_DUP_OLD_HDR | 硬件重复维护副本 | C，独立维护可更新账户 | C，**不要求当前FOE BIND或仍存在** |
| 0x16 HIT_BIND_FORCE_TO_CPU | 硬件继续发送/私有回注可能性，不等于正常原始慢路径 | 本profile不开放CPU continuation，Q | Q；不能按旧act_dp/WCID发送，不能猜原入口后路由 |
| 0x17 HIT_BIND_WITH_OPTION_HEADER | 原始异常数据慢路径 | S | S |
| 0x18 MULTICAST_TO_CPU | 部分投递ownership：CPU还可能负责无线/某些端口 | 本profile源头禁PPE multicast，Q | Q；不能统一S或当完整重复无条件C且宣称无损 |
| 0x19 MULTICAST_TO_GMAC_CPU | 硬件GMAC投递+CPU份额，属于部分投递 | 同上，Q | 同上，Q；旧epoch不能查新entry重建未发出口集合 |
| 0x1a HIT_PRE_BIND | 原始数据，尚未完成BIND | S；完整资格才学习 | S，禁止绑定 |
| 0x1b PACKET_SAMPLING | 采样份额，不属于普通应用再投递 | 本profile禁止sampling，Q/C | Q/C；不得再次发送 |
| 0x1c HIT_BIND_EXCEED_MTU | 原始数据异常慢路径 | S，交MTU/ICMP/分片政策 | S |
| 0x1e PKT_FWD_WITHOUT_PPE | 正常native CPU数据，或SoC特定规范化输入 | S；按确切descriptor规范化 | S；规范化证明为副本的除外 |
| 0x01、0x04–06、0x1d、0x1f及未知数值 | reason本身不提供复制/私有身份 | 批准native来源的普通主机数据为S且不绑定；只有其他可信descriptor来源/格式已独立证明是禁止的私有继续包时才Q | 同左；不得把“reason未知”或“有PPE metadata”直接当成复制来误丢普通用户数据 |

信任层次必须保留：无可信descriptor来源的用户包，即使可见head/magic/负载数值看起来是0x15，也仍是HOST_DATA。普通非PPE WED包（PPE_VLD=0）同理，清理/不采纳加速元数据后S。来自批准native输入、仅reason未知的普通数据也是S且不绑定；Q还须有独立的私有来源/格式证明，不能以“有效PPE descriptor但不认识reason”代替该证明。表中的Q不能取代拓扑资格UNKNOWN的正常S。无效/截断DMA descriptor仍沿生产者既有RX错误处置，不凭它构造一份可信正常skb。

0x18与0x19并非“整个包早已发送完”的可靠标志：原post只拒GMAC类出口而保留其他份额已直接证明它们是部分ownership。没有不可变“硬件已发出口集合、尚需CPU出口集合和原始入口/VID”时，不能在epoch改变后用当前index重建。生产从启动禁止其生成是本次可执行最小选择；将来支持需要完整复制域合同，不能仅多加一个return。

## 5. 返回值与释放责任

新内部helper返回**类型化决定** `HOST_CONTINUE / CONSUME_COPY / QUARANTINE_PRIVATE`，不复用NF_ACCEPT整数作全局协议。helper不free，由所属caller执行一次终结动作。

| 位置 | 正常HOST_DATA | 可信重复/禁止的私有实例 | 谁负责释放 |
| --- | --- | --- | --- |
| E/mtk_poll_rx（GRO/PPD重定向前） | 继续既有RX | 若已有skb，free并进入正确skip_rx/补环分支；如在建skb前处理，沿已有descriptor释放/补环分支 | E接收caller，不能随后再napi_gro_receive |
| D/gen_tuple及两个caller | 正常返回原drop位/继续 | 设置drop_pkt=TRUE，由现有`:1734`附近和`:2227`附近RELEASE_NDIS_PACKET_IRQ路径消费；不调用WARP tuple hook | D接收caller，helper不另free |
| H NF pre/bridge pre（防御性检查） | NF_ACCEPT=1；资格拒绝仍NF_ACCEPT | ownership仍证明为非HOST时NF_DROP=0；不手工free | netfilter核心 |
| H common post | 0让wrapper NF_ACCEPT | -1/明确非0让wrapper NF_DROP；在任何资格早返回前判断 | wrapper/netfilter核心；common本身不free |
| H直接RX hook | 不以统一NF整数擅自消费 | 不依赖它承担ownership；WHNAT caller可能忽略返回值 | 必须在源头D/E消费 |
| WARP TX最终绑定hook | 不绑定返回NF_ACCEPT=1，使普通TX继续 | 不承担已知RX副本所有权；若残留非HOST违反约定，应在实际拥有skb的TX caller拒发/释放 | 与WARP caller的DropPkt合同一致，不把1误作丢包 |
| HQOS packet_type callback（如残留注册） | 普通用户0x5678不能被误认硬件，应由移除该私有handler恢复正常协议分发 | 对已证明私有实例，callback自己consume后返回NET_RX_DROP=1，不能只return1而泄漏skb | packet_type callback必须持有并终结其引用 |

推荐源头消耗后，正常协议栈原则上只接收HOST_DATA，NF防御检查不再读当前FOE或仅凭magic重新构造ownership。缺少extension并不授权旧私有快转；若它是普通host数据则继续软件、不绑定。先做ownership，后做路径/gate，最后才允许任何需要活跃后端的查表/账户/commit。

## 6. 生产源头约束：具体设置路径

### 6.1 禁private HQOS，不关闭普通WED

H/hnat_debugfs.c:40的qos_toggle默认为0，但debugfs writer`:3541–3601`可改为1/2，初始化/恢复不能只依赖这个默认值。H/hnat_nf_hook.c:3108–3180、:3501–3508、:3627–3634可写HQOS私有sp_tag/vlan index和fqos；NETSYSv2的Wi-Fi TX也会写fqos。H/hnat_nf_hook.c:4449、:4657与H/hnat.c:1588的ptype随后按私有字段取FOE/发包。

生产profile使private HQOS capability为不可启用：初始化与warm/SER恢复前qos_toggle/qos_ul_toggle/qos_dl_toggle=0；debugfs及其他enable入口返回-EOPNOTSUPP，不发布HQOS ptype，不让其根据用户EtherType进入私有解释。所有开放的FOE提交点强制检查无HQOS magic、无CPU/QDMA私有继续发送、无未批准winfo出口；不需要为每个被限制的可选engine另开发完整加速模式。只清一个qos_toggle但允许API直接写等价FOE不合格。

按r2保留的独立有线BIND，中央最终准入除拓扑NONBRIDGE_VERIFIED外，还应限定批准的**直接GMAC→GMAC用户flow能力**：无bridge/WED移动依赖、无PPD/CPU continuation、无HQOS专用sp_tag/act_dp回注、无采样/多播复制、无未完成的TOPS/crypto/xlat私有ownership。普通802.1Q/802.1AD标记不是HQOS封装，不得一起禁掉。WED本身仍可收发普通软件路径帧，不因为禁PPE flow绑定而关DMA/token/加密/重排。

新旧FOE不共存地启用这项能力：冷启动/重新enable必须先阻提交、清pending/表、可检查地清cache并完成所需DMA/队列排空，再按父生命周期合同READY。运行中不能通过一个enable写入从旧HQOS状态热切换。异常可信QDMA+FORCE_TO_CPU+私有封装到来在源头Q，绝不按payload index取当前FOE，也不剥四字节后虚构普通ingress继续路由。

### 6.2 禁PPE multicast复制与sampling

H/hnat.c:1214启动会设MCAST_TB_EN=1；H/hnat_mcast.c:1400–1416、`:1494`及SER helper可再次开启，不能只设置mcast_hook_toggle=0。生产profile在每个PPE初始化/恢复最后强制清MCAST_TB_EN并readback；跳过mcast_enable/重放组表，不注册可重新启用的mtk_hnat_mcast_offload能力，debugfs/API enable返-EOPNOTSUPP。清空/作废旧multicast FOE/硬件复制表及cache后才READY。

所有提交者（普通post、WARP最终TX、直接API/crypto/xlat）共同拒绝组播/广播用户流、entry的mc/mcast复制位与packet-sampling位；H/hnat_get_filled_unbind_entry():2062–2066已清mc/ps等字段，但需在真正commit处保证其他写者不恢复。H/hnat_mcast.c:138–155可设置iblk2.mcast并dp=0，故仅检查Ethernet DA不够。Linux bridge/MDB与普通WED负责软件广播/组播功能，不关闭正常组播协议。

### 6.3 复用现有维护，不新建worker

H/hnat.c:1184先清KA_CFG/KA计数，`:1192–1202`设置KA_CFG=3和TCP/UDP/NTU计时。本方案保留r2正向证明的独立有线能力，不要求先将该设置改0；probe/warm恢复应固定批准模式，禁止debugfs/API启用尚未证明0x13/0x14职责的替代模式。KA_CFG3的实际输出reason仍须在硬件验收中确认，不能从寄存器数值单独推导所有芯片的格式契约。

**已有维护事实：** H/hnat_nf_hook.c:4255–4261用0x15触发 `mtk_hnat_update_acct_ifindex()` 和 `hnat_get_count()`；H/hnat_debugfs.c:1260–1294读MIB差量，更新接口计数，并在nf_stat_en时更新ct的bytes/packets。`:1251`虽然调用 `nf_conntrack_tcp_established(ct)`，已读HNAT C文件没有显式 `nf_ct_refresh`/超时写入，不能把这个调用名当成“已证明刷新TCP timeout”。原副本还会经过正常netfilter/conntrack，这与单独helper更新计数不是同一件事。

在已检索的HNAT C中，MIB读者是这些数据包路径、debugfs及hnat_api主动查询。H/hnat.c:632–674的delayed worker只是清pending hnat_flow_entry，不是周期MIB/conntrack刷新器；本轮未找到可直接替代副本全部作用的既有周期worker。不据此宣称整个系统绝无外部维护调用。

**最小可审维护入口：** 源头已确定0x15后，在消费前调用父P1-3读者协议保护的可选maintenance callback。回调不取得包所有权、不能forward或留存skb；缺hook、gate不READY、资源代际不一致或不能证明DMA世代时立即skip，caller仍C。它不是重新运行普通RX hook、更不是把副本交协议栈：

1. 在任何FOE解引用之前核验callback/resources存活、可信producer实例、真正有效的producer epoch与当前backend/resource epoch相同。**CPU capture_epoch相同不足够。** 若旧descriptor的真实epoch未知，skip维护；不得为了做统计而按旧index访问新资源。
2. trylock entry_lock（忙则skip），在锁内再次核验READY/epoch、index范围及当前表分配，随后按实际旧header有界解析出的完整tuple与当前slot的已提交owner记录比对：协议族/协议、双向地址与端口、netns/zone、域与原始ingress等必要信息。身份不完整、封装不支持、tuple/owner不符均skip。H/hnat_flow_entry_match():268的比较只接收两个FOE形态且不含整个owner域，不能直接拿它当完整验证；可复用匹配字段逻辑。
3. 只有验证通过才沿既有MIB/helper做best-effort维护；仅以硬件实际MIB差量累计，不能把这份旧副本的字节数直接记到新owner。slot复用前后账户重置仍由commit/生命周期合同负责。
4. 既有 `hnat_get_count()` 包含设备ndo统计callback及ct引用操作，不能未经锁图检查整段塞入entry_lock。最小调整是锁内核验/读取MIB并复制immutable账户/tuple、取得必要引用，锁外执行可能回调的计数更新；不让FOE/账户裸指针逃出锁。H/read_mib():1111是atomic poll，最长10ms，失败skip，不循环重试、不影响消费。
5. DSCP/mcast timestamp/任意API修改FOE同样需要世代与owner证明；本profile不开放mcast/HQOS，直接跳过相关维护，不为了兼容私有可选模式重造功能。

此维护合同不会让“不确定的副本再发送”；它只决定能否对**当前已验证账户**做附带维护。正常READY可保留现有可证明的计数能力，过渡/失败时允许漏一次维护并消费，这是与重复发送风险不同的取舍。

**尚未证明的能力边界：** 直接在源头消费会绕过原正常conntrack数据包处理；仅更新ct计数不自动等价于TCP/UDP timeout维护。长期独立有线BIND验收必须确认实际保留的维护机制，不能以本报告冒充通过。若必须新增独立维护worker或改变conntrack策略，先作为额外范围/产品代价交主线程与Astra裁决；不把它塞进P1-4必做。关闭全部BIND/KA、保留软件和普通WED只能列为明确备选，**不是已选默认或隐含结论**。

## 7. gate、epoch、SER与旧descriptor的跨窗口合同

所需父生命周期接口只有：`backend_state`、单调`backend_epoch`、`producer_incarnation/profile_id`、提交许可，以及DMA停流/排空完成证明。不要求把长期SER阶段计入短RTNL nesting。

- **gate关闭/UNKNOWN/策略拒绝：** normal HOST_DATA仍S；0x15仍C；Q类仍Q。复制分类不访问backend表，不因cache清理失败跳过。
- **旧epoch：** descriptor所有权固定；0x15不查新表、不调基于当前index的DSCP/mcast timestamp/账户callback。native原始数据可交当前软件网络政策；未知包的旧epoch只拒绑定，不凭它将普通包归为复制。
- **SER/缓存失败：** ownership在E/HIF的固定解码继续有效，即使HNAT hooks撤销；不触碰不可用寄存器/已释放表。profile恢复失败不READY，不注册private消费者，不“先开DMA再补禁private配置”。硬件未停/旧cache未清不能发布新的可依赖拓扑，此项由父物理屏障阻断。
- **旧descriptor延迟：** 能确定是复制则消费，与epoch无关。可信private异常到来Q并保持/转FAILED，说明源头profile或排空证明失败。不能在开始新epoch后拿该index的当前BIND状态猜它是否已发送。
- **module退出：** descriptor分类和终结动作属于仍持有RX ring的生产者，不依赖已注销HNAT对象。源头profile含禁private设置须在物理排空前保持；不可先撤消费者、让硬件私有继续包裸奔进协议栈。

对于Q类，本报告不承诺恢复单个旧私有包的全部合法投递份额。生产正确性来自其**从启动不生成**、enable/SER不恢复该能力，以及不能证明排空时拒绝进入READY；故障注入时丢弃无法安全归属的硬件实例是显式错误动作。普通用户HOST_DATA在资格不符时继续软件的承诺仍完整成立。

## 8. 实施面与关闭P1-4的验收

实施面：E/mtk_eth_soc.c在descriptor→skb/PPD/GRO之前的分类；D/hif_pci.c gen_tuple及两caller的drop ownership；W/warp_wifi.c/proxy只接收继承证明、不再根据head建立信任；共享正式skb extension及COW/copy；H所有NF入口/common post在资格分支之前区分ownership；HQOS ptype/生产者与各最终FOE提交者；hnat.c init/warm/SER的KA/MCAST安全profile；debugfs/API能力限制。按r2既定独立有线BIND能力纳入上文的最小维护核验和验收，不为被禁用的可选模式新增功能；维护闭环缺口交主线程/Astra决定，不能以文案悄悄改为全部关闭。此设计不修改任何固定设备/地址规则，不加入生产诊断开关。

不可跳过的验证：

1. 全32个reason和SoC规范化的C表驱动测试；普通负载/私有EtherType伪装无可信metadata时保持HOST_DATA，不误丢，不获取加速许可；无PPE_valid普通WED包照常软件上送。
2. 合法独立有线BIND运行时注入0x15，分别置gateclosed、UNKNOWN、alg、metadata重建、headroom不足、旧epoch、FOE已INVALID/复用/表已撤销，均**零次CPU再发送**，且不触碰新表。人为损坏index也不影响固定副本消费。
3. 0x18/0x19不能“直接accept后再发GMAC”；源头寄存器readback、commit拒绝与无multicast重放证明新profile不产生它们。注入可信旧实例时Q且可计数，所有正常软件广播/组播双向业务仍工作。
4. 所有HQOS生产/消费入口在profile下不可启用；用户普通0x5678帧不造成FOE查表或旧act_dp/WCID xmit；注入真正QDMA/私有回注descriptor时不泄漏封装给bridge/协议栈，不双free、leak或沿旧出口发。
5. disable/enable、SER、cache失败、probe unwind、HNAT hooks已撤销但旧RX完成到达；source分类与释放不依赖hnat_priv/wifi表的存活，DMA补环/统计正确，未排空不新READY。
6. clone/COW/新skb创建不污染BSS lease与ownership独立字段；extension分配失败不让普通数据被当副本，副本消费也不依赖分配成功。
7. r2保留的合法独立有线长TCP、UDP连续流，其conntrack/超时/统计必须在实际保留的维护机制下正确；未闭环时明确报告该能力的放行阻断，由主线程/Astra决定补足范围或采用明确降级，不能静默关闭全部BIND。不能只验证“没有重复包”却遗漏连接老化回归。

P1-4的关闭依据是这张所有权表、源头profile及返回/终结合同，而不是一次清表、一个epoch或“桥flow不会BIND”的推断。实际源码实现与硬件验收仍待完成。

## 9. 追加核查：keepalive 与 conntrack 寿命（未选能力，范围裁剪后定案）

**本节优先于本文沿用 r2 的“保留独立有线 BIND”表述。** 当前 [r3 主稿](roaming-bridge-final-design-r3.md) 已按固定全软件转发收敛，不开放独立有线 PPE BIND；因此下述 CT 维护补充仅记录未选能力的研究，不是本版待实现功能，也不是本版放行依赖。副本始终不得重新发送的所有权结论保留。不得将未选能力的 CT owner、刷新 helper 或回收扩展混入当前实现。

### 9.1 本轮取得的直接源码证据

除本文 H/E/F 引用外，新读 raw v6.12.112 refs 位于当前 worktree `.analysis/linux-6.12.112-refs`：`include/net/netfilter/nf_conntrack.h`、`nf_conntrack_core.h`，以及 `net/netfilter/nf_conntrack_proto_tcp.c`、`nf_conntrack_proto_udp.c`。F 指 `sources/linux-6.12.112-bridge-review-postimage/net/netfilter`，其标准 flowtable 代码已叠加本产品补丁。

| 问题 | 已核实的代码与结论 |
| --- | --- |
| 原副本如何维护 timeout | TCP C `:1321–1379` 取该 CT 的 timeout policy 或所在 netns 默认策略，并在协议状态/校验处理后调用 `nf_ct_refresh_acct()`；UDP C `:96–127` 同样取 policy，按 SEEN_REPLY、`stream_ts` 等状态选择超时，再调用该 API。原 0x15 经过正常 conntrack 的这段处理，不能由 HNAT 计数更新自动替代。 |
| HNAT helper 是否已有等价刷新 | H/hnat_debugfs.c `:1184–1258` 根据当前 FOE/zone 查 CT、累加 bytes/packets，`:1251` 调用 `nf_conntrack_tcp_established(ct)` 且丢弃返回值；其中没有显式 `nf_ct_refresh*`。该状态检查也用于 TCP C `:1234/:1245` 的条件判断；本轮未取得其定义，不能据调用名声称它刷新 timeout。 |
| 可否复用现有无计数接口 | raw `nf_conntrack.h:211–229` 明确定义 `nf_ct_refresh(ct, skb, extra_jiffies)`，调用 `__nf_ct_refresh_acct(..., do_acct=false)`；因此未来需要额外刷新时，不必把副本重新送网或为刷新再算一次包数。该 header 是接口和意图证据，**不是 core 函数体已核验的证据**。 |
| HNAT 是否已经拥有 OFFLOAD 状态 | 检索本轮 H 的 C/H 及本产品 HNAT 补丁，未找到 HNAT 设置 `IPS_OFFLOAD`/`IPS_HW_OFFLOAD`。F/nft_flow_offload.c `:662` 由真实 flowtable 准入设置 IPS_OFFLOAD；F/nf_flow_table_core.c `:329/:378` 分别调用 `nf_ct_offload_timeout()`、teardown 清状态。不能只借这个 bit 或 timeout helper，省去真实 flowtable owner、活动维护及 teardown。 |
| `nf_ct_offload_timeout()` 实际做什么 | raw `nf_conntrack.h:333–340` 在剩余寿命小于半天时写入 `nfct_time_stamp + NF_CT_DAY`。这是明确的延长写入；孤立使用会改变普通 CT 的老化行为，不能当本版“几乎无代价”的兼容修补。CT core 的 GC 对 OFFLOAD 状态的完整处理本轮未读到，不作已核验声称。 |
| 标准 flowtable 有没有现成定时维护 | F/nf_flow_table_core.c `:598–634` 已有 1 Hz GC；F/nf_flow_table_offload.c `:1035–1056/:1160–1173` 按 backend stats 的 lastused 推进 flow timeout，必要时更新 CT 计数。但 HNAT 没有因此自动成为该 flowtable backend；E/mtk_eth_soc.c `:7671–7694` 在 HNAT 配置存在时还排除了 Linux PPE 初始化/绑定。不能声称这个既有 worker 已替 HNAT 刷新寿命。 |
| HNAT 的既有 worker 能否证明已维护 CT | H/hnat.c `:632–669` 的 worker 每秒调度，清理超过 30 秒未用的 pending flow；它不读 MIB、不刷新 CT。现有 HNAT MIB 读者为 keepalive/包路径、debugfs/API。H/hnat.c `:1243–1245` 开启 MIB_READ_CLEAR，因此其他读者会取走硬件差量；未来活动判断必须统一累计，不能只看某次 KA 读出的差量是否非零。 |

来源局限：`nf_conntrack_core.c` 的父线程下载请求遇到自动审批超时，本轮未取得函数体；`nf_conntrack_tcp_established` 的精确定义也未取得。不得写成“已证明 core 尊重 FIXED_TIMEOUT/并发状态”，或把常见内核实现记忆当作本产品源码证据。已读 TCP/UDP C 与公开 header 足以否定“现有 HNAT 计数 helper 已经证明等价刷新”的说法，但不足以给未经实现的维护路径背书。

### 9.2 若未来单独恢复有线 BIND，有限改动的实现合同

有可执行的事件驱动方向，无需默认关闭所有 PPE 或新建维护 worker：**可信 0x15 在源头消费前，调用受后端 reader lease 保护的 CT 维护 helper；最后始终消费，不把维护成功与否转换成发包许可。** 这属于未来有线加速能力，须作为独立改动审核，当前不实施。

1. 在既有计划中的 entry owner 元数据中增加精确 `nf_conn` 引用及方向，随最终 checked commit、slot generation 和资源 epoch 一起发布；引用在 BIND 可见之前建立，旧 owner 在复用前撤销。仅保存裸指针、FOE index、同一个五元组或 `nf_ct_get_id()`，都不足以证明同一个连接实例。新 owner 取自当次正常已确认 skb 的 CT，不根据旧副本创建新 CT。
2. 维护先证明 producer/resource 世代有效，再在 entry_lock 下核验当前 owner、完整原始/转换 tuple、netns/zone、实际活动累计及 slot generation，取得该 CT 的短期引用后解锁。未知/旧 epoch、owner 不符、MIB 失败或锁忙只跳过此次维护，仍消费副本；绝不按旧 index 把一个新 CT 当作旧包的 owner。
3. CT 寿命更新与 `nf_stat_en` 统计开关分开。复用 `nf_ct_refresh()` 的无计数接口，依该 CT 的有效 policy 选择超时；TCP 仅处理仍允许加速的成熟状态，不能把 FIN/CLOSE/已过期或 DYING 状态延长成 ESTABLISHED；UDP 不能仅因有硬件包就伪造 SEEN_REPLY/ASSURED。FIXED_TIMEOUT、自定义短 timeout、helper/NAT clash 等不满足合同的连接逐流留在软件。不能一律写固定秒数、设置 IPS_OFFLOAD，或调用可能创建新 CT 的 `nf_conntrack_in()`。实现前补齐 core 函数体并验证锁与 fixed-timeout 语义，这是该未来提交的具体源码门槛。
4. 所有 MIB 读者复用同一受锁累计；刷新以当前 owner 尚未用于续期的真实活动累计为依据，避免 debugfs/API 的 READ_CLEAR 读走差量导致误判，也不能让空闲合成事件永久续命。既有 1 Hz teardown worker 可以附带回收已失效 owner 引用并检查维护期限；**不增加新 worker，不让该 worker 扫描 MIB 来创造新维护子系统**。其每轮预算、BIND 失效/cache 确认、`cancel_delayed_work_sync()`、reset/error及 per-net 退出时的引用排空须明确；CT 引用不能被假定等同于 namespace 生命周期引用。
5. 若维护不能在该连接的剩余 timeout 前完成，既有 worker 在留足余量时撤销这条 BIND、确认 cache 失效并交回软件；不得让 CT 已到期而硬件绑定继续冒充有效连接。自定义 timeout 小于已证明 KA/回收延迟预算的，仅拒这条 flow 的加速。副本丢弃本身不依赖此期限或 FOE 状态，硬件撤销失败沿既有 FAILED 合同报错，不能伪称已安全转回软件。

这套未来合同的实际代价是 entry owner 中的 CT 引用/少量活动账本、一个不取得 skb 所有权的维护 helper，以及对既有回收/namespace 退出的补充。它不是零成本地把原 `hnat_get_count()` 挪到 RX，也不是一个已存在的隐藏维护机制。所需验收是连续有线 TCP/UDP 超过普通 CT timeout 仍保持同一有效 CT、停止数据后按策略老化、计数无重复、原始/回复方向与 NAT/zone 正确，以及同 tuple 重建、FOE 复用、KA 丢失、计数读取竞争、SER/旧 descriptor 和 cache 失败均不续错连接或再次发送副本。

当前 r3 不开放 BIND，故无需实现上述扩展；保留源码事实与有限恢复路径，避免将未选的加速能力变成漫游根本修复的新增范围。
