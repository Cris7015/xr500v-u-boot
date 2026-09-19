EN751221 BootROM XMODEM chainloader
===================================

What it is
----------

``en751221-chainloader.bin`` is a ~9 KiB standalone loader for the EN751221
internal BootROM recovery path. The BootROM downloads it to ``0x80009000``
over XMODEM and jumps there; it then pulls an **unmodified** ``u-boot.bin``
over XMODEM into ``CONFIG_TEXT_BASE`` and jumps to it.

Nothing in U-Boot proper changes. This replaces an earlier attempt at linking
U-Boot itself at ``0x80009000`` so the BootROM could load it directly, which
needed ``SKIP_LOWLEVEL_INIT``, ``SKIP_RELOCATE``, a second ``TEXT_BASE``, a
second device tree and SoC breadcrumbs inside ``arch/mips/cpu/start.S`` and
``common/board_f.c``.

Use
---

Enable ``CONFIG_ECONET_BOOTROM_CHAINLOADER`` (default ``y`` on
``TARGET_EN751221``) and ``make``. The image lands next to ``u-boot.bin``.

Hold the board in BootROM recovery, send the chainloader with the BootROM's
own XMODEM, then send ``u-boot.bin`` to the chainloader::

  picocom -b 115200 --send-cmd "<path>/xsend.sh" /dev/ttyUSB0

Send ``u-boot.bin`` **through a wrapper that flushes the serial input queue**
before running ``sx``::

  #!/bin/sh
  python3 -c 'import termios; termios.tcflush(0, termios.TCIFLUSH)'
  exec sx -k -X "$@"

Without the flush, the ``'C'`` characters the receiver emits while waiting pile
up in the host's input queue. ``sx`` consumes one during the handshake and
reads the rest in place of the first ACK; in lrzsz ``case WANTCRC:`` falls
through to ``case NAK:``, so every one of them prints ``Retry N: NAK on
sector`` until ``Retry Count Exceeded``. ``-k`` uses 1 KiB blocks: 367 instead
of 2936 for a 375 KiB ``u-boot.bin``.

Running the DDR stage (CONFIG_ECONET_BOOTROM_CHAINLOADER_DDR)
--------------------------------------------------------------

The BootROM recovery path is not the flash boot path. The BootROM only runs
its own DRAM init (``7512DRAMC V1.0``); on the flash path boot2 then runs the
V1.2.2 DDR stage (``en751221/ddr``, 0x9fa32800) before U-Boot. Without that
stage, measured on the TP-Link Archer XR500v v1:

- the CPU/bus PLLs keep the BootROM's setting (``0xbfa2019c``/``0xbfa201ac`` =
  ``05102408``/``03b33333`` instead of ``05102308``/``03800000``): the CPU runs
  at ~940 MHz, a U-Boot ``sleep 20`` takes 19.15 s;
- ``REG_SAVE_INFO`` has no clock field, so Linux registers a 0 Hz bus clock
  and its watchdog resets the board as soon as procd opens it;
- DRAMC keeps the BootROM's parameters instead of the V1.2.2 BGA ones;
- QDMA never writes DONE back into TX descriptors: U-Boot has to rely on
  the TX IRQ queue, and Linux stalls after its first TX completions.

With the option enabled, the image downloaded by the BootROM embeds a second
build of the chainloader (``-DSRAM_STAGE``, linked at 0x9fa30800 by
``chainloader-sram.lds``) and ``en751221_ddr.bin``. After ``self_check()``
passes, the DRAM copy sets ``SHARE_FEMEM_SEL``, copies both into FE SRAM with
32-bit stores, reads them back and jumps to the SRAM copy. That one calls the
DDR stage with its return address in ``SCREG_WR0`` exactly like boot2, applies
boot2's post-calibration writes (SLM bypass, arbiter, SMC), recalibrates its
timebase and receives ``u-boot.bin`` as usual::

  FE SRAM copy: loader 0x00001180 ddr 0x00004040 readback bad=0x00000000
  running V1.2.2 DDR stage at 0x9fa32800
  EN751221 DRAMC v1.2.2 ...
  BGA IC / Xtal: 25Mhz / DDR3 init. / DRAM size=256MB / ddr-1066
  calibration status: 0
  DDR stage returned; recalibrating CP0 Count against the UART
  ticks/ms=0x0006dcaa pll 19c=0x05102308 1ac=0x03800000 save_info=0x000e1100
  waiting

