# 通用桥转发：配置、隔离与接口生命周期设计

主线程后续核验补充（2026-11-09）：`vlan.c`、`vlan_dev.c`、私有 `vlan.h` 连同 `vlan_core.c/if_vlan.h` 已按实际5个相关kernel patch准备于 `sources/linux-6.12.112-vlan-review-postimage`，`PATCHES.json`记录。公共 `register_vlan_dev()` 在 `vlan_vid_add/register_netdevice` 前有统一添加 namespace 检查/继承的落点，覆盖 netlink 与 ioctl。`netns_local` 是独立 bitfield，普通 features 重算不实现也不会清除该约束。私有 `vlan_find_dev()`直接查本设备的 vlan_info；使用 deep helper 时必须限定已验证的 AP/VLAN 链，不能扩大到任意 master。

2026-11-09。只读源码设计输入，未修改实现、运行构建或操作路由器。本轮只新增本文。用户已否决固定两 MAC 的诊断候选；下述选择不依据 IP、MAC、BSSID 或接口名称。

## 推荐结论

对**普通 AP 客户端、且接入 Linux bridge 的 AP netdev**，统一采用 bridge-owned 转发：完成原无线接收安全与 VLAN 处理后，只上送 Linux，不再按目的 STA 表选择本地空口。单播、广播和组播应采用一致的唯一转发所有权，避免同一帧由驱动内转及桥 hairpin 各发一次。非 bridge 的独立 AP 保留原拓扑行为；否则同 AP 的 ARP 广播没有 Linux bridge 回送，不能仅因为接口 mode 是 AP 就停内部转发。

生产行为可直接随驱动版本启用，不需要新增 UCI 开关、DAT 键、iwpriv hook 或在线实验参数。所需用户态变化是**内部能力通知**，保证 bridge hairpin 不再依赖是否开启组播转单播；它不是让用户手工开启修复。

隔离须按 Linux bridge 已选出的实际 ingress/egress AP netdev 执行，不能继续查目的 STA 来推断目的 BSS。建议 mt_wifi 自注册 `NFPROTO_BRIDGE/NF_BR_FORWARD` hook，按当前注册 AP wdev 映射保留 `NoForwarding` 和 `NoForwardingBTNBSSID` 策略。普通允许流量的转发与安全策略由不同步骤处理：前者交桥，后者不因交桥被忽略。

其中有一项明确行为修正：旧内转组播分支不检查这两个隔离标志。主线程已选择新策略把隔离应用于 AP↔AP 的所有 L2 帧，包括组播/广播；这符合客户端隔离目的，但不能描述为逐包保持旧组播行为。默认未隔离网络不受此差异影响。

## 核验的代码基线

- 固件：`sources/immortalwrt-mt798x`，生产 `25.12`，`c196312d3916fad77fa0bb47db57e4b685c15f03`。
- MTK：`sources/vendor-mt79xx-7.6.7.3/review-baseline/mt_wifi`，已施加当前生产 42 个补丁。
- netifd：`sources/netifd-cbb83a185740/openwrt-netifd-cbb83a1`，与配方 `package/network/config/netifd/Makefile:8` 的 commit `cbb83a1857407a28a63dc09412a1f209195914ef` 一致；未发现该包本地 patches 目录。
- Linux：初步基于 `sources/linux-6.12.112-roam-refs` stable 6.12.112 子集，最终又核验 `sources/linux-6.12.112-bridge-review-postimage` 中选定文件的实际补丁后源码。父线程按 generic→platform 顺序应用 49 个相关补丁，5 个现有上游补丁使用 GNU patch fuzz；`PATCHES.json` 留存记录，不是完整内核编译或全部 fuzz=0 的证明。以下 NF 导出/同步结论均已在该 postimage 再核实。

## 1. 现有配置真实语义

### UCI isolate 与 bridge_isolate 是两套设置

`package/mtk/applications/mtwifi-cfg-ucode/files/usr/share/ucode/mtwifi/converter.uc:311` 把 `isolate` 写为每 BSS 的 `NoForwarding`。其默认位于 `.../schema/mtwifi/dat-defs.json:26`，值 0。驱动 `embedded/common/cmm_profile.c:8049` 读取分号列表到 `MBSSID[].IsolateInterStaTraffic`。

