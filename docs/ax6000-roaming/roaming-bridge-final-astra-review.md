# 通用桥转发 r2：Astra 独立关键设计审查

2026-11-09。审查对象为 `roaming-bridge-final-design.md` r2 及 RX、policy、offload 三份详细设计。本轮仅只读核验并新增本报告；未修改实现、构建、联网、操作路由器、提交、推送或启动子代理。

**裁决：当前 r2 为 NO-GO，尚不能作为“设计审核通过”进入整套实施。** 全量普通 AP 交桥、接收时授权、桥依赖流恒定走软件的方向成立；以下四项 P1 是需要先补齐的控制流/生命周期合同，均有收敛路径，并非该方向不可实现。修订四项后可重新裁决，无需重做固定双设备候选、等待旧关联根因全部查明或先上真机试验。P2 是随实现完成的明确要求，P3 是说明修正。

本裁决只讨论是否进入实施，不授权刷机或生产推广。通过设计复审后仍须实现审查、CI 和经用户授权的真机验收；性能尚未通过。沿用用户通知 CI 完成、不持续查询的约定。

## 1. 证据范围与引用约定

下文 `前缀/文件:行` 均指以下本地准确基线，行号为本轮读取的 postimage，非 `.orig`、`partial-*` 或另一版本源码。

| 前缀 | 绝对路径 |
| --- | --- |
| R | `/home/illya/workspace/router/ax6000/research` |
| D | `/home/illya/workspace/router/ax6000/sources/vendor-mt79xx-7.6.7.3/review-baseline/mt_wifi` |
| K | `/home/illya/workspace/router/ax6000/sources/linux-6.12.112-bridge-review-postimage` |
| V | `/home/illya/workspace/router/ax6000/sources/linux-6.12.112-vlan-review-postimage` |
| H | `/home/illya/workspace/router/ax6000/sources/hnat-6.12.112-review-baseline/drivers/net/ethernet/mediatek/mtk_hnat` |
| W | `/home/illya/workspace/router/ax6000/sources/vendor-warp-20250408-5f71ec/source/warp` |
| N | `/home/illya/workspace/router/ax6000/sources/netifd-cbb83a185740/openwrt-netifd-cbb83a1` |
| F | `/home/illya/workspace/router/ax6000/sources/immortalwrt-mt798x` |

实际核对了固件 HEAD `c196312d3916fad77fa0bb47db57e4b685c15f03` 和 clean 工作树；使用给定的 42-patch 驱动、14-patch HNAT、49-patch 选定内核文件与 5-patch VLAN 文件准备结果。读取了 PATCHES.json；未重新解包验 SHA、重放补丁或做完整内核 prepare。内核选定文件准备含既有补丁 fuzz，不能称全部 fuzz=0 或完整编译通过。WARP 使用实际外部源，并检查仓库 patches 涉及范围，未用驱动 bundled WARP 替代。

`policy:3` 的 VLAN 源码补充有效，后文“尚需取回”已过期。r1 不属于本轮审查；历史可行性/根因文档中的双 MAC 建议不构成约束。

已证实的阻断机制保持成立：`R/evidence/roam-k-off-20261108-1735/failure-summary.json:9`、`:15` 两条连接都有 wan SYN、无匹配 wan SYNACK，驱动仍走旧同 wdev 条目；`recovery-summary.json:5`、`:8`、`:9` 对应老化后 lookup-miss 与实际 wan 回包。与 `D/embedded/ap/ap_data.c:4079`、`:4090` 的控制流相符。这里没有把提交成功当 RF 交付，也不声称旧关联未及时清除的全部原因已知。

## 2. P1：进入实施前必须补齐

### P1-1：配置失败“移出桥 + device_release”会让 external AP 恢复旧内转

**设计位置：** `R/bridge-universal-policy-design.md:69`、`:148`；主设计 `:43`；RX 设计 `:137`。设计要求失败保持 BSS down/blocked，但给出的 netifd 失败落点并不提供这个保证。

**直接源码：**

- `F/package/network/config/wifi-scripts/files/lib/netifd/wireless-device.uc:55` 将无线接口以 `external: 2` 管理；`:71` 的 `device_set()` 与 `:75` 的 link 操作没有在这里构成就绪确认。
- `N/bridge.c:648` 的 addif 失败仅更新成员状态并 `device_release()`；`:326` 的正常移除先 delif，再 release。
- `N/device.c:175` 对 external 设备明确不执行 `system_if_down()`；`:760` 在仍有其他 claimant 时连 set_state(false) 都不做。
- `wireless-device.uc:456` 遍历 handle_link 后，`:460` 无条件将无线状态设为 up。

