# r3：BSS 激活、配置失败与真正阻流的合同

2026-11-09。这是对 Astra r2 审查 P1-1 及相关 netns P2 的源码设计修订，供主方案合并与复审。仅新增本文；未修改实现、联网、编译、操作路由器或启动子代理。生产基线仍为 `c196312d3916fad77fa0bb47db57e4b685c15f03`；实施工作树已独立为 `fix/ax6000-bridge-forwarding`。

## 1. 结论与收敛边界

需要一个**内部、默认关闭的 BSS 普通数据门**，以“prepare 并确认阻流 → 应用无线配置与全部真实 bridge/VLAN 端口 → 写后读回 → commit 放行”代替旧设计的“失败则移桥、release”。它覆盖该 BSS 的全部终端与全部普通数据，不按 MAC/IP/BSSID 选择，也不是新的用户开关。

BSS 配置租约用于拒绝驱动内跨配置保留的 BA/TX 旧包；不建立 STA 身份 generation。已经交给 `napi_gro_receive()` / `netif_rx()` 的包属于 Linux 正常在途数据，已经提交 DMA 的帧属于硬件正常在途数据，不要求追回。**不新增 `net/core/dev.c:xmit_one()` 的每包租约 registry，不为撤回全部 Linux backlog、qdisc 或 socket 数据扩大本次修改面。** Linux 上既有 bridge/NF 出口策略继续按实际端口执行。

配置失败时保守阻整个 BSS，包括其 untagged standalone 域与所有 tagged bridge 域；不实现按某个终端或某个 VLAN 选择性继续服务。已确认阻流后才允许移桥/删除 child/release；若不能确认，保留当前已知拓扑，报告失败并停止清理，不能先解除关系使仍活动的 AP 恢复 legacy。

本文撤回旧 policy 文中“策略失败即可禁用/移出该成员”的实现建议，以及“ifdown/release 成功等于普通数据已停止”的推断。正常 ifup、external 设备状态、claimant 数量都不具有放行权限。

## 2. 准确源码证据

使用 Astra 审查中的 D/N/F/K/V 前缀：D 为准确 42-patch `vendor-mt79xx-7.6.7.3/review-baseline/mt_wifi`；N 为配方锁定 netifd `cbb83a185740`；F 为生产固件仓；K/V 分别为选定 bridge 与 VLAN 内核 postimage。额外 raw stable 6.12.112 参考为 `sources/linux-6.12.112-roam-refs/{include/linux/skbuff.h,net/core/skbuff.c}`，以及本工作树 `.analysis/linux-6.12.112-refs/net/core/dev_ioctl.c`。raw 文件不能冒称完整内核 prepare；已检查仓库修改这些文件的补丁涉及范围，实施必须再在完整 prepare 后核对。

| 已核实入口 | 对合同的影响 |
| --- | --- |
| F `package/network/config/wifi-scripts/files/lib/netifd/wireless-device.uc:55/71/75/450` | external=2；device_set/link 返回值原来未检查；mark_up 原来无条件写 up |
| N `device.c:155/175/748/760`；`bridge.c:315/625/648` | external release 不保证 down；多 claimant 甚至不调用 set_state(false)；addif 失败只 release |
| F `package/mtk/applications/mtwifi-cfg-ucode/files/usr/share/ucode/mtwifi/config.uc:167/179/185/214` | DAT 变化会影响 sibling；ifdown、reload、ifup 与 runtime hook 原先没有完整失败合同 |
| 同目录 `driver.uc:46/54/137/152`；`netifd.uc:15/22/40/48` | shell system 状态及 notify 返回值原先被忽略；不能依赖缺失的 iwpriv |
| N `ucode.c:65/156/194` | 可增加可靠的 C 控制 binding；interface_handle_link 已有真实 int，当前仅包装成 bool |
| D `embedded/common/cmm_data.c:6210/6752`；`ba_action.c:2018/2264` | 普通数据授权分流与 BA 入队之间可捕获配置租约 |
| D `cmm_data.c:3515/3608/3635`；`embedded/ap/ap_data.c:4035/4057/4068/4079` | common announce 与 AP callback 位于 Size/组播复制/DA lookup 之前，是晚期阻 legacy 的必要位置 |
| D `os/linux/rt_linux.c:2731/2738` | 真正交 Linux 是 GRO 或 netif_rx；不是 `to_os=TRUE` |
| D `os/linux/rt_main_dev.c:773/821`；`cmm_data.c:1557` | netdev TX 和内部数据发送入口；内部 WPA 的 RTMPToWirelessSta 也会直接调用 send_data_pkt |
| D `embedded/common/qm.c:1544`、`fp_qm.c:434`、`fp_fair_qm.c:306`、`cmm_data.c:7685` | 出队、ACK 缓存、PS/BMC/聚合仍须在最后提交边界核验，不仅在 netdev 入队检查 |
| D `os/linux/rt_linux.c:1980/1990`；K `include/linux/netdevice.h:1445` | 目前只装 ndo_do_ioctl；可以增加准确版本支持的 ndo_siocdevprivate |
| raw `net/core/dev_ioctl.c:479/508` 与末尾 private ioctl 分支 | private callback 在 RTNL 下调用，仅要求 device present，没有 IFF_UP 要求；该分支本身没有 CAP_NET_ADMIN 检查，必须由新 driver handler 明确检查 |
| raw `skbuff.c:1533/5075/5106/7134/7196`；`skbuff.h:4909` | 正式 extension 自动随标准 clone/copy 继承；skb_ext_add 负责 COW；add/put 均普通 EXPORT_SYMBOL；ID 数量须小于 8 |

