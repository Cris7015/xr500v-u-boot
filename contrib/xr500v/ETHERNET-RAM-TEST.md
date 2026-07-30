<!-- SPDX-License-Identifier: GPL-2.0+ -->

# XR500v RAM-only Ethernet test

Date prepared and E1-E4 tested: 2026-07-29
Device: TP-Link Archer XR500v v1, EN751221, 256 MiB DDR3
Status: E4 TX completion reclaim passed; cold packet path stable for 16/16 pings

## Boundary

This is a separate volatile target.  It chainloads U-Boot proper after the
proprietary Bootbase/BLDR and adds only the EN751221 FE, QDMA0 and integrated
MT7530 path over the validated console target.

It has no SPI, MTD, NAND, SPL, TPL, persistent environment, DHCP or network
loader.  It must not change `bflag`, touch GPON/EN7570 or be flashed.  Recovery
after every run is a physical cold power cycle of at least 35 seconds.

The probe is not passive: `board_late_init()` resets FE and QDMA0, replaces
their descriptor state and programs the integrated switch before the U-Boot
prompt.  TX and RX DMA remain disabled until `ping` is invoked.

The four LAN sockets belong to an external MCM MT7530.  This first target does
not initialize that second switch or the TRGMII training path; it deliberately
tests whether their Bootbase state survives the chainload.

## Build result

Build with:

```text
contrib/xr500v/build-ram-eth.sh
```

Historical E1/E3 artifact provenance:

- original generated name: `build/xr500v-u-boot-ram-eth.xmodem.bin`
- raw U-Boot plus DTB: 239073 bytes
- padded XMODEM transfer: 239104 bytes (`0x3a600`)
- entry: `0x81000000`
- SHA-256:
  `57c1682e8baa3fcac4ba73c9f3920d2afe919c6ee213a3021b03daad07a03b78`

That exact 239104-byte payload is no longer present under the generated
filename: the same name was later overwritten by the E4 build.  Do not send
the current generic file with the historical `0x3a600` command.  For every
new build, use only the `file`, `size_hex` and `sha256` printed together by
the wrapper.

The wrapper verifies the entry point, one-MiB size ceiling, 128-byte XMODEM
padding and the absence of every persistent or network-loading feature listed
above.

## OpenWrt baseline

A read-only snapshot from the working 6.12.80 OpenWrt image immediately before
the experiment returned:

```text
RESET_CONTROL2  0x1fb00834 = 00000000
FE_SRAM_SEL     0x1fb00958 = 00000000
QDMA0_GLO_CFG   0x1fb54004 = 1c0800f5
GDM1_FWD_CFG    0x1fb50500 = 03f04004
GDM1_LEN_THR    0x1fb50514 = 05f2003c
SWITCH_MFC      0x1fb58010 = ffffffe0
SWITCH_PMCR5    0x1fb5b500 = 0009e30b
SWITCH_PMCR6    0x1fb5b600 = 0005e30b
SWITCH_PHY_POLL 0x1fb5f018 = 007f8600
TRGMII_CLK      0x1fb5f804 = 0001600f
TRGMII_EN       0x1fb5f808 = 00000001
TRGMII_PORT_EN  0x1fb5f830 = 00000001
TRGMII_CTRL     0x1fb5fa00 = 00020000
TRGMII_RESET    0x1fb5fa40 = 40020000
```

The PMCR values exposed a bug in the imported driver: it used shrink IPG on
both ports.  The RAM-Ethernet build now keeps shrink IPG on the P5 TRGMII
cascade and uses short IPG on CPU P6.  The first-light value `0x7f7f8c08` for
PHY polling is retained because the OEM Bootbase source explicitly programs
that value on the integrated switch; Linux DSA subsequently leaves a
different runtime value.

## E1: historical probe-only UART test

The following command records the original E1 procedure and must not be
replayed with the current generated artifact.  The fibre was disconnected and
the historical image was loaded from a cold BLDR prompt:

```text
xmdm a1000000 3a600
```

The original transfer reported `received len=3a600`.  Its
`memrl a1000000` and `memrl a1000004` values were `1000013f` and
`00000000`, respectively.  It was then entered with:

```text
jump 81000000
```

At the U-Boot prompt, do not issue a network command.  Capture:

```text
version
bdinfo
dm tree
net list
printenv ethaddr
md.l bfb00834 1
md.l bfb00958 1
md.l bfb54004 1
md.l bfb50500 1
md.l bfb50514 1
md.l bfb58010 1
md.l bfb5b500 1
md.l bfb5b600 1
md.l bfb5f018 1
md.l bfb5f804 1
md.l bfb5f808 1
md.l bfb5f830 1
md.l bfb5fa00 1
md.l bfb5fa40 1
```

Expected values after a successful probe:

```text
bfb00834: reset bits 1 and 21 clear
bfb00958: 00000000
bfb54004: 1c0000f0
bfb50500: 02000000
bfb50514: 0600003c
bfb58010: ffffffe0
bfb5b500: 0009e30b
bfb5b600: 0005e30b
bfb5f018: 7f7f8c08
```