**反例：** AP 已运行，新的无线策略事件使 hairpin/isolated 写后读回失败。按详细设计移出 bridge，release external AP 后无线接收仍可继续。此时实际 `IFF_BRIDGE_PORT` 为假；按“确定 standalone 才 legacy”的 RX 分类，它会再次落入旧 DA-STA 内转。启动 addif 失败也可发生同类问题。VLAN upper 被移除时，其 AP base 更不会因 child release 自动关停。单纯“不 mark_up”也不能停止已经在运行的 BSS。

**必须修正：** 在设计中明确一次端到端 activation/failure 握手，包含“准备/阻流 → 配置 bridge 与所有实际 VLAN port → 读回确认 → 放开普通数据”，以及活动端口失败的逆序。失败时必须先确认 AP 普通数据已阻止，再解除成员关系；不要把 release 成功当关停成功。

可采用内部 BSS/网络域的准备/失败状态，或经过确认的同步 BSS 停流机制；它是控制面状态，不是终端身份 ctx，也不是用户开关。若使用 down，要证明在 driver 中真正阻止 RX/legacy 与队列出队，并处理 down 失败。对同一 AP 的 standalone/tagged bridge 混合域，定义故障时保守停整个 BSS还是只阻相应域。kernel/driver 对尚未就绪或配置失败的流量不得因“当前已不在桥中”自动恢复 legacy。明确 ucode/C 回传失败以及 already-up、VLAN child、多个 claimant 的处理。

**关闭条件：** 修订给出实际阻流入口、完成确认、错误传播及恢复条件；实现验收注入 addif/hairpin/isolated/down 失败，持续发包证明没有恢复旧内转、没有带错误隔离的端口继续转发。

### P1-2：SER 跨 RTNL 释放窗口不能复用全局“嵌套事务深度”

**设计位置：** 主设计 `:83`、`:85`、`:89`；offload `:131`、`:143`。普通同步拓扑事务的 RTNL 嵌套可行，但 SER 不能只写“同样重置 epoch/pending”而省略恢复窗口协议。

**直接源码：**

- `F/target/linux/mediatek/patches-6.12/999-eth-93-mtk_eth_soc-add-internal-SER-notify-event.patch:293` 发 STOP_TRAFFIC/START_RESET，`:301` 释放 RTNL 等 Wi-Fi 完成，`:317` 重取；`:347` 还存在另一释放窗口。
- 同补丁 `:328` 的 FE reset complete、`:337`/`:341` 的恢复通知在 `:352` 的 NAT_DONE 之前。
- `H/hnat_nf_hook.c:515` 当前只在 NAT_DONE 调 `hnat_warm_init()`；`H/hnat.c:1553` 开始重设表地址/清表，`:1569` 初始化硬件，`:1580` 恢复 GMAC PPE forwarding。

上述是生产补丁中直接可见的顺序；本地没有整个 mtk_eth_soc.c 的准确 postimage，因此不把更外层调用/硬件停流效果宣称为已完整证明。

**反例：** 若从 START_RESET 到 NAT_DONE 持有一个 core begin 的全局 nesting depth，释放 RTNL 后另一个合法 br_add_if 会被误当作“内层事务”，可能跳过 prepare/清缓存而发布拓扑。若只在 NAT_DONE begin，则先前恢复阶段没有设计出的 backend unavailable 屏障，不能凭最后一次换 epoch证明整个 reset 期间的提交、控制面操作和寄存器访问都受保护。

**必须修正：** 分开两类状态：

1. **短同步 topology transaction**：整个 begin/end/abort 生命周期持有 RTNL；全局 nesting depth 只允许这一类，不可跨 unlock。明确 ASSERT_RTNL、失败 unwind 和内层错误传递。
2. **跨窗口 backend lifecycle 状态**：在 reset/start-stop 开始时，RTNL 下将 HNAT 标记 RESETTING，推进 token并禁止该后端提交/快消费，然后结束这次短事务；跨 unlock 保留不可用状态，绝不保留 nesting depth。其他 link 若需要该后端完成硬件屏障，返回明确 busy/error，不得因为“暂时没有可用回调”而按无后端成功。
3. NAT_DONE 通过新的短事务、entry commit 屏障、pending 清理和可检查的 cache/hardware 初始化后才恢复 READY。重复/缺失事件、失败、卸载、重新注册均有确定状态；resume 事件顺序若必须调整，要把实际 Ethernet SER caller 列入实施面。

