# AX6000 桥转发根治：双 MAC 候选设计（已撤回）

**已撤回，不实施。** 用户明确否决固定两设备的候选，要求直接设计全量通用最终版。当前以 [通用最终版 r2](roaming-bridge-final-design.md) 为准。下文仅保留旧设计及其源码依据；固定 pair 许可、固定 pair PPE 限制和以其为前提的实施步骤均不再有效。原 r1 Astra 审查已请求取消，通用设计重新审核。

2026-11-08，r1 送审稿。用户已经选择方案 B：修正旧 STA 表凌驾于桥 FDB 的转发机制，不继续以 RSSI 或老化门限作为主线。本文待 Astra 设计审核；还没有修改固件实现、推送或刷机。

## 目标与证据边界

两轮实测已经证明：平板移到 GS7 后，AX 的桥正确把它学到 `wan`，但 `.13` 返回的报文被 `ap_rx_pkt_foward()` 按旧关联 WCID 在 `rax0` 内部直转，`to_os=0` 使桥没有选择出口的机会。k/v/r 均关闭时也发生；自然老化后该路径恢复。旧关联为何没有更早清理仍未确定，不是本次数据路径修正的前置依赖。

本轮先交付默认关闭、精确限两终端的机制验证固件，并继续推进生产化验收。测试固件编译通过不等于真机修复通过；只有实验支持后，才把适用范围扩成普通 AP 客户端的桥转发策略。不会把仅覆盖两 MAC 的候选称作最终通用修复。

固定测试身份：A=`08:eb:21:f2:88:4d`（`.13`），B=`6a:37:f7:8b:b8:da`（`.32`）；仅允许 A→B 或 B→A。目标为 AX `rax0`、BSSID `a6:a9:30:90:1c:b3`。桥 hairpin 已现场核实为 1，VLAN filtering 为 0；后者不替代对报文、wdev 和动态站点 VLAN 的排除检查。

## 精确代码基线

- 固件生产基线：`sources/immortalwrt-mt798x`，`25.12`，`c196312d3916fad77fa0bb47db57e4b685c15f03`，起始工作区干净。
- MTK 7.6.7.3：`mt79xx_20250408-705eb4.tar.xz`，SHA-256 `b029d7b43c498193092dc6a3c857361c8835ce34bb306561c550ac0539784a78`。已将生产系列 42 个补丁按顺序、`fuzz=0` 应用到 `sources/vendor-mt79xx-7.6.7.3/review-baseline`；`PATCHES.json` 记录输入摘要。这是源码分析树，不是新实现。
- 外部 WARP：`warp_20250408-5f71ec.tar.xz`，SHA-256 `0954055e165dcb71cd2703f07b0c2d521ca3e9c8968d2b17b68e8c9a7853f9b2`，已下载并解压到 `sources/vendor-warp-20250408-5f71ec/source/warp`。不能用 mt_wifi 归档内的 bundled WARP 代替实际包。
- HNAT：不能只看 `files-6.12` overlay。已按内核补丁顺序，对 `drivers/net/ethernet/mediatek/mtk_hnat/*` 应用相关补丁，分析树为 `sources/hnat-6.12.112-review-baseline`，`PATCHES.json` 记录 14 个引用该目录的补丁（含 hook、HQOS、调试/多播等）。新的实验改动应另加末尾内核补丁，不直接改 overlay 前像造成中间补丁冲突。
- 现有 043 插桩来自 Build 19，只复用其观测能力；原 `(SA 命中 || DA 命中)` 过滤不能参与转发许可。

## RX 身份与转发改动

详细源码核验见 [RX 身份设计](bridge-rx-identity-design.md)。采用值型 RX context，保留现有 `wifi_dev_ops` ABI。头转换时 Ethernet SA/重建的 Addr2 不能证明无线 TA；需核实 FMAC Group4 和 descriptor 原始 WCID，并与已经通过原 RX 检查的普通 client 绑定。未知或无法证明的路径不启用新许可。

最终 guard 放在原安全、隔离和转发分类全部完成之后、`DuplicatePacket/CopyPacket` 之前。仅原普通同 BSS 客户端分支的 `to_os=0,to_air=1` 可改成 `1,0`；原 `0,0` 保留。EAPOL、控制/组播、VLAN、跨 BSS、A4/WDS/MWDS/APCLI、隔离和未授权来源均不纳入。第一候选仅允许 Ethernet II 的 IPv4、IPv6、ARP 三种 EtherType；TCP/UDP/端口/标志不参与许可。其它 EtherType 和 length-coded LLC/SNAP 维持原路径并计数，不宣称已经覆盖全部 L2 封装。