`bfb54004` bits zero and two must still be clear, proving that the probe did
not start TX or RX DMA.  Power off for 35 seconds after the capture; do not
jump back into BLDR.

The five TRGMII reads do not yet have asserted U-Boot values.  They are
captured to determine whether BLDR's `jump` reset destroys the trained
cascade state.  If they revert while the PMCR/QDMA values pass, the next fix
belongs in the TRGMII/external-switch handoff rather than the descriptor path.

### E1 result

The image was loaded through BLDR XMODEM and the RAM header was verified
before execution.  U-Boot reached its prompt, enumerated `airoha-gdm1` as
`eth0`, and returned:

```text
bfb00834: 00000000
bfb00958: 00000000
bfb54004: 1c0000f0
bfb50500: 02000000
bfb50514: 0600003c
bfb58010: ffffffe0
bfb5b500: 0009e30b
bfb5b600: 0005e30b
bfb5f018: 7f7f8c08
bfb5f804: 0001608f
bfb5f808: 00000001
bfb5f830: 00000001
bfb5fa00: 00020000
bfb5fa40: 40020000
```

`0x0600` is U-Boot's 1536-byte aligned RX buffer threshold, not the Linux
runtime limit of 1522 bytes.  QDMA TX/RX enable bits remained clear, and the
critical TRGMII enable/control values survived the handoff.

Raw UART capture:
`build/xr500v-ram-eth-e1-20260729.log`, 6424 bytes, SHA-256
`e1f1d2fc39dff84d32e233019bf7d58e3437e8a59db71f658577cc1b9cc16656`.

## E2: one-packet-path test

Only after E1 passes, use one directly connected PC LAN interface and no
fibre, bridge or Internet sharing.  A reproducible example is:

```text
PC:       192.168.77.2/24
U-Boot:   192.168.77.1/24
MAC:      02:58:50:00:00:01
```

At the U-Boot prompt:

```text
setenv ethaddr 02:58:50:00:00:01
setenv ipaddr 192.168.77.1
setenv serverip 192.168.77.2
setenv netmask 255.255.255.0
setenv netretry no
setenv ethrotate no
ping 192.168.77.2
```

Capture both UART and the PC interface.  No ARP egress points toward
QDMA/cascade TX; ARP egress plus a reply but no completed ping points toward
RX descriptors/cache/parser.  A successful ping validates the complete
Bootbase-retained external-switch, integrated-switch and FE/QDMA path.

Never use `saveenv`, `reset`, `go`, `mw`, a network loader or a soft jump back
to BLDR during this experiment.

### E2 result: warm-state DSA boundary

The first packet-path attempt was not a valid cold-chainload test: OpenWrt
booted before BLDR was recaptured, so U-Boot inherited Linux DSA state.
The plain ARP reached QDMA and integrated switch P6, where `RX_FILTER`
advanced while PVC6 was `0x00000120`.  Clearing only `PORT_SPEC_TAG`
temporarily (`0x120 -> 0x100`) allowed integrated P5 `TX_BCAST` to advance.

The external MCM then received and filtered the same frame.  Its inherited
state was:

```text
MCM P6 PCR: 0x001e0001
MCM P6 PVC: 0x00000920
MCM P6 RX_FILTER: 8 -> 9
MCM P1 TX_BCAST: 0 -> 0
```

All temporary MMIO/PBUS changes were restored before power removal.  This
test established that QDMA, integrated P6/P5 and TRGMII were carrying the
frame, but also that a warm Linux-to-BLDR path cannot be used as the Ethernet
oracle.

Raw UART capture:
`build/xr500v-ram-eth-e2-20260729.log`, 27038 bytes, SHA-256
`0049af2f66a737bfd781f8053050d75db69ce1691d2906f5a3b82a0ea8f2a4b5`.

## E3: genuine cold-chainload packet path

The automatic BLDR capture caught the 0.1-second prompt immediately after a
35-second physical power removal.  OpenWrt never ran.  The E1/E3 image was
received at `0xa1000000`, reported `received len=3a600`, and its first words
were verified as `0x1000013f` and `0x00000000` before the jump.

Before any network command, both switches were in Bootbase's ordinary
Ethernet mode rather than the Linux DSA special-tag mode:

```text
integrated P5 PCR/PVC: 0x00ff0000 / 0x810000c0
integrated P6 PCR/PVC: 0x00ff0000 / 0x810000c0
external MCM P6 PCR/PVC: 0x00ff0000 / 0x810000c0
```

With `192.168.68.221`, server `192.168.68.248` and locally administered MAC
`02:58:50:00:00:01`, the first command completed:

```text
Using airoha-gdm1 device
host 192.168.68.248 is alive
```

The matching hardware evidence was:

```text
QDMA TX_CPU/TX_DMA/RX_CPU/RX_DMA:
  before: 0 / 0 / 0 / 1
  after:  2 / 2 / 7 / 8
integrated P6 RX_FILTER: 0 -> 0
integrated P5 TX_BCAST:  0 -> 1
```

