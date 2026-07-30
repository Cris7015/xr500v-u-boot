#!/bin/sh
# SPDX-License-Identifier: GPL-2.0+
set -eu

script_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
out_dir=${1:-"$script_dir/build"}
cross_compile=${CROSS_COMPILE:-mips-linux-gnu-}

mkdir -p "$out_dir"

"${cross_compile}gcc" -EB -mips32r2 -mabi=32 -G0 -ffreestanding \
	-fno-pic -mno-abicalls -nostdlib -c \
	-o "$out_dir/uart-smoke.o" "$script_dir/uart-smoke.S"
"${cross_compile}ld" -EB -T "$script_dir/uart-smoke.lds" \
	-o "$out_dir/uart-smoke.elf" "$out_dir/uart-smoke.o"
"${cross_compile}objcopy" -O binary \
	"$out_dir/uart-smoke.elf" "$out_dir/uart-smoke.bin"

raw_size=$(wc -c < "$out_dir/uart-smoke.bin")
xmodem_size=$(( (raw_size + 127) / 128 * 128 ))
cp "$out_dir/uart-smoke.bin" "$out_dir/uart-smoke.xmodem.bin"
truncate -s "$xmodem_size" "$out_dir/uart-smoke.xmodem.bin"

size_hex=$(printf '%x' "$xmodem_size")
sha256=$(sha256sum "$out_dir/uart-smoke.xmodem.bin" | awk '{print $1}')

printf 'file=%s\n' "$out_dir/uart-smoke.xmodem.bin"
printf 'load_address=a1000000\n'
printf 'entry_address=81000000\n'
printf 'raw_size_bytes=%s\n' "$raw_size"
printf 'transfer_size_bytes=%s\n' "$xmodem_size"
printf 'size_hex=%s\n' "$size_hex"
printf 'sha256=%s\n' "$sha256"