该选项**不是“停止内部转发但交桥”的开关**。`embedded/ap/ap_data.c:4084` 的同 wdev 单播分支先设 `to_os=0,to_air=1`，`:4093` 隔离只把 to_air 清零，最终变为 `0/0` 丢包。

另一个 UCI `bridge_isolate` 由 `package/network/config/wifi-scripts/files/lib/netifd/wireless-device.uc:55–69` 传给 `device_set()` 的 `isolate`；netifd `system-linux.c:1002` 把它写到桥端口 `isolated`。该字段与 `wireless_isolate` 分开：后者来源于 UCI `isolate`，控制 hairpin。

### NoForwardingBTNBSSID 不能被静默丢弃

驱动 `cmm_profile.c:8065` 读取全局 `NoForwardingBTNBSSID` 到 `ApCfg.IsolateInterStaTrafficBTNBSSID`；`ap_data.c:4095–4106` 的不同 wdev 目的单播因此丢弃。同 wdev 在该全局标志下仍可通信，除非本 BSS 自己隔离。

当前 MTK ucode converter/schema 没有为此 DAT 选项提供 UCI 映射；不能由“当前 UCI 没这个字段”推定任意已有 profile 均为 0。无需为本次修复新增 UI，但必须尊重驱动实际读取的值。

Linux `BR_ISOLATED` 不能等价替代 BTNBSSID：`net/bridge/br_private.h:897` 在源和目的端口均 isolated 时拒绝，**包括同一个 hairpin 端口**。而 BTNBSSID 的原单播语义是同 BSS 可通、不同 BSS 禁止。把所有 AP 端口一律设 isolated 会额外阻断本来允许的同 BSS 通信。

### 旧组播行为差异必须公开

`ap_data.c:4068–4076` 的广播/组播内转没有检查 `NoForwarding` 或 BTNBSSID。直接把两标志迁到全帧 bridge hook 会收紧原先可能泄漏的组播。本设计推荐保留这一收紧，明确作为隔离行为修正验收：网关 local-in、来自有线服务器的 DHCP/ARP 广播仍可正常工作；不能把“客户端互相隔离”做成“连网关也不可达”。

## 2. hairpin：必须补齐的通用配置链

准确 netifd `system-linux.c:940–958`：

1. `mcast_to_ucast` 初值为 `dev->wireless_ap`。
2. 接口显式 `multicast_to_unicast` 或 bridge 级关闭设置可覆盖该值。
3. `hairpin = mcast_to_ucast || dev->wireless_proxyarp`。
4. `dev->wireless_isolate` 为真再强制 hairpin 为 false。
5. `system_bridge_addif():982` 在 bridge 端口加入时执行此设置。

所以当前实机 hairpin=1 **不证明通用方案无需改 netifd**：用户关闭 bridge 组播转单播且未开 proxy_arp 后，hairpin 会为 0。普通同 AP 单播如果全部交桥，将不能再发回同一无线端口。

建议新增一个内部布尔能力，例如 `wireless_bridge_forwarding`，修改链为：

1. MTK `.../mtwifi/netifd.uc:40 set_vif()` 的通知数据增加 `bridge_forwarding`；`.../lib/netifd/wireless/mtwifi.sh:235–239` 按 **AP 模式及实际驱动 bridge-owned 能力**报告，而非按 ra/rax 名称。STA/APCLI 不报告为真。
2. 共用 `wireless-device.uc:495–500` 已通过 `...data` 保存任意通知字段。`:46 handle_link()` 在调用 `device_set()` 时增加 `wireless_bridge_forwarding: ap && !!data.bridge_forwarding`。
3. netifd C 的 `device.h:278–281` 增加布尔状态；`ucode.c:98–108` 的 `uc_netifd_device_set()` 接收该能力。
4. `system_bridge_set_wireless()` 改为 `hairpin = dev->wireless_bridge_forwarding || mcast_to_ucast || dev->wireless_proxyarp`，最后仍让 `wireless_isolate` 强制 false。不强迫开启 multicast_to_unicast，也不改变其它驱动的缺省行为。

