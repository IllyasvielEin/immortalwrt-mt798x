# AX6000 通用桥转发最终设计

当前修订版本：[r3.1：固定软件转发](roaming-bridge-final-design-r3.md)。这是唯一当前主合同，尚未获 Astra 放行，未实施。原 r3 快照 `0de772dc` 的 [独立审查](roaming-bridge-final-astra-review-r3.md) 关闭三项原 P1，仅剩 WARP STOP 的实际错误传播与 DMA 释放边界。

r2 原送审稿保存在 [不可覆盖快照](roaming-bridge-final-design-r2-reviewed.md)，其 [Astra 审查结论](roaming-bridge-final-astra-review.md) 为 NO-GO。r3 拟以固定软件路径收敛改动，保留 MT7986 在线 L1 SER；不沿用 r2 全局 topology epoch/动态 backend/保留独立有线 BIND 的实现范围。

工作区、分支和验收顺序见 [README](README.md)；暂停检查点保留在 [PROGRESS](PROGRESS.md)。
