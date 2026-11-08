# r3：SER 短事务与持久后端状态

2026-11-09。修订 Astra r2 的 P1-2，并与 P1-3 生命周期合同对接。这里只修改设计，未修改驱动实现。改动限于本机受控 FE/Wi-Fi SER 的状态与必要顺序，不扩展为所有平台热移除或硬件无损恢复框架。

## 1. 准确 caller 与新增反证

已补齐 `sources/linux-6.12.112-ethernet-review-postimage/drivers/net/ethernet/mediatek/{mtk_eth_soc.c,h,mtk_eth_reset.c,h}` 四个选定文件。按 generic→platform 应用82个相关补丁，6个既有输入使用 GNU patch fuzz；PATCHES.json 记录，非完整内核编译。以下 E 指该目录；D/H 指此前42patch mt_wifi/14patch HNAT基线。

- `E/mtk_eth_soc.c:6176` 的 pending_work 在6184持RTNL，6190已经动硬件；6204释放RTNL等待Wi-Fi，6220重取；6283还有NETSYSv3的第二等待窗口。不能将任一 begin nesting depth跨这些 unlock。
- `:6229–6264` 先stop、mtk_hw_init、mtk_open重新开DMA，后恢复MAC；`:6273/:6277` 先通知Wi-Fi恢复，`:6288`才NAT_DONE。`mtk_open():4874–4894` 会先启动DMA并默认把GDMA指向PPE，必须改其恢复期选择，不能等NAT_DONE才声称资源安全。
- `H/hnat.c:1540–1584` warm_init包含清表/硬件初始化及重新开启GMAC→PPE，目前没有“只准备，不发布READY”边界。
- 当前目标 D 的 `hw_ctrl_basic.c:400/:411` 只处理 START_RESET/RESET_DONE，未处理 STOP_TRAFFIC/START_TRAFFIC；`D/hw_ctrl.c:1227/:1229` 回STOP完成后无期限等FE reset。不能盲用E的所有新事件为本目标承诺恢复。
- D最终N9恢复边界在 `hw_ctrl.c:1297–1326` 的 WAIT_N9_NORMAL / N9_NORMAL_STATE，调用ErrRecoveryEndDriverRestore。不能把较早RESET_DONE通知等同于Wi-Fi已经恢复完毕。

## 2. 两种互不替代的状态

**短 topology transaction** 只在同一段持RTNL的同步调用栈内存在。begin/end/abort都ASSERT_RTNL，嵌套只表示这条同步调用栈，退出前深度必须归零。任何rtnl_unlock前断言没有未结束短事务。

**持久 backend record** 独立保存 INIT/READY/QUIESCING/RESETTING/FAILED/DEAD、generation、reset_id、last_error及资源暴露状态；与 [HNAT生命周期](bridge-r3-hnat-lifecycle.md) 的状态表一致。RESETTING/FAILED保留slot，撤活动ops不代表硬件不存在。packet最终提交同时验证READY、原proof generation、相同稳定topology token；未READY时dispatcher仍按ownership合同处理普通包/副本，不访问FOE/MMIO。

RESETTING跨unlock保持，但短事务已经结束。别的link在RTNL下看见RESETTING，需该后端屏障时返回-EBUSY；不得当嵌套跳过prepare，也不得把空ops当无后端成功。严格无bridge-BIND的unlink沿既有单调性证明处理；设备remove不适用该证明。

后端生命周期转换使用单独的受控短事务入口：调用者持 RTNL，提供期望旧状态和 reset_id，经 core 验证后关 gate 并执行 entry 提交屏障，不递归调用会因本后端 RESETTING 而拒绝的普通 topology prepare。它不发布新的拓扑关系，也不能替其他 link 越过 RESETTING。所有退出仍配对结束短事务，任何 RTNL unlock 前深度为零。

## 3. 目标事件、序号与等待合同

本首版MT7986/7.6.7.3使用已实现的完整L1 START_RESET→RESET_DONE路径。E由hang detector提出的STOP_TRAFFIC请求在本目标正规化为START_RESET，不发送D不支持的START_TRAFFIC。该目标不新增一个未完成的轻量恢复协议；NETSYSv3专用路径不能因共用代码自动获准。

每次受控SER分配不复用reset_id，记录当前在线Wi-Fi participant的netdev引用及stage位。START只接受READY→RESETTING；相同id重复是幂等，不重复初始化work/completion；不同id的重入、非匹配DONE、失败后的DONE不得清FAILED。

停止/恢复ACK从匿名全局计数改为带reset_id的明确报告：D在接到START时保存id，停止完成和最终N9恢复各报告一次 `ack(dev, reset_id, stage, status)`。注册/participant快照及ACK更新在RTNL下，participant身份是实际netdev，不是客户端MAC。重复ACK不减计数，过期/未知participant拒绝。接口可由builtin Ethernet SER coordinator提供普通导出函数；不调用未导出的call_netdevice_notifiers_info，也不从普通notifier返回值推测硬件完成。

等待在RTNL外且有产品超时，初始内部deadline为每阶段5秒，需在实现/真机核定，不承诺无损时延。未收到所需ACK、participant离线、FE init/DMA open失败进入FAILED。增加明确ABORT结果：唤醒D的fe_reset_done等待者并带失败状态，D检查失败后不执行后续HIF reinit/DMA enable；不能用成功RESET_DONE解开失败等待。该调用与ACK携带相同id；completion在发送START之前初始化，不能事后重置丢ACK。

## 4. 正常顺序及锁上下文

