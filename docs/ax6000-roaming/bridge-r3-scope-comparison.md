# 通用桥修复的范围复核：固定软件转发候选

2026-11-09。有限的只读范围复核，未实施、联网、操作路由器或启动子代理。已读主设计 r2、Astra r2审查、四份r3合同及准确HNAT/MT7986 Ethernet/mt_wifi postimage、实际外部WARP源码。本文只提出一个供选择的具体候选，不替换已选主方案，不把“更小”当成已经验收。

**判断：r3全部HNAT/SER框架不是修正旧STA截流的无条件前提。** 桥RX/TX、授权、隔离、VLAN、hairpin与激活失败阻流直接服务于根本修复；其中不能因范围顾虑而省略。全局拓扑epoch、双向加速路径证明、backend动态注册、pending世代、SER reset-id/多参与者ACK和副本账户owner，主要来自另一个要求：**继续允许一部分独立有线路径BIND，并支持它们在运行中拓扑变化/SER后重新放行。** 放弃这项能力，可以大幅缩减相应框架。

但仅在最后加一行“禁止BIND”，不能合法删掉其余合同：原HNAT仍注册裸hooks、启动work/timer、拥有DMA表并提供debugfs/API。必须把产品真正收敛到一个从启动固定、不能热恢复旧加速的能力集合。

## 1. 唯一比较候选

**候选C：本AX6000镜像所有连接使用Linux慢路径，WED保留基础收发；不提供PPE/NF flow cache，也不提供运行中FE/PPE重建。** 是镜像级固定能力，无MAC/IP/BSSID/端口名单、无用户热切换开关。

- 所有PPE BIND一律不可建立，包含纯有线/NAT/WAN、Wi-Fi、桥、crypto、xlat、静态项；所有NF软件/硬件flowtable同样不建立、不消费。所有普通协议栈功能继续使用原软件路径。
- 保留WED/WDMA ring、token、正常TX/RX、无线加密和已支持的重排序。**不把`whnat_en=0`当成等价实现，也不删除WARP包**；WARP配方硬依赖`kmod-mediatek_hnat`，实际WED RX还带PPE metadata，不能未经证明宣称直接去掉HNAT/PPE即可保留全部WED能力。
- 保留兼容WED所需、有效地址的**常驻慢路径FOE表**，不绑定、不启动软件学习/维护者；PPE是否仍参与非命中包的传递由现有硬件链决定。表不在运行中释放、不换所有权，不能用“没有BIND”推导“硬件不会DMA”。
- FE/PPE自动SER、手动重置、破坏性HNAT disable/unload/unbind在实际入口拒绝；遇到需FE/PPE恢复的故障明确要求重启。普通BSS down/up、netifd桥重配置不因此禁用；仅影响WED自身、且不重建FE/PPE的既有恢复不在这里擅自删除。

最后一项是候选C为了显著缩范围而付出的**明确维护能力代价**。如果用户要求保留在线FE/PPE SER，就不能声称本候选还能原样省去所有SER顺序修订；至少要保留“重建慢路径profile先于DMA/流量恢复、错误不放行”的较小合同。本文不再展开第二个候选。

## 2. 和主方案的可比较差异

| 项目 | 主方案r2+r3 | 候选C |
| --- | --- | --- |
| 普通AP漫游根治 | Linux桥决定出口，TX服从所选wdev | 相同，不退回旧STA直转 |
| 桥客户端LAN/WAN | 已选软件路径 | 相同 |
| 可验证独立有线NAT/路由 | 保留BIND，需资格/生命周期/维护证明 | 也走软件；失去这一部分PPE吞吐/CPU收益 |
| NF flowtable | 保留正向证明路径，add/lookup/refresh带epoch | 镜像级全部拒建立/消费；不解析拓扑资格、不存epoch |
| 拓扑变化 | core begin/end/abort+backend屏障 | 没有获准的缓存可变成不安全；不需要为缓存新增通用upper事务 |
| HNAT运行时消费者 | RCU packetops、SRCU控制ops、registry/pending/work | 根本不发布资源型ops、不启动维护队列；不需要为不存在的消费者建RCU/SRCU框架 |
| FE/PPE在线恢复 | 新reset-id、ACK、RESETTING跨窗口、checked warm/commit | 拒绝发起；故障重启，不进入这些跨RTNL窗口 |
| DMA退出/初始化失败 | 完整首版常驻/错误保留合同 | **仍需最小常驻/错误保留合同**，不能省略 |
| 副本/私有包 | 源头所有权+可选账户/tuple/世代维护 | 源头终结仍保留；不需MIB/conntrack维护callback及owner世代 |
| 产品代价 | 代码/验证面较大，保留更多维护/性能能力 | 所有路由CPU成本、故障恢复需重启；不能填未测百分比 |