## 3. 最小状态与同步规则

每个 AP-capable netdev 在首次注册前建立独立 gate，对应对象可以放入 D `embedded/include/rtmp_type.h:220` 的 `mt_dev_priv` 扩展，并在 wdev_init 后绑定到 wdev。**gate 的建立不依赖 ifup 成功，不依赖 adapter 当前 wdev 指针已可用。** 主接口在 `RtmpPhyNetDevInit()`，MBSS 在 `ap_mbss.c:187/203/267` 创建/attach 之前都要初始化。缺 gate 的普通 AP 数据一律拒绝，不能视为旧版独立 AP。

必要状态为 `BLOCKED`、`PREPARING`、`READY`、`FAILED_BLOCKED`、`STOPPING`。初始是 BLOCKED。另存 kernel 分配的唯一 owner incarnation、配置 transaction token、当前许可 lease、最后错误和一个普通数据 active reader 计数。它们不保存终端身份。

| 操作/条件 | 状态与许可 | 完成后的保证 |
| --- | --- | --- |
| 创建、ifup、open、claim、重新绑定 wdev | BLOCKED，lease=0 | 可以建立无线控制资源；不能接收/内转/发送普通数据 |
| PREPARE：包括 already-up | 先关闭门、撤销旧 lease，进入 PREPARING | 此后不能建立新的普通数据许可 |
| PREPARE 的 reader drain 与所需 backend barrier 全成功 | BLOCKED，返回 transaction token 与 blocked=true | 获旧许可的驱动 CPU 决策已结束；后续驱动队列旧包无许可 |
| 任一准备/配置/读回失败 | FAILED_BLOCKED，lease=0 | 没有隐式恢复；不得凭当前 bridge flag 变成 legacy |
| COMMIT 全部前置条件成立 | 原子发布新 lease 与 READY | 新入站/新 TX 可以获当前配置许可；旧 lease 仍失效 |
| teardown、wdev/adapter 重建开始 | STOPPING，lease=0 | 不再允许 commit；对象释放前遵原有队列退出及 grace |

gate 的短锁只保护状态、lease 与 reader 进入；不在持 gate 锁时做 bridge 操作、RTNL 操作、队列 flush、等待或调用 backend。普通决策进入时，在短锁下核验 READY/lease 并计数，随后放锁；完成后减计数、唤醒准备者。prepare 在 RTNL/process context 关闭门，放短锁后等待计数归零。不得持 MacTabLock、RxReRingLock、ring lock 或 active reader 反向等待 RTNL。早期 reader 覆盖获准后的同步处理，到 BA/其他队列接管或实际 handoff 才退出；BA 等待期间不持 reader，出队后重新核验租约并建立晚期 reader。

reader 的作用范围必须覆盖它保护的实际动作：晚期 RX reader 一直覆盖到内部转发决定结束或实际交 Linux 的调用返回；最终 TX reader 覆盖到 descriptor 提交/拒绝结束。只在读 READY 的那一瞬间计数没有意义。已提交 DMA 不再持计数，也不声称 prepare 确认无线介质上没有旧帧。