**关闭条件：** 增补最小状态表及上述 caller 的准确顺序核验。不得拿 epoch 当阻止硬件运行的物理屏障。无需建立每 MAC 状态，也无需开放桥加速来解决此项。

### P1-3：后端注销缺少 CPU hook、工作项及硬件 DMA 的三重退出合同

**设计位置：** 主设计 `:81`、`:83`、`:89`；offload `:137`、`:143`。RTNL 保护 core 回调列表，只能解决该列表的生命周期。不能由“RX 不遍历 backend callbacks”推导 vendor RX/TX 不再访问 HNAT。

**直接源码与反例：**

- `W/warp_wifi.c:157` 检查裸 `ra_sw_nat_hook_tx`，`:164` 再次读取并调用；`W/warp_main.c:1184` 的外部 TX 入口未在该层提供 RCU/read-side 协议。`D/os/linux/rt_profile.c:1823`、`:1839` 也有 RX hook 消费。`H/hnat.c:1508`、`:1509` 清指针，`:1511` 注销 NF hooks，随后即扫描表。并发 disable 可落在“检查非空”和“再次调用”之间；已进入的回调也可能继续访问表。不能只给控制回调 unregister 加 RTNL 就认为排空完成。
- `H/hnat.c:669` 仅 `cancel_delayed_work()`；handler 在 `:658` 无条件重新调度。`H/hnat_nf_hook.c:631` 用 delayed_work_pending 决定是否在 `:643` 同步取消，不能覆盖已经 running、pending 位已清的工作项。`hnat_remove():1873` 后继续走 `hnat_stop()` 释放资源。
- `H/hnat.c:1390` 忽略 `ppe_busy_wait()` 错误；该函数 `:1350` 明确可以 timeout。随后 `:1421` 释放 coherent FOE 表。cache/CPU 门不能代替“硬件不再 DMA 旧表”的证明。

HNAT 在 `hnat.c:35`、`:37` 导出 hook 变量，常规模块依赖可能阻止仍有 WARP 引用时 rmmod；本报告**不据此断言普通 rmmod 必然产生 UAF**。但 hook disable/enable、平台 remove、异步 work 和资源释放仍需合同，模块引用也不能保证设备私有资源仍有效。

**必须修正：**

1. 列出所有外部 hook 的 reader/writer，采用一次取指针、明确 read-side 和注销排空的协议；或证明并实施先停所有调用者。若选 RCU，修改实际 WARP/mt_wifi 调用端，不能只改 HNAT；这不要求修改 WED DMA/crypto 基础功能。持 RTNL调用 synchronize_net 本身可行，但只有纳入该 read-side 的读者才被覆盖。
2. stopping 状态阻止 work/timer重新排队，可靠同步取消后再释放表/列表；不得只检查 pending 位。为需 RTNL 的 worker使用先关闭入口、再在合适锁外同步取消的两阶段退出，不在 RTNL 内等待反向取 RTNL 的工作。
3. 明确 prepare/stop/cache 失败状态的归属。失败后不能仅注销 backend使下一次 link以“无后端”放行。只有确认硬件停止消费/DMA旧表后才能销毁资源和撤销该阻断。若产品不能支持运行中安全 remove，需在允许发起 remove 的入口禁用该能力并规定重启恢复，不能让 void remove 返回后留下已释放表；仅返回一个被忽略的 errno 也不解决。
4. 注册时先建立 state/锁/worker，再清表与检查 cache，再发布外部 hooks/READY；失败按逆序撤销所有已发布入口。错误状态不能由一次 enable 写入覆盖。

**关闭条件：** 在设计中给出带锁与上下文的启停序列，补足 WARP 调用端和实际 remove/enable入口的实施面；安全退出无法证明的操作明确拒绝。实现阶段再用并发 disable、probe失败、正在运行的 work、DMA busy timeout 验证，不能把以上合同全留到实现复审才决定。

### P1-4：拒加速的早返回会跳过硬件重复副本的消费规则

