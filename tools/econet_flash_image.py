#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0+
"""Pack unsigned 128 KiB EcoNet startup plus an uncompressed U-Boot uImage."""
import argparse
from pathlib import Path
import struct
import zlib

LIMIT = 0x100000
PAYLOAD = 0x20000
CRC = PAYLOAD - 4
MINFO = 0xff00


def pack(stages, symbols, uboot, soc, load, minfo=None):
    endian = 'big' if soc in ('en751221', 'en751627') else 'little'
    if not 0x81000000 <= load < 0x82000000:
        raise ValueError('load address must be in 0x81000000..0x81ffffff')
    if len(stages) >= MINFO or len(stages) < 0x60:
        raise ValueError('initial stages overlap mi.conf or are incomplete')
    if len(uboot) < 64 or len(uboot) > LIMIT - PAYLOAD:
        raise ValueError('U-Boot must fit in the remaining 896 KiB')
    magic, hcrc, _, size, addr, entry, dcrc = struct.unpack_from('>7I', uboot)
    header = bytearray(uboot[:64])
    header[4:8] = bytes(4)
    if magic != 0x27051956 or zlib.crc32(header) != hcrc:
        raise ValueError('invalid U-Boot legacy header/CRC')
    if size != len(uboot) - 64 or zlib.crc32(uboot[64:]) != dcrc:
        raise ValueError('invalid U-Boot payload size/CRC')
    if addr != load or entry != load or uboot[29:32] != bytes((5, 5, 0)):
        raise ValueError('expected uncompressed MIPS firmware at configured TEXT_BASE')
    if load + size > 0x82000000:
        raise ValueError('payload exceeds the minimum 32 MiB DRAM window')
    image = bytearray(b'\xff' * LIMIT)
    image[:len(stages)] = stages
    if soc == 'en751221':
        # This SDK revision enters at +0x48.  The tcboot dumped from the
        # target enters at +0x40; +0x40..+0x47 are reserved NOPs, so make
        # the reset vector match the target and fall through to +0x48.
        if int.from_bytes(image[:4], 'big') != 0x0bf00012 or image[0x40:0x48] != bytes(8):
            raise ValueError('unexpected EN751221 SDK reset/header layout')
        image[:4] = (0x0bf00010).to_bytes(4, 'big')
    else:
        # Reserved header fields must be deterministic, not arbitrary padding.
        image[8:0x60] = bytes(0x58)
    def put(offset, value):
        image[offset:offset+4] = value.to_bytes(4, endian)
    def interval(name):
        a, b = (symbols[f'__{name}_{side}'] - 0xbfc00000 for side in ('start', 'end'))
        if not 0x60 <= a < b <= len(stages):
            raise ValueError(f'invalid {name} interval')
        return a, b
    move = interval('move_data')
    boot2 = interval('boot2')
    loader = interval('lzma')
    ddr = interval('spram')
    if move[1] >= 0x800 or not move[1] <= boot2[0] < boot2[1] <= loader[0] < loader[1] <= ddr[0]:
        raise ValueError('invalid first-page placement or overlapping stages')
    if soc != 'en751221':
        put(8, PAYLOAD)
    image[12:16] = b'6578'
    put(0x10, loader[0])
    put(0x14, loader[1])
    put(0x18, PAYLOAD)
    put(0x1c, PAYLOAD + len(uboot))
    if soc != 'en751221':
        put(0x28, 0x00040010)  # SDK controller ECC: 4 bits, 16 spare bytes/sector
        put(0x30, 0x9fa30000)
        put(0x34, 0x80000000)
        if soc == 'en7528':
            put(0x40, 0x035a3c96)  # SDK efuse clock width = 3
        put(0x50, boot2[0])
        put(0x54, boot2[1])
        put(0x58, ddr[0])
        put(0x5c, ddr[1])
    if soc == 'en751627':
        # The SDK ELF entry is +0x280; boot2 enters the raw image at +0.
        # Preserve the original payload, adding only a jump in its zero prefix.
        if image[ddr[0]:ddr[0]+8] != bytes(8):
            raise ValueError('EN751627 DDR entry prefix is not reserved zero space')
        image[ddr[0]:ddr[0]+8] = struct.pack('>II', 0x0be8c0a0, 0)
    if minfo is not None and len(minfo) != 0x100:
        raise ValueError('mi.conf must be exactly 256 bytes')
    image[MINFO:MINFO+0x100] = bytes(0x100) if minfo is None else minfo
    # Both move_data binaries compare the native word against 0x50414745.
    put(0xffb0, 0x50414745)
    if soc == 'en7528':
        # Additional marker required by the LE internal-ROM NAND ECC path.
        put(0x1fff8, 0x50414745)
    image[PAYLOAD:PAYLOAD+len(uboot)] = uboot
    put(CRC, zlib.crc32(image[:CRC]) ^ 0xffffffff)
    return image


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--soc', choices=('en751221', 'en751627', 'en7528'), required=True)
    for name in ('stages', 'symbols', 'uboot', 'output'):
        p.add_argument('--'+name, type=Path, required=True)
    p.add_argument('--load', type=lambda s: int(s, 0), required=True)
    p.add_argument('--minfo', type=Path, help='optional exact 256-byte manufacturing area')
    args = p.parse_args()
    try:
        symbols = {}
        for line in args.symbols.read_text().splitlines():
            fields = line.split()
            if len(fields) == 3:
                symbols[fields[2]] = int(fields[0], 16)
        image = pack(args.stages.read_bytes(), symbols, args.uboot.read_bytes(),
                     args.soc, args.load, args.minfo.read_bytes() if args.minfo else None)
        temp = args.output.with_suffix(args.output.suffix+'.tmp')
        temp.write_bytes(image)
        temp.replace(args.output)
    except (ValueError, KeyError, OSError) as exc:
        p.exit(1, f'flash image: {exc}\n')


if __name__ == '__main__':
    main()