租约值由本次已有 built-in L2 guard/core 提供一个普通导出的单调 u64 分配 helper；core 不能随 mt_wifi 卸载而重置。owner、transaction、lease 均不可复用，溢出则拒绝激活。prepare 立即撤销许可，commit 分配新 lease；重建 adapter、ifindex 被复用、BSS down/up 都不会使旧包重新有效。无需全局逐包 lookup registry：RX/TX 均拿当前实际 wdev 的 gate 与包内值比较。

## 4. 阻流入口、旧队列与控制帧

### 4.1 RX：在真实入口阻新包，在共同晚期入口阻旧包

1. 先按 r3 RX parser 合同完成有界格式/控制/非法分类，保持实际 RX peer 的 Class2/3、密码学、PrivacyFilter、PortSecured 等检查。不能只读 skb 的 EAPOL 位或 EtherType 创建例外。
2. 两个 `rx_*data_frm_announce()` 的普通 AP 普通数据分支，在 BA/普通指示之前，要求 gate READY，并把 owner+当前 lease 写入 skb extension。关闭时普通数据立即释放；不入 BA，不调用 legacy。未授权聚合按 RX 方案整体拒绝或逐 MSDU 分离，不能借第一子帧放行。
3. BA、SW/HW A-MSDU、Ralink 第二 MSDU 后进入 common announce 时，先核验 RX lease 属于当前 gate 且仍 READY，建立覆盖现有 VLAN policy 处理至 handoff 的 reader。AP callback 再保证普通数据不能在任何 `MacTab.Size` 快捷路径、内部 group clone、DA lookup 之前漏过同一许可。不能等 VLAN 改写已读了新配置之后才开始 reader。缺租约、旧租约、错误 owner 都拒绝；**不得给 BA 出队或缺标签的子帧补发当前租约。**
4. reader 覆盖共同晚期决定至实际 Linux handoff，不能只有 AP callback 返回时的检查。这样 prepare 能确认所有旧许可的驱动内转/上送决定已经退出；新包即使发现当前无 bridge flag，也没有机会进入 legacy。

这里不要求 Linux 撤销已经收到的包。若 prepare 关闭时旧包已经进入 GRO/netif_rx，它之后的 bridge、路由、qdisc 或本机投递按正常在途语义运行；它不再进入 vendor RX legacy 分支。测试不得把此类已接收包当成“prepare 后新普通 RX 被放行”的证据。

### 4.2 TX：准入 stamp，最后提交核验，不重许旧缓存

普通 AP TX 在 `send_data_pkt()` 的共用准入处、VLAN 改写和队列操作之前获得 **TX 目标 owner+lease**；reader 覆盖同步 VLAN/目的处理到队列接管。netdev `rt28xx_send_packets()` 可先作快速拒绝，但不能只改该入口。RTMPToWirelessSta 等内部数据入口也必须经过同一规则。

已存在 TX lease 的 packet 只能继续验证，不能重新 stamp 当前 lease。最终 queue/TX_BLK 绑定校验在 r3 TX 方案已经要求的本 wdev、DA/TR/WCID/VLAN 校验基础上再核验目标 lease；ACK 缓存重发、PS/UAPSD、BMC、多包聚合及 WTBL pending 等都必须通过最终门。聚合中每个普通 MSDU 均有效才可提交；不能只检查第一个 skb。旧配置缓存一律释放，不能改成另一个 wdev，也不能按新的当前 lease 原地续期。

这不需要追回 Linux qdisc：来自 Linux、首次进入 driver TX 的 skb，按当时已就绪的真实出口获得许可，属于正常 Linux 在途行为。需要拒绝的是**已经进入 vendor queue/ACK/PS 等缓存的旧 TX 租约**。驱动内部重建的普通 TX skb 必须显式继承旧 TX lease，不能伪装成一次新的 Linux 入队。

### 4.3 正式 skb extension，不借用已占满的 cb

新增一个正式 extension ID，例如 `SKB_EXT_MTK_BSS`，结构内用两个独立子区：配置租约与 P1-4 ownership。配置区最小为 RX `{owner,lease}`、TX `{owner,lease}` 四个 u64；不放 adapter/wdev/STA 指针，不需要模块析构 callback。ownership helper 不得清除/改写租约，反之亦然。