**设计位置：** 主设计 `:67`；offload `:74`、`:78`、`:85`、`:126`。普通用户包的 pre=NF_ACCEPT、common post=0、WARP TX=1 是正确返回值，但不能把它们直接推广为所有 PPE reason 的统一早返回。

**直接源码：** `H/hnat_nf_hook.c:4254` 处理 `HIT_BIND_KEEPALIVE_DUP_OLD_HDR`，`:4274` 在仍 BIND 时返回 -1，由 wrapper `:4418`、`:4432` 消费/丢弃该副本；`:4282` 的 multicast-to-CPU 同样有“硬件已经发过，不再发往 GMAC”的逻辑。这些位于计划新增的 common post 早返回之后。HQOS 入口 `:4657` 处理的也是带私有封装/ownership 的包，不等价于普通 Ethernet 慢路径包。

**反例：** 已获准的独立有线 BIND 流产生 keepalive duplicate；拓扑 begin 阻绑定/清表期间，一份副本进入 CPU，路径被判 gate closed或UNKNOWN。按新规则 common post直接返回0，wrapper放行，原本应消费的重复数据可能再次发出。cache失败后旧硬件项可能继续工作，因此不能靠“bridge 从不 BIND”排除这个反例。这里不要求已有 bridge BIND，也不依赖一个被禁止的配置。

**必须修正：** 在任何资格拒绝/magic 重建/alg清理之前定义有界的 packet ownership 分类：可信硬件生成的重复/维护副本、带私有封装的回注包、正常用户数据分别处理。拒缓存必须让正常用户包继续软件；重复副本应安全消费，不能再送一次。分类只使用合法 descriptor/metadata，不将普通负载伪装的 magic视为可信，也不能在 epoch 变化后按重用的 FOE index访问新条目来决定旧副本的命运。HQOS若无法安全恢复正常包，需从源头禁止该类私有路径，或先落实完整的停流/排空协议，不可按旧 WCID继续发。

**关闭条件：** 补一张 reason→ownership→拒加速动作/返回值表，明确普通慢路径与维护副本的顺序；故障注入覆盖 begin/cache失败/旧 descriptor 延迟到达，检查无重复、无私有封装漏给协议栈、无用户包被当副本误丢。

## 3. 已认可的关键设计与 P2 实现要求

### 3.1 RX、接收授权和 EAPOL：方向 GO；扩大解析落点，不能只修函数参数

`D/embedded/common/cmm_data.c:3608` 的共同回调位于现有 VLAN策略之后；`D/embedded/ap/ap_data.c:4057` 是 Size捷径，`:4068` 是 group复制，`:4079` 是 DA查表。在这些分支之前选择普通桥路径可以同时消除同BSS内转、跨BSS旧目的隔离拒绝及group重复，不需要pair ctx或FDB查询。

**接收时授权语义可用于最终通用版。** 以实际 RX peer完成普通数据授权后，已接收的 BA/Linux队列数据作为在途数据延后是可接受的模型。无需为“deauth撤回所有已接收包”设计新站点 generation；仍须保持现有删除/队列生命周期，VLAN/角色改变应执行定义好的停流/队列处理。此认可不等于未授权聚合可借用一个EAPOL例外。

P2：本轮补实了输入格式，不需要再联网取材料才能开始实现 parser。`D/mac/mt_fmac.c:2891` 将普通ToDS头转换帧的DA/SA放在Ethernet偏移0/6；`D/embedded/common/cmm_data.c:7405` 在 +12 分类Ethertype，普通EAPOL在 `:7437`/`:7441` 被重建802.11帧再走控制接收。因此 `rx_802_3_data_frm_announce():6754` 的FALSE不仅是“真EAPOL可能漏认”，还可能让异常普通Ethernet内容误走握手例外。`D/embedded/security/cmm_wpa.c:1506` 跳14后又在 `:1516` 查88:8e，证明直接改TRUE不成立。

规范helper应返回格式、控制/普通/非法分类及有界payload视图，覆盖 `rx_packet_process():7405` 的前置分类、VLAN剥除/rebuild路径和两个announce入口，不能只替换末端一个bool。前置读取发生在新helper之前时，后面的有界性不能追溯保护它。802.11的QoS/可选头长度也不能继续以 `:7454` 固定偏移代替事实。保留 `RTMPCheckWPAframe():1547` 等既有状态机副作用的正确调用位置，纯解析不执行计时器/握手操作。