本机多数有线设备也接`br-lan`，它们在主方案里本来就属于桥依赖，已经退软件。因此两方案在**当前典型拓扑**的性能差异可能小于“全部关闭PPE”字面上看起来的幅度；只有真正符合主方案NONBRIDGE_VERIFIED的路径才是候选C新增损失。没有测量和实际资格清单，不能保证差异小或给吞吐数值。

## 3. 最小可执行源码落点

沿用其它报告代号：H为14patch HNAT postimage，E为82patch四文件Ethernet postimage，D为42patch mt_wifi，W为实际external WARP，K为49patch选定bridge/netfilter postimage。以下均是应实现的修改边界，不是已打补丁。

### 3.1 HNAT改为固定慢路径profile，而非保留完整旧服务后“最后拒BIND”

在`H/hnat.c:1593 hnat_probe`的产品分支只完成慢路径硬件必需初始化，**不调用**`hnat_enable_hook`、NF注册、netdevice/netevent注册、邻居work、roam polling、pending teardown、HQOS ptype、硬件mcast和可写debugfs。`hnat.c:1818–1848`现在集中调用这些入口，适合形成清楚的产品分支，避免对未启动对象做stop/cancel。

保留包依赖/头文件与必要普通导出，资源型API在入口、任何`hnat_priv/CFG_PPE_NUM`解引用之前返回不支持。`hnat_get_foe_entry`不返回表指针；所有direct API、crypto bind、generic update、static entry、cache/HQOS/enable写入没有可达放行路径。读诊断仅提供有界快照，不顺带开放原可写维护服务。外部可选provider不能直接写export槽后启用。这样不必为主方案中每种合法BIND建立proof/owner/epoch。

本轮重新检索实际WARP的全部C/H：只有`warp_wifi.c:130,157,164`消费`ra_sw_nat_hook_tx`；**未发现WARP直接解引用`hnat_priv`或调用HNAT table/API函数**。D全部C/H也未发现`hnat_priv`、`hnat_get_foe_entry`、crypto bind、MIB/API直接调用；它使用上份生命周期报告列出的条件裸槽位。这个结论限于所检查源码和实际配方，仍需用最终模块undefined-symbol表核验，不能延伸为任意外部二进制保证。WARP包依赖保留不等于这些资源消费者必须发布。

`H/hnat.c:1132 hnat_hw_init`当前显式设SMA=BUILD_ENTRY、SCAN、KA、MCAST、cache和MIB；候选不能照原样调用后称“软件模式”。固定profile要求：从第一次允许流量起`SMA_ONLY_FWD_CPU`、KA=0、SCAN关闭、硬件mcast/private HQOS关闭、cache不启，且没有任何定时器重新打开学习；空FOE地址/大小有效，CPU默认出口按本SoC真实表达配置。`H/hnat.h:1190–1191`已区分“Only Forward to CPU”和“build new FOE entry”，`hnat.c:1515`现有disable使用前者，但这只是具体实现起点，**不是所有WED路径完成硬件验收的证明**。

所有PPE表先完整分配/清零再暴露地址；无需pending/MIB/accounting分配，前提是对应硬件功能也不启动。原`hnat_start():1260`先写BASE再清零且MIB可晚失败，仍须调整；这部分是最小初始化正确性，不能因无BIND保留原危险unwind。

### 3.2 真正取消裸资源消费者，不为它们建立通用dispatcher

H导出RX/TX/delete/dev-register槽位从始至终保持NULL；不执行旧enable赋非NULL。D/W产品分支取消这些HNAT调用/写槽操作，W `wifi_tx_tuple_add():123`本来就是HNAT metadata/最终bind回调，固定软件profile可直接不执行该tuple绑定部分，普通WED TX本身继续。RX tuple只作来源metadata，不赋予FOE访问许可。

D `rt_profile.c:1823`原NULL分支会继续到正常`RtmpOsPktProtocolAssign`/Linux接收；桥普通路径仍须正确跳过WIFI_PKT_FWD等私有旁路、且只做一次protocol赋值。不能仅删hook调用而遗漏后续Linux入口。D APCLI cover保存/恢复全局RX hook的产品分支去掉；普通AP功能不受影响。