Two TX completions are the ARP request and ICMP echo request.  Equal TX
CPU/DMA indices and the one-index RX sentinel show that QDMA completed both
directions without leaving a descriptor backlog.  This validates the cold
BLDR handoff, QDMA/FE path, both MT7530 switches, TRGMII cascade and one
complete ARP/ICMP round trip.

Repeated commands exposed a separate completion-lifecycle bug.  A second
ping succeeded, but later commands stopped after ARP.  The live ARP buffer
still contained a correct broadcast frame, while these registers showed:

```text
QDMA INT_STATUS:       0x0000050f
LMGR free / usage:     0 / 0
IRQ base/cfg/clear/status: all 0
```

Bits 8 and 10 in `INT_STATUS` report an empty/low hardware-forward descriptor
pool.  The four HWFWD descriptors contained the four previous CPU TX
contexts.  The EN751221 branch enabled TX writeback and polled `DONE`, but,
unlike the common Airoha path and the OEM driver, it had never allocated the
TX completion queue or advanced `IRQ_CLEAR_LEN`.  Consequently LMGR could
not reclaim the four-entry first-light pool.

The WSL `eth2` pcap is not packet-path evidence: WSL is in mirrored networking
mode and the capture contained only an unrelated ARP handled by Windows.
Windows and WSL did both learn
`192.168.68.221 -> 02:58:50:00:00:01`, which independently confirms that the
successful ARP reached the host.

Raw UART capture:
`build/xr500v-ram-eth-e3-cold-20260729.log`, 12296 bytes, SHA-256
`b39f7c019555192160ca6f44cf1e6a5a057dfb77773d91b83637d220b824e2b4`.

## E4: TX completion reclaim

The E4 staged image added only the missing legacy EN751221 TX completion
queue:

- allocate and register the OEM-sized 32-entry IRQ queue;
- enable the QDMA IRQ queue mechanism while leaving interrupt delivery
  masked;
- wait for one completion entry after TX writeback `DONE`;
- advance `IRQ_CLEAR_LEN` by one so LMGR reclaims the HWFWD descriptor.

It does not add SPI, MTD, NAND, persistent environment or network loading.
The historical, versioned artifact is:

```text
file: build/xr500v-u-boot-ram-eth-e4-irq-reclaim-20260729.xmodem.bin
size: 239360 bytes (0x3a700)
entry: 0x81000000
SHA-256: a70f525ededa62d3bb50bc72070e38ea55192b01a9bf074dd1fb5dc72c659ff5
```

E4 must again be loaded from a genuine cold BLDR prompt.  Before traffic,
`0xbfb54060` must contain a nonzero IRQ queue base and
`0xbfb54064` must describe threshold 4/depth 32.  The pass condition is
several consecutive successful pings with `DBG_LMGR_STATUS` remaining
nonzero and the completion entry count returning to zero after each send.

A rebuild from a later commit can have a different size and hash because the
U-Boot version string is embedded in the image.  The fixed values above are
evidence for the tested E4 payload, not transfer parameters for arbitrary
future builds.

### E4 result

E4 was loaded directly from the next genuine cold BLDR prompt.  XMODEM
reported `received len=3a700`; the words at `0xa1000000` and `0xa1000004`
were again `0x1000013f` and `0x00000000`.

Before the first packet:

```text
IRQ base:             0x0fdbffa0
IRQ cfg:              0x00040020  (threshold 4, depth 32)
IRQ clear/status:     0x00000000 / 0x00000000
LMGR free / usage:    4 / 0
QDMA INT_STATUS:      0x00000000
HWFWD count:          4
```

Sixteen consecutive `ping 192.168.68.248` commands completed successfully.
This required at least 32 ARP/ICMP transmissions; completion-head accounting
showed 35 TX completions in total.  After ten pings, IRQ head was `0x17`
with zero pending entries.  Six more pings advanced it by twelve and wrapped
it exactly over the 32-entry queue:

```text
IRQ clear/status after 10 pings: 0x00000000 / 0x00000017
IRQ clear/status after 16 pings: 0x00000000 / 0x00000003
LMGR free / usage throughout:    4 / 0
QDMA INT_STATUS final:           0x0000000e
QDMA TX_CPU/TX_DMA:              2 / 2
QDMA RX_CPU/RX_DMA:              2 / 3
```

`INT_STATUS` never regained bits 8 or 10, so neither HWFWD empty nor low
recurred.  LMGR returned all four descriptors after every synchronous send,
and both TX and RX rings ended quiescent.  E4 therefore validates the missing
completion-queue diagnosis and the reclaim implementation on EN751221
hardware.

Raw UART capture:
`build/xr500v-ram-eth-e4-irq-reclaim-20260729.log`, 6007 bytes, SHA-256
`9d254110bff602e35e1b4569615ff4bb6dee3bc68a0e68ed999ea44de56e8cd5`.

## Remaining driver boundary

E4 validates the synchronous happy path, including completion-queue wrap.  It
does not validate recovery after a descriptor or completion timeout.  Before
calling the EN751221 driver generally robust, a follow-up should validate the
completion slot and head, drain stale entries when restarting TX, and prove
that LMGR/HWFWD state is recovered after an injected failure.  Keep that
hardening separate from the exact source state validated above.