必须每次报告明确 true/false，防止 netifd `device` 对象在角色或驱动改变后残留旧 true。配置路径、hotplug 和重建 VIF 都使用同一通知链；不使用一次性的 sysfs shell 命令，避免下次 bridge 重建又被 netifd 覆盖。

Linux `br_forward.c:21–30 should_deliver()` 在 hairpin、端口转发状态、VLAN egress、switchdev 与 isolated 共同通过时才发包；不会因 hairpin=1 自动绕过其它桥策略。`:267–269` 的组播转单播另有防止 hairpin 回送同一源地址、破坏 IPv6 DAD 的处理。

现有 sysfs setter 返回 void 且忽略 `write_file()` 错误（netifd `system-linux.c:427–433/:551–554`）。桥模式将 hairpin 变成同 AP 可用性的必要条件，至少应对新能力端口读回 `brport/hairpin_mode`：与计算值不符则返回明确配置失败，不能记录 bridge 启用成功。最小改法是在 `system_bridge_addif()` 配置后通过现有 `system_get_dev_sysfs()` 读回；失败时撤销新加入成员并交由 `bridge_enable_member():625–630` 的现有失败路径处理。不要让“写命令发出”充当生效证据。

## 3. 用真实桥出口实施 AP 隔离

### hook 选择与匹配规则

Linux `br_forward.c:73–117 __br_forward()` 在确定真实 `to->dev`、完成 bridge VLAN 处理后，对非 local-origin 帧调用 `NF_BR_FORWARD`，传入真实 `indev` 与 `skb->dev`。本机生成/路由后重新输出桥的包走 `NF_BR_LOCAL_OUT`，不会误被该 hook 当作直接 AP↔AP 互访。不能只在驱动 TX 用 `skb_iif` 判定：路由包也可能保留原 iif。

推荐每 adapter 一项 `nf_hook_ops`：

- `.pf = NFPROTO_BRIDGE`，`.hooknum = NF_BR_FORWARD`，`.priority = NF_BR_PRI_FIRST`，`.priv = pAd`。
- 不更改 skb、mark、cb、MAC header，不 clone、不发包、不释放包；只返回 `NF_ACCEPT` 或 `NF_DROP`，由 netfilter 处理 ownership。
- 在 `WdevListLock` 下遍历当前已注册的 `pAd->wdev_list`。只接受 `wdev_type == WDEV_TYPE_AP`、`if_dev == state->in/out`，以及有界 func_idx 且 `wdev == &MBSSID[func_idx].wdev` 的映射。只复制 BSS index/隔离布尔值用于本次决定，不把 wdev 指针带出锁。
- in/out 都属于同 adapter 的普通 AP：同 BSS 由该 BSS `NoForwarding` 判定；不同 BSS 由 adapter 的 BTNBSSID 判定。任一端不是此 adapter 的 AP 时，该 hook 不额外限制；Linux bridge 原 `bridge_isolate`/VLAN/firewall 仍独立生效。
- 两标志用明确的 `READ_ONCE` 读取，其本身的写入语义是新策略对后续决定生效；不承诺撤回已完成许可的在途帧。回调不读目的 STA 表，不以关联/WCID 残留推导物理出口。

源 AP→有线出口允许；有线→AP 允许；跨 bridge 的 L3 路由由正常路由/防火墙负责。本 hook 不扩大 NoForwarding 的定义为禁止 AP 访问网关或所有跨子网流量。不同 adapter 间的隔离仍按原 per-adapter BTNBSSID 范围及 Linux `bridge_isolate`；不得把一个 adapter 的 DAT 全局值冒充整机所有无线芯片的全局 ACL。

若实际 bridge port 是 AP netdev 的 802.1Q/802.1ad upper，而非 AP netdev 本身，映射还须在 RCU 保护下沿已验证的 VLAN real_dev 还原到注册 AP，保留实际 bridge in/out 供 Linux VLAN 判断。不能因为 upper 不等于 wdev->if_dev 就把该 AP 当“有线来源”绕过隔离。该归一化只解释已识别 VLAN upper，不应把任意 bond/macvlan/隧道下层都当成同一 BSS；产品实际支持的 stacked topology 必须列入覆盖矩阵。

### 导出、配置与卸载同步

