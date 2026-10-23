# NUCLEO-H745ZI-Q (Cortex-M7 core): STM32 SAI Zephyr DMA test report

| | |
|---|---|
| Date | 2026-10-22 |
| Board | NUCLEO-H745ZI-Q, target `nucleo_h745zi_q/stm32h745xx/m7`, ST-LINK V3 (SN 004A00383331511634333834) |
| Change | final driver revision (Zephyr commit `3a2a6ecfead`, "drivers: i2s: stm32_sai: migrate dma from stm32_hal to zephyr api") |
| Baseline | `main` at `b70dbaf045e` (HAL DMA driver), built from a separate worktree |
| DMA | DMA1 (`st,stm32-dma-v1`) + DMAMUX1. SAI1_A TX: `&dma1 2` = stream 1 / request 87. SAI1_B RX: `&dma1 3` = stream 2 / request 88 |
| Cache | `CONFIG_DCACHE=y`. Slabs in AXI SRAM (0x24001E80 / 0x24002E80, cached), aligned to 32 bytes |
| Clock | PLL2_P from HSE as SAI1 kernel clock, 44.1 kHz frame clock (+0.03 %), from `samples/drivers/i2s/output` |
| Wiring | PE6 (SD_A) to PE3 (SD_B), used by the sync image only |

Pins: SAI1_A MCLK PE2, FS PE4, SCK PE5, SD PE6. SAI1_B SD PE3, FS PF9, SCK PF8 (async image).
The sync image uses `boards/nucleo_h745zi_q_stm32h745xx_m7_sync.overlay`. Only the M7 image was
flashed; the M4 core was not used.

## Summary

| Image | `main` | Change |
|---|---|---|
| async | **FAIL**, 308 failures before the 150 s timeout | **PASS** 1063/1063 |
| sync loopback | **FAIL** 40/64 | **PASS** 64/64 |

## Async image (change)

| Case | Result |
|---|---|
| TX/RX stream timing | 435-436 ms (expected 435 ms), all sizes, both clock modes |
| TX+RX together | 435 ms both |
| DROP rounds, then read/stream again | PASS |
| FS pin (PE4) | gated: 0 Hz idle, 44.0-44.2 kHz streaming, 0 Hz after DROP; free-running: 44.0-44.2 kHz throughout |
| MCLK pin (PE2) | ~4900 edges while streaming and after DROP, in both modes |

MCLK keeps running after a gated stop, as on the U575: the H7 SAI has `SAI_xCR1_MCKEN`.

## Sync loopback image

| Word size | `main` | Change |
|---|---|---|
| 16 | **1730-1883 errors per round, ~11900 L/R swapped** | 0 errors, 0 swapped (3 rounds + after DROP) |
| 24 | **12672 of 12673 samples wrong** | 0 errors, 0 swapped |
| 32 | same as 24 | 0 errors, 0 swapped |

The 16-bit errors in `main` are stale cache lines (no RX invalidate), as on the F767ZI; see
`nucleo_f767zi.md` for the control run without the invalidate.

## Request routing

`stm32h7.dtsi` gives SAI1 `dmas = <&dma1 2 87 ...>` / `<&dma1 3 88 ...>`. As on the G4,
`dma_stm32.c` writes the request into the DMAMUX channel register, so requests go through
DMAMUX1. The `&dmamux1` form was not tried on this board.

## Not tested

- `&dmamux1` routing, BDMA (SAI4), SAI2/SAI3
- Buffers in DTCM or SRAM1-3 (D2)
- The M4 core image (`nucleo_h745zi_q/stm32h745xx/m4`)
- TX as synchronous slave, mono, formats other than I2S standard, rates other than 44.1 kHz

## Files

- `logs/nucleo_h745zi_q_stm32h745xx_m7/uart_{async,sync}-new.log`: change
- `logs/nucleo_h745zi_q_stm32h745xx_m7/uart_{async,sync}-old.log`: `main`

