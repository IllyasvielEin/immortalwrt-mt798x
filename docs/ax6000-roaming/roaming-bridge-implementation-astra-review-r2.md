# AX6000 通用桥转发：实际实现独立复审 R2

2026-11-09，Asia/Singapore。

**裁决：GO，仅允许将当前隔离分支推送进入一次完整生产 CI 编译。上轮两项 P1 和一项 P3 均已实质修正；在本轮检查范围内，未发现仍须阻止该步骤的具体实现问题。** 这不是 CI 已通过、刷机、发布、硬件停止语义或性能验收结论。本轮没有执行推送或任何 CI 操作。

## 1. 对象、基线与执行边界

- 唯一工作目录：`/home/illya/workspace/router/ax6000/worktrees/bridge-forwarding`；分支 `fix/ax6000-bridge-forwarding`；审查 HEAD `33c74ea5d77a897de5996aa9d56a21f67504878d`。开始审查时工作树干净。
- 先读 [上轮实际实现审查](roaming-bridge-implementation-astra-review.md) 与 [PROGRESS.md](PROGRESS.md)。上轮实际实现是 `692a32bd`，`550fa6eb`、`7ef5cd27` 仅为文档。本轮复审差异是 `7ef5cd27..33c74ea5`：`0aae3209` 修 WARP，`f51369f5` 修 netifd，`34de212a` 修 fq_en，`33c74ea5` 仅更新进度。
- 完整实现可追溯 `4a0c52dd..HEAD`；生产基线为 `c196312d3916fad77fa0bb47db57e4b685c15f03`。未进入或修改原生产 `25.12` checkout。
- 自行完成，未启动子代理、联网、操作设备、提交、推送、查询或触发 CI。只新增本报告，保留原报告；未修改实现或测试源码，未重复 ARM64 构建。宿主测试自行产生和清理临时编译材料。

下文 W=`.analysis/warp-next`，N=`.analysis/netifd-next`，D=`.analysis/driver-next/mt_wifi`，K=`.analysis/kernel-next`，均位于上述 worktree。源码行号指本轮真实 postimage。设计 GO 只作为预期，不作为以下关闭依据。

## 2. P1-1：消息节点、同步回调与 caller 寿命——关闭

生产补丁位置：[WARP100:760](/home/illya/workspace/router/ax6000/worktrees/bridge-forwarding/package/mtk/drivers/warp/patches/100-ax6000-checked-warp-ser.patch:760)、[WARP100:1012](/home/illya/workspace/router/ax6000/worktrees/bridge-forwarding/package/mtk/drivers/warp/patches/100-ax6000-checked-warp-ser.patch:1012)、[WARP100:1146](/home/illya/workspace/router/ax6000/worktrees/bridge-forwarding/package/mtk/drivers/warp/patches/100-ax6000-checked-warp-ser.patch:1146)。

实际发送链仍是 W `warp_checked.c:251` → `mcu/warp_woctrl.c:487` 的 `warp_checked_wo()` → `mcu/warp_msg.c:684` 的 `warp_msg_send_cmd()` → `woif_bus_tx()`。响应由 W `mcu/warp_wo.c:475` → `mcu/warp_cmd.c:63` → `warp_msg_handle_rsp()` 进入。`warp_checked_wo():508` 仍仅接受零成功，timeout 保留 `-ETIMEDOUT`，其他失败转换为远端错误；成功之后还检查预算，未将“命令已发出”改成停止证明。

### 所有权和交错

