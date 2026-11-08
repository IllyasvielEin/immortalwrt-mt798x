# AX6000 通用桥转发进度

更新时间：2026-11-09，Asia/Singapore。用户已经明确恢复任务；本文件替代早期暂停记录。

## 当前状态

Astra 实际实现 R2 复审 **GO，仅限推送隔离分支进入一次完整生产 CI**，见 [R2 复审报告](roaming-bridge-implementation-astra-review-r2.md)。审查 HEAD `33c74ea5`；两项 P1 和一项 P3 均已关闭，在复审范围内未发现新阻断。该结论取代首轮 `692a32bd` 的 [NO-GO 报告](roaming-bridge-implementation-astra-review.md)，原报告保留为问题与修正依据。完整 CI、硬件及性能验收仍待完成，GO 不包含刷机或正式发布许可。

本次只将 `fix/ax6000-bridge-forwarding` 推至 `origin`，运行 `build-redmi-ax6000.yml`，输入 `go_bootstrap=external`、`cold_cache=false`。包含本复审报告后的提交仅增加文档，生产实现保持已审查内容。触发回执、实际源码 SHA 和运行 URL 保存在本地 `.analysis/bridge-ci-dispatch-20261109.json`；不持续轮询，等待用户通知编译结果。

第一次 Astra 实现审核被平台以“possible cybersecurity risk”过滤中止，未产出技术审查结论。该执行失败不代表代码 GO 或 NO-GO；仍保留审核通过后才推送编译的顺序。重提审核聚焦同一代码范围的路由器转发、资源生命周期、配置和编译正确性，不改变审查标准。

重提审核已完成，除了上述 P1，还发现 `fq_en` 的 BOOLEAN-style setter 返回负 errno 被 ioctl 分派当作成功（P3）。本轮一并改为 FALSE，并通过真实注册项与 ioctl 分派验证调用返回错误；原实现对同一回归失败，修正后通过。

## 实现审查修正

- `0aae3209`（P1-1）：消息节点分别由发送方、队列/已认领的响应方持有引用；超时只撤销仍在队列的节点，不释放响应方引用。响应只写节点私有缓冲，输出复制和同步回调在成功等待的发送线程返回前执行，避免迟到响应接触调用者栈或缓冲。发送失败、队列退出、payload 分配失败都按相同所有权处理，等待仍有界。`WAIT_NONE` 无回调保留；已检查 external WARP 与生产 proxy 调用者未使用 `WAIT_NONE+handler`，该无生命周期契约组合明确返回 `-EOPNOTSUPP`，不承诺其他树外调用者兼容。
- `f51369f5`（P1-2）：CPU profile 的 `config_init_all()` 建立一次同步 reload 批次，仅登记本轮 PREPARE、原先 committed 且不受无线 controlled 事务管理的实际 AP owner。所有设备/成员/VLAN/配置阶段结束后才安排完整快照与 COMMIT；批内任何相关失败保持阻流和错误，不由旧 timer 擅自清除。修正配置后的下一次相关 reload 重新 PREPARE。没有 mode 文件时不启用批次、不改变旧产品返回语义。
- `34de212a`（P3）：`fq_en` 返回 FALSE，由原有 BOOLEAN-style ioctl dispatcher 转为错误；真实注册项和完整 `RTMPAPPrivIoctlSet()` 路径回归覆盖，不再只断言叶节点 errno。

修正版使用完整真实 `warp_msg.c/h` 的确定性线程交错回归通过；同一交错在旧 postimage 捕获 `warp_msg_handle_rsp:617` 的 ASan UAF，见 `.analysis/msg-old-regression.log`。旧源码保留在 `.analysis/warp-before-p1-1`；新测试还覆盖超时先摘队、发送失败时响应已认领、队列退出、payload OOM、同步回调线程与 caller 缓冲寿命。host 适配器不等于真实内核调度。

真实 netifd/UCI 整体 reload 回归在旧 postimage 稳定复现永久 BLOCKED，修正版通过无变更、多 BSS/实际 VLAN、simple AP `NO_CHANGE`、成员删除失败与修正重试、未触及独立 AP 和 controlled 无线场景。旧/新记录为 `.analysis/netifd-reload-before-p1-2-test.log`、`.analysis/netifd-review-fix-tests.log`。`uv run --no-project tests/ax6000_bridge_control/run_netifd_reload.py` 链接本地锁定 native 对象和真实 UCI，仅替换外部 system/ubus/driver 边界；缺依赖明确报错，不能算跳过后通过。该完整集成测试仍独立本地执行；当前 CI 的 12 项控制测试内已补真实批次函数回归，不宣称 CI 运行了完整 native reload。

