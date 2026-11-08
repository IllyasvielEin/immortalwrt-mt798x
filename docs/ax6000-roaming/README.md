# AX6000 桥转发修复：独立开发工作区

**用户已明确恢复，当前修订 r3.1 的唯一 WARP STOP 阻断，尚未进入生产实现。暂停时的检查点、未解决问题与恢复顺序见 [PROGRESS.md](PROGRESS.md)。**

当前工作区：`/home/illya/workspace/router/ax6000/worktrees/bridge-forwarding`。

分支：`fix/ax6000-bridge-forwarding`，基线：`c196312d3916fad77fa0bb47db57e4b685c15f03`。原仓库 `ax6000/sources/immortalwrt-mt798x` 的 `25.12` 保持原检出和文件状态；本任务后续源码、设计、测试和审查修订均在新 worktree 进行。

用户要求直接推进所有普通 AP 终端适用的通用修复；已撤回固定双 MAC 候选。原研究材料按 `migration-manifest.json` 复制到本目录，原路径仅作为历史归档与证据来源，不继续写入。原始日志和只读源码基线保存在 `/home/illya/workspace/router/ax6000/research/evidence` 与 `ax6000/sources`，导入报告中的历史引用仍指向这些归档。

当前设计状态：[r3 Astra 审查](roaming-bridge-final-astra-review-r3.md) 已关闭原配置阻流、HNAT 常驻资源和硬件副本三项 P1；仅剩实际 WARP STOP 错误被吞掉、未确认停机就释放 DMA 的 P1。当前 [r3.1 修订稿](roaming-bridge-final-design-r3.md) 保留固定软件转发、WED与在线L1 SER，只补这条停流/失败终态及明确P2；尚未独立放行或实施。

后续顺序：修订设计 → Astra 复审 → 实现与测试 → Astra 实现审查 → 推送 CI。生产补丁不混入旧 043 诊断，不改变 uBoot/分区，不自动刷机或发布正式 Release；沿用用户通知 CI 完成、不持续查询的约定。

范围控制：每组变更写明与直接转发故障及必要安全合同的依赖，按独立可审提交推进。保留“已进入 Linux 或已提交硬件的数据属于正常在途”的边界，不自行增加撤回所有 socket/backlog/qdisc 数据的目标。新增维护限制、功能关闭和接口改动必须在设计与实现审查中显式列出，不由文档假定已获技术放行。

本目录的 `roaming-bridge-final-design-r2-reviewed.md` 保留原送审版本；`roaming-bridge-final-design.md` 指向当前r3.1，原r3可从Git `0de772dc`读取。接口细节为activation/cpu-profile-activation/cpu-profile-ser与新增 `bridge-r3-warp-stop-contract.md`，按主稿裁剪生效。主稿拟选后者终态B：无法确认DMA停止或无法安全串行的销毁交叉触发noreturn紧急硬重启；已说明该维护代价，尚未验收。`bridge-r3-hnat-lifecycle.md`、`bridge-r3-ser-contract.md`及保留有线BIND账户部分是未选能力研究，不作为附加实施；不采用比较稿取消在线FE恢复的候选。