| 阶段 | 真实源码与审查结果 |
| --- | --- |
| 创建/入队 | [W/mcu/warp_msg.c:319](/home/illya/workspace/router/ax6000/worktrees/bridge-forwarding/.analysis/warp-next/mcu/warp_msg.c:319) 创建 sender 引用 1；`:352` 在 wait_lock 下检查队列状态，`:363` 增加队列引用后发布节点。入队拒绝只需释放 sender 引用。 |
| 响应认领 | [W/mcu/warp_msg.c:402](/home/illya/workspace/router/ax6000/worktrees/bridge-forwarding/.analysis/warp-next/mcu/warp_msg.c:402) 在同一锁下查找并摘队，队列引用转交响应方。它不是一个可被 sender 超时无条件释放的裸指针。 |
| timeout / send 失败 | [W/mcu/warp_msg.c:389](/home/illya/workspace/router/ax6000/worktrees/bridge-forwarding/.analysis/warp-next/mcu/warp_msg.c:389) 按节点身份取消；只有确实摘到本节点才释放队列引用。`:730` 的取消之后，`:734` 只释放 sender 引用。响应已认领时保留其引用；timeout 先摘队时迟到响应找不到节点。 |
| 响应写入/完成 | [W/mcu/warp_msg.c:522](/home/illya/workspace/router/ax6000/worktrees/bridge-forwarding/.analysis/warp-next/mcu/warp_msg.c:522) 检查消息长度，写节点私有 status/payload，随后 complete，再释放响应引用。payload 分配失败发布 `-ENOMEM` 并唤醒；不把失败当成功或调用 callback。 |
| 队列退出 | [W/mcu/warp_msg.c:579](/home/illya/workspace/router/ax6000/worktrees/bridge-forwarding/.analysis/warp-next/mcu/warp_msg.c:579) 在锁下设 UNKNOWN、摘走整个等待链，逐个发布 `-ESHUTDOWN`、complete 并释放队列引用。sender 引用仍在；此前已被响应方认领的节点不在该链上，不会被重复完成或释放。 |
| 最后释放 | [W/mcu/warp_msg.c:337](/home/illya/workspace/router/ax6000/worktrees/bridge-forwarding/.analysis/warp-next/mcu/warp_msg.c:337) 仅在引用归零时释放私有 payload 和节点；对 payload 先判非 NULL，避免 vendor allocator 的 NULL-free 报错。 |

sender 成功等待之后才读已发布的数据，sender 自身引用覆盖复制和 callback；响应引用覆盖 complete 调用及其返回。即使 response 在发送 transport 返回前完成，或 transport 报错时 response 已摘队，两方也各自只释放其所有权。退出没有通过无限等待响应来补生命周期，原等待预算仍有界。

### caller 与回调

[W/mcu/warp_msg.h:57](/home/illya/workspace/router/ax6000/worktrees/bridge-forwarding/.analysis/warp-next/mcu/warp_msg.h:57) 的节点已不存 callback、user_data 或 caller 输出地址。[W/mcu/warp_msg.c:491](/home/illya/workspace/router/ax6000/worktrees/bridge-forwarding/.analysis/warp-next/mcu/warp_msg.c:491) 仅在等待成功且 status 为零时复制输出，并在**发送线程返回之前**调用同步 callback。因此 timeout/send 失败返回后，迟到 response 即使继续运行，也只接触自身持有引用的节点。

特别核对了 [D/embedded/plug_in/warp_proxy/warp_proxy.c:655](/home/illya/workspace/router/ax6000/worktrees/bridge-forwarding/.analysis/driver-next/mt_wifi/embedded/plug_in/warp_proxy/warp_proxy.c:655)：它传入栈上的 `&cmd_id`，回调 `:621` 读取这个地址。新版 callback 在该函数返回前执行，确实修复了 caller 栈寿命，而非只保护 node。W `warp_main.c:309` 的 RRO 配置、`mcu/warp_woctrl.c:487` 的 checked 状态请求也使用栈命令，响应侧不再保留其地址。W `warp_proc.c:1186` 的 RXCNT 同步 callback 已检查；该 proc 写入口在本 CPU profile 下由 `:1079` 拒绝，不能误称它是 AX6000 的活动调用路径。

检查 external WARP 及 D 的生产 proxy 内全部 `warp_msg_send_cmd` 调用，没有发现 `WAIT_NONE+handler`；HALT、异常日志发布、ring dump 等无回调 NONE 用法仍可发送。无生命周期契约的 NONE+handler 在 W `mcu/warp_msg.c:697` 明确返回 `-EOPNOTSUPP`，接口说明位于 W `mcu/warp_msg.h:105`。这是明确收窄的 API 合同，不是对未检查树外调用者的兼容保证。

退出上层也已核：W `warp_main.c:530` 先 checked shutdown，`mcu/warp_wo.c:1015` 将 CCIF stopping 与发送锁同步，随后 quiesce/kill RX tasklet；`warp_main.c:577` 的 WO 退出先于 `:583` 的消息退出。节点引用修正没有替代或取消这些父对象/transport 的停止前提，也未声称仅靠 node 引用便可支持任意并发模块卸载。