## Update: RX cache handling (option 2)

The final driver keeps the cache handling of `main`: TX blocks are flushed, RX blocks are not
invalidated. A comment in `sai_sub_rx_callback()` requires the RX slab buffer to be `__nocache`
with `CONFIG_NOCACHE_MEMORY=y`. The check app now sets that option for this board
(`boards/<board>.conf`) and places both slab buffers in `.nocache`.

| Image, slabs `__nocache` | Result |
|---|---|
| Final driver, async | **PASS** 1064/1064 |
| Final driver, sync loopback | **PASS** 65/65 |
| Change with RX invalidate removed (control) | **PASS** 65/65 |
| `main`, sync loopback | **FAIL** 41/65: 16-bit counters clean but all L/R swapped; 24/32 bit 12672/12673 wrong |

The cached-slab results above in this report are from the earlier revision that invalidated RX
blocks. Logs: `uart_{async,sync}-new-nocache.log`, `uart_sync-noinv-nocache.log`,
`uart_sync-old-nocache.log`.

## Update: BDMA (SAI4) and DMAMUX1 routing

Overlays, used together with `boards/nucleo_h745zi_q_stm32h745xx_m7.overlay`:

- `variants/h7_sai4_bdma.overlay`: adds a SAI4 node, which `stm32h745.dtsi` does not describe (0x58005400,
  APB4 bit 21, kernel clock PLL2_P through `SAI4A_SEL(1)` / `SAI4B_SEL(1)`). SAI4_A TX and SAI4_B
  RX use DMAMUX2 channels 0/1 (requests 15/16, BDMA channels 0/1). Same pins as SAI1, on the
  SAI4 alternate functions, so SAI1 is disabled and the PE6-PE3 jumper is unchanged. SRAM4 is
  marked non-cacheable and the slab buffers are placed there (`slab-memory-region = <&sram4>`):
  the BDMA only reaches the D3 domain.
- `variants/h7_sai4_bdma_sync.overlay`: SAI4_B as a synchronous RX slave (loopback image).
- `variants/h7_sai4_bdma_direct.overlay`: SAI4 references `&bdma1` directly instead of `&dmamux2`.
- (removed) SAI1 through `&dmamux1` channels 2/3 (DMA1 streams 2/3) instead of
  `&dma1 2`/`&dma1 3`, which on DMA v1 with DMAMUX select DMA1 streams 1/2 (1-based cell).

| Image | DMA path | Slabs | `main` | Change (final) |
|---|---|---|---|---|
| async | SAI4, DMAMUX2 + BDMA | SRAM4 0x38000000 | **FAIL**: devices not ready, `HAL_SAI_Transmit_DMA: <FAILED>` | **PASS** 1064/1064 |
| sync loopback | SAI4, DMAMUX2 + BDMA | SRAM4 | **FAIL** 4/53 | **PASS** 65/65 |
| async | SAI4, `&bdma1` direct | SRAM4 | **FAIL**: `SAI Sub-Block DMA Init <FAILED>, ret: -22` | **PASS** 1064/1064 |
| sync loopback | SAI4, `&bdma1` direct | SRAM4 | **FAIL** 4/53 | **PASS** 65/65 |
| async | SAI1, DMAMUX1 + DMA1 | `.nocache` 0x24000000 | not run | **PASS** 1064/1064 |
| sync loopback | SAI1, DMAMUX1 + DMA1 | `.nocache` | not run | **PASS** 65/65 |

`main` cannot use SAI4 on BDMA in either form. With `&dmamux2`, its HAL DMA handle is built from
the DMAMUX2 address. With `&bdma1`, the HAL-override DMA setup is rejected with `-EINVAL`.
Logs: `uart_*bdma-*.log` (DMAMUX2), `uart_*bdmad-*.log` (direct), `uart_*mux-new.log` (DMAMUX1).
