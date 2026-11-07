# AX6000 生产主线维护记录

审查完成日期：2026-11-08，Asia/Singapore。基线为 Build 17 的
`b8e146aec85b02450f9b12050fd6c42528ce9d43`。本轮保持 MTK U-Boot 110MiB、
Linux 6.12.112、mt_wifi 7.6.7.3、MT7986 20260601 固件和 HNAT/WARP 配置。

## 上游选择

SDK 上游 `chasey-dev/immortalwrt-mt798x-rebase:25.12` 仍为
`9f397975c7b47ce6986a6e39529d2f72fd5c729d`，没有新的 SDK 分支提交。
另审查 ImmortalWrt `openwrt-25.12` 的 `3682a14d…` 及 OpenWrt 同分支的
`71dbef3f…`，按需 cherry-pick 以下提交，提交说明保留原始来源：

| 原始提交 | 合入内容 |
| --- | --- |
| `1684a761a3bb99f11bb87eb02f62894563e119c6` | musl regcomp 多处整数溢出修补 |
| `514fad5218edaa4a36af7c15c0f46cdff0139309` | dnsmasq 2.93 大型 hosts/SIGHUP 重载退化修补 |
| `29bc851e50675070809ebe4a3c90b3f8961eb368` | 通用 wifi.utils 的 sysfs MAC 读取修正 |
| `79d87e9f6f835031698882877c84c1ff51272789` | 两阶段网口命名，处理名称交换冲突 |
| `6c57f7cd04028d370f9360de71f5d59412046194` | libubox 非 extended 属性越界读修补 |
| `5c334c12e26ee536d12f684c8f5b213cabb165c6` | uhttpd 2026-10-19 前置更新 |
| `d4cb7022585390564196f9849b2f5d10aba006e9` | uhttpd 2026-11-05 请求边界、脚本资源和 ubus 编码修补 |

uhttpd 最后一项同时更新配方、init 和配置，包含新的单源脚本槽限制；升级后应验证
LuCI/ubus 并发访问。通用无线工具修补不能宣传为 mt_wifi 漫游修复。

没有引入无本机需求的 Airoha、QCA/IPQ50xx、其他机型 U-Boot 或 6GHz/mesh 改动。
OpenWrt 的 `8303e9b2…` 将内核升至 6.12.113，但其完整刷新补丁不能直接应用于此
SDK：部分官方补丁不存在，MediaTek SPI 校准和 ASUS mtdparts 补丁上下文也不同。
该项需另在实际内核源码上刷新 SDK 补丁并构建验证，本轮不只改内核版本/hash。

## 软件包与锁定

六个 feeds 的完整提交固定在 `feeds.conf.default`。packages 更新至
`fa25995d74a720c1c98bf6265d3dd8b4eb620d1f`，所选软件中的相关更新包括：

- curl 8.22.0 → 8.23.0。
- yq 4.53.6 → 4.54.1。
- htop 3.5.1 → 3.5.3。
- 默认 Go 构建工具链 1.26.9 → 1.26.10，Tailscale 使用的另一 Go 工具链至 1.27.3。

LuCI 后续提交仅涉及本机未选择的 Passwall，保留 Build 17 的 LuCI 提交；routing、
telephony、video 也保留原固定版本。Tailscale 1.104.1、daed 1.27.0 和 OpenSSL 3.5.9
不是本轮 feed 变化带来的新版本，不作升级宣称。

Nikki 上游仍为 `78e1a751…`，没有新的主提交。受控本地补丁记录于
`config/ax6000/feeds/`：

- 生成嗅探协议字段从 `port` 修为 `ports`，Nikki 包 release 从 1 升至 2；保留原 UCI 字段。
- Mihomo 从稳定版 1.19.32 更新为 1.19.33，源码固定至
  `3f27ef39f8c96bbfbc8b0b9bb10f5c9ef60149d8`，归档摘要固定；不使用 alpha 或预编译替代品。
- Mihomo 构建时间改用 `SOURCE_DATE_EPOCH`。

`scripts/prepare-ax6000-feeds.py` 离线验证六个 HEAD、补丁摘要和三个完整文件的
preimage/postimage，在临时树全部验证后写入；重复执行保持幂等，`--check` 只验证。
浮动 feed、未审查的 HEAD 以及这三个文件的意外修改会被拒绝；其他 feed 文件只核对
HEAD，不检查整个工作区的内容或脏状态。CI 使用新 checkout，不恢复 feed 工作区。
准备脚本和 feeds 更新不能同时执行。

`scripts/install-ax6000-feeds.sh` 安装默认 LuCI、defconfig 明确选中的软件及递归依赖，
不再全量链接未使用的软件包。全量安装在本次锁定 feeds 下会为未选中的 dnsdist、
WeeChat、Snort 等产生 Kconfig 自循环；没有修改这些软件包，也不绕过依赖检查。
CI 同时拒绝 defconfig/diffconfig 输出中的错误，避免工具返回0时漏掉失败。

## 诊断与生产构建隔离

审查时生产 `25.12` 没有 Build 19 的 `043-roaming-path-diagnostics.patch`。
诊断分支 `fix/ax6000-roaming-diagnostics`、其 `6eeefe96…` 提交及已发布测试包保留，
不合并到生产主线，也不将诊断更新称为永久修复。

`scripts/check-ax6000-production.py` 在配置前检查驱动补丁系列，拒绝诊断文件名及
`ROAMDIAG`/`roam_diag_`/`mt_roam_diag` 标记。完整编译后再检查 mt_wifi 模块符号，
并保留现有 RRM/WNM/FT 和 HNAT/WARP 模块验收。

生产工作流不再临时合并上游，构建时要求 HEAD 与 `GITHUB_SHA` 相同；只编译已经
推送的源码和固定 feeds，准备后再次验证。产物仍提供配置、feeds、源码树、补丁摘要、
镜像校验及版本记录；记录 feeds 时按明确名称读取，避免把生成目录或 `.tmp` 算成 feed。

本地验证和 CI 编译是不同阶段。完整镜像成功及模块检查应以该次 CI 的实际结果为准；
新镜像未部署，不能据此宣称已完成启动、漫游、Nikki 或长期稳定性验收。