### 回归证据

本轮重跑 `tests/ax6000_bridge_warp/run.py` 通过。新 [test_msg.py:32](/home/illya/workspace/router/ax6000/worktrees/bridge-forwarding/tests/ax6000_bridge_warp/test_msg.py:32) 逐字节复制完整真实 `warp_msg.c/h/cmd.h`；pthread、completion、refcount、allocator 和 transport 是宿主边界适配，队列/节点逻辑不是同名替身。

[test_msg.c:109](/home/illya/workspace/router/ax6000/worktrees/bridge-forwarding/tests/ax6000_bridge_warp/test_msg.c:109) 固定响应摘队后暂停、sender timeout 并释放 caller buffers、响应继续的交错，并覆盖相反次序、成功 callback 的发送线程、transport 失败竞争、退出、OOM、长度错误及 NONE 合同。ASan/UBSan 通过，节点/payload 分配计数归零；runner 关闭了全局 LeakSanitizer，不将它说成完整泄漏扫描。读取旧日志 `.analysis/msg-old-regression.log`，同一真实旧源码交错在 `warp_msg_handle_rsp:617` 报 heap-use-after-free；本轮未重复旧版崩溃运行。宿主适配不证明真实内核调度的全部交错。

## 3. P1-2：普通 reload PREPARE 后无收尾——关闭

生产补丁位置：[netifd900:142](/home/illya/workspace/router/ax6000/worktrees/bridge-forwarding/package/network/config/netifd/patches/900-ax6000-transactional-ap-bridge.patch:142)、[netifd900:357](/home/illya/workspace/router/ax6000/worktrees/bridge-forwarding/package/network/config/netifd/patches/900-ax6000-transactional-ap-bridge.patch:357)、[netifd900:829](/home/illya/workspace/router/ax6000/worktrees/bridge-forwarding/package/network/config/netifd/patches/900-ax6000-transactional-ap-bridge.patch:829)。

入口仍是 N `ubus.c:43` → `main.c:276` → [N/config.c:798](/home/illya/workspace/router/ax6000/worktrees/bridge-forwarding/.analysis/netifd-next/config.c:798)。UCI network 加载失败在 begin 前返回；成功加载后 `:811` 开始批次，依次经过设备、VLAN、接口、配置处理、旧设备整理、`device_init_pending()`、interface flush/start，以及最后的 ucode config_start，至 `:836` 才结束批次。没有在 `device_apply_config()` 的单个成功返回处直接放行。

无变化 bridge 仍由 N `device.c:1217` → `bridge.c:1093` 对实际成员 AP PREPARE。修正落在 [N/ap-bridge.c:115](/home/illya/workspace/router/ax6000/worktrees/bridge-forwarding/.analysis/netifd-next/ap-bridge.c:115)：`ap_bridge_owner():47` 沿 wireless_parent 找实际 AP owner；本轮首次触及、原先 committed、非 controlled 的 owner 登记 touched/pending，取消早期 retry，清理上一次尝试状态并重新 PREPARE。simple AP 的 `DEV_CONFIG_NO_CHANGE` 同样经过此登记，不再依赖成员变化事件偶然补 COMMIT。

批内 [N/ap-bridge.c:325](/home/illya/workspace/router/ax6000/worktrees/bridge-forwarding/.analysis/netifd-next/ap-bridge.c:325) 的 reconcile 不排自动 COMMIT；`:313` 的旧 timer 即使进入回调也受 active/controlled/pending-error 检查约束。结束阶段 `:351` 只遍历实际仍存在的、符合上述条件的 owner，成功后排 timer；timer 再做 fresh PREPARE，由 `:199` 的真实 commit 收集 base 与全部现存子域、bridge/VLAN/port flags，逐域 STAGE 并 COMMIT，核 READY、lease、driver/profile-ready。成功才清 `reload_pending`。这恢复的是完整快照和新 lease，不是裸 READY。

错误链也已沿真实调用者检查：