修改 kernel `include/linux/skbuff.h` 的 enum、共享值型结构头及 `net/core/skbuff.c` 的 `skb_ext_type_len[]`；生产选择 `CONFIG_SKB_EXTENSIONS=y`。保留原 `SKB_EXT_NUM<8`/长度 BUILD_BUG_ON，不能为了塞字段取消边界。调用者用 `skb_ext_add()` 获可写 COW 视图，不能修改 `skb_ext_find()` 返回的共享内容。extension 新分配不假设整段数据已清零：公共 helper 在确认为首次创建时初始化全结构，已存在时只改自己拥有的子字段。内存失败普通数据拒绝，不无标签继续。

标准 `skb_clone/copy/copy_expand` 由准确 `__copy_skb_header():1533` 与 extension refcount/COW 规则覆盖。厂商自建 skb 则必须逐点补继承：

- `os/linux/rt_linux.c:553 ClonePacket()` 本来使用 skb_clone；只读继承即可，写之前 COW。
- `duplicate_pkt_vlan():626` 是重新分配的 packet；`cmm_data.c:4082` 的 Ralink 第二 MSDU 必须把来源租约显式复制给新包。
- `RTMPAllocateNdisPacket():477` 是无来源的新包 API，不能一律猜测来源或复制全局“当前包”。需要从有来源的调用处传原 skb 或使用一个明确的 clone-metadata helper；真正本机新数据才允许新 TX 准入。
- BA 现有 `reordering_mpdu->pPacket` 保存的 skb 本身携带租约；其 timeout/flush 不补当前值。分配新子帧时显式继承。若实现另外加入值型 BA 字段，两份不能分别更新形成歧义。
- ACK/AMSDU/分片重建若创建新普通包，继承当前处理的原包租约；不能从当前 TR/WCID 重新构造。缺继承点时拒绝该包，而不是绕检查。

### 4.4 控制流的最小例外

关闭的是普通数据。802.11 管理/控制与无普通 MSDU 载荷的必要协议帧仍按原 MLME/无线状态机处理；RX 的合法 EAPOL 可进入现有本机 MLME/安全流程，不进入 bridge 普通转发。

**本次最小方案不为 TX 数据帧增设“看起来是 EAPOL 就绕门”的通道。** Linux netdev 的 EAPOL 与 `RTMPToWirelessSta():1301` 生成的数据 EAPOL 在 BLOCKED 期间同样不发送，commit 后正常重试。后者当前只因头部 88:8e 设置 cb flag，不能将这个 flag 当可信授权。`MiniportMMRequest():1084` 也不能因 packet type 为 TX_MGMT 就免检全部内容：原始 802.11 Type=DATA 且携带普通 MSDU 的提交仍受门约束，真正 management/control 保留。这个选择会使配置中断期间的握手重试/重认证成为正常结果，不要求配置完成前维持业务或完成 WPA 握手。

因此不新增一个需要审计所有 caller 的“可信 EAPOL TX”产品能力；将来若要减少握手中断，须单独证明内生控制来源与最终出队的可信继承。本轮不会靠控制例外让普通数据复活。

## 5. 不依赖 iwpriv 的实际控制 API

选择 **`SIOCDEVPRIVATE+15`（0x89ff）的 `ndo_siocdevprivate` 专用二进制 ABI**。该号在已检索准确 D Linux 源中没有现用路径；旧 `rt_os.h` 的版本兼容代码曾把 WEXT 常量也叫 SIOCDEVPRIVATE，不能因此混用 0x8be* 与真正 0x89f*。实施要在真实预处理输出确认实际号，并只给 Linux 6.12 分支安装新 callback。

在 `RtmpOSNetDevAttach()` 安装独立 callback，不从 `rt28xx_ap_ioctl()` 的 WEXT OID/cookie 分流进入。后者原有 PREPARE、DOWN 拒绝和共享 `OS_Cookie.ioctl_if` 均不适合本合同。新 callback 直接拿内核传入 netdev 的 gate，不先要求 GET_PAD/GET_WDEV 成功，所以接口 DOWN 时也能 GET/PREPARE。未初始化 wdev 只能返回 blocked=true、driver_ready=false，不能 COMMIT。

准确 raw `dev_ioctl.c` 证明 callback 已在 RTNL 下调用，handler 必须 ASSERT_RTNL，不能再 rtnl_lock。该 private 分支不自动检查 CAP_NET_ADMIN；**handler 对所有操作显式 `ns_capable(dev_net(dev)->user_ns,CAP_NET_ADMIN)`**，并校验固定 adapter netns。C userspace 用 `socket(AF_INET,SOCK_DGRAM|SOCK_CLOEXEC,0)` 与 `ioctl()`，不执行 shell、不安装 iwpriv。