未授权 SW A-MSDU/Ralink聚合应整体拒绝，或逐MSDU解封后仅将合法控制子帧送控制面；不得按第一子帧放行整个聚合。HW拆分按每个交付MSDU检查。测试包含目的地址字节伪装88:8e、短Ethernet/SNAP/EAPOL、截断VLAN、不同EAPOL长度、混合普通/控制子帧。没有证明现场存在利用，不写成已证实漏洞。

P2：`to_os=TRUE` 不是抵达桥的证明。`D/os/linux/rt_profile.c:1822` 的FAST_NAT、`:1984` 的WIFI_PKT_FWD仍在后续链；普通bridge/kernel-only类别须明确绕开不满足合同的私有转发，保留必要的descriptor正常化及标准 `eth_type_trans()`/接收接口。`D/os/linux/rt_linux.c:2667` 与 `:2731` 分别给出协议赋值及最终Linux入口。不能为绕旁路重复调用eth_type_trans、漏赋值或双交付。

### 3.2 TX：本 wdev查找与最后提交边界可行，不批准仅改入队

实际全局查找在 `D/embedded/ap/ap_data.c:342`、`:580`；`D/embedded/mgmt/mgmt_entrytb.c:568` 的MacTableLookup2可复用。`D/embedded/common/qm.c:1544` 取得packet wdev后，`:1550` 确实按TR重定向，r2要求移除此行为是必要的。fp_qm `:434`、fp_fair_qm `:306` 也需进入共同核验。

P2：选择“同原桥出口重新查本wdev的DA”或“拒绝本无线副本”其中一种明确行为；不得仅替换wdev而继续沿旧WCID/密钥发送。最终校验必须处在还知道Ethernet DA/所选域的位置，并保证之后没有另一次WCID/wdev改写；ACK优化 `D/embedded/common/cmm_data.c:7685`、广播BMC、聚合、PS队列均纳入调用图。tag提前剥除时保存值型VLAN域，不凭“已无tag”跳过校验。认可正常在途语义，不宣称一次多字段READ_ONCE检查能撤销已提交硬件帧。

### 3.3 VLAN、hairpin、隔离与 netns：接口能力足够；必须固定生命周期规则

`V/net/8021q/vlan_core.c:18` 先在当前设备找VLAN，`:26` 对DOWN VLAN丢弃；`K/net/core/dev.c:5705` 的VLAN demux先于`:5716`的RX handler。policy的“metadata优先、逐层直接VLAN、无匹配后再判bridge、VID0特例”方向正确。`vlan_dev_real_dev():102`确实折叠多层，因此入口即时父设备检查必须使用直接real_dev，不能误用折叠结果。未知master/kernel-only不能回legacy。

`K/net/bridge/br_forward.c:91`、`:115`提供真实in/out；`NF_BR_FORWARD`映射AP/VLAN来源能保留同BSS NoForwarding与不同BSS BTNBSSID。不同adapter按原策略边界处理合理。隔离扩展到group已公开，不能再写“逐包保持旧group行为”。普通NF导出及注销后grace路线成立；`K/net/netfilter/core.c:529`无同步排空，`K/net/core/dev.c:11455`允许RTNL下expedited grace。

P2：netns_local应当是**netdev整个生命周期的不可撤销约束**，不随wireless_bridge_forwarding true→false或AP→STA清除。`K/include/linux/netdevice.h:2437`是独立bitfield；`V/net/8021q/vlan_dev.c:606`只计算features，不能靠给features OR旧NETIF_F_NETNS_LOCAL实现，也不需要假设普通features重算会清这个bit。

公共 `V/net/8021q/vlan.c:149` 在`:158`/`:180`之前检查namespace并继承约束的落点成立。最简单的角色合同：所有将来可能承担受限AP角色的adapter netdev在首次注册前就固定namespace/标志；派生VLAN递归继承且不解除。若允许既有未受限netdev转AP，必须在激活前验证现存所有VLAN后代均同ns并补齐约束；发现已跨ns upper应拒绝角色转换，不能仅约束将来新建的upper。当前cfg80211/GO未启用不代表未来共享ops可自动获得能力。建议明确拒绝未经核验的原地角色转换，避免无依据扩大范围。

