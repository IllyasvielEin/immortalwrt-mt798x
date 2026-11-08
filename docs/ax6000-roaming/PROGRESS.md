# 暂停检查点

记录时间：2026-11-09 01:06（Asia/Singapore）。

**暂停已由用户随后发送的“可以继续”解除。下文保留暂停时快照；恢复后的当前状态以 README 与主设计为准。**

## 恢复后进展

### r3 审查之后的当前修订

- Astra新增 `roaming-bridge-final-astra-review-r3.md`：原P1-1/P1-3/P1-4设计关闭，仅剩WARP STOP错误被吞掉、STOP ACK前释放DMA这一P1。原送审文档快照为Git `0de772dc`，报告保持独立不改。
- 主稿现在为r3.1，补入 `bridge-r3-warp-stop-contract.md`：值型checked结果；STOP只确认停机、保留ring；成功FE_DONE后再清理/重建，WO/engine返回与首错/deadline真正传播。正常WED/L1 SER保留。
- 为避免扩大运行态retain与PCI热移除改造，拟选明确终态B：无法确认DMA停止/完成或不可安全串行销毁时，同步进入整机noreturn故障入口尝试HARD复位；不走normal shutdown/panic/kexec，失败可能需断电。相关真实析构入口和proxy退出共享本轮SER准入/fatal guard，补足SMP停机best effort的局限。此新增故障体验已向用户说明，仍待Astra技术放行与后续产品验收。
- 8个准确fatal/reset选定文件已下载并准备于 `.analysis/linux-6.12.112-fatal-postimage`，仅1个相关MTK WDT patch；有SHA/清单，非完整内核prepare。PSCI/WDT实际复位完成仍是硬件门。
- P2已并入：早期reader先于rx_pkt_allowed配置读取、完整0/1策略/false传播、native/compat外层ioctl失败视不确定后GET+重新PREPARE、永久callback发布也是寿命承诺、下层reset错误/普通open与内部restart/实际活动participant及ownership补环要求。
- 仅设计文档修改；未进入固件代码实施、推送、构建或路由器操作。下一步提交r3.1文档快照并新建Astra复审唯一P1及其有限依赖，通过后持续实施。

### r3 首次送审前的记录

- 已形成 `roaming-bridge-final-design-r3.md`，入口 `roaming-bridge-final-design.md` 指向该稿。
- 根据范围担忧，拟采用固定全软件转发，取消所有 PPE/NF-flow cache，保留 WED 基础能力及最小在线 L1 SER；全局 topology epoch/动态 backend/保留 wired BIND 的账户框架不进入本版。已通过可选偏好问题向用户说明性能取舍，尚未收到额外选择；未把无回复当验收或刷机许可。
- 新增固定 CPU 激活、SER 和范围比较材料；r2 NO-GO 与原四份细化稿保留。范围比较里额外取消在线 FE 恢复的候选未采用。
- 当前选中的 MTK eQoS 将复用已有软件 tc/HTB/IFB，所有规则走软件；mtkhqos_util 不打包。主稿已列硬件 QoS 损失、classid/默认叶类要求及 UI/CLI 适配，未悄然保留失效服务。
- 补齐单文件准确 TC postimage：`.analysis/linux-6.12.112-tc-postimage`，14相关补丁全部应用成功，附 PATCHES.json；不是完整内核 prepare/构建。仅源码准备，无功能实现修改。
- 保留 wired BIND 的 CT 维护研究已收口为未选能力。部分 CT refs 下载成功；conntrack core 源码下载的自动审批两次超时，未获得函数体，不宣称其已核验；这不再阻断固定 CPU 稿。
- 原生子任务均已完成，文档已静止。下一步提交文档快照并新建 Astra r3 独立设计复审；通过后才实施，再做实际实现审查。
- 本轮恢复后未操作路由器、推送、编译或刷机；原生产分支仍干净且基线未变。重启后的实机状态仍待需要验证时重新只读核验。

## 工作区与版本