6.12.112 `net/netfilter/core.c:542/:585` 明确 `nf_unregister_net_hook` / `nf_register_net_hook` 为普通 `EXPORT_SYMBOL`，没有 `br_fdb_find_port()` 的 GPL-only/RTNL 查询障碍。

但注销**不等于同步排空**：`:529–540` 只移除 hook；`:75–88` 对旧 hook blob 使用 `call_rcu()`。必须在注销后显式 `synchronize_net()` 或等价 RCU grace，之后才能释放 `.priv` 指向的 adapter、hook ops 及依赖锁。回调在 `include/linux/netfilter.h:238` 的 RCU read-side 运行。postimage `net/core/dev.c:11455–11463` 定义 `synchronize_net()`，在 RTNL 已锁定时使用 `synchronize_rcu_expedited()`，否则 `synchronize_rcu()`，并使用普通 `EXPORT_SYMBOL(synchronize_net)`；因此该 drain API 也没有 GPL-only 障碍，且不能把持 RTNL 一概判成死锁。不得在持有回调所需的 `WdevListLock` 等锁时等待 grace，也不能在 RX/softirq 内执行注销等待。

实际配置已具备 bridge netfilter 核心：`package/kernel/linux/modules/netdevices.mk:2363–2369` 的 mediatek_hnat 选择 `CONFIG_BRIDGE_NETFILTER=y`、`CONFIG_NETFILTER_FAMILY_BRIDGE=y`。`kmod-nft-bridge` 包未选不构成此 hook 的依赖缺口；本设计不需要新增 nft bridge 用户态规则或包。CI 要检查实际内核配置，不能仅根据软件包名推定。

### 注册/退出与 netdev 映射

可选最小、可统一错误清理的注册点是 `os/linux/rt_main_dev.c:666` 的 `RtmpPhyNetDevInit()`：主 netdev 已设置进 adapter，尚未注册到 OS/接受流量。在该处注册 hook；失败返回创建失败，不能无 hook 继续启用 bridge-owned RX。后续子 BSS 无需再注册一个 hook。

在 `os/linux/rt_profile.c:2241 RTMPFreeAdapter()` 最早清理阶段统一 unregister + grace，再释放 adapter 其它资源；这样 PCI/RBUS/WBSYS probe 失败中直接调用 RTMPFreeAdapter 的路径也能覆盖。必要时在物理退出开始先停止新 RX/进入 stopping 状态，但不能把 hook 从仍可转发的安全域过早拆掉。正常 VIF down/up 不卸载 adapter hook。

不要扫描 `MBSSID[].wdev.if_dev` 裸数组当作生命周期保护。`ap_mbss.c:382–392` 先 unregister netdev，随后 `wdev_deinit()` / `wdev_idx_unreg()`，之后才 free netdev、清 if_dev。应使用 `WdevListLock` 保护的注册列表：`wdev_init():544–567` 在注册前初始化 type/if_dev/func_idx；`wdev_idx_unreg():111–134` 在同一锁内移除列表元素。新回调只检查这些基础字段，不调用可能已退出的 operate/hc 对象。

该锁现有已核验临界区是注册/注销/查找，没有同步发包或进入 bridge 的调用；新回调只取这一个锁，不拿 MacTabLock/RxReRingLock，不做日志/分配/RTNL/等待。不能沿用旧双MAC方案的 trylock 失败后回旧直转；通用策略必须保持确定性，不允许锁竞争恢复错误出口。

网络命名空间的最小实现选择为**adapter netns 固定**，不引入全栈 per-net hook：当前 OpenWrt adapter 在 init_net，hook 注册在持有引用的 adapter namespace。所有可承担 AP 角色的 netdev 在首次注册前设置 `dev->netns_local=true` 并核实 namespace；这是整个 netdev 生命周期不可撤销的独立 bitfield，AP→STA、能力 false 或 down/up 均不清除。实际 postimage `net/core/dev.c:11693` 在 namespace 移动前检查该字段并拒绝。

仅锁定 AP base 不够：VLAN upper 在 `register_vlan_dev()` 中、`vlan_vid_add()` 与 `register_netdevice()` 之前继承直接 real_dev 的 netns_local；受限时要求两者 namespace 相同，否则创建失败。递归约束整条 VLAN stack，覆盖 netlink 与旧 ioctl。不能在 NETDEV_REGISTER 后才撤回已发生的跨 namespace 创建。已有跨 namespace upper 或未受约束设备的原地 AP 角色转换不开放，要求正式 teardown/recreate。