The DDR stage prints capital ``C`` characters (``DDR CALI``, ``Calculate``);
a host script should only look for the XMODEM ``C`` after ``waiting``.

Three SoC quirks this had to work around
----------------------------------------

**1. Uncached 8-bit stores corrupt the rest of the word.** An ``sb`` to
uncached DRAM becomes a read-modify-write on the peripheral bus whose read side
returns stale bus data, so the other three bytes of the word come back as
leftovers of unrelated transactions. Measured on the board, writing
``0x10..0x1f`` with byte stores and reading back as words::

  KSEG0 cached:   10111213 14151617 18191a1b 1c1d1e1f
  KSEG1 uncached: 10101210 14101610 08100a10 0c100e10

The chainloader therefore runs with ``Config.K0 = 3`` (cached), keeps its stack
in KSEG0, and receives U-Boot into cached memory. ``chainload_jump`` does
``Index_Writeback_Inv_D`` plus ``Index_Invalidate_I`` before handing over, so
the received image is in DRAM, and leaves KSEG0 uncached for U-Boot to
reconfigure.

**2. The UART needs a settle between LSR and RBR.** ``LSR.DR`` going high does
not mean ``RBR`` is valid yet; reading it immediately returns bus leftovers.
The vendor driver has the same workaround::

  /* tc3162_uart.c */
  #define READ_OTHER(x) ((x & 0xc) + 0xbfb003a0)
  tmp = VPint(READ_OTHER(CR_UART_RBR));  wmb();
  ch  = UART_RDL(CR_UART_RBR);           wmb();

``uart_rx_settle()`` does the equivalent with a register this board is known to
tolerate. All UART access is 32-bit; the registers are 32-bit spaced
(``RBR/THR`` 0x00, ``IER`` 0x04, ``IIR/FCR`` 0x08, ``LCR`` 0x0c, ``MCR`` 0x10,
``LSR`` 0x14) at ``0xbfbf0000``.

**3. The BootROM leaves dirty cache lines.** It writes the downloaded image
through the cache, so ``start.S`` writes back and invalidates before touching
anything. ``LSR`` bit 6 (TEMT) is never asserted on this part, so nothing waits
on it.

Self-check
----------

The BootROM validates its own XMODEM download with an 8-bit checksum, which
lets corruption through. ``tools/econet_chainloader_image.py`` fills the
``.imgchk`` table at the end of the image with a CRC32 per 128-byte block and
pads to 128 bytes; the chainloader recomputes every block in DRAM and reports
the first bad one::

  EN751221 BootROM chainloader
  XMODEM 128/1k, CRC16 ou checksum -> 0x81000000
  ticks/ms=0x00070627 self len=0x00001200 blocks=0x00000024 bad=0x00000000 OK

``.imgchk`` must have contents (``LONG()`` in the linker script): a section
holding only ``FILL`` is NOBITS, ``objcopy -O binary`` leaves it out, and the
table would land on the end of ``.rodata`` instead.

Everything mutable lives in ``.bss``, which is outside the checked region and
zeroed by ``start.S`` (the BootROM's XMODEM padding lands there).

There is no wall clock, so ``calibrate()`` derives CP0 Count ticks per
millisecond from the time the banner took to shift out at 115200 8N1. Timeouts,
the 3 s handshake cadence and the purge-before-NAK all hang off that.

On failure the report gives ``blocks=`` ``dup=`` ``tries=`` ``mode=`` ``err=``
``lsrerr=`` plus the first error: ``kind`` 1 bad header, 2 intra-packet
timeout, 3 CRC/checksum, 4 out-of-sequence, with the first 16 bytes received.
