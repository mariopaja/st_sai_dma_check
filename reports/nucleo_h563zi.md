# NUCLEO-H563ZI: STM32 SAI Zephyr DMA test report

| | |
|---|---|
| Date | 2026-11-02 |
| Board | NUCLEO-H563ZI (STM32H563ZI, Cortex-M33), ST-LINK V3 (SN 0026003B3132511238363431) |
| Change | PR 123259 after review, rebased on `main` (Zephyr commit `7bdffd3af34`, driver commit "drivers: i2s: stm32_sai: migrate dma from stm32_hal to zephyr api") |
| Baseline | the same series before the review fixes (`21f685a8699`), built from a separate worktree |
| DMA | GPDMA1 (`st,stm32u5-dma`). SAI1_A TX: channel 1, request 53. SAI1_B RX: channel 0, request 54 |
| Cache | no data cache on the internal SRAM; slabs in normal RAM |
| Clock | PLL3_P from HSE, 44.1 kHz frame clock, from `samples/drivers/i2s/output` |
| Wiring | PE6 (SD_A) to PE3 (SD_B), used by the sync image only |

Pins: SAI1_A MCLK PE2, FS PE4, SCK PE5, SD PE6. SAI1_B SD PE3, FS PF9, SCK PF8 (async image).
Overlays: `boards/nucleo_h563zi.overlay`, `boards/nucleo_h563zi_sync.overlay` (new). First
hardware run on an STM32H5.

The board had been an RDFM test board (TrustZone on, MCUboot + TF-M). For this test TrustZone
was disabled (TZEN = 0xC3) and the flash mass-erased.

## Summary

| Image | Result |
|---|---|
| sync loopback | **PASS** 65/65: 12800 samples per round, 0 errors, 0 L/R swapped, all sizes |
| async | **PASS** 1064/1064 |
| DROP storm, change | **PASS**: 6 x 1000 DROPs, no error, leak or failed restart, no log line |
| DROP storm, before the review fixes | PASS, no log line (see below) |

The async log only contains the expected lines of the overrun case: 6 x `RX queue full` and
6 x `RX invalid state: 4`, as on the other boards.

## Async image

| Case | Result |
|---|---|
| TX/RX stream timing | 438-439 ms for 300 blocks (expected 435 ms), all sizes, both clock modes |
| TX+RX together | 438-439 ms both |
| DROP rounds, then read/stream again | PASS |
| FS pin (PE4) | gated: 0 Hz idle, 43.6-43.8 kHz streaming, 0 Hz after DROP; free-running: 43.8 kHz throughout |
| MCLK pin (PE2) | ~3050 edges while streaming; keeps running after DROP (H5 SAI has `MCKEN`) |

The FS reading is the polled edge count (200 Hz resolution), within the 2 % window.

## DROP storm and the DROP race

Review comment: DROP stopped the DMA while the stream was still RUNNING, so a DMA completion
callback running meanwhile could reload the channel with a block that `queue_drop()` then
frees. The storm mode of the check app (`variants/drop_storm.conf`) uses 8-frame blocks and
DROPs TX and RX 1000 times each for 16 bit gated, 16 bit free-running and 32 bit
free-running, with the DROP delay swept in 13 us steps over two block periods. After every
DROP it checks that all blocks are back and that the stream restarts.

| Driver revision | Result | Log |
|---|---|---|
| Before the review fixes (`21f685a8699`) | PASS | none: a callback inside DROP reloads the channel silently; `dma_stop()` cancels the reload afterwards |
| DROP sets READY first (review fix) | PASS | 4 x `TX bad status: 1`: the window was hit 4 times in 3000 TX DROPs, and the callback stopped instead of reloading |
| DMA stop under `irq_lock()`, DROP order unchanged (tried, rejected) | **FAIL**: 3 restart errors (`dma stream 1 is busy`, START `-16`) | the lock defers the completion interrupt past `dma_stop()`; `dma_stop()` sees the finished channel as stopped and keeps `busy` set, the deferred callback still sees RUNNING and re-arms the channel, which never completes with `DMAEN` cleared |
| Final: READY first, READY not logged as an error | **PASS** | none |

So the window exists on GPDMA (about 1 per 750 DROPs here). The code before the fixes
recovered through `dma_stop()`; with the state set first, a late callback no longer touches
the channel at all.

## Not tested

- TX as synchronous slave, mono, formats other than I2S standard, rates other than 44.1 kHz
- TrustZone non-secure build (`nucleo_h563zi/stm32h563xx/ns`)

## Files

- `logs/nucleo_h563zi/uart_{sync,async}-new.log`: final change
- `logs/nucleo_h563zi/uart_storm-new.log`: storm, final change
- `logs/nucleo_h563zi/uart_storm-old.log`: storm, before the review fixes
- `logs/nucleo_h563zi/uart_storm-state_first_err_log.log`: storm, READY first with the error log
- `logs/nucleo_h563zi/uart_storm-irq_lock.log`: storm, DMA stop under `irq_lock()` (FAIL)