- N `config.c:268`、`:274`、`:280`、`:352` 等把设备创建/应用失败登记到批次；N `ap-bridge.c:29` 保存首个非零 batch 错误，`:305` 的 fault 同时记录 owner 错误。成功的零值 fault 不清错误。
- N `bridge.c:217`、`:255`、`:434`、`:679` 分别覆盖成员 VLAN、本地 VLAN、bridge 删除及成员启用失败；VLAN 子设备的失败也通过 ap_bridge_fault 回传。失败 PREPARE 不被当作“已经阻流”，其调用者拒绝继续相关修改。
- 实际成员删除链 N `bridge.c:333` → `:347` 的 DELIF 失败记录 fault 并保留 active；`:723` 保留 retired 成员及下一次删除重试。end 将 batch 错误传播给本轮触及的 owner，旧 timer/reconcile 不会清错并发布旧策略。
- 下次**相关** reload 在首次触及该 committed、非 controlled owner 时重新 PREPARE，再实际重做成员操作；成功批次方能排完整快照 COMMIT。仅到期的旧 timer 不等于配置错误已经修复。
- N `ap-bridge.c:291` 的无线 `prepare_new()` 保留 controlled 所有权，batch finish 与 timer 均跳过它。未触及的独立 AP 不因 device_foreach 本身被 PREPARE/COMMIT；free 仍在 `:257` 取消 owner timer，无新增批次裸指针列表。

不把 `config_init_all()` 返回零说成所有 AP 已同步 READY：批次成功表示可以排后续完整确认，COMMIT 仍可能失败并保持阻流。无线状态为 up、配置未变时，`wireless-device.uc:402` / `:427` 仍不会主动重建事务；本次修正由 C 批次真正补齐该路径，不靠无线重启兜底。

无 mode 文件时，N `ap-bridge.c:35`/`:343` 不启用 batch，`:29` 不累积新增错误，`:363` 返回零，ap_prepare/commit 保持原 no-op 分支。本轮未将 CPU batch 的负返回扩散为其他产品新的 network reload 错误码；N `config.c` 新增的空指针/失败提前返回也已检查。此结论不等于已构建或运行其他产品。

本轮重跑 [run_netifd_reload.py:17](/home/illya/workspace/router/ax6000/worktrees/bridge-forwarding/tests/ax6000_bridge_control/run_netifd_reload.py:17) 通过：链接当前 native netifd 各实际对象、真实 UCI/device/bridge/VLAN/ap-bridge，外部 system、ubus、ucode peer 和 ioctl/ifindex 由适配器隔离。核对 CMake 源目录指向 N，相关对象时间晚于修正源码；没有拿旧对象的结果代称新版已测。

[test_netifd_reload.c:111](/home/illya/workspace/router/ax6000/worktrees/bridge-forwarding/tests/ax6000_bridge_control/test_netifd_reload.c:111) 经过实际 `config_init_all()`，覆盖 up/committed hotplug AP 无变化 reload、多个 BSS 与真实 VLAN 子设备、simple AP NO_CHANGE、实际 DELIF 失败后的保留与修正重试、独立 AP 不被触及及 controlled 不被代 COMMIT；`:62` 明确拒绝整个 reload 返回前出现 COMMIT。旧 `.analysis/netifd-reload-before-p1-2-test.log` 在无变化场景的 READY/new-lease 断言失败，本轮读取了该证据，没有重复旧版运行。

此外重跑 `test_control.py` 的 12 项和 `test_ucode.py` 的 8 项，均通过；真实 activation 函数已覆盖 batch 成功/失败/修正、controlled 和 missing-mode。完整 native runner 缺依赖会报错，不 skip；其 system-dummy 和 peer 替身仍不能验证真实内核拓扑、设备重建和全部 daemon/ubus 交错。

## 4. P3-1：fq_en 的 BOOLEAN-style 分派——关闭

[D/embedded/common/fq_qm.c:1024](/home/illya/workspace/router/ax6000/worktrees/bridge-forwarding/.analysis/driver-next/mt_wifi/embedded/common/fq_qm.c:1024) 的 CPU profile 分支现在返回 FALSE，对应 [无线044:2346](/home/illya/workspace/router/ax6000/worktrees/bridge-forwarding/package/mtk/drivers/mt_wifi/patches-7673/044-ax6000-universal-ap-bridge.patch:2346)。实际注册仍为 D `embedded/ap/ap_cfg.c:2460` 的 `fq_en`；[ap_cfg.c:3661](/home/illya/workspace/router/ax6000/worktrees/bridge-forwarding/.analysis/driver-next/mt_wifi/embedded/ap/ap_cfg.c:3661) 的 `if (!set_proc(...))` 将其转换为 `-EINVAL`，不再把负 errno 当 TRUE。