adapter namespace、base、每层 VLAN upper 及实际 bridge 必须一致；控制面发现不一致即拒绝该拓扑，数据面异常则不进入旧内部转发。这样有明确、可测试的产品支持边界，不会以 init_net 漏保护假装支持任意 netns。完整 per-net 支持不是本次必要前置，但不得在未实施上述约束时直接放行。

上述 hook 只有在 HNAT/PPE 不绕过它时才完整。所有在同一 bridge 直接二层转发的 IPv4 HNAPT、IPv6 5T、其它 L2 flow 都须遵循父方案的通用卸载策略；不能只拒 PPE 的“L2”枚举类型。pre-hook 私有 fast forwarding 同样须受约束。这是与 offload 子设计的必要接口，不是只改 NoForwarding 即可完成的事项。

## 4. VLAN 与现有产品能力

静态 VLAN 不应继续作为“永久不修”的包级排除。现有 common announce 在 AP callback **之前**做 `VLAN_RX_DROP/REPLACE_VID/REPLACE_ALL/ALLOW`（`cmm_data.c:3552–3606`），普通 802.11 与软件 A-MSDU 也按 wdev 补 VLAN。新桥路径保持这些步骤与标签语义；Linux FDB 学习和 egress 依据 bridge VID，不能只按裸 MAC 合流。任意 EtherType、802.1Q/802.1ad/priority-tag、skb VLAN metadata 的处理要沿原 RX/TX 与 Linux桥路径验证，不得借“桥支持 VLAN”推定所有 vendor 标签形式已无误。

直接 AP bridge port 与“VLAN upper 在 bridge 中”都应进入最终设计。后者的 AP base netdev 可能没有 IFF_BRIDGE_PORT；只检查 base `netif_is_bridge_port()` 会误落独立 AP 旧直转。须按本帧最终 VLAN 表达解析实际 bridge ingress，由 Linux VLAN demux 执行真正上送；混合 untagged 独立 AP 与 tagged bridge 不能用“存在任意一个 bridge upper 就把全部帧上送”猜测。具体可执行分类见下一节。先前仅列 direct bridge 为可实现边界的结论已被新接口源码核验替代。

### 4.1 按 Linux demux 顺序只读分类本帧

新取得的 stable 6.12.112 `include/linux/if_vlan.h` 与 `net/8021q/vlan_core.c` 可直接用于 P 模块：`__vlan_find_dev_deep_rcu()`、`vlan_dev_real_dev()`、`vlan_dev_vlan_id()`、`vlan_dev_vlan_proto()` 在 vlan_core.c:100/111/117/123 均为普通 EXPORT_SYMBOL。在本地 generic backport/pending/hack 与 mediatek patches-6.12 中按这两个路径检索，未找到修改它们的补丁；仍应在完整 prepare/编译确认。无需使用 GPL-only 的 `netdev_walk_all_upper_dev_rcu()`。

分类 helper 只读 skb 和拓扑，在 RCU 内完成并只返回值型枚举；不把 netdev 指针保存到 RX 队列，不剥 tag、不改 skb->dev/mark/cb、不替代真实 Linux demux。输入为**全部现有 RX/MAP/VLAN变换完成后的** Ethernet 帧与 skb VLAN metadata。按以下次序解释：