修正版 WARP 与 D/proxy ARM64 modules/MODPOST、native netifd 编译通过；六个新生产补丁重新 fuzz=0 重放且 postimage 零差异。记录见 `.analysis/warp-review-fix-build.log`、`driver-review-fix-build.log`、`production-patch-review-fix-replay.log`；WARP、driver、全部 20 项控制及完整 native reload 针对性回归均通过，见各 `*-review-fix-tests.log`。vendor 既有警告和最终 CI/硬件限制继续保留。

- worktree：`/home/illya/workspace/router/ax6000/worktrees/bridge-forwarding`
- branch：`fix/ax6000-bridge-forwarding`
- 设计冻结 HEAD：`4a0c52dd7129ac9e90b9374f12c55c01cce8059c`；实现提交从其后开始，最终审查范围以 Git 为准。
- 生产源码基线：`c196312d3916fad77fa0bb47db57e4b685c15f03`；原 `sources/immortalwrt-mt798x` / `25.12` 作为只读生产 checkout。
- 实际生产补丁已经生成：内核 `999-zzz-5400..5402`、MTK 7.6.7.3 的 `044-ax6000-universal-ap-bridge.patch`、WARP `100-ax6000-checked-warp-ser.patch`、netifd `900-ax6000-transactional-ap-bridge.patch`。本地 postimage 在忽略目录 `.analysis/{kernel,driver,warp,netifd}-next`；对应 `*-base` 是准确基线。CI 使用包配方中的真实 patch 目录，不依赖 `.analysis`。

## 已确认事实、目标与取舍

两轮实验中，平板漫游到 GS7 后 Linux FDB 已正确指向有线上联，MTK 的旧无线关联仍截走回包；旧条目老化后恢复。关闭 k/v/r 仍能复现。旧关联为什么未及时清理尚未确定；已确认的直接转发绕桥行为是本版修复对象。

实现面向所有普通 AP 终端，无固定 MAC/IP/SSID。保留 RX 身份授权与隔离，让 Linux 桥选出口；最终无线 TX 绑定桥选定的原 wdev。r3.1 固定 CPU profile 不保留 PPE BIND、nf_flow 缓存或硬件 HQOS，保留基础 WED 和成功在线 L1 SER，eQoS 使用软件 HTB/IFB。吞吐、CPU、时延代价尚未测量。

无法证明 DMA 停止/完成，或 SER 与无法可靠拒绝的析构交叉，进入同步 noreturn 故障入口尝试硬复位；可能丢未落盘状态、失败可能需断电。普通配置错误/坏包不因此触发整机复位。该合同已通过设计审查，不等于实机故障体验验收或刷机授权。

## 实施分工与已完成验证

1. 内核 bridge/gate/VLAN/skb extension/NF-flow 限制与固定 HNAT CPU 两对象已实现。真实 helper 的 123 项桥门控检查、nf_flow 三入口无副作用拒绝、HNAT 38 个导出 API 安全拒绝及寄存器/首错测试通过。实际 value-state 生命周期和真实 noreturn 故障函数已测试，覆盖中断/抢占先于选举、单一复位执行者、底层复位返回后仍不回到清理路径。
2. Ethernet 已接入 participant、checked DMA stop、warm reset 错误传播、先 restore 后 restart、可信 descriptor 消费和 TC 硬件拒绝。已完成真实 ARM64 `vmlinux`、模块链接及 `Module.symvers`；真实 FE worker 测试覆盖 STOP 超时、restore/open 失败和禁止假 DONE。
3. 普通 AP driver 已实现授权 reader、RX 一次交桥、TX 原 wdev/最终 WCID 绑定、聚合继承及配置互斥；SER/HIF/proxy/实际析构入口已接线。79 项 wire/binding、33 项真实授权检查通过，320 个并发 token 请求仅产生 127 个唯一有效 slot。真实 ARM64 `mt_wifi.ko`、`mtk_warp_proxy.ko` 与 MODPOST 通过，使用真实 conninfra/WARP 符号，不是假导出。
4. WARP 已实现 value-only checked SER、producer gate、停止与重新准备/启动分离、15 个 busy 位与读回、WO 状态返回、取消和 proof 撤销；冷分配/映射失败按实际暴露边界清理。WO/CCIF 的 HALT、reset、IRQ/tasklet 与 FWDL 错误链已接线。真实 ARM64 `mtk_warp.ko` 与 MODPOST 通过。保留 BIT29 N9 控制中断，同时关闭数据触发器；mapped 拒绝返回 handled，避免 D 回退 native MMIO。173 项实际 proxy/权限测试通过，包括读掩码切换竞态和实际寄存器映射。
5. netifd 两阶段完整快照、outer ioctl 不确定后的重新阻流、free/replace/member 失败保留与重试、MTK 配置全链和软件 eQoS 已实现。锁定用户态依赖均本地构建；真实 netifd 链接及 8 个 ucode 模块编译通过。20 项控制测试通过，包含真实函数、ucode/datconf、配置失败、释放边界与 eQoS sandbox。
6. 产品选项仅由实际 AX6000 MTK U-Boot 单设备 defconfig 选择，并排除 mtkhqos_util。历史 ax6000-256m 文件实际选择 netcore_n60/multi-profile，不属本产品，保持原内容。OF_OVERLAY、MODULE_FORCE_UNLOAD 对本 feature 禁用；mode 文件按 feature 打包，CI 检查实际内核配置、rootfs 文件、模块符号并运行真实准备源码上的 helper 测试。控制测试中当前 CI 执行 `test_control.py` 的 12 项 C/shell 测试；8 项实际 ucode/plugin 测试另在本地锁定环境运行，不能将该 CI 步骤表述为已包含全部 20 项。禁用运行时 FqEnable 队列替换 CLI，避免与 SER 交叉释放；它不等于 eQoS 软件 HTB/IFB。