上下文按值经过 BA 重排序、A-MSDU 拆分；每个最终子帧重新检查 SA 与可信 TA。禁止保存 RX_BLK、STA、wdev 指针到队列或共享的“当前包”字段。新增 guard 不能阻塞获取 MacTabLock：BA 出队持有 RxReRingLock，站点重置存在反向锁序。非阻塞检查失败应保留原路径并计数；覆盖不足不能称为完整修复。

关联世代采用何种契约须交 Astra 明确裁决：初选兼容现有 RX 的“入口已授权接受的在途数据允许延后，出口再次核对当前同 MAC/同 wdev 授权”，不宣称逐关联即时撤销所有排队数据。现有指针、WCID 和 StaConnectTime 均不是 generation token；MacTabLock 也不保护所有 PortSecured 写入。若审核要求严格世代绑定，须先实现完整身份更新/撤销机制，不能给上下文随意补一个计数器冒充解决。

初版固定 MAC 对/BSSID 为编译期常量，只暴露 root 可写、默认 0 的 `roam_diag_bridge_enable`（只接受 0/1），不提供在线改目标身份的接口。每个包在最终转发决定前只采样一次 enable；OFF 对之后的决定生效，不宣称撤回已经交桥或排队的 ON 包。日志记录实际采样模式，不能在上送点再采样一次导致包内前后策略不同。

计数独立于限速日志：精确 pair 命中、有效/缺失 ctx、授权/域/封装拒绝、锁竞争、BA queued/immediate、实际 override 和后续上送；记录 ctx 捕获时间与排队时长，但不把它当作 RF 接收时间或当前物理位置证明。旧 BA 帧晚到可能短暂回写源 FDB，是新增桥学习影响，须与长期旧 STA 黑洞区分。上下文大小乘实际 pool 结点数记录内存代价。

## 真正交桥与卸载边界

硬性要求：`to_os=1`、跳过一个 NAT hook 或单条路径日志均不等于报文实际进入桥。必须核对驱动上送、HNAT ingress/pre/post 和外部 WARP；不能在 ON 时通过清 STA、清 FDB、重载无线或全局关闭卸载制造恢复。

详细源码与函数返回值见 [真实上送与快路径设计](bridge-fastpath-design.md)。正常 RX normalization 和接收函数保持原序，不另设共享 skb 标记、不直接跳过原 announce 的安全/转换处理。需要观测真正的 Linux receive 边界、HNAT pair guard、桥/wan 出口与业务传输，不能仅依靠新 helper 的自报结果。

候选实验采用：**目标 MAC 对的 PPE 软件限制在测试固件中恒定生效，独立于默认关闭的桥开关**。MAC 对的限制覆盖这两终端彼此间所有 BSS/VLAN/EtherType，只限制性能/绑定，不改变允许/拒绝；它比桥许可的 BSS/无 VLAN 范围宽，是为了防止其它封装先建立同流硬件条目。第三终端不命中该规则。这个范围代价须写进固件说明，不能把桥开关 OFF 描述为完全恢复原生产性能。

首个 OFF 基线发生在新固件完整启动后，旧固件 PPE 表不得跨入窗口；必须证明目标对未形成 BIND、没有额外手工注入条目。已核实正常启动代码：`hnat_start()` 为各 PPE 分配 FOE 表，设置 PPE_TB_BASE 并将全表清零；`hnat_hw_init()` 启用缓存前清缓存；probe 在各 PPE 启动后才启用 hook。这支持新固件正常启动不会继承旧 BIND，仍需核对启动无错误及运行观测。OFF/ON 期间不 flush。当前 HNAT 的 `mtk_sw_nat_hook_rx()` 只归一化 WED metadata 并返回 NF_ACCEPT，可保留原 announce/协议转换/接收顺序；限制应放在真正快速转发/绑定的入口。拟在 IPv4/IPv6 pre、bridge pre/local-in、pong/HQOS、共同 post-routing 和 `mtk_sw_nat_hook_tx()` 等位置加入精确 pair guard。TX 使用有界的 saved MAC header，不能把带 TXD 前缀的 skb->data 当 Ethernet header。尤其需先于 5111 增加的 magic 重建，不能被其重新赋值覆盖。具体接口、返回值和所有写 BIND 位置以独立快路径报告为准；若有未覆盖的绑定入口，先解决再放行。