P2：netifd继承需有源值/有效值区分，false与移除也传播，覆盖先建VLAN再收到wireless通知；同时落实P1-1的失败顺序。NoForwarding、BTNBSSID、BR_ISOLATED不能互相替代。验证双层VLAN、base与upper各自接不同bridge、未匹配VID、VID0、metadata+in-frame及DOWN upper。MT7986硬件tag/padding实际表达仍待实现和真机验证，不能从硬件宏推定已正确。

动态perSTA VLAN可维持未支持状态并显式拒绝启用；若交付，则入口捕获域值、BA/子帧继承、TX域验证、每VID组播/GTK闭环缺一不可。这不影响标准静态VLAN属于本次必要能力。

### 3.4 HNAT与NF flowtable：拒桥依赖缓存的策略成立

对桥客户端routed WAN也拒双向缓存是必要的；禁止范围不能仅是L2_BRIDGE枚举、某个MAC或WED出口。无法证明移动先被Linux观察时，FDB notifier和SP_CMP_EN均不足以开放bridge BIND。

P2：实施前给NONBRIDGE_VERIFIED一份短而正向的能力定义：两端完整可识别的独立有线路径，无bridge/WED/未知RX handler/未支持虚拟层；解析前逻辑设备事实不丢失，缺任何方向证据为UNKNOWN。不要把“hw_path最终是GMAC”“skb_iif非零”或接口名称当资格。无法证明的独立路径也走软件即可，不为保留加速捏造证明；同时不预先保证本机多数WAN仍加速。

`H/hnat_nf_hook.c:3301`、`:3660`、`:2449`的FOE提交和`:4125`的xlat直接memcpy必须统一门；pending在`:3270`存、`:3375`复制后释放，epoch须随值拷贝并在最终entry_lock内核验。保持flow_entry_lock与entry_lock不嵌套的明确顺序，读token早于路径证明、提交时验证同一稳定token；仅在提交时临时读取“当前epoch”会给旧route冒充新世代。

`K/net/netfilter/nft_flow_offload.c:336`确是int，`:448`传播任一方向失败；不采用raw stable void fallback推论。`K/net/netfilter/nf_flow_table_core.c:306`的add是共同发布点，`:534`的lookup、`:340`的refresh必须配套。新flow保存证明时token，过渡/不匹配时本包慢路径及teardown；NEIGH也不能免检。保留r2对未验证generic硬件provider/BPF消费者不开放的决定，不用异步GC冒充硬件撤销屏障。

P2：提交清单补 `H/hnat_debugfs.c:3808` 的static_entry写入口：`:4013`能直接写包含调用者info_blk1的FOE，未经过上述共同helper。生产安全模式应拒绝无路径证明的静态BIND/原始cache写操作，保留只读诊断；这是内置维护接口的合同，不要求防御任意root篡改内核。HNAT API现有更新/删除也检查是否可能保留或恢复旧BIND，不能仅搜索字面“= BIND”。

### 3.5 core事务、cache失败：同步主体可实现，错误必须贯穿完整调用链

`K/net/bridge/br_if.c:630`注册RX handler、`:634`置bridge flag，早于`:636`upper link；r2外层begin位置正确。`K/net/core/dev.c:8096`分配失败无POST，`:8116` rollback无配对POST；unlink `:8192`忽略通知错误，因此改真实caller而不只加notifier是正确选择。

可收敛的同步顺序为：持RTNL发布奇数token → 排空通过旧门的CPU读者 → 对每后端过entry_lock提交屏障 → 分别在flow_entry_lock清pending、entry_lock清FOE并做DMA可见性屏障 → 可检查cache清理 → 发布关系 → 最外层发布新偶数token。不得持这些spinlock等待grace/worker；callbacks注册和注销在同一RTNL序列，failed/RESETTING后端状态遵P1-2/3。软件旧flow由epoch拒消费即可，正常GC异步回收。

link失败可保持原拓扑且禁止新BIND；不谎称旧cache已全CPU。unlink的“受移除桥域flow原本没有BIND”证明在严格正向资格与P1失败处理下成立，只能用于这种删除，不可推广到任意获准虚拟路径、设备remove或硬件reset。VLAN自己的demux发布在 `V/net/8021q/vlan.c:194`、删除在`:111`，不等同于upper邻接表更新时间；若今后允许这类虚拟路径加速，要扩展外层事务到这些实际发布点。当前将其归UNKNOWN可避免不必要扩大首版加速能力。