CPU 分支在解析参数、改变 FqEnable 或退出/替换 QM/TM 前返回；非 CPU 的 `#else` 体本轮未改。此限制与软件 HTB/IFB eQoS 无关。

本轮 driver suite 通过。[run.py:31](/home/illya/workspace/router/ax6000/worktrees/bridge-forwarding/tests/ax6000_bridge_driver/run.py:31) 抽取真实叶函数、实际注册项和完整 `RTMPAPPrivIoctlSet()`；[test_setter.c:55](/home/illya/workspace/router/ax6000/worktrees/bridge-forwarding/tests/ax6000_bridge_driver/test_setter.c:55) 验证不同 fq_en 参数及大小写返回错误，并核对 copy/alloc 失败和无状态修改、分配释放平衡。读取旧 `.analysis/driver-review-old-setter-reproducer.log`，同一 dispatcher 回归在第一条 fq_en 断言失败；不再用叶节点负值断言证明 ioctl 行为。

## 5. 独立一致性检查与验证范围

本轮核对全部六个生产补丁 SHA256 与 `.analysis/production-patch-replay.json` 一致，并自行将补丁从准确 `*-base` **在内存按原行精确重放**，逐段检查上下文/删除内容，再逐字节比较 `*-next`；无 fuzz、无偏移猜测、无 postimage 差异，也没有向源码树写入重放结果。核对触及文件数：三内核补丁 26，WARP 21，无线 37，netifd 13。这个计数是本轮补丁触及文件数，不冒充既有重放日志中扫描的全部源码文件数。

同时比较已保存的各修正前源码与当前 C/H：WARP 仅 `mcu/warp_msg.c/h`，netifd 仅 `ap-bridge.c/config.c/device.h`，driver 仅 `embedded/common/fq_qm.c` 改变。三内核生产补丁、WO HALT/停止/reset 实现、其他 driver 数据面、eQoS、mode 打包及 CI 定义本轮不变。

| 本轮执行 | 结果与边界 |
| --- | --- |
| `UV_OFFLINE=1 UV_CACHE_DIR=/tmp/router-uv-cache uv run --no-project tests/ax6000_bridge_warp/run.py` | 通过；实际 control/MMIO、173 项 proxy/权限检查及完整消息生命周期回归。 |
| 同环境运行 `tests/ax6000_bridge_driver/run.py` | 通过；wire/binding、授权、并发 token 以及真实 fq_en dispatcher。 |
| 同环境运行 `tests/ax6000_bridge_control/run_netifd_reload.py` | 完整 native reload 回归通过；本地 pinned prefix 可用，无 skip。 |
| 同环境分别 unittest discover `test_control.py`、`test_ucode.py` | 12 + 8 项全部通过。 |
| 六补丁 SHA/准确基线内存重放；`git diff --check 4a0c52dd..HEAD` | 通过。 |

读取 `.analysis/warp-review-fix-build.log`、`.analysis/driver-review-fix-build.log`，尾部确有 WARP、mt_wifi、proxy 模块链接和 MODPOST 完成，仍有 vendor 警告。这是既有 ARM64 构建证据，本轮未重新构建。实际 config/artifact/mode 与 production guard 的此前通过记录沿用 PROGRESS，**本轮未重新执行这些检查**。

本轮深查范围是三项修正及其上述 caller→leaf、资源/错误路径；未对全部 `4a0c52dd..HEAD` 未变实现再做一次首轮同等覆盖的审查，也未重跑未改内核套件或顶层全部回归。没有把原报告的全实现审查范围冒称为本轮重新完成。

## 6. 剩余阻断与非阻断事项

**本次 CI 准入无剩余阻断，三项已关闭问题不再需要最小修正。** 未发现新的、有具体可执行路径支持的实现缺陷；不据此承诺所有未覆盖情形无缺陷。

以下为已知非阻断边界及最小后续动作，不将它们重新包装为本轮 P1：

