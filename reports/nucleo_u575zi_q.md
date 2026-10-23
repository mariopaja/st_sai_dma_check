# NUCLEO-U575ZI-Q: STM32 SAI Zephyr DMA test report

| | |
|---|---|
| Date | 2026-10-22 |
| Board | NUCLEO-U575ZI-Q (STM32U575ZI), ST-LINK V3 (SN 003800433532510E31333430) |
| Change | final driver revision (Zephyr commit `3a2a6ecfead`, "drivers: i2s: stm32_sai: migrate dma from stm32_hal to zephyr api") |
| Baseline | `main` at `b70dbaf045e` (HAL DMA driver) |
| DMA | GPDMA1 (`st,stm32u5-dma`, `dma_stm32u5.c`). SAI1_A TX: ch 1 / req 36. SAI1_B RX: ch 0 / req 37 |
| Clock | PLL2 from MSIS, 44.1 kHz frame clock (-0.03 %), from `samples/drivers/i2s/output` |
| Wiring | PE6 (SD_A) to PE3 (SD_B), used by the sync image only |

Pins: SAI1_A MCLK PE2, FS PE4, SCK PE5, SD PE6. SAI1_B SD PE3, FS PF9, SCK PF8 (async image).
The sync image uses `boards/nucleo_u575zi_q_sync.overlay`.

Images were flashed with `STM32_Programmer_CLI -w <hex> -v` on the probe selected by serial
number (`scripts/run.sh`), never with `west flash`. The baseline images contain
`HAL_DMA_Start_IT`/`HAL_SAI_TxCpltCallback`; the change images contain `sai_sub_dma_reload`.
The tested change image is byte-identical to a rebuild of the current tree.

## Summary

| Image | `main` | Change without `SAIEN` wait | Change (final) |
|---|---|---|---|
| async | **FAIL**, 312 failures before the 150 s timeout | **FAIL** 1060/1063 | **PASS** 1063/1063 |
| sync loopback | **FAIL** 40/64 | **PASS** 64/64 | **PASS** 64/64 |

## Defect found on this board: SAI left disabled after a gated DROP

With `I2S_OPT_BIT_CLK_GATED`, RX delivered no block after the DROP-while-running rounds:
START succeeded, the state stayed RUNNING, and `i2s_read()` timed out (`-11`) at 16, 24 and
32 bit. The DMA v1/v2 boards never showed it.

Cause: `sai_sub_disable()` clears `SAIEN`, but the SAI stops only at the end of the current
frame and `SAIEN` reads 1 until then. When START follows within that window,
`sai_sub_dma_start()` sees the sub-block as enabled and skips `__HAL_SAI_ENABLE()`. The pending
disable then completes, leaving the DMA armed on a disabled SAI.

Fix: `sai_sub_disable()` waits (up to `SAI_DISABLE_TIMEOUT_US`) until `SAIEN` reads 0. That
takes at most one frame: 23 us at 44.1 kHz, 125 us at 8 kHz. The wait also runs in the DMA
callback when a gated stream stops.

| Check | Without `SAIEN` wait | With the wait (final) |
|---|---|---|
| RX read after 20 gated DROP rounds (16/24/32) | **FAIL** `-11` x 3 | PASS |
| TX stream after 20 DROP rounds | PASS | PASS |
| everything else | PASS | PASS |

The first run without the wait, which did not yet have the "read after drop rounds" check, caught
the same defect later, in the overrun case (`read in error state -11`, `PREPARE - invalid state: 2`).

## Async image (change with fix)

| Case | Result |
|---|---|
| TX/RX stream timing | 435-436 ms (expected 435 ms), all sizes, both clock modes |
| TX+RX together | 435-436 ms both |
| FS pin (PE4) | gated: 0 Hz idle, 44.0-44.2 kHz streaming, 0 Hz after DROP; free-running: 44.0-44.2 kHz throughout |
| MCLK pin (PE2) | ~1230 edges while streaming and after DROP, in both modes |

MCLK keeps running after a gated stop. The U5 SAI has `SAI_xCR1_MCKEN`, which the HAL sets for
`mclk-enable`; it generates MCLK independently of `SAIEN`. The F4 and L4 lack `MCKEN`, and MCLK
stops with the sub-block there. The check app expects MCLK to keep running wherever
`SAI_xCR1_MCKEN` is defined.

## Sync loopback image

| Word size | `main` | Change |
|---|---|---|
| 16 | counters in order, **every sample L/R swapped**, 1 error after each restart | 0 errors, 0 swapped |
| 24 | **12672 of 12673 samples wrong** | 0 errors, 0 swapped |
| 32 | same as 24 | 0 errors, 0 swapped |

## Not tested

- TrustZone / secure builds (`CONFIG_ARM_SECURE_FIRMWARE`)
- Power management (see `nucleo_u575_hw_testing` notes: PM images need a POWERDOWN connect)
- TX as synchronous slave, mono, formats other than I2S standard, rates other than 44.1 kHz

## Files

- `logs/nucleo_u575zi_q/uart_{async,sync}-new.log`: change with SAIEN wait
- `logs/nucleo_u575zi_q/uart_async-head_209d616.log`: driver without the `SAIEN` wait
- `logs/nucleo_u575zi_q/uart_{async,sync}-old.log`: `main`
