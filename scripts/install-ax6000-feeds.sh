#!/bin/sh
# Install only this target's feed packages and their recursive dependencies.
set -eu

config="${1:-defconfig/mt7986-ax6000.config}"
[ -f "$config" ] || {
	echo "Configuration not found: $config" >&2
	exit 1
}

python3 scripts/prepare-ax6000-feeds.py --check
./scripts/feeds uninstall -a
# Select the stable provider before resolving Nikki's virtual mihomo dependency.
./scripts/feeds install mihomo-meta luci luci-proto-ipv6
sed -n 's/^CONFIG_PACKAGE_\([[:alnum:]_+.-]*\)=y$/\1/p' "$config" |
	xargs -r ./scripts/feeds install
./scripts/feeds uninstall mihomo-alpha