本候选不选择“保留裸函数指针不停切换但赌不会并发”。它选择**所有实际资源型读者从未接入、所有发布者从未启动**。已加载消费者的模块依赖可维持普通代码寿命；DMA资源另按下节常驻，二者不混同。若保留任何资源型callback，候选就必须补相应lease/drain，不能拿全局禁BIND免掉它。

### 3.3 软件flowtable同样固定关闭

不能删除`kmod-nft-offload`来表示本能力限制：firewall4依赖它，target已有NF_FLOW_TABLE=y。保留包/普通防火墙，修改固定profile下的缓存准入。

`K/net/netfilter/nf_flow_table_core.c:306 flow_offload_add`在发布任一tuple、修改ct timeout或排硬件work之前固定返回不支持；lookup固定不给出fast命中、refresh不重排offload。策略从本次冷启动起确定，因此不需要拓扑token/旧表世代。

实际`nft_flow_offload.c:685`失败路径释放flow并清`IPS_OFFLOAD_BIT`，`xt_FLOWOFFLOAD.c:793`也有失败清理并继续协议栈；仍应在nft/xt前端更早跳过尝试，避免每包做route分配以及修改TCP liberal等offload准备副作用。不能只把UCI两个offloading字段写0：用户以后重建规则仍须受固定能力限制。可保留普通fw4规则与软件防火墙，不必禁用整个nft。

E保持`CONFIG_NET_MEDIATEK_HNAT`构建路径，避免去掉HNAT后反而启动另一套generic PPE（`mtk_open():4878`和probe`:7671`的条件编译）。另对`ndo_setup_tc`的flow/FT硬件offload请求明确返回不支持，防不同provider绕过；普通非offload qdisc不因此一概禁止。当前E有`.ndo_setup_tc=mtk_eth_setup_tc`（`:6822`），该函数全部准确postimage本轮未读到，实现前只需补这一个函数的直接核验，不需要发明新backend框架。任意root加载完全不同数据面不属于内置接口支持合同。

### 3.4 冷启动和实际重建/销毁入口

- E `mtk_open():4886–4896`在本产品固定用已有`MTK_GDMA_TO_PDMA`；不按`offload_version`重新指PPE。WED是否仍通过PPE慢路径由前述有效表/profile保障，不凭GMAC已走PDMA就宣称WED问题消失。
- H继续采用首版常驻模块无module_exit、H和builtin Ethernet禁止sysfs解绑、产品运行时OF_OVERLAY关闭、force-unload实际为n；probe在DMA暴露后失败保留绑定/资源并报FAILED。**这些是保留了硬件可见表的必要维护边界，候选无法省掉。** 不再有表消费者/worker，不需要完整RCU/SRCU退出框架。
- E `mtk_pending_work():6176`在`mtk_prepare_for_reset`、通知Wi-Fi、任何rtnl_unlock和实际reset之前拒绝在线FE/PPE恢复，记录明确故障/要求重启。多个触发点都最终到该worker（`mtk_eth_reset.c:76`及E多个schedule点），所以共同入口有实际效果；manual触发也不能越过。
- 对`mtk_eth_reset.c:71`接收的`MTK_FE_START_RESET_INIT`也明确拒绝，不能另排一个会触发FE reset的路径。**本轮D全树未找到发送该INIT事件的调用者**：实际D是`hw_ctrl_basic.c:400`收到FE START后才置`notify_fe=1`并启动SER，随后`hw_ctrl.c:1223–1229`才进入FE completion等待。因此本目标只要E共同worker在发任何START之前拒绝、D的FE START处理也在置notify/发CmdSER之前拒绝，就不会因本产品主动造成“先STOP再等永不来的DONE”；不需要凭空新增D请求接口/ACK协议。其它未核实provider若以后发INIT，须接受不支持，不能默默获准。
- 最小profile readiness/error可以是E既有owner上的固定产品字段，H初始化通过普通导出setter最后标就绪，E open/BSS activation检查未就绪则不启数据；不需要core backend列表、拓扑epoch或短txn。若要在原HNAT在线恢复中重新置就绪，就超出此候选边界。

E冷初始化已有`mtk_hw_init(false)`→`mtk_hw_reset`（`:5700`后、`:5318`）复位FE/PPE，支持从本镜像冷启动建立空profile；仍要核实际复位完成/旧cache不沿用，不能由软件表清零代替硬件完成。候选不支持从旧固件运行态热切到该profile，也不宣称kexec/异常复位已验收。