1. 从实际 AP ingress dev 开始建立只读虚拟视图。若 skb metadata 带 VLAN，它是最外层，优先于帧内 VLAN；没有 metadata 时才读取帧内 802.1Q/802.1ad。每层协议、TCI/VID 和下一个 EtherType 都有界读取，协议要包含 Q/AD，不能只按 VID 查找。
2. 对当前层 `(current_dev, vlan_proto, VID)` 用 `__vlan_find_dev_deep_rcu()` 查实际已配置 VLANdev。发现有效、UP、同 namespace 的直接 VLAN upper 后，虚拟消费该层，current_dev 改为该 upper，再解释下一层，支持 metadata+in-frame、Q-in-Q。**VLAN demux 在 bridge RX handler 之前**：Linux `net/core/dev.c:5677–5688` 先 vlan_do_receive，找到后 another_round，再执行该 dev 的 RX handler。因此不能在查 VLANdev 前就因 base 是 bridgeport 停止。
3. 若本层没有 VLANdev，才检查 current_dev 是否真实 bridgeport；若是，判 bridge-owned，保留本层 tag 给 Linux bridge 的 ingress/PVID/VLAN策略，不再自行消费内部 tag。即便 base 本身在桥中且又配置了匹配 VLAN upper，也应先选 VLAN upper，不能把其 VLAN 域误归 base bridge。
4. VID=0 不能视为“无 tag”：若配置 VLAN0 upper，应先按正常 VLAN 查找选它；若不存在 VLAN0 upper且当前 dev 是 bridgeport，交桥决定 PVID；仅当当前 dev 非桥且无其它受支持 RX handler 时，按 Linux `dev.c:5717–5738` 的 priority-tag 规则虚拟忽略外层 VID0，内层仍是 VLAN则在同一 current_dev 继续 lookup。无内层 VLAN时才可按 priority-only 的独立域分类，不能把 VID0 与裸帧在解析前混同。
5. 无法匹配的非零 VID 不继续猜测内层：Linux会在未被 bridge handler 接管时设 OTHERHOST。判为 kernel-only/defer，让 Linux处理或拒绝，**不能回旧内部空口转发**。找到但 DOWN 的 VLANdev同样不回内转，因为 vlan_do_receive 会丢弃该帧。没有 tag、没有桥、没有未知 RX handler 的已知普通独立 AP，才判 standalone-legacy；这由确定拓扑决定，不是查表/资源失败后的 fallback。
6. 上界沿 `VLAN_MAX_DEPTH=8`；截短、非法长度、循环/深度超限为明确 reject，未知 master/macvlan/bond/其它 RX handler 为 kernel-only 且禁止绑定，不把未知当 standalone。`__vlan_find_dev_deep_rcu()` 可能沿 master 递归：仅在找到的 VLANdev 即时 real_dev 与 current_dev 一致的已核 direct VLAN链上推进；若返回来自别的 master，应归 unknown/kernel-only，不能假定它就是本帧 Linux 私有 `vlan_find_dev()` 的等价结果。可通过公开 if_vlan.h 的 vlan_dev_priv()->real_dev 检验即时下层，勿用会折叠所有层的 vlan_dev_real_dev() 误作即时父设备。

真实 Linux receive 仍可能因 tag/端口在 classify 后发生配置变化而选择另一合法入口或丢弃，这与读取 RCU 拓扑的普通竞争一致；helper 不是把旧快照强行写回内核的缓存。更新窗口不能因此选回旧 STA出口。若需要内层选择严格覆盖额外 master 拓扑，须进一步对 Linux 私有 vlan_find_dev()/RX handler 证明，不为凑覆盖扩大推断。

NF_BR_FORWARD 的 BSS 识别则使用 `vlan_dev_real_dev()` 折叠已识别的 VLAN upper，得到最终普通 AP base 与注册 wdev 匹配；实际 state->in/out 及 VLAN语义保留。前者是只读入口分类，后者是已选出口隔离，两者用途不要混在一起。

### 4.2 netifd 将能力与隔离传到实际 VLAN bridge port

当前 netifd `vlan.c:131–158 vlan_dev_cb()`、`vlandev.c:130–150 vlandev_base_cb()` 只转发 presence/rename/topology，未继承 wireless/AP/isolate 等字段；`.vid`/显式 8021q/8021ad 都可能因此在 upper 丢失 hairpin能力。只在 AP base 设置新字段不完整。

建议在 `device.c/device.h` 实现一个内部无线策略 helper，并让两类 VLAN设备按实际 dependency 链继承，不能按接口名中的点号判断：

