<!-- SPDX-License-Identifier: GPL-2.0+ -->

# XR500v RAM-only U-Boot hardware validation

Date: 2026-07-29
Device: TP-Link Archer XR500v v1, EN751221, 256 MiB DDR3
Upstream base: `aba03cf8d8a37b72b5d1aef52f99b060aa7139a3`

## Scope and safety boundary

These tests chainloaded U-Boot proper from the proprietary Bootbase/BLDR.
They did not replace Bootbase, change `bflag`, write NAND, load SPL/TPL, or
probe the SPI-NAND, GPON MAC or EN7570 PHY.  Stages 1 and 2 were console-only;
stages 3 and 4 deliberately exercised only the Ethernet switches and
FE/QDMA path.

The OpenWrt debug helper read `0x1fb00284 = 0x000e1100` before reboot. The low
12 bits are `0x100`, confirming the 256 MiB value consumed by U-Boot's
`dram_init()`.

BLDR's XMODEM length is informational rather than a write bound. Both
transfers therefore used explicitly padded artifacts and 128-byte XMODEM
packets. XMODEM-1k was not used.

## Stage 1: minimal UART marker

Artifact:

- File: `build/uart-smoke.xmodem.bin`
- Size: 128 bytes (`0x80`)
- SHA-256:
  `afaf6581a3e02dcc0cff3e66634b90fe7730df4b4482facde9f90263f15cbce2`

BLDR sequence:

```text
xmdm a1000000 80
received len=80
memrl a1000000  -> 40086000
memrl a1000004  -> 2409fffe
jump 81000000
```

Observed result:

```text
Jump to 81000000

XR500V RAM JUMP OK
```

This proves the uncached XMODEM load, cached execution alias, MIPS32r2
entrypoint and inherited EN751221 UART path.

## Stage 2: console-only U-Boot proper

Artifact:

- File: `build/xr500v-u-boot-ram-only.xmodem.bin`
- Raw U-Boot plus DTB: 206855 bytes (`0x32807`)
- Padded transfer: 206976 bytes (`0x32880`)
- SHA-256 of padded artifact:
  `32b57e4d6e42067fc26fad983901b42ebbe28f2cae7052a8caaf892ac6e82754`

BLDR sequence:

```text
xmdm a1000000 32880
received len=32880
memrl a1000000  -> 1000013f
memrl a1000004  -> 00000000
jump 81000000
```

Observed startup:

```text
U-Boot 2026.07-00056-gaba03cf8d8a3-dirty

CPU:   EcoNet/Airoha EN7512/EN7521 MIPS34K
Clock: 225 MHz
Model: TP-Link Archer XR500v v1 (RAM-only chainload)
DRAM:  255 MiB
Core:  3 devices, 3 uclasses, devicetree: separate
Loading Environment from <NULL>... OK
In:    serial@1fbf0000
Out:   serial@1fbf0000
Err:   serial@1fbf0000
=>
```

The 255 MiB figure is intentional: `CONFIG_SYS_MEM_TOP_HIDE=0x100000`
preserves the XR500v's top-of-RAM `ramoops` reservation.

`bdinfo` reported:

```text
DRAM bank   = 0x00000000
-> start    = 0x80000000
-> size     = 0x0ff00000
flashstart  = 0x00000000
flashsize   = 0x00000000
relocaddr   = 0x8fec0000
fdt_blob    = 0x8fdbe840
```

The effective driver-model tree contained only:

```text
root_driver
`-- simple_bus
    `-- serial_en75xx at serial@1fbf0000
```

The command list had no network, MTD, SPI, NAND or persistent-environment
commands. `saveenv`, `mtd`, `sf`, `nand`, `tftpboot`, `ping` and `dhcp` were
absent. Commands such as `mw`, `go`, `bootm` and `reset` can still modify or
execute arbitrary RAM/MMIO when entered manually, so the first-light test
used only `version`, `bdinfo`, `meminfo`, `dm tree` and `help`.

## Console-only result and boundary

Console-only U-Boot proper is hardware-validated on the XR500v. Recovery
remains a physical cold power cycle; no persistent state was changed.

The later stages below add the EN751221 Ethernet path as a separate RAM-only
target. SPI, MTD, NAND, SPL and TPL remain disabled until the XR500v
F50L1G/BMT/BBT behavior is modeled explicitly. The reference-board
`u-boot-en7512.bin` image must never be flashed to this device.

Raw UART capture:
`build/xr500v-chainload.log`

## Stage 3: RAM-only Ethernet probe

The separate Ethernet target was chainloaded without fibre.  The XMODEM
transfer completed at `0x3a600` bytes and the first words at `0xa1000000`
were verified as `0x1000013f` and `0x00000000` before jumping to
`0x81000000`.

U-Boot reached the prompt and enumerated exactly one network device:

```text
Net:
Warning: airoha-gdm1 (eth0) using random MAC address
eth0: airoha-gdm1
```

The probe-only register capture matched the intended FE/QDMA and integrated
switch configuration.  In particular, QDMA global configuration was
`0x1c0000f0`, with TX/RX DMA still disabled; switch P5/P6 were
`0x0009e30b`/`0x0005e30b`; and the TRGMII enable/control registers retained
their trained state across the BLDR handoff.

No network command was executed in this stage.  SPI, MTD, NAND and persistent
environment support remained absent.

Raw UART capture:
`build/xr500v-ram-eth-e1-20260729.log`, SHA-256
`e1f1d2fc39dff84d32e233019bf7d58e3437e8a59db71f658577cc1b9cc16656`.

## Stage 4: cold Ethernet round trip and TX reclaim

A genuine cold BLDR chainload preserved both MT7530 switches in ordinary
Ethernet mode (`PCR6=0x00ff0000`, `PVC6=0x810000c0`).  The first RAM-only
image completed an ARP/ICMP round trip, proving QDMA, FE, integrated switch,
TRGMII and external MCM operation.  Repeated commands then exhausted the
four-entry HWFWD pool because the EN751221 branch had no TX completion queue.

The E4 image restores the legacy queue at QDMA offsets `0x60`-`0x6c` and
advances `IRQ_CLEAR_LEN` once after each synchronous TX completion.  On
hardware it completed 16/16 consecutive pings, kept LMGR at `free=4`,
`usage=0`, and wrapped the 32-entry completion head from `0x17` to `0x03`
without pending entries or HWFWD low/empty status.

Artifact:
`build/xr500v-u-boot-ram-eth-e4-irq-reclaim-20260729.xmodem.bin`, 239360
bytes, SHA-256
`a70f525ededa62d3bb50bc72070e38ea55192b01a9bf074dd1fb5dc72c659ff5`.

Raw UART capture:
`build/xr500v-ram-eth-e4-irq-reclaim-20260729.log`, 6007 bytes, SHA-256
`9d254110bff602e35e1b4569615ff4bb6dee3bc68a0e68ed999ea44de56e8cd5`.