P2：cache失败不能只修 `H/hnat.c:1015` 的idle timeout。该清理还调用`:1033`的 `__hnat_write_cache_line()`，其`:762`的CAH_REQ timeout同样仅打印；`:1064`的cache enable调用者也吞掉结果。必须逐层返回错误、停止后续成功发布/自动恢复，涵盖cold start、warm init、enable、topology prepare及stop；idle成功不代表后续所有动作成功。公开helper若保留旧ABI，也要有真实返回状态的内部事务API，并明确不允许旧void入口越过其错误闩。硬件invalidate完成条件需以准确寄存器语义或验证证据确认，不能以写寄存器返回作为完成证据。

FOE和pending扫描均按实际分配的 `foe_etry_num`；本轮确认 `H/hnat.c:623`、`:642` 的现有pending清理也使用DEF_ETRY_NUM/4，不仅旧MAC删除存在这个问题。清理它们属于新生命周期实现的一部分。

## 4. P3：文档收口

- 以主设计无在线实验开关为准，删RX设计`:135`仍保留的“必要时legacy开关”建议，避免实施者引入未审维护切换。
- policy文首更新应合并到正文，删`:109`等过期“尚需取得VLAN源码”句。P模块API可用的结论维持，不再把缺源码当阻断。
- 引用最终postimage行号：VLAN demux是 `K/net/core/dev.c:5705`，namespace拒移动是`:11693`；旧raw行号只可明确标注为raw。
- “全量”指所有满足普通三地址AP/已支持网络域合同的终端，不表示新支持全部WDS/A4/GO、动态STA VLAN、任意namespace和虚拟堆叠。当前设计这一边界合理；实现须显式拒绝未支持角色激活，不能按编译MAP宏排除普通fronthaul。

## 5. 关闭 P1 后仍不可跳过的审查与验收

1. **设计复审材料：** P1-1的阻流/就绪握手；P1-2的短RTNL事务与跨窗口状态表；P1-3的hook读者、worker、DMA退出锁图；P1-4的reason/ownership表。每项明确caller、错误和恢复出口。无需再提出固定pair试验。
2. **实现审查：** 精确生产postimage补丁；普通AP数据没有legacy逃逸；源授权与控制流分离；group只交付一次；最终TX的DA/TR/wdev/VLAN不再被改写；NF hook映射/退出无裸生命周期；VLAN/netns/能力false传播完整；每个BIND与软件flow消费者都受同一资格/token约束。普通数据拒加速不变成误丢包。
3. **真实helper与并发测试：** 直接调用实际实现，覆盖恶意/短EAP封装、混合聚合、VLAN demux、同MAC多VID、错误wdev/WCID、双向UNKNOWN；遍历begin/end/abort失败点，旧pending跨epoch、SER释放RTNL期间link、外部AP配置失败、running work退出、cache与DMA busy超时。不能只测补丁字符串或复制实现算法到测试里。
4. **CI：** 按真实MT7986宏与生产锁定feeds完整构建driver/kernel/netifd/ucode；确认builtin/module依赖、专有模块符号许可、WARP外部版本、RRM/WNM/FT及既有生产guard；不混入043诊断，不改uBoot/分区布局。设计认可不能代替这些结果。
5. **真机，需另获刷机授权：** 任意终端同/跨BSS、跨频段、AX→外部AP→返回，旧STA故意仍在；冷/热IPv4/IPv6 TCP/UDP、ARP/ND/非IP L2/group、BA/SW/HW聚合；mcast_to_ucast=0的同AP可达、所有隔离组合、静态VLAN/VID0/stacked upper、动态重载及P1故障。将匹配请求/回包、源学习、实际出口与业务数据交付关联起来，不以to_os或submit_rc验收。
6. **性能独立验收：** 桥客户端LAN/WAN吞吐、CPU、延迟、重配置停顿；另测确实获准的独立有线路径PPE。没有实测不填损失百分比，也不把正确性通过当性能通过。用户通知CI完成后再继续处理构建产物，不持续查询CI、不自动刷机。

本轮未覆盖完整Ethernet SER postimage、硬件cache/DMA完成契约、MT7986真实VLAN输入表示、所有可选宏构建及真机性能。以上明确区分为待核设计证据或后续实现/硬件验收，不以“在已读文件中未找到”推断机制不存在。核心转发故障证据不因这些缺口撤回。