- `vlan.c` 从 `vldev->dep.dev`，`vlandev.c` 从 `mvdev->parent.dev` 获取父对象，复制有效 `wireless`、`wireless_ap`、`wireless_bridge_forwarding`、`wireless_isolate`、`wireless_proxyarp`。在 ADD 回调中必须先计算这些值，再 `device_set_present(child,true)`，确保 bridge_member 的 ADD 第一次 addif 就看到正确属性；在 config_init/set_up 中也刷新，覆盖先建 upper 后收到无线状态的顺序。
- 为这些**内部策略值**增加 `DEV_EVENT_WIRELESS_POLICY`。ucode.c 的 device_set 在完整布尔组更新后、值确实改变时广播；两类 VLAN父事件回调重新计算并向其子设备传播，嵌套 VLAN递归生效。不是仅传播 DEV_EVENT_TOPO_CHANGE——现有 bridge_member_cb:758–816 根本不处理它来重写端口策略。
- 源策略与继承有效值要有明确来源，不能一次 OR 后永远留 true。角色变 STA、bridge能力变 false、父对象被替换/移除时，继承值覆盖为 false并通知后代。若 upper 有明确的直接配置，保存为本地来源，与派生值分开，不反向覆盖 UCI；隔离有效值不能因 upper 没收到一次通知而意外变宽。
- `bridge_isolate` 是 `device.settings` 中独立的端口设置；不要把父 settings.flags 整组复制给子设备。可新增内部 `wireless_bridge_isolate` 派生布尔值，effective isolated 为 upper自身明确 isolate 与父有效隔离的 OR，父解除后清掉派生值，upper本地 true 保留。`wireless_isolate` 仍控制 hairpin=0，不能以它代替 BR_ISOLATED。
- bridge.c 的 `bridge_member_cb()` 新处理该策略事件：active端口立即经公开 system helper重算 hairpin/mcast/proxyarp/isolated，**写入 0 和 1 两个方向**，并对必需字段读回。现 system_bridge_set_wireless 是 static，可增加 `system_bridge_update_wireless_port()` 封装；能力由 true变false时也要调用，不能仅 `if (dev->wireless)` 跳过清旧值。失败应禁用/移出该成员并报告错误，不能保留已知错误的隔离或默认直转。

该补充不新增用户开关，仅使内核真正接收帧的 VLANport拥有与 AP来源一致的内部属性。实际用户的组播转单播设置仍按自身/bridge配置解析，不为了hairpin强制开启该功能。双层 VLAN、先/后创建、parent角色变false、upper移除重建与独立 upper隔离组合都须测量最终 sysfs 状态，而不只测试基接口默认值。

动态 STA VLAN 是有源码、但未证明当前 UCI 端到端支持的功能：

- `os/linux/rt_profile.c:1786–1820` 根据 skb WCID 的 `entry->vlan_id` 插标签，存在依赖 RX WCID metadata 完整性的要求。
- `ap_data.c:354–375/:600–619` 在 TX 检查目标 STA VLAN 并移除标签；RX/TX/VLAN 不能按不同身份来源各自猜测。
- `ap_cfg.c:28479 Set_Sta_Vlan()` 可以按当前 STA 设置 VID；测试用 `Set_Dvlan_Proc()` 写 WCID1，不应成为通用控制接口。
- 当前 MTK schema 声明 wifi-vlan/station VID，但 ucode handler 未处理 `iface_data.vlans/stas`，`mtwifi.netifd.set_vlan()` 只有定义、未见调用，converter 没有动态/静态 VLAN 映射。仅有 schema 或 C setter 不能称 LuCI/UCI 动态 VLAN 已受支持。

最终版应修复所有当前实际启用普通 AP/VLAN 数据路径，不为一个新修复顺带宣称已补齐原本未实现的 Radius/STA VLAN 控制面。若要将动态 VLAN 列为放行功能，必须同时完成可信 RX WCID、标签/授权域与热改变失效的集成验证；不能默默 fallback 到旧内转再称全量。

## 5. 生命周期、应用失败与回退

本设计无在线桥模式开关，不需要为“切换模式”另加 generation。正常 UCI 配置仍沿既有应用序列：`mtwifi/config.uc:167–181` 先对相关接口 ifdown，再提交 DAT，必要时 reload driver，`:214–221` ifup 并执行 runtime hooks。DBDC DAT 改变可能同时 down sibling 接口并恢复，不能宣称无中断。