共享 UAPI 使用定长整数、显式 ABI version/结构 size/零 reserved，不用 long、用户裸指针、内核指针或隐式 padding；payload 由 callback 收到的 `void __user *data` 读取，支持 32/64 位外层 ifreq 转换。输入包含 operation、期望 ifindex、owner incarnation、transaction token，输出包含状态、当前 lease/错误、driver_ready 与 blocked 确认。u64 经 ubus/ucode 以固定十六进制字符串传送，避免 JSON 数值精度与有符号转换歧义。

最小操作为：

| 操作 | 必须行为 |
| --- | --- |
| GET | 查询 ABI/owner/state/driver_ready；不改变门；接口 DOWN 可用 |
| PREPARE | 校验 owner，关闭门、撤销 lease、drain，完成父方案所需 backend 屏障；成功才返回 blocked=true 与新 transaction token |
| STAGE_DOMAIN | 仅匹配 BLOCKED transaction 可写配置意图；暂存普通 AP 模式、静态网络域/实际 port、必要策略值，不放行 |
| COMMIT | 校验相同 owner/token、全部 driver/profile 资源就绪、域快照、稳定 topology token、后台可用；发布新 lease/READY |
| ABORT | 匹配事务保持 FAILED_BLOCKED，清理未发布 snapshot；不恢复旧 READY |

STAGE_DOMAIN 可按配置中的实际端口逐项提交，域记录只含 netdev/VLAN/policy 事实，不含终端身份；这样不需要一个带任意用户指针的大批量结构。事务结束有明确 count，重试不重复添加，OOM/截断/重复冲突均失败。COMMIT 前把完整 snapshot 和输出预先准备好；copy_to_user 失败时仍 BLOCKED。**READY 发布后不得再执行可能失败的分配、用户拷贝或 backend 操作。** syscall 返回成功才可把 reply 当放行确认。

同一 netifd owner 的嵌套 bridge/VLAN 操作复用一份在途 transaction，不能每次 enable_member 都再次分配 token 使上层事务过期。C helper 在已有已确认 BLOCKED 事务时幂等返回该 token；相冲突的并行配置返回 EBUSY。恢复/重做事务显式废弃旧 token 后再 prepare。netifd/handler 退出或超时不自动放开门；重启的 parent 必须 GET、重新 prepare 和重建全部就绪证据，不能接管旧 token 后直接 commit。ABORT 只处理已阻流事务；已经 READY 的失败处理必须先正式 PREPARE，不能把 ABORT 名称当同步阻流证明。

关键 errno：不支持 ABI/角色为 EOPNOTSUPP，结构/保留位为 EINVAL，owner/事务过期为 ESTALE，拓扑变化为 EAGAIN，backend RESETTING/不可完成屏障为 EBUSY/EIO，硬件/配置资源不就绪为 EAGAIN，已注销为 ENODEV。任何负返回不代表阻流已经确认；userspace 只能依据成功 PREPARE 的 blocked=true 清理拓扑。PREPARE 已经关闭软件门但后端失败时保留 fault 状态，不能返回假的总成功。

## 6. netifd/ucode 的完整顺序与错误传播

### 6.1 单一 BSS owner 与所有 VLAN claimant

N `device.h/device.c` 增加内部 activation owner 引用与就绪状态；它与 active/claim count、wireless capability 分开。N `vlan.c` 从 `vldev->dep.dev`，`vlandev.c` 从 `mvdev->parent.dev` 继承同一 owner；双层 VLAN 仍指向同一 BSS。源值与有效值独立，false/移除也传播，覆盖先建 child 后收到 AP 通知。

owner 收集所有实际需要的网络绑定/bridge port/VLAN child，不允许单个 claimant 或单个成功 addif 提前 COMMIT；already-up 的接口同样先 PREPARE。child 删除/release 不解除 owner 的 BLOCKED，最后一个 claimant release 也不会放行。owner 生存期由明确引用管理；child 不保留旧 ifname 字符串充当身份。

