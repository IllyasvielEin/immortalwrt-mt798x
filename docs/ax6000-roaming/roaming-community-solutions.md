# 漫游旧状态与回包黑洞：社区方案调研

核查日期：2026-11-08。本轮调研公开问题、实际代码和已合入提交，未修改路由器、固件代码或编译配置。

**找到可借鉴的既有处理机制和相似故障修复，但尚未找到可以直接 cherry-pick 到当前 MTK 7.6.7.3、且有社区同症验收记录的补丁。最直接的参考是 OpenWrt/mac80211 已有的“关闭无线内部转发，交给 Linux 桥”设计。**

## 1. 最适合借鉴：让 Linux 桥负责 STA 间转发

这不是仅凭本机现象提出的新概念。现行 OpenWrt 源码在 `multicast_to_unicast || proxy_arp` 时设置 `ap_isolate=1`：

- [官方 ap.uc，固定到 292104ab（2026-10-28）](https://github.com/openwrt/openwrt/blob/292104ab33e743ae5f9d6cd8f24b957f4f56063c/package/network/config/wifi-scripts/files-ucode/usr/share/ucode/wifi/ap.uc#L177)。
- [Linux 6.12.112 cfg.c](https://github.com/gregkh/linux/blob/v6.12.112/net/mac80211/cfg.c#L2790) 将此映射为 `IEEE80211_SDATA_DONT_BRIDGE_PACKETS`，并刷新 fast RX 设置。
- [普通 RX](https://github.com/gregkh/linux/blob/v6.12.112/net/mac80211/rx.c#L2686) 和 [fast RX](https://github.com/gregkh/linux/blob/v6.12.112/net/mac80211/rx.c#L4849) 因而跳过内部 STA→STA 转发，继续上送本地网络栈。是否允许桥再发回同一无线接口，由 hairpin 和桥隔离策略决定。

[OpenWrt FS#272（2016）](https://github.com/openwrt/openwrt/issues/5308#issuecomment-1039712597) 中，报告者最初认为 hostapd 的 `ap_isolate=1` 不正确，随后确认“Packets are still being sent from STA to STA via the bridge and not at a lower level”。该帖2017年的另一组配置问题，维护者通过移除重复加入桥的无线接口配置等恢复同AP互访；不能把该配置纠错误称为已验证本机的漫游修复。

与本机的对应：我们已经证明 Linux 桥知道平板在 `wan`，但 MTK 内部直转绕过了桥。交桥处理与已定位的错误点直接对应，也能让源 MAC 正常参与桥学习，适合作为首选修复候选。

**不能照抄参数：** 标准 mac80211 的上述 `ap_isolate` 路径是“停止内部直转后继续上送”；本机 MTK 的 `NoForwarding=1` 是“关闭 `to_air`，但不打开 `to_os`”，结果可能直接丢包。现机没有核实可直接切换到同义桥转发的配置。应适配代码语义，不能直接开启客户端隔离。

**不能承诺换 mt76 就根治：** 未启用 `DONT_BRIDGE_PACKETS` 时，mac80211 普通/fast RX 也会按目的 STA 存在进行内部转发，而非查询桥 FDB。是否能绕开本问题，取决于实际生成配置、数据路径及桥策略，仍需实测。OpenWrt的条件式设计不能表述为“所有mac80211配置无条件交桥”。

这组来源证明该架构已有成熟代码，不等于已经找到同一 MTK 故障的社区验收报告。

## 2. MTK/Padavan 已合并的配置缓解：低 RSSI 踢除

[Padavan PR #407](https://github.com/hanwckf/rt-n56u/pull/407) 于2020-06-14合并（`9a818bec5551583e04e63685efb8e8f445f6b95e`），启动脚本增加默认注释的漫游助手示例：`KickStaRssiLow=-85`、`AssocReqRssiThres=-80`。合并的是示例，不是修复旧STA/FDB协调的驱动补丁，也没有本机同症验收。

本机精确7.6.7.3也有 `Set_AP_KickStaRssiLow_Proc()`（`ap_cfg.c:14952`），0关闭、非零有效范围-100至-1；`ap.c:4032` 在Rssi_Updated为真且平均RSSI低于阈值时进入踢除。第一轮平板旧条目显示-83 dBm，第二轮-92；显示值还不能直接等同RTMPAvgRssi口径，因此-85只是社区示例，不能保证覆盖两轮。Rssi_Updated不证明样本新鲜；BSS内其他边缘终端也可能被踢、反复连回原AP。

UCI源码调用链为：`wireless.iface.json:55`声明`kicklow`；`dat-defs.json:79–82`映射到`KickStaRssiLow`；`driver.uc:181–188`的`apply_runtime_hooks()`调用`driver.uc:152`的外部`iwpriv`包装。**源码映射不等于本机可执行。** Astra指出Build19未选择wireless-tools，主线程21:18只读确认标准PATH及四个标准路径没有iwpriv，现机脚本也仍调用外部命令且不检查退出状态。调用工具或替代接口必须先补齐/验证，本轮未安装或执行setter。此结论未穷举所有非标准路径。

该配置不是converter直接写DAT，不能仅看DAT判断运行值；命令被打印也不代表执行成功。setter可运行时设置，不代表通过完整无线setup应用UCI就无中断：setup会ifdown/ifup，若如此应用需重新漫游，不能将重载清旧状态当作RSSI效果。不能照搬Padavan接口名或同时开启关联拒绝门限。

若希望先试现成机制，需先解决调用能力并接受覆盖代价，才把低RSSI踢除列为单变量缓解候选；若要解决“终端已移到其他AP，仍被旧条目截包”的直接转发问题，第一类桥转发实验仍优先。详见 [经Astra讨论后的取舍与验收](roaming-options-proposal.md)。

## 3. 控制面路线：新 AP 通知旧 AP 清理 STA

历史 hostapd 已有真实实现：[hostapd 2.9 的 iapp_process_add_notify()](https://github.com/lwfinger/rtl8188eu/blob/f5d1c8df2e2d8b217ea0113bf2cf3e37df8cb716/hostapd-2.9/src/ap/iapp.c#L260)。收到其他 AP 的 ADD-notify，查出本机旧 STA 后调用 `ap_sta_disconnect()`，清理旧上下文。

这说明业界可以通过 AP 之间的控制面同步解决旧关联保留问题，不必完全依赖终端发离网通知或等待空闲超时。但这里引用的是历史 IAPP，不能直接当成当前 AX6000+GS7 的可用配置：

- [Linux 已合入 3e493173（2019-09-11）](https://github.com/torvalds/linux/commit/3e493173b7841259a08c5c8e5cbe90adb349da7e) 明确说明 IEEE 802.11F-2003 在2006年撤销；该提交自身解决的是授权后再发 L2 Update。
- 本机两台 AP 使用不同的管理/驱动体系，尚未确认存在可直接对接的受控站点同步机制。
- L2 Update 只解决桥学习，不等于删除旧无线 STA。本机 FDB 已正确，因此单独补 L2 Update 不能解释为修好了已定位路径。

可借鉴“收到可信的新接入事件后使旧条目失效”的思路；不建议直接把历史 IAPP 当现成跨厂商修复下发。

## 4. 社区确有已合入漫游修复，但应区分故障层

[OpenWrt #24760](https://github.com/openwrt/openwrt/issues/24760) 于2026-08-16报告 MT7531 上类似的“从本地 AP 漫游到有线下游 AP 后黑洞”。软件 master FDB 正确，但交换芯片 CPU-port/self 条目异常；报告者实测定向 `bridge fdb replace … master static` 再删除可以即时恢复，另有用户反馈相同临时处理有效。

[c7924dcd：generic: dsa: track host fdb entries per port](https://github.com/openwrt/openwrt/commit/c7924dcd74a6bd30abd5f935209528c071ddd287) 已于2026-10-26合入 OpenWrt。它修复重复、乱序 MAC 移动事件下 DSA host FDB 引用计数错误。本地 Viking 已有同系列 `pending-6.18/643-net-dsa-fix-host-fdb-refcount-when-a-bridge-fdb-entry-roams.patch`。

适用边界很关键：该报告的其他 DSA 有线源失败，而 CPU/本地无线源正常；本机却已抓到**本地无线源回包在 mt_wifi 中被 `to_os=0` 截走**。DSA补丁不改这一分支或MTK STA表，不能替代本机驱动修复。其QEMU dsa_loop复现/通过记录，也不能外推为已在本机或报告者MT7531硬件验收。这里只确认OpenWrt已合入，不声称对应原始patch SHA已经合入Linux主线。

因此值得作为另一个故障层单独评估，不能因“漫游后等几分钟”这一表面相同就直接套补丁。

## 对当前方案的影响

优先适配第一类已有设计：保持既有认证、隔离和特殊转发路径，把受影响的普通无线单播交给 Linux 桥。先用默认关闭、限定两终端、能在故障中动态切换的实验确认即时恢复，再检查双向漫游、源 MAC 学习、普通/fast路径、隔离和性能。mac80211在模式变化时刷新fast RX的做法，也提示MTK实现应核对其快速路径，不能只验证单个握手包。

控制面清 STA 可作为另一条方案，但异构 AP 的事件来源和重关联竞争需另行设计。缩短 `BssMaxIdle` 只减少等待，不是以上两类机制的替代。当前未实施新补丁、参数、固件升级或清表。

## 检索范围与访问限制

已查 Linux/mac80211/hostapd 代码与提交、OpenWrt公开问题/PR、MTK/Ralink相关代码别名及公开问题。检索包含 `roaming`、`stale station`、`intra-BSS`、`ap_isolate`、`NoForwarding`、`DisConnectSta`、`ap_rx_pkt_foward`、`APFowardWirelessStaToWirelessSta` 等。原始源码及社区证据保存在 `evidence/community-roaming/`。

OpenWrt论坛搜索返回HTTP429；DuckDuckGo返回验证页面，未绕过；Bing RSS未正确遵守多词/站点查询，结果不可靠，未把它用于缺失结论。FS#272引用的2015邮件旧链接现在HTTP404，未把未读到的邮件正文当证据。此类访问限制不等于相关问题不存在。厂商私有支持库、未索引论坛和全部第三方SDK未覆盖。

目前的准确结论是：在已核对来源内，有成熟的桥转发架构可借鉴、有历史站点同步实现、有不同层的已合入漫游修复；尚未找到当前MTK7.6.7.3可直接照搬且有同症社区验收的专用补丁。
