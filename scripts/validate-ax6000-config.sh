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

require_value() {
	symbol="$1"
	value="$2"
	grep -qx "${symbol}=${value}" "$config" || {
		echo "Required symbol value is missing: ${symbol}=${value}" >&2
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
	CONFIG_PACKAGE_kmod-mediatek_hnat \
	CONFIG_PACKAGE_kmod-mt_wifi \
	CONFIG_PACKAGE_kmod-warp \
	CONFIG_MTK_MT_WIFI_DRIVER_VERSION_7673 \
	CONFIG_MTK_MT_WIFI_MT7986_20260601 \
	CONFIG_MTK_WIFI_FW_BIN_LOAD \
	CONFIG_MTK_DBDC_MODE \
	CONFIG_MTK_WARP_V2 \
	CONFIG_MTK_FAST_NAT_SUPPORT \
	CONFIG_MTK_HDR_TRANS_RX_SUPPORT \
	CONFIG_MTK_HDR_TRANS_TX_SUPPORT \
	CONFIG_MTK_RED_SUPPORT \
	CONFIG_MTK_CFG_SUPPORT_FALCON_MURU \
	CONFIG_MTK_MUMIMO_SUPPORT \
	CONFIG_MTK_MU_RA_SUPPORT \
	CONFIG_MTK_TXBF_SUPPORT \
	CONFIG_MTK_WIFI_TWT_SUPPORT \
	CONFIG_MTK_SCS_FW_OFFLOAD \
	CONFIG_MTK_SMART_CARRIER_SENSE_SUPPORT \
	CONFIG_MTK_MT_DFS_SUPPORT \
	CONFIG_MTK_MAP_SUPPORT \
	CONFIG_MTK_MAP_R2_VER_SUPPORT \
	CONFIG_MTK_MAP_R3_VER_SUPPORT \
	CONFIG_MTK_DOT11K_RRM_SUPPORT \
	CONFIG_MTK_DOT11R_FT_SUPPORT \
	CONFIG_MTK_DOT11W_PMF_SUPPORT \
	CONFIG_MTK_WNM_SUPPORT \
	CONFIG_MTK_WPA3_SUPPORT \
	CONFIG_WED_HW_RRO_SUPPORT \
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

require_value CONFIG_MTK_WHNAT_SUPPORT m
require_value CONFIG_MTK_MT_WIFI_FIRMWARE_PATH_MT7986 '"mt7986-fw-20260601"'
require_value CONFIG_WARP_CHIPSET '"mt7986"'
require_value CONFIG_WARP_VERSION 2
require_value CONFIG_FEED_video m
require_value CONFIG_FEED_nikki m

nikki_key="files/etc/apk/keys/nikki.pem"
nikki_key_sha256="677ef1af372065e2e856175363e2da9471a6c5c6443563b912c8d325bfa1fbad"
[ -f "$nikki_key" ] || {
	echo "Missing Nikki APK public key: $nikki_key" >&2
	exit 1
}
actual_nikki_key_sha256="$(sha256sum "$nikki_key" | awk '{print $1}')"
[ "$actual_nikki_key_sha256" = "$nikki_key_sha256" ] || {
	echo "Unexpected Nikki APK public key fingerprint: $actual_nikki_key_sha256" >&2
	exit 1
}

nikki_feed_file="files/etc/apk/repositories.d/customfeeds.list"
nikki_feed_url="https://nikkinikki.pages.dev/openwrt-25.12/aarch64_cortex-a53/nikki/packages.adb"
[ -f "$nikki_feed_file" ] || {
	echo "Missing Nikki APK feed configuration: $nikki_feed_file" >&2
	exit 1
}
nikki_feed_count="$(grep -Fxc "$nikki_feed_url" "$nikki_feed_file" || true)"
[ "$nikki_feed_count" -eq 1 ] || {
	echo "Nikki APK feed must appear exactly once: $nikki_feed_url" >&2
	exit 1
}

converter="package/mtk/applications/mtwifi-cfg-ucode/files/usr/share/ucode/mtwifi/converter.uc"
for mapping in \
	'RRMEnable.*ieee80211k' \
	'WNMEnable.*ieee80211v'
do
	grep -Eq "$mapping" "$converter" || {
		echo "Missing MTK roaming mapping: $mapping" >&2
		exit 1
	}
done

wnm_dbdc_patch="package/mtk/drivers/mt_wifi/patches-7673/041-merge-wnm-dbdc-profile.patch"
grep -q 'multi_profile_merge_perbss(data, "WNMEnable".*MPF_APPEND_0' "$wnm_dbdc_patch" || {
	echo "Missing MTK DBDC WNM profile merge fix" >&2
	exit 1
}

for profile in \
	package/mtk/drivers/wifi-profile/files/mt7986/mt7986-ax6000.dbdc.b0.dat \
	package/mtk/drivers/wifi-profile/files/mt7986/mt7986-ax6000.dbdc.b1.dat
do
	grep -qx 'WNMEnable=0' "$profile" || {
		echo "Missing AX6000 WNM default: $profile" >&2
		exit 1
	}
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