在 N `ucode.c` 增加 C binding，例如 `bss_prepare/activation_stage/activation_commit/activation_abort/activation_get`，返回 `{ok,errno,blocked,owner,transaction,...}` 结构。N 内部 C 的 bridge/VLAN 变更路径调用相同 activation helper，不经 shell/异步 ubus 自调用。

### 6.2 初次 setup 与 DBDC reload

1. F MTK handler 计算本次配置实际影响的全部现存 AP/sibling，先调用 parent netifd 的内部 prepare 命令；parent C 同步 PREPARE 并记录 owner/token。任何活动 BSS 无确认即停止后续 ifdown/DAT/reload/bridge 清理。
2. 新创建的 AP 由 driver 默认 BLOCKED 保证没有启动窗口。ifup 只初始化控制面；等待接口出现后 GET/PREPARE，取得新 owner/token。旧 owner 的事务不能拿来提交重建接口。
3. `config.uc` 执行 DAT/profile、必要 reload/ifup；ifdown、modprobe/rmmod、ifup 的失败必须真实返回。影响 sibling 的路径不能继续盲目 ifup 然后靠后台 `wifi up &` 宣称成功；由 parent 对受影响 owner 重新收集绑定和就绪结果。profile 全局值（含 BTNBSSID）完成前不提交任一依赖 BSS。
4. MTK `netifd.uc:set_vif/set_vlan` 通过 SET_DATA 报内部 ABI 能力、owner 与 transaction。`wireless-device.uc:wdev_set_data()` 保存它们，`handle_link()` 向 device_set 传显式 true/false 能力与 owner。
5. parent 在已 BLOCKED 的 BSS 上执行所有 `device_set/interface_handle_link`，创建/加入真实 bridge/VLAN port，应用 hairpin、NoForwarding 对应策略、isolated、VLAN membership/PVID 等。标准返回值全部检查，不能只看 notify 成功。
6. 汇总所有端口，读取实际 bridge master、tag ancestry、UP、hairpin/isolated 和所需 VLAN 配置。开始读回前 GET 一个稳定 core topology token，读回后 COMMIT 必须仍匹配同 token；期间有变化则保持 BLOCKED 并重新读取，不能冒用新 token 给旧读回背书。所需设置写入路径都应纳入父方案已要求的同步 topology 事务。
7. STAGE_DOMAIN 记录本轮明确的 standalone/bridge 意图，COMMIT 获取新 lease。最后才将 wdev state 设 up 并成功回复 CMD_UP。

这里需要真实修复原本无返回的控制层：N `system-linux.c:427` 一类 void sysfs setter、`:940 system_bridge_set_wireless()` 改为返回可检查错误并读回；`system_bridge_addif()` 只有在实际 master 正确时才继续策略阶段。N `bridge.c` 的 VLAN/membership helper 同样逐层返回错误，不能在 `bridge_member_enable_vlans()` 吞掉子操作失败再宣称 member READY。readback 使用实际 port ifindex/master 以及 kernel netlink/sysfs 值；预期布尔 false 也必须写入并确认，不能只核 true。

F `mtwifi/netifd.uc:notify()` 必须 return/check ubus 调用结果；`set_up()` 等继续传播。`wireless-device.uc:handle_link()` 返回明确结果，`wdev_mark_up()` 遇任一失败进入失败处理，不能执行无条件 `state="up"`。N `ucode.c:device_set()` 的失败不得 NULL 后被当真；`interface_handle_link()` 的真实错误可增加结构化包装，至少 false/NULL 都必须触发失败。`setup_failed()` 必须触发/核验 parent 的 abort/block 路径，不能只打印和禁重试。

### 6.3 活动更新和失败逆序

所有受支持的 netifd 变更入口均先取得同一个 owner 的 prepare：bridge add/del、成员策略事件、VLAN create/delete/重父、PVID/隔离调整、角色改变、正常 wireless teardown/hotplug。重点落点是 N `bridge_enable_member()/bridge_disable_member()/bridge_remove_member()/bridge_member_cb()`、两类 VLAN set_up/set_down 与政策更新 helper。先 prepare，再改 bm->active/关系/端口设置；不能只在 error: 才补一个 release。

正常失败序列为：已确认 BLOCKED → 标记 FAILED_BLOCKED → 逆序撤销本事务新加的关系/资源 → 报 setup/link 失败。既有活动端口若因异步读回发现错误，先 PREPARE 成功，再 delif/release。多个 claimant、VLAN child 均不能绕开此顺序。