现有 `driver.uc:46–58` 的 ifup/ifdown 和 `:137–148` 的 rmmod/modprobe 忽略 `system()` 返回值，`:152` 的 iwpriv 同样如此。新桥策略不依赖 iwpriv；但若控制面因新能力/策略变更而依赖成功 down/reload，必须检查失败并停止，避免继续 commit/mark_up，不能把日志成功字样当作操作完成。接口 up/down/reload 返回值的改动应只覆盖必要错误传播，不展开无关重构。

桥 attach/detach 的选择依据真实 bridge port 状态；Linux `br_if.c:324–367` 先禁端口、清 VLAN/FDB、unlink upper，再移除 IFF_BRIDGE_PORT/RX handler。父 offload 设计必须在这些拓扑事件、VLAN/隔离更改和无线关闭时失效相关硬件 flow；不能等到新流量偶然清理。ARP/邻居/FDB 不需要由用户手工清空来“应用修复”。

回退是恢复已知可用且布局兼容的旧固件，或切回旧驱动代码后构建；不保留隐藏的 per-packet 旧内转 fallback，也不留下 root-only 双MAC实验开关进入生产。升级/回退/无线重载结束当前漫游对照窗口，不把其清旧 STA/PPE 的效果计作新路径因果证据。

## 6. 行为矩阵与验收

| 场景 | 推荐最终行为 |
| --- | --- |
| 非隔离 AP，bridge port，同 BSS 普通单播 | 只上送，桥依据 FDB hairpin；STA 表只用于最终 TX无线接收者查找，不能先选出口 |
| 终端从本 AP 漫游到有线下游 AP | 上送后桥选有线出口，即使旧本地 STA 尚未删除 |
| 终端返回本 AP | 正常源帧经过桥重新学习，不允许内部直转长期绕开源学习 |
| 同 BSS广播/组播 | 只由桥回送，遵循 MDB、VLAN、mcast_to_ucast、hairpin，避免双发 |
| `isolate=1`/NoForwarding=1 | hairpin禁用；NF hook也禁止实际同BSS AP↔AP，包括group；网关及有线来源按正常策略 |
| 同 adapter跨 BSS，BTNBSSID=1 | NF hook对真实不同AP端口拒绝，含group；无目的STA查询 |
| `bridge_isolate=1` | 保留Linux端口isolated原语义，不拿它等价替代BTNBSSID |
| 跨 VID/不同bridge | 原VLAN接收策略+Linux bridge ingress/egress/FDB VID，跨网段仍由路由/防火墙决定 |
| 独立AP、不在Linux bridge | 保留其本地转发语义，不以“全量”名义破坏ARP |
| APCLI/WDS/A4/MWDS/mesh/TDLS等特殊角色 | 按功能能力保留原专用路径，明确不由“普通三地址AP桥”自动背书；无需固定任何终端身份 |

实施与实机最低验收：

- 所有普通 AP/BSS/客户端均无身份白名单；生产无诊断模块参数及固定目标信息。
- bridge multicast_to_unicast=0/1、proxy_arp=0/1 与 isolate=0/1 的组合；同 AP TCP/UDP、ARP、IPv6 ND/DAD 不因 hairpin 来源改变而失效。
- NoForwarding、BTNBSSID、bridge_isolate 及其组合，含 IPv4/IPv6/非 IP L2、广播/组播；确认网关、DHCP和有线服务器可达性。
- 同一 MAC 在不同 VID，tag/metadata/untag/priority-tag，以及产品实际启用的动态 VLAN；无跨域泄漏。
- STA漫游和返回、APbridge attach/detach、DBDC重载、模块probe失败/卸载压力；无 UAF/锁死/重复/环路。bridge hook注销须用故障路径及并发 RX 验证。
- 配置能力通知 true/false 的角色转换与netdev重建；sysfs写入失败的错误传播，不能只测正常默认hairpin=1。
- 普通 Linux bridge 与 private HNAT/PPE 都不绕过同一策略，且新软路径不由锁竞争退回旧出口；性能需实测。

本文提供的是通用最终策略设计输入。关联授权与 BA/AMSDU 身份证明由 RX 子设计负责；HNAT 的所有桥流与事件失效由 offload 子设计负责。两者与本策略共同通过 Astra 及实际实现审查后，才构成可推送编译的完整方案。
