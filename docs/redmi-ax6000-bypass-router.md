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
