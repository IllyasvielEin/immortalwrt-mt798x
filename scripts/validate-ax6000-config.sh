#!/bin/sh

set -eu

config="${1:-.config}"

[ -f "$config" ] || {
	echo "Configuration not found: $config" >&2
	exit 1
}

require_y() {
	symbol="$1"
	grep -qx "${symbol}=y" "$config" || {
		echo "Required symbol is not enabled: $symbol" >&2
		exit 1
	}
}

reject_package() {
	package="$1"
	if grep -Eq "^CONFIG_PACKAGE_${package}=[ym]$" "$config"; then
		echo "Unwanted package is enabled: $package" >&2
		exit 1
	fi
}

for symbol in \
	CONFIG_TARGET_mediatek_filogic_DEVICE_xiaomi_redmi-router-ax6000-mtkuboot \
	CONFIG_KERNEL_DEBUG_INFO_BTF \
	CONFIG_KERNEL_CGROUP_BPF \
	CONFIG_KERNEL_BPF_EVENTS \
	CONFIG_KERNEL_NETKIT \
	CONFIG_KERNEL_XDP_SOCKETS \
	CONFIG_BPF_TOOLCHAIN_HOST \
	CONFIG_PACKAGE_luci-app-turboacc-mtk \
	CONFIG_PACKAGE_luci-app-eqos-mtk \
	CONFIG_PACKAGE_luci-app-nikki \
	CONFIG_PACKAGE_mihomo-meta \
	CONFIG_PACKAGE_luci-app-daed \
	CONFIG_PACKAGE_luci-app-tailscale-community \
	CONFIG_PACKAGE_luci-proto-wireguard \
	CONFIG_PACKAGE_htop \
	CONFIG_PACKAGE_tcpdump \
	CONFIG_PACKAGE_zram-swap
do
	require_y "$symbol"
done

profile_count="$(grep -Ec '^CONFIG_TARGET_mediatek_filogic_DEVICE_.+=y$' "$config" || true)"
[ "$profile_count" -eq 1 ] || {
	echo "Expected exactly one Filogic device profile, found: $profile_count" >&2
	exit 1
}

grep -q 'IMAGE_SIZE := 112640k' target/linux/mediatek/image/filogic-ext.mk || {
	echo "The MTK U-Boot 110 MiB image size definition changed upstream" >&2
	exit 1
}

for package in \
	luci-app-upnp miniupnpd luci-app-sqm sqm-scripts \
	luci-app-watchcat watchcat luci-app-wol etherwake \
	luci-app-lucky lucky luci-app-ddns ddns-scripts \
	luci-app-smartdns smartdns luci-app-mosdns mosdns \
	luci-app-openclash luci-app-passwall luci-app-passwall2 mihomo-alpha \
	luci-theme-argon nano
do
	reject_package "$package"
done

echo "AX6000 configuration validation passed"
