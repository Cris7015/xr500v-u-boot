#!/bin/sh
# SPDX-License-Identifier: GPL-2.0+
#
# Build the EN751221 BootROM XMODEM chainloader.
#
# The BootROM loads it at 0x80009000 and it pulls an unmodified u-boot.bin
# over XMODEM into UBOOT_LOAD_ADDR (CONFIG_TEXT_BASE), then jumps there.
set -eu
soc="$1"
case "$soc" in
 en751221) endian=-EB; emulation=elf32btsmip ;;
 *) echo "unsupported chainloader SoC: $soc" >&2; exit 1 ;;
esac
: "${srctree:?}" "${objtree:?}" "${CROSS_COMPILE:?}" "${UBOOT_LOAD_ADDR:?}"
src="$srctree/arch/mips/mach-econet/chainloader"
build="$objtree/.econet-chainloader-$soc"
out="$objtree/$soc-chainloader.bin"
mkdir -p "$build"
cc="${CROSS_COMPILE}gcc"
ld="${CROSS_COMPILE}ld"
objcopy="${CROSS_COMPILE}objcopy"
flags="$endian -mabi=32 -mips32r2 -msoft-float -mno-abicalls -fno-pic -fno-pie \
 -ffreestanding -fno-builtin -fno-stack-protector -Os -G0 -Wall"
"$cc" $flags -c "$src/start.S" -o "$build/start.o"
"$cc" $flags -DUBOOT_LOAD_ADDR="$UBOOT_LOAD_ADDR" \
 -c "$src/chainloader.c" -o "$build/chainloader.o"
"$ld" $endian -m "$emulation" -T "$src/chainloader.lds" \
 -Map "$build/chainloader.map" -o "$build/chainloader.elf" \
 "$build/start.o" "$build/chainloader.o"
"$objcopy" -O binary "$build/chainloader.elf" "$out"
# Per-128-byte self-check table plus padding to the BootROM XMODEM block size.
"${PYTHON3:-python3}" "$srctree/tools/econet_chainloader_image.py" "$out"