这保留 WED 硬件收发/重排序，不等于关闭所有硬件功能，也不等于已经验证正常 PPE 加速。完成因果实验后，须另做恒定限制移除/范围扩展的设计审查和测试，不能直接把该实验补丁当作生产修复合入。

[22:26 只读预检](evidence/community-roaming/bridge-offload-preflight-20261108.txt)：`nft list flowtables` 成功且未列出 flowtable 定义；bridge-nf-call-iptables/ip6tables/arptables 均为 0。UCI 的两个 flow_offloading 字段未定义，不能写成显式 0。包选择不能代表运行状态；新候选启动、实验开始和配置变化后必须重新核验。若出现其它软件 flowtable/绕过路径，应结束本窗口并另行审查，不能在 ON 时全局关闭后把恢复归因于桥开关。

## 独立测试分支与构建

1. 在实际固件仓库从上述生产基线建立 `fix/ax6000-roaming-bridge-test`。保留生产 `25.12` 和原 Build19 诊断分支引用；不合入生产、不改 uBoot/分区布局、不自动刷机。
2. 测试补丁以 043 诊断 + 后续桥转发补丁区分，默认开关为 OFF。固件、工作流标题、artifact 和元数据明确标为 bridge test，不创建正式 Release。
3. 原生产 guard 文件及默认拒绝诊断语义保留。测试分支构建使用独立测试校验：只允许 manifest 中精确命名和 SHA-256 匹配的实验补丁，其余生产补丁仍按生产规则检查；编译后检查必需诊断/桥参数符号以及 RRM/WNM/FT、HNAT/WARP 模块。
4. 测试分支的配置校验入口和工作流明确改成测试校验，不能使用静默跳过 guard 的环境变量。构建输入记录改为真实的 `roaming_diagnostics=included-default-off`，记录实验 manifest、补丁和默认策略；保留 feeds 锁定、Go 工具链、源码 HEAD、配置和镜像大小等既有核验。生产源码保持原状。
5. 优先复用已经审查的 CI 缓存/外部 Go bootstrap。实现审核放行后提交、推送此测试分支，以 `external + cold_cache=false` 手动触发一版构建；不持续轮询 CI，沿用用户通知完成的约定。

## 编译前验收

- 在精确打补丁基线上验证整个系列 `fuzz=0`，检查所有旧接口/ops 调用点、queue 初始化与释放、单一报文所有权和最终路径计数。
- 对实际使用的独立许可/解析 helper 做有意义的主机测试：精确 A↔B、第三终端、伪造 SA 与 TA 不同、未知 RX 身份、未授权、原拒绝、跨 BSS、VLAN 各种表示、截短/非线性报文、EtherType/控制帧和模式切换。测试不能仅断言补丁字符串存在。
- 现有 CI/feeds/生产 guard 回归继续通过；测试 guard 必须拒绝额外或被修改的实验补丁，生产 guard 必须仍拒绝测试系列。编译验证覆盖 MT7986 实际宏组合，并运行 `git diff --check`。
- Astra 另做实际实现审查；修正阻断项后再推送构建。设计通过不能代替实现审查和编译。

## 真机因果与生产验收

测试固件 OFF 必须先复现相同旧 STA 故障。位置保持平板 GS7、固定端 AX，记录旧条目身份、正确桥 FDB 和新业务失败；在不删除旧 STA、不改 k/v/r/GS7/老化、不清 FDB/不重载的同一窗口切 ON。用新连接匹配四元组、seq/ack 和真实 wan 出口，并传实质数据；尝试 OFF→ON→OFF→ON。旧条目删除、重关联、接口重建或位置变化即结束可比窗口。

ON 会改变桥学习、邻居与连接状态；关开关只回退策略，不撤销这些状态。OFF 未复现时记录反向对照未完成，不把它自动判为修复失败或完整闭环。卸载限制若不同于生产，性能和硬件覆盖须另验，不能用软件实验替代。

后续推广须验证双向漫游及返回 AX 的源学习、同 AP/同 GS7/分处两 AP、IPv4/IPv6/TCP/UDP/ARP、冷/热流、普通与加速路径、非目标和隔离不变，测量实际 CPU/吞吐/时延。出现越界放行、重复/环路、误投、非目标影响、崩溃或不能可靠关断即停止；恢复已知兼容镜像是独立刷机步骤，刷机结束该因果窗口。
