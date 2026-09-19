#!/bin/sh
# SPDX-License-Identifier: GPL-2.0+
#
# Build the EN751221 BootROM XMODEM chainloader.
#
# The BootROM loads it at 0x80009000 and it pulls an unmodified u-boot.bin
# over XMODEM into UBOOT_LOAD_ADDR (CONFIG_TEXT_BASE), then jumps there.
#
# With DDR_STAGE_BIN set (CONFIG_ECONET_BOOTROM_CHAINLOADER_DDR), a second
# copy linked for FE SRAM and the V1.2.2 DDR stage are embedded: the DRAM
# copy moves both into FE SRAM, and the SRAM copy runs the DDR stage as
# boot2 does on the flash path before receiving u-boot.bin.
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
ddr="${DDR_STAGE_BIN:-}"
mkdir -p "$build"
cc="${CROSS_COMPILE}gcc"
ld="${CROSS_COMPILE}ld"
objcopy="${CROSS_COMPILE}objcopy"
flags="$endian -mabi=32 -mips32r2 -msoft-float -mno-abicalls -fno-pic -fno-pie \
 -ffreestanding -fno-builtin -fno-stack-protector -Os -G0 -Wall"
objs="$build/start.o $build/chainloader.o"
mode=

if [ -n "$ddr" ]; then
	test -s "$ddr" || { echo "missing DDR stage $ddr" >&2; exit 1; }
	"$cc" $flags -DSRAM_STAGE -c "$src/start.S" -o "$build/sram-start.o"
	"$cc" $flags -DSRAM_STAGE -DUBOOT_LOAD_ADDR="$UBOOT_LOAD_ADDR" \
	 -c "$src/chainloader.c" -o "$build/sram-chainloader.o"
	"$ld" $endian -m "$emulation" -T "$src/chainloader-sram.lds" \
	 -Map "$build/sram.map" -o "$build/sram.elf" \
	 "$build/sram-start.o" "$build/sram-chainloader.o"
	"$objcopy" -O binary "$build/sram.elf" "$build/sram.bin"
	"$cc" $flags -DSRAM_STAGE_BIN="\"$build/sram.bin\"" \
	 -DDDR_STAGE_BIN="\"$ddr\"" -c "$src/ddr_blobs.S" -o "$build/ddr_blobs.o"
	objs="$objs $build/ddr_blobs.o"
	mode=-DEMBED_SRAM_STAGE
fi

"$cc" $flags -c "$src/start.S" -o "$build/start.o"
"$cc" $flags $mode -DUBOOT_LOAD_ADDR="$UBOOT_LOAD_ADDR" \
 -c "$src/chainloader.c" -o "$build/chainloader.o"
"$ld" $endian -m "$emulation" -T "$src/chainloader.lds" \
 -Map "$build/chainloader.map" -o "$build/chainloader.elf" $objs
"$objcopy" -O binary "$build/chainloader.elf" "$out"
# Per-128-byte self-check table plus padding to the BootROM XMODEM block size.
"${PYTHON3:-python3}" "$srctree/tools/econet_chainloader_image.py" "$out"