- 当前 worktree：`/home/illya/workspace/router/ax6000/worktrees/bridge-forwarding`。
- 当前分支：`fix/ax6000-bridge-forwarding`。
- 基线 / 当前 HEAD：`c196312d3916fad77fa0bb47db57e4b685c15f03`。
- 原生产 checkout：`/home/illya/workspace/router/ax6000/sources/immortalwrt-mt798x`，分支 `25.12`，同一 HEAD；暂停前检查工作区干净。
- 新 worktree 仅有本目录的未跟踪研究/设计材料，尚未提交；没有固件源码实现变更，也没有本轮推送、编译、刷机或发布。
- 本目录迁移材料见 `migration-manifest.json`；原 `ax6000/research` 与 `ax6000/sources` 继续作为只读历史、证据和源码基线。

## 已确认事实与目标

- 两轮故障中，平板从 AX6000 漫游到 GS7 后，Linux FDB 已指向有线上联，但 MTK 旧关联条目仍将回包直接送往旧无线 STA；旧条目自然老化后恢复。直接转发路径截走回包已证实，旧关联未及时清理的原因尚未确定。
- 关闭 k/v/r 后仍能复现，不能将现有证据归因于 802.11k/v/r。
- 用户要求面向所有普通 AP 终端的通用桥转发修复；固定 `.13` / `.32` 或双 MAC 方案已撤回。
- 目标是保留 RX 身份授权与隔离检查，将桥域普通 AP 数据交给 Linux 桥选择出口；缓存与快速路径不得绕过这一选择。
- 用户担心改动范围过大：每组改动必须说明根因修复依赖，区分必要合同与独立旧代码加固，形成可独立审查的提交。不新增撤回所有 Linux socket/backlog/qdisc 正常在途数据的目标。

## 设计与审查状态

- `roaming-bridge-final-design.md` 仍是 r2，尚未合并 r3；`roaming-bridge-final-design-r2-reviewed.md` 是原送审快照。
- Astra 对 r2 的结论为 **NO-GO**，见 `roaming-bridge-final-astra-review.md`。四个 P1 尚未经过新一轮复审放行。
- 已完成四份 r3 细化草案：
  - `bridge-r3-activation-contract.md`：整 BSS 默认阻流、prepare/读回/commit、配置租约与驱动内旧队列边界。
  - `bridge-r3-ser-contract.md`：持续 RESETTING 状态、跨 RTNL 窗口的恢复时序与确认。
  - `bridge-r3-hnat-lifecycle.md`：dispatcher、RCU/SRCU、资源生命周期与首版禁止破坏性热退出的边界。
  - `bridge-r3-packet-ownership.md`：可信 descriptor 来源、硬件副本源头消费、释放责任和能力边界。
- 三个原生子任务均已完成，仅产生上述设计文档；暂停时没有仍在运行的子任务，没有正在运行的 Astra 委派。

## 恢复后优先处理的缺口

1. 将四份 r3 草案整合为一致的主设计，明确收敛范围；同步修正旧细化报告，避免 r2 旧表述残留。
2. 硬件 keepalive 副本提前消费后，独立有线 PPE 绑定流的 conntrack 超时维护尚未闭合。现有 helper 的计数维护不足以证明 timeout 维护；不得悄然声称全部加速能力已保留，也不得未经取舍默认关闭全部 PPE。
3. SER 草案还需明确：生命周期状态转换使用专用受控短事务，不能借会拒绝 RESETTING 的普通 topology begin；最终数据路径 proof 在事务结束后获取。WiFi/FE 的实际 ACK 能力与失败退出须与源码吻合。
4. 配置租约仅解决本轮配置阻流与驱动内部旧队列，不扩展成撤回所有已进入 Linux 的在途数据。HNAT 热卸载、overlay 等限制须在方案中明确，并验证实际构建配置与入口。
5. 修订完成后，新建 Astra r3 设计复审。通过后再实施与测试，再做实际实现审查，通过后才推送 CI。当前不具有技术放行结论。

## 路由器与后续操作约定

- 最近确认运行的是 Build 19 漫游诊断版；本轮未更改路由器，暂停前没有日志抓取或路由器操作任务。
- 用户准备自行重启路由器。重启后的关联表、FDB、缓存、运行时设置不能沿用重启前结论；恢复且需要实机验证时，先重新进行只读状态核验。
- 历史抓包与故障证据已在本地 `ax6000/research/evidence` 保存，重启不影响这些本地材料。
- 不自动刷机或发布正式 Release。不持续跟踪 CI，保留用户通知编译完成的约定。
- 用户明确恢复前，只保留此检查点，不主动继续任务；此暂停条件现已解除。