1. **RTNL短事务A**：在原pending_work首次硬件prepare之前，READY→RESETTING，推进backend generation/reset_id，关闭新提交/硬件快消费并撤active ops，按entry_lock提交屏障处理已进入者；设置stopping禁止异步重新排队。结束事务A后才释放RTNL。此处不free DMA，不将软件token当硬件停机。
2. **RTNL外drain**：按生命周期合同完成network RCU/SRCU grace和所有running work/timer同步取消。等待期间可以有别的控制请求，但它们见持久RESETTING并明确失败或按受限unlink合同处理。不可握着RTNL等待会获取RTNL的worker，不保留全局transaction depth。
3. **重取RTNL，核id/状态**：确认仍为同一次RESETTING后，才执行保存配置、mtk_prepare_for_reset及发送Wi-Fi START_RESET。随后结束本段短操作，释放RTNL等停止ACK；不把这一等待包成短topology transaction。停止ACK不齐不得继续物理reset。
4. **重取RTNL，物理恢复准备**：保留原有必要硬件stop/reset次序，但检查mtk_hw_init与每次mtk_open结果。拆分HNAT warm_init为 `warm_prepare_checked(reset_id)`：所有原DMA资源保持所有权，清FOE/pending/MIB、cache请求及enable错误逐层检查；准备完成仍为RESETTING，无活动硬件ops，不恢复GMAC PPE。
5. **Ethernet先以明确PDMA入口恢复**：`mtk_open`在后端非READY/恢复期明确选择已有 `MTK_GDMA_TO_PDMA`，经mtk_gdm_config写入并读回。H现 `set_gmac_ppe_fwd(false)` 已显式写 CPU_PDMA，不再靠“PPE port link down自然转CPU”推定。所有数据面拒绑定；硬件表已准备成功后才能通知Wi-Fi FE RESET_DONE并等最终N9恢复ACK。若WED恢复期间可送PPE，只能面对保留且已初始化的空表，不开放BIND。
6. **Wi-Fi恢复ACK窗口**：保持RESETTING、资源持有及generation，释放RTNL等待所需ACK。参与者实际恢复完成前不READY。原V3专用第二窗口不直接套用目标能力；本目标的新最终ACK使用前述已核N9边界。
7. **RTNL短事务B，唯一commit**：通过专用生命周期入口确认id/状态、全部ACK、表/cache检查、无旧pending、registry与当前拓扑一致。全部通过才READY、发布ops、恢复允许的GMAC PPE路径和worker，结束事务并发布新的稳定 topology token。事务内 token 为过渡态，不在这里创建可用于数据面的路径 proof；后续新包必须在事务结束后、实际路径资格检查之前重新捕获稳定 token，不能复用 reset 开始时的 token 或给旧 pending 换新 token。结束短事务后清FE resetting标记，执行原必要WED reset-complete/QDMA恢复收尾。收尾若出现可检测错误仍转FAILED，不按一条旧NAT_DONE自动成功。

原 MTK_FE_RESET_NAT_DONE只能作为可观测完成通知，不能在普通notifier中再次调用会自动恢复PPE的旧warm_init。控制错误由实际caller直接接收并传播。不存在START但收到NAT_DONE、错误id或FAILED后重发都不触发重建/READY。

## 5. 失败与恢复出口

| 失败点 | 确定动作 |
| --- | --- |
| 进入RESETTING前校验失败 | 不开始SER；保持已知原状态并返回错误 |
| RTNL外drain失败、停止ACK缺失 | sticky FAILED，发带id的ABORT唤醒等待者；不进入物理reset/不free已暴露DMA |
| FE init、checked warm/cache、PDMA选择读回失败 | sticky FAILED；不发成功恢复通知，不READY、不enableGMAC PPE，保留资源及core错误slot |
| 某端口open或最终Wi-Fi恢复ACK失败 | FAILED、阻新提交，标明确受影响接口未恢复；停止其新用户数据调度，不追加一个未经证明的free路径 |
| 过期/重复事件、FAILED后enable/DONE | 拒绝或幂等报告，不能重新启work、换表或清错误 |

失败后不保证网络持续可用；也不谎称旧硬件流已全部转CPU。资源由常驻后端及builtin Ethernet owner持有直到受控重启，符合P1-3的首版维护边界。正常BSS reload、桥成员更新不因此拥有HNAT DMA回收权限。物理reset失败不通过注销backend伪造“无后端，可以安全link”。

## 6. 实施面与关闭验收

- E：pending_work前后顺序、mtk_open恢复期GDMA选择、mtk_eth_reset coordinator/reset_id/participant ACK、目标支持事件与ABORT。
- H/core：持久state/generation、短txn与ASSERT_RTNL、checked warm_prepare/唯一commit、所有最终BIND门、registry/worker排空，不再由NAT_DONE直接warm_init。
- D：START记录id、停止ACK、FE等待有界且检查ABORT、N9最终ACK。保留既有WPA/无线加密，不顺带重写SER所有硬件阶段。
- 故障注入：每个rtnl_unlock窗口并发br_add_if，须busy且不能跳过prepare；重复/过期ACK不改变当前计数；无START的DONE不READY；worker正在运行、warm/cache/DMA open失败、超时ABORT均保资源、不越门。强制检查所有短事务深度在unlock前归零。
- 正常MT7986完整构建与受控SER验证后，才能认定恢复能力通过。软件generation、一次读回或未触发故障均不能代替硬件完成验证。

此设计修订按已核实际caller提出可执行顺序，仍须Astra复审；尚未实现，也不宣称P1已被独立放行。