| 事项与准确位置 | 对本结论的影响；最小后续动作 |
| --- | --- |
| [CI 工作流:264](/home/illya/workspace/router/ax6000/worktrees/bridge-forwarding/.github/workflows/build-redmi-ax6000.yml:264)、[CI 工作流:266](/home/illya/workspace/router/ax6000/worktrees/bridge-forwarding/.github/workflows/build-redmi-ax6000.yml:266)；[native runner:12](/home/illya/workspace/router/ax6000/worktrees/bridge-forwarding/tests/ax6000_bridge_control/run_netifd_reload.py:12) | WARP runner 已包含新 msg 回归，driver runner 已包含 setter；当前 control CI 只发现 12 项 `test_control.py`，其中有 batch helper 回归。**完整 native reload 和 8 项真实 ucode 仍为独立本地门**。保留本轮独立通过记录即可进入本次 CI；持续集成的后续最小补充是准备 pinned native/ucode 依赖并显式调用两个入口，缺依赖须失败，不能写成当前 CI 已覆盖全部 20 项或 native reload。 |
| [W/mcu/warp_msg.c:697](/home/illya/workspace/router/ax6000/worktrees/bridge-forwarding/.analysis/warp-next/mcu/warp_msg.c:697)、[W/mcu/warp_msg.h:105](/home/illya/workspace/router/ax6000/worktrees/bridge-forwarding/.analysis/warp-next/mcu/warp_msg.h:105) | NONE+handler 明确不支持；已检查 external WARP/生产 proxy 没有该组合。当前分支无需为未发现的树外调用者恢复不安全旧行为；若其他产品有此调用，先核对实际 caller，再使用同步路径或单独设计具备 user_data 所有权/释放合同的异步 API。 |
| [N/ap-bridge.c:343](/home/illya/workspace/router/ax6000/worktrees/bridge-forwarding/.analysis/netifd-next/ap-bridge.c:343)、[D/fq_qm.c:1026](/home/illya/workspace/router/ax6000/worktrees/bridge-forwarding/.analysis/driver-next/mt_wifi/embedded/common/fq_qm.c:1026) | netifd batch 按 mode、fq_en 按 feature 隔离；WARP 消息修正是共享代码。缺 mode 宿主用例通过不等于其他产品验收。后续对实际使用这些共享路径的产品做编译/回归，不以 AX6000 单产品链接成功替代。 |

## 7. 本轮没有改变的后续门

1. **一次完整生产 CI：** 最终 GCC 14.3/OpenWrt prepare、defconfig、依赖、build、image、实际 rootfs 与模块产物；核对 kernel/D/W/proxy/netifd/ucode、external WARP/rbus 选择、RRM/WNM/FT、skb extension、mode 及禁止 force-unload/overlay 等设置。本地 GCC 13.3、手工组合内核配置及 native 链接仅为诊断证据。
2. **另获授权的硬件停止/复位验收：** 冷启动、在线 L1、正常关闭/重开、失败恢复，FOE 空表/固定 CPU profile、无 BIND/cache/private producer，基础 WED/token/crypto/reorder；核 WO 状态应答、HALT_DONE、CCIF、busy 位和 DMA 静止的真实含义。正常关闭仍为 ENABLE→HALT；W `mcu/warp_wo.c:917` 虽允许异常 SER_RESET 来源，也必须等新鲜 HALT_DONE，不能据此宣称固件支持该转换。PSCI/WDT/ATF 的真实复位范围、完成及可能需断电恢复仍待验收。
3. **转发与动态配置验收：** 普通终端漫游、旧 STA 尚存时的 FDB/桥选端口/物理出口/回包，IPv4/IPv6、ARP/ND、非 IP L2、group、同 MAC 不同 VID、隔离/VLAN、聚合/BA、reload 与失败重试。重启后重新读取状态，不沿用重启前关联/FDB。
4. **性能/eQoS：** 有线、无线、LAN/WAN、纯桥的吞吐、CPU、时延及重配置停顿；HTB/IFB、默认叶、零设备限速、规则边界、stop/restart。本轮没有性能测量，不承诺原峰值或损失比例。

终态 B 及其代价没有改变：无法确认停止或停止证明失效时，仍须遵守已记录的同步 noreturn 故障出口；普通配置失败不因此变成整机复位。先前关于 WO 异常转换、硬件静止、复位范围、性能和其他产品的限制全部保留。

本 R2 对 `33c74ea5` 的 CI 准入裁决取代上轮实现 NO-GO；原报告继续作为问题与修正依据保留。允许的下一步仅为推送隔离分支进入一次 CI 编译，其后仍须依据实际 CI 结果继续验收。
