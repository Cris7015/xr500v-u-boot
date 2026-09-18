#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0+
"""Finalize the EN751221 chainloader binary.

The chainloader reserves a CRC32 table at the end of its loadable image.  Each
entry covers one 128-byte chunk of everything before the table, matching the
self_check() implementation in arch/mips/mach-econet/chainloader/chainloader.c.
The remaining table entries stay zero.

The image is then padded to a multiple of 128 bytes so the BootROM does not
invent padding of its own on top of .bss.
"""
import binascii
import struct
import sys

XMODEM_BLOCK = 128
CRC_TABLE_ENTRIES = 96
CRC_TABLE_SIZE = CRC_TABLE_ENTRIES * 4

path = sys.argv[1]
data = bytearray(open(path, 'rb').read())
if len(data) <= CRC_TABLE_SIZE:
    sys.exit('chainloader image too short')

chk_start = len(data) - CRC_TABLE_SIZE
nchunks = (chk_start + XMODEM_BLOCK - 1) // XMODEM_BLOCK
if nchunks > CRC_TABLE_ENTRIES:
    sys.exit('chainloader CRC table too small')

# .imgchk is linked as zero-filled storage.  Fill one big-endian CRC32 entry
# per 128-byte chunk of the image preceding the table.
data[chk_start:] = b'\x00' * CRC_TABLE_SIZE
for i in range(nchunks):
    start = i * XMODEM_BLOCK
    end = min(start + XMODEM_BLOCK, chk_start)
    crc = binascii.crc32(bytes(data[start:end])) & 0xffffffff
    off = chk_start + i * 4
    data[off:off + 4] = struct.pack('>I', crc)

pad = (-len(data)) % XMODEM_BLOCK
data += b'\x00' * pad
open(path, 'wb').write(bytes(data))
print('  CHAIN   %s: checked=0x%x blocks=%d table=%d size=%d'
      % (path, chk_start, nchunks, CRC_TABLE_ENTRIES, len(data)))
