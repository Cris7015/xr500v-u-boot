#!/bin/sh
# SPDX-License-Identifier: GPL-2.0+
set -eu

script_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
source_dir=$(CDPATH='' cd -- "$script_dir/../.." && pwd)
build_dir=${BUILD_DIR:-"$source_dir/build/xr500v-ram-eth"}
artifact_dir=${ARTIFACT_DIR:-"$script_dir/build"}
cross_compile=${CROSS_COMPILE:-mips-linux-gnu-}
jobs=${JOBS:-$(nproc)}

export CROSS_COMPILE="$cross_compile"
SOURCE_DATE_EPOCH=${SOURCE_DATE_EPOCH:-$(
	git -C "$source_dir" show -s --format=%ct HEAD
)}
export SOURCE_DATE_EPOCH

mkdir -p "$build_dir" "$artifact_dir"

make -C "$source_dir" O="$build_dir" en7512_xr500v_ram_eth_defconfig
make -C "$source_dir" O="$build_dir" -j"$jobs"

config="$build_dir/.config"
elf="$build_dir/u-boot"
raw="$build_dir/u-boot-dtb.bin"
artifact="$artifact_dir/xr500v-u-boot-ram-eth.xmodem.bin"

test -s "$raw"

entry=$("${cross_compile}readelf" -h "$elf" |
	awk '/Entry point address:/ { print tolower($4) }')
test "$entry" = "0x81000000"

for symbol in MTD SPI SPL TPL CMD_SAVEENV CMD_BOOTP CMD_DHCP \
	CMD_TFTPBOOT CMD_MII CMD_MDIO CMD_NFS CMD_SNTP CMD_WGET; do
	grep -qx "# CONFIG_${symbol} is not set" "$config"
done
for setting in \
	'CONFIG_SYS_MEM_TOP_HIDE=0x100000' \
	'CONFIG_ENV_IS_NOWHERE=y' \
	'CONFIG_NET=y' \
	'CONFIG_DM_ETH=y' \
	'CONFIG_AIROHA_ETH=y' \
	'CONFIG_CMD_PING=y'; do
	grep -qx "$setting" "$config"
done

raw_size=$(wc -c < "$raw")
test "$raw_size" -le 1048576
xmodem_size=$(( (raw_size + 127) / 128 * 128 ))
cp "$raw" "$artifact"
truncate -s "$xmodem_size" "$artifact"

size_hex=$(printf '%x' "$xmodem_size")
sha256=$(sha256sum "$artifact" | awk '{print $1}')

printf 'file=%s\n' "$artifact"
printf 'load_address=a1000000\n'
printf 'entry_address=81000000\n'
printf 'raw_size_bytes=%s\n' "$raw_size"
printf 'transfer_size_bytes=%s\n' "$xmodem_size"
printf 'size_hex=%s\n' "$size_hex"
printf 'sha256=%s\n' "$sha256"
