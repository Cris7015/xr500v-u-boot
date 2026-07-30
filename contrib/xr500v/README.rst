.. SPDX-License-Identifier: GPL-2.0+

XR500v RAM-only chainload bring-up
==================================

This directory contains the first, deliberately non-persistent validation
stage for entering code linked at ``0x81000000`` from the XR500v proprietary
Bootbase/BLDR.

The UART marker, console-only target and RAM-only Ethernet target were
validated on real XR500v hardware on 2026-07-29.  See
``HARDWARE-VALIDATION.md`` for the captured addresses, historical hashes and
runtime output.

The order is intentional:

1. Load ``uart-smoke.xmodem.bin`` through the uncached RAM alias with
   ``xmdm a1000000 <padded-size-hex>`` and execute through the cached alias
   with ``jump 81000000``.  Both addresses refer to physical ``0x01000000``.
   This pairing matters because BLDR's XMODEM and jump paths do not flush
   caches.  Success is exactly ``XR500V RAM JUMP OK`` followed by an infinite
   loop.  Recover by removing power.
2. Only after that marker is proven, load the console-only
   ``contrib/xr500v/build/xr500v-u-boot-ram-only.xmodem.bin`` and use the same
   jump address.
3. Use the separate ``en7512_xr500v_ram_eth_defconfig`` target for the
   volatile Ethernet path described in ``ETHERNET-RAM-TEST.md``.  Its
   cold-boot TX/RX path and completion reclaim are hardware-validated, but it
   must not replace the console-only recovery target.
4. Keep SPI/MTD disabled until the XR500v NAND ID, read protocol and BMT/BBT
   handling have been validated separately.

Neither first-light payload contains a flash write path.  Never use
``u-boot-en7512.bin`` on the XR500v: that image is the reference-board
TPL+DDR-blob+SPL+U-Boot flash layout and is larger than the XR500v 256 KiB
``boot`` partition.

Build the marker with a MIPS cross compiler, for example::

    STAGING_DIR=/path/to/openwrt/staging_dir \
    CROSS_COMPILE=/path/to/toolchain/bin/mips-openwrt-linux-musl- \
      contrib/xr500v/build-uart-smoke.sh

Build console-only U-Boot proper with::

    contrib/xr500v/build-ram-only.sh

Build the separate Ethernet experiment with::

    contrib/xr500v/build-ram-eth.sh

Both scripts pad their transfer artifacts to a complete 128-byte XMODEM
packet and print the exact hexadecimal byte count.  BLDR hexadecimal
arguments do not use a ``0x`` prefix.  Changing ``bflag`` is unnecessary and
would write persistent boot metadata, so leave the current known-good slot
alone.  Do not automate the final ``jump``: keep UART visible and issue it
manually only after confirming that XMODEM completed.

Always use the ``file``, ``size_hex`` and ``sha256`` printed by the same build
invocation.  Generated filenames are reused by later builds, while the hashes
and sizes in the validation documents describe historical payloads.  Never
combine an old ``xmdm`` size with a newly generated file.

BLDR's ``xmdm`` implementation does not enforce its length argument as a
write bound.  It accepts packets until EOT and writes every byte it receives.
Selecting the wrong file can therefore overwrite unrelated RAM even when the
command contains the expected length.  Use standard 128-byte XMODEM with
``sx --xmodem --binary``; never add ``-k`` or ``--1k``.

Both RAM-only targets hide the top 1 MiB of DDR from U-Boot relocation so the
XR500v OpenWrt ``ramoops`` reservation is not overwritten.  Neither has SPI,
MTD, SPL or TPL support.  The console-only target also omits Ethernet.

First hardware test
-------------------

Use a cold boot and keep the fibre disconnected.  Open the only UART session
with a send command that cannot select XMODEM-1k::

    sudo picocom --baud 115200 --databits 8 --parity n --stopbits 1 \
      --flow n --send-cmd 'sx --xmodem --binary -vv' \
      --logfile contrib/xr500v/build/xr500v-chainload.log /dev/ttyUSB0

For the 0.1-second Bootbase interrupt window, the included expect wrapper can
watch the same UART, send the interrupt immediately, and stop all automatic
input as soon as ``bldr>`` appears::

    expect contrib/xr500v/capture-bldr.exp /dev/ttyUSB0 \
      contrib/xr500v/build/xr500v-chainload.log

Intercept ``bldr>`` and run::

    xmdm a1000000 80

While BLDR emits ``C``, press ``Ctrl-A``, then ``Ctrl-S``, and select exactly::

    contrib/xr500v/build/uart-smoke.xmodem.bin

After ``received len=80``, verify the uncached load before executing it::

    memrl a1000000
    memrl a1000004

The expected words are ``40086000`` and ``2409fffe``.  Only then run::

    jump 81000000

Success is the fixed marker followed by the deliberate hang.  Remove power
for the known 35-second cold-reset interval; do not use the Bootbase re-entry
address between payloads because no instruction-cache flush is performed.

On the next cold boot, build the console-only target and copy its printed
``size_hex`` value into::

    xmdm a1000000 <size_hex>

Send exactly the ``file`` printed by that invocation and verify its printed
SHA-256 first.  After transfer, the expected first words at ``a1000000`` and
``a1000004`` are ``1000013f`` and ``00000000``.  Then run
``jump 81000000``.  At the first U-Boot prompt use only ``version``,
``bdinfo``, ``meminfo`` and ``help``.  Recover with physical power removal.