ifdown 返回失败但 PREPARE 已确认成功时，可以在 gate 持续关闭的条件下清理本事务资源，记录 degraded/down-failed；不以 carrier/flags 的变化替代 gate。若控制 ioctl 失败、owner 变化、backend 屏障失败而无总 blocked 确认，则停止关系清理，保留现有拓扑并报告故障。**不得运行旧的 teardown 路径继续无条件 handle_link(false)。** 新一次恢复必须重新 prepare、配置和读回，以新租约提交；不能因“桥看起来仍在”恢复旧 READY。

DAT 不新增用户修复键：gate 默认行为由生产 driver 编译版本决定，新的字段只用于上述内部控制 ABI/SET_DATA。DAT 中现有 NoForwarding/BTNBSSID/VLAN/role 仍由 profile 读取；会改变本合同的 legacy runtime setter（例如 `ap_cfg.c:14135/14170` 的隔离 setter）必须要求已准备的 BLOCKED 事务，或明确拒绝在线改写并要求正常 reload，不能绕过 parent 的端口策略读回。仅增加日志或继续忽略 runtime hook 的 system 返回值不符合合同。

## 7. standalone/tagged 混合域与意外移桥

gate 是整个 BSS 的准入门，之后才执行 r2 已认可的 metadata→逐层 VLAN→实际 bridgeport 解析。因此 `BLOCKED/FAILED` 永远先拒绝，不会被“当前没有 IFF_BRIDGE_PORT”覆盖。

READY 另持一份本次控制事务明确发布的**配置意图 snapshot**。它记录实际 AP base、直接 VLAN ancestry/proto/VID、预期 bridge port/master 与显式 standalone 域；不保存 STA，不用设备名称推理。所需 netdev 引用属于控制 snapshot，可在 RTNL 下取得并在停止/grace 后释放；不随包保存裸指针。ifindex 仅用于 API 校验，不能代替对象引用而产生复用 ABA。

数据面仅在“实际解析为 standalone，且本次 snapshot 也明确允许该域 standalone”时使用 legacy。原本应进入 bridge 的 tagged prefix/untagged 域若发现 VLAN child 或 master 已失，归未就绪/kernel-only 或拒绝，绝不改成 legacy；发现 snapshot 与实际域冲突可立即关闭该 BSS 的软件 gate并报告，随后控制面必须通过正式 prepare 确认后才清理。此闭锁发布不得在 RX 中等待、取 RTNL 或访问用户态。

| 配置域/状态 | 普通数据行为 |
| --- | --- |
| 全 BSS BLOCKED/FAILED，包括 standalone base | 全部拒绝；无 DA 表内转 |
| READY，直接 AP bridge port | 只交 Linux，保留既有实际桥出口隔离 |
| READY，匹配的 tagged VLAN upper 在桥 | 按 Linux demux 交真实 upper/bridge；base 没有桥标志也不能 legacy |
| READY，untagged 显式 standalone + tagged bridge | 裸域按已确认 standalone；tagged 域按其 bridge；任何配置失败停整个 BSS |
| bridge child/master 在事务中移除或意外消失 | 已准备则 gate 关闭；意外缺失也不得把已承诺 bridge 域转 legacy |
| VID0、双 tag、metadata+in-frame | 沿已审 parser/demux 顺序及 snapshot 匹配；不能先丢 tag 再判断 standalone |
| 不匹配 VID、DOWN upper、未知 master/handler | kernel-only/明确拒绝，不能以解析未知为 legacy 许可 |
| 动态 per-STA VLAN、未支持角色 | 激活时明确拒绝，不产生 READY；不按终端选例外 |

snapshot 是配置事实的最小锁存，用于阻止失败/意外移桥恢复 legacy；不是把 Linux FDB、队列、每个 packet 的全部拓扑做影子副本。常规网络变更由上述 netifd prepare 协调；任意 root 私自改内核/禁 hook 不构成本次产品接口支持。

## 8. backend 与退出的边界

CPU gate 不能声称阻止 PPE 已缓存流、WARP 私有回注或正在 DMA 的硬件。PREPARE 的总成功需要父方案 P1-2/P1-3 的 backend 状态/真实停消费屏障；backend RESETTING、FAILED、注销未完成均不可因“当前没有 callback”被当无后端成功。驱动 gate 先关闭，但这些失败必须原样上报，保持 blocked fault，关系清理不越过没有完成的屏障。

