# CMSIS-DAP reference sources (verbatim copy)

`DAP.c` / `DAP.h` / `SW_DP.c` are unmodified copies from the ARM CMSIS-DAP
reference implementation (https://github.com/ARM-software/CMSIS-DAP,
Firmware/Source + Firmware/Include, Apache-2.0 — same license as this repo).

Per repo code rules the copies are read-only ("Copy" tier of
Wrapper > Hook > Copy): do not edit them; all board specifics live in
`board/ports/DAP_config.h`, transport glue lives in `../bridge_dap.c`.
JTAG/SWO/UART code paths compile out via `DAP_JTAG=0`, `SWO_UART=0` etc.