正常关闭采用 WO ENABLE→HALT，不能声称固件支持 SER_RESET→HALT。异常 SER 失败后的关闭仍须收到新鲜完成确认，否则不得释放资源，按故障终态处理。目标 WO 只有二进制；相关状态转换和硬件静止语义属于真机验收门。

## 本地构建材料

- `.analysis/linux-full-verified`：从官方 6.12.112 归档按四层 overlay 顺序重新准备，重放全部 1061 个原生产补丁。generic/files-6.12 为空；其余 overlay 分别为 generic 104、mediatek common 128、mediatek 6.12 45 文件。原补丁有 19 个使用 GNU patch 默认允许的 fuzz；不宣称原系列 fuzz=0。核对 87,245 个文件确认旧准备仅漏 common 的 128 个文件，已有内容无差异，已撤回旧 prepare 完整的表述。
- `.analysis/kernel-compile`：单独完整编译树和真实符号；多个模块共享此树时串行 make，不并发修改配置。该配置是 generic/filogic 与实际包必要选项组合，未执行完整 OpenWrt defconfig，不能等同最终生产配置。
- `.analysis/toolchain/toolchain-aarch64_cortex-a53_gcc-13.3.0_musl`：已核 SHA 的官方 OpenWrt 工具链，用于本地诊断 ARM64 编译；最终 CI 使用仓库 GCC 14.3，不能将两者等同。
- `.analysis/host-tools`：本地解包 bison/flex/m4，需 PATH、BISON_PKGDATADIR、M4 指向对应绝对路径；未全局安装。
- `.analysis/host`、`.analysis/host-src`、`.analysis/host-build`：锁定用户态依赖与真实 native 编译产物。host provenance JSON 记录源码来源和 SHA。
- `.analysis/production-patch-replay.json`：新内核、D、WARP、netifd 补丁均 fuzz=0 重放，并与对应 postimage 对比。新补丁不包含编译生成的 `.mod.c`。
- `.analysis/{kernel-final-build,warp-final-build,driver-final-modules-build,bridge-control-final-test}.log` 及各 `*-final-tests.log` 保留本地证据。vendor 编译仍有类型、原型等警告；成功链接不等于所有警告已处理。
- 既有顶层回归运行 72 项，成功且 1 项跳过：当前隔离 checkout 没有 locked packages feed，无法执行该项真实 Make-call 检查（`tests/test_ax6000_feeds.py`）。本次桥控制的 20 项测试没有跳过。

## 下一验收顺序

两项 P1 和 P3 修正、真正消息并发/网络 reload 路径回归、生产补丁重放、受影响模块增量编译及 Astra R2 复审均已完成。下一步：推送隔离分支并触发一版 CI → 等待用户通知结果 → 按实际构建结果继续验收。完整固件、硬件 DMA/WO/复位、任意终端漫游、隔离/VLAN/聚合、eQoS 和性能验收仍未完成；宿主适配测试不能证明真实 Linux RCU 调度或目标硅片行为。

不持续轮询 CI，等用户通知编译完成。不自动刷机或发布正式 Release。后续实机验证需先只读确认重启后的状态，不沿用重启前的关联表/FDB；历史证据保留在 `ax6000/research/evidence`。