## 4. Astra四P1如何缩减而不忽略

| 审查项 | 候选C仍需 | 可删除的新增框架 |
| --- | --- | --- |
| P1-1 activation失败回legacy | 原r3 BSS/域准备、阻流、确认/失败顺序完整保留 | 无；与PPE是否绑定无关 |
| P1-2 SER跨RTNL | 在线FE/PPE重建在发起端及共同worker都实际拒绝；无假成功通知/无限等待 | 不进入重建窗口，故不需新SER reset-id、多参与者最终ACK、长RESETTING/core短txn套接 |
| P1-3 hooks/work/DMA | 不发布所有资源消费者、不启动这些work；常驻资源、禁销毁入口、暴露后失败不free | 通用activeops RCU/SRCU、registry重建、running worker排空与允许热重绑的状态机 |
| P1-4 duplicate/private ownership | E descriptor、D HIF源头消费可信副本；禁KA/mcast/HQOS产生者；普通native包继续软件 | 不存在合法BIND账户，无需MIB/conntrack刷新callback、slot owner/tuple比对和producer世代维护 |

可信旧0x15副本即使在本产品“不应生成”也应源头消费，私有异常不作为普通Ethernet放行。无需为了消费已确定副本而先分配skb extension；正常报文的BSS policy lease仍按P1-1独立需要处理。所有普通源授权、EAPOL格式/聚合检查、TX最终wdev/VLAN约束、hairpin/隔离与静态VLAN合同都保留。

这张缩减后的producer合同可以很小：E在`mtk_poll_rx`取得合法DDONE/SoC descriptor后、`hnat` head重建和PPD重定向之前解码；D在`hif_pci.c:1553 gen_tuple`确认为PPE_VLD后、WARP hook之前解码，通过两个caller既有`drop_pkt`路径释放/补环，不能在void WARP回调中free后继续使用。可信0x15固定消费；可信0x13/14、0x16、0x18/19、0x1b或真实QDMA/HQOS私有实例视为禁止profile异常，消费并限速告警/标记故障，不查FOE决定出口。其余native原始数据（TTL、fragment、UN_HIT、UNBIND、MTU等）继续软件；未知reason不自动等于私有包。PPE_VLD=0以及仅负载/magic伪装的用户帧不能被当副本丢弃。既有RX descriptor错误/drop位保留。

从源头禁sampling/HQOS/mcast并且根本没有BIND账户后，这里不需要把producer epoch、capture epoch、tuple/owner、MIB引用带到后续NF hooks；也无需为了0x15做conntrack刷新。与BSS lease共享skb extension的账户字段可删除，BSS自己的生存期字段不能混删。SMA/UNBIND表与WED native出口需在真机验证，不能凭0x15分类helper就称整个硬件profile通过。

## 5. 本轮结论、仍需证明与选择建议

候选C是**可以进入一个范围更小的设计复审的执行路径**，不是已经证明可直接运行的寄存器开关。最短的额外证据只有：①上述固定慢路径profile在MT7986 WED基础RX/TX正常；②cold init后无BIND/private producer；③所有FE重建发起方能明确拒绝而不先停后等；④TC provider实际入口已覆盖。任一项不成立都应停下修正，不能称“关硬件加速已经解决”。这些缺口不要求先实现r3整个框架。

**零BIND是否必然要求重写ACK：否。** 为缓存正确性新增的reset-id/多参与者最终N9 ACK不再是固定CPU profile的天然依赖；本候选明确拒绝在线FE恢复，所以完全不进入该协议。若希望保留最小SER，至少还要证明既有stop/reset物理合同、在首个`mtk_open`/Wi-Fi恢复前同步恢复固定profile、任何错误不发成功DONE并可靠解除失败等待；是否须改现有ACK取决于这项恢复能力的实际缺口，不能从“零BIND”直接推定现有无限等待/匿名计数已正确。主r3的整套ACK框架不是唯一答案，但本文不以未经核验的最小SER替代上述明确候选。

如果“固件改动尽量小、先完成桥根治”优先于“保留独立有线PPE及在线FE自动恢复”，建议把候选C交Astra作替代设计评审，并向用户明确上述性能与维护代价后选择。若用户要保留这两类能力，则r3相应生命周期/错误处理大部分确是必要依赖；不能一面保留动态加速，一面删除其正确性合同。

不建议将所有r3工作称为“顺便加固必须全做”，也不建议回到两MAC临时实验或只改`to_os`。本轮仅提供候选比较，主方案未变，未实施任何能力关闭。
