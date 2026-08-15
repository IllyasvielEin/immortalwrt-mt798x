# Redmi AX6000 单臂旁路由配置

本固件只适用于 `xiaomi,redmi-router-ax6000-mtkuboot`，也就是 MTK U-Boot 的
110 MiB UBI 分区布局。已经运行这一布局的设备升级时使用 `sysupgrade.bin`，不要刷
stock 或 ubootmod 镜像。

固件不会自动修改 LAN、WAN、防火墙或 DHCP，避免恢复出厂后因网络环境不同而失联。
下面的设置以主路由 `192.168.50.1`、AX6000 `192.168.50.2` 为例。

## LuCI 配置

1. 在“网络 → 接口 → 设备 → `br-lan`”中把 `wan` 加入桥接端口。AX6000 原有
   `lan2`、`lan3`、`lan4` 继续保留在桥中。
2. 禁用或删除逻辑接口 `wan` 和 `wan6`。这里只处理逻辑接口，不删除物理端口
   `wan`，因为它已经属于 `br-lan`。
3. 把 LAN 协议设为静态地址：
   - IPv4 地址：`192.168.50.2/24`
   - IPv4 网关：`192.168.50.1`
   - DNS：`192.168.50.1`，或填写你信任的上游 DNS
4. 在 LAN 的 DHCP 服务器页面勾选“忽略此接口”。在 IPv6 设置中关闭 RA 服务、
   DHCPv6 服务和 NDP 代理。
5. 在“网络 → 防火墙”的 LAN zone 中保留 input/output/forward 为 ACCEPT，并开启
   IPv4 masquerading。单臂路由的流量从 `br-lan` 进入后又从 `br-lan` 发往 GS7；
   masquerading 可强制回程再次经过 AX6000，避免透明代理和连接跟踪出现非对称路径。
6. 关闭 ICMP redirect，防止 AX6000 提示客户端绕过自己：

   ```sh
   cat >> /etc/sysctl.d/99-bypass-router.conf <<'EOF'
   net.ipv4.conf.all.send_redirects=0
   net.ipv4.conf.default.send_redirects=0
   EOF
   sysctl -p /etc/sysctl.d/99-bypass-router.conf
   ```

7. 需要经过旁路由的客户端把 IPv4 网关和 DNS 都设置成 `192.168.50.2`。不需要代理
   的游戏机或 P2P 设备可以继续使用 `192.168.50.1`，避免额外一层 NAT 对 NAT 类型
   造成影响。

修改管理地址前建议保持一条有线连接，并分步骤应用设置。

## MTK 驱动与硬件加速基线

构建固定使用 MTK `mt_wifi` 7.6.7.3 和 MT7986 `20260601` 固件，并保留
`conninfra → mt_wifi → WED/WARP v2 → WHNAT/HNAT` 数据通路。CI 还会检查硬件
收包卸载（WED HW RRO）、头部转换、RED、MU-MIMO/OFDMA、波束成形、TWT、SCS 和
DFS 等关键能力，避免跟踪上游时因 Kconfig 变化悄悄退回软件路径。

没有启用 MTK Band Steering、MBO/OCE 的原因不是驱动缺少对应代码，而是本机只使用
5 GHz，且当前 WEXT 栈缺少与 7.6.7.3 ABI 配套的 WAPP/MAP 用户态控制面。只编进这些
开关不会得到可靠的跨品牌主动漫游，还可能改变终端看到的管理帧能力。

## Wi-Fi 漫游（802.11k/v）

本固件已把 MTK 专有无线驱动的 802.11k/v 参数接入“网络 → 无线 → 接口配置 →
高级设置”。修改这些选项会重载 MTK 无线驱动，Wi-Fi 中断约 20 秒属于正常现象。

与华硕 GS7 原厂系统搭配时，建议：

- 两台 AP 使用相同的 SSID、加密方式和密码，但选择错开的信道。
- AX6000 开启 802.11k 和 802.11v。它们只向终端提供测量信息和切换建议，最终是否
  漫游仍由手机、电脑等终端决定，不要求两台 AP 属于同一品牌。
- 不启用 802.11r。MTK 驱动模块虽然编译了 FT 代码，但跨 AP 传递密钥还依赖与驱动
  ABI 匹配的 WAPP/IAPP/KDP 守护进程。25.12 源码没有提供这组用户态组件，GS7 的
  Broadcom/ASUSWRT 漫游栈也不能与 MTK 的私有 KDP 直接协同。

没有 WAPP/MAP 控制面时，`WNMEnable=1` 只启用驱动的 802.11v 协议能力，不等于具备
基于 RSSI 阈值的主动 steering。固件因此不会显示一个只有 `FtSupport` 开关、实际却
不能完成跨 AP 密钥分发的 802.11r 入口。

AX6000 的 2.4 GHz/5 GHz 参数由驱动合并成一份 DBDC 配置。本固件同时修复了原版
7.6.7.3 未合并 `WNMEnable` 的问题，因此只开启 5 GHz 时，11v 不会在驱动加载阶段
被第一频段的默认值覆盖。

驱动 DAT 配置可用下面的命令核对，此命令不会显示 Wi-Fi 密码：

```sh
grep -E '^(RRMEnable|WNMEnable|FtSupport)' \
  /etc/wireless/mediatek/*.dat
```

其中 `RRMEnable=1`、`WNMEnable=1` 分别表示 k、v 已写入驱动配置；`FtSupport` 应
保持为 `0`。

社区调研依据：上游维护者在 [Issue #204](https://github.com/hanwckf/immortalwrt-mt798x/issues/204)
和 [PR #111](https://github.com/hanwckf/immortalwrt-mt798x/pull/111) 中说明单开
`FtSupport` 无效；[PR #205](https://github.com/hanwckf/immortalwrt-mt798x/pull/205)
尝试加入旧版 WAPP 二进制和驱动补丁，但一直没有合并，也不适用于当前 25.12/6.12
组合。2026 年的后续讨论 [Issue #424](https://github.com/hanwckf/immortalwrt-mt798x/issues/424)
同样建议新方案转向 nl80211/hostapd，而不是继续扩展旧 WEXT 控制面。

## IPv6

固件保留了 IPv6 内核模块及用户态组件，之后可以直接在 LuCI 中重新开启，不需要重编。
但 AX6000 和 GS7 位于同一个二层网络时，即使 AX6000 不发布 RA，GS7 的 IPv6 RA 仍会
到达客户端。测试 Nikki/daed 的全流量代理时，应临时关闭客户端 IPv6；若以后需要严格
控制 IPv6，应给旁路由客户端划分独立 VLAN 或子网。

## 服务默认状态

- TurboACC-MTK 使用 MediaTek HNAT，默认启用。
- EQoS-MTK 已安装，默认关闭；启用前填写实际上下行速率。
- Nikki、daed 均已安装且默认关闭。配置好代理后只启用其中一个，不能同时接管流量。
- daed 使用固件内置的 kernel BTF；切换到外部 `vmlinux-btf` 没有必要。
- Tailscale 和 WireGuard 已安装，但没有预置账号、密钥或隧道。
- htop 和 tcpdump 已安装，用于查看代理资源占用以及诊断延迟、丢包和策略路由。
- zram-swap 已启用作为内存不足时的保护，并把 `vm.swappiness` 设为 10，避免正常负载
  下主动交换影响延迟。
- 2.4 GHz 默认关闭，5 GHz 国家码默认设为 `US`；SSID、加密、信道由 LuCI 设置。
- MTK 驱动的 WPS 运行时默认关闭。

使用 `US` 国家码前应确认所在地区的无线电法规允许对应信道和发射功率。