本设计不再单独重写 backend SER、hook/work/DMA 生命周期；使用对应 r3 合同。对已确认桥依赖流恒不加速的正常配置，其软件 ordinary gate 就能阻新 RX/legacy，但仍不能把此证明扩大到硬件 teardown/remove 的所有资源安全。

driver/wdev 退出先 STOPPING/revoke，停止新 gate reader，drain，按既有 BA/TX/timer 生命周期排空或释放，最后注销 NF hook并按准确合同 grace 后释放 adapter/快照/gate。新 owner 永不继承旧 lease。队列持有的 extension 只有值，普通 skb 释放不回调已经卸载的模块。

## 9. netns P2 的明确修正

`netns_local` 是 K `include/linux/netdevice.h:2437` 的独立 bitfield，**不是 `dev->features` 中的 NETIF_F_* 位**。K `net/core/dev.c:11693` 按该字段拒绝移动。V `vlan_dev.c:606` 的普通 features 重算既不会实现，也不应被当成清除此 bitfield 的原因。

采用不可撤销的生命周期合同：所有可能承担本修复 AP 角色的 adapter netdev 在首次注册前固定 adapter netns，并设 `dev->netns_local=true`；AP→STA、capability true→false、正常 down/up 都不清除。所有 VLAN 后代在 V `register_vlan_dev():149`、`:158` vlan_vid_add 和 `:180` register_netdevice 之前验证与直接 real_dev 同 netns，继承 true；递归约束双层 VLAN，在创建时即拒绝跨 ns upper。

本次不支持未受约束既有 netdev 原地转入 AP。请求此转换时拒绝并要求通过正式 teardown/recreate；若将来扩展，激活前必须检查已有全部 VLAN 后代均同 ns、补齐不可撤销约束，存在跨 ns upper 就拒绝。不能只限制转换后新建的 child，也不能在 init_net 注册 hook 后默认其他 namespace 已受保护。

netifd 的无线 role/capability false/removal 仍正常向后代传播，清理旧的有效 hairpin/isolate 派生状态；这是控制面能力状态，不能顺便解除 netdev 的 namespace 生命周期约束。

## 10. P1-1 的实现放行门槛

设计可以具体收敛到上述接口，无需把真正阻流的决定留到真机。进入实现后至少必须直接测试真实 helper/状态机：

1. 默认/ifup/already-up/多个 claimant/child release 均不会自行 READY；仅正确 COMMIT 可打开门，旧 owner/token、错误 namespace、ABI/OOM/copyout 错误全部失败。
2. 注入 addif、hairpin、isolated、VLAN 读回与 ifdown/reload 失败。持续新发包验证：确认阻流后没有新普通 RX 进入 BA/legacy/Linux；driver 旧 BA/ACK/PS/BMC/聚合缓存即使 commit 新配置也不能重新获许可。区分此前已进 Linux/DMA 的正常在途帧。
3. 失败清理顺序可观测：必须先出现真实 blocked ACK，再 delif/release；无法 ACK 时无关系清理。不以无线状态字段、日志、ifconfig 退出码代替流量门确认。
4. 双层 VLAN、先建 child 后收通知、一个成功端口另一个失败、mixed standalone/tagged、child 意外消失、ifindex 复用，均不能使已承诺 bridge 域恢复 legacy。
5. 标准 clone/COW 与厂商新 skb 继承使用真实 helper；验证同一 multicast clone 的 TX stamp 不改另一出口副本，缺 metadata 不偷偷补当前 lease。ownership 字段与租约互不覆盖。
6. 管理/控制接收保持可用，合法 RX EAPOL 仅进本机安全状态机；关闭期 TX 数据 EAPOL按本合同被阻，commit 后认证正常重试。恶意 EtherType/cb flag、混合聚合不得绕门。
7. 完整 MT7986 CI 核实 netifd/ucode/driver/kernel UAPI 同步、CONFIG_SKB_EXTENSIONS 与 extension ID/大小边界、专有模块普通导出可链接；单独进行性能与授权真机验收。设计认可不等于这些检查已通过。

P1-1 的关闭条件是：主方案明确采用这套默认关闭/同步确认/整 BSS 失败/旧驱动队列租约失效合同，合入所有所列真实入口和错误传播，而不是再用 external release 或 mark_up 充当数据面动作。它不要求全 Linux 在途数据撤销，也不要求引入任意终端身份状态。
