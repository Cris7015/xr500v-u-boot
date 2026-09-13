EN751221 DDR calibration source
===============================

The EN751221 DDR calibration stage is built entirely from source.

The six calibration translation units recovered by Merbanan are combined with
the original GPL bootrom startup, UART, timer, string and SPRAM implementations.
Legacy BSP headers are not build dependencies; the required EN751221 register
interface is provided by ``en751221_ddr.h``.

The original stage executes from FE SRAM at ``0x9fa32800``. Keep the source
order in ``tools/build-econet-ddr.sh`` synchronized with the known-good
EN7512 V1.2.2 stage:

``head, setup, main, init, time, string, dramc, dramc_dq_dqs_cal,
dramc_dle_cal, dramc_dqs_gw_cal, en7512_dramc_init, spram``.

The vendor recovery baseline was a 20336-byte ``spram.img``. A source build
using a newer compiler is not required to be byte-identical; the ABI, SRAM
VMA and hardware behaviour are the acceptance criteria.

``tools/build-econet-ddr.sh en751221`` currently emits
``en751221_ddr.bin`` as a temporary build artifact. It is not a checked-in
firmware blob.
