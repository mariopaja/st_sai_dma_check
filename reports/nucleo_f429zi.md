# NUCLEO-F429ZI: STM32 SAI Zephyr DMA test report

| | |
|---|---|
| Date | 2026-10-22 |
| Board | NUCLEO-F429ZI (STM32F429ZI), ST-LINK V2-1 (SN 0663FF353445503043082449) |
| Change | driver revision before the `SAIEN` wait in `sai_sub_disable()`; otherwise as the final revision (Zephyr commit `3a2a6ecfead`) |
| Baseline | `main` at `b70dbaf045e` (HAL DMA driver) |
| DMA | DMA2 (`st,stm32-dma-v1`). SAI1_A TX: stream 1 / channel 0. SAI1_B RX: stream 5 / channel 0 |
| Clock | PLLSAI from HSE, 43.836 kHz frame clock (-0.37 %), from `samples/drivers/i2s/output` |
| Wiring | PE6 (SD_A) to PE3 (SD_B), used by the sync image only |

Pins: SAI1_A MCLK PE2, FS PE4, SCK PE5, SD PE6. SAI1_B SD PE3, FS PF9, SCK PF8 (async image).
The sync image uses `boards/nucleo_f429zi_sync.overlay` (SD_B on PE3, `synchronous`).

Each image was built with `west build -p` and flashed with `STM32_Programmer_CLI -w <hex> -v`
on the probe selected by serial number (`scripts/run.sh`). Before flashing, the ELF symbols were
checked: `HAL_DMA_Start_IT`/`HAL_SAI_TxCpltCallback` in the baseline, `sai_sub_dma_reload` in
the change. The F4 has no PEMPTY flag, so `PEMPTY_FIX` stayed off.

## Summary

| Image | `main` | Change |
|---|---|---|
| async | **FAIL**, 317 failures before the 150 s timeout | **PASS** 1003/1003 |
| sync loopback | **FAIL** 40/64 | **PASS** 64/64 |

## Async image

| Case | `main` | Change |
|---|---|---|
| TX/RX stream timing (expected 438 ms at 43.836 kHz) | 438 ms at 16 bit, then write/read timeouts once the slab is exhausted | 438-439 ms, all sizes, both clock modes |
| DROP while running, STOP, PREPARE | **slab leaks**, then `start -12` in 91 rounds | PASS |
| TX+RX together | **timing off** (cascade) | 438 ms both, all sizes |
| FS pin (PE4) | **0 Hz while streaming** (TX never started) | gated: 0 / 43.8 kHz / 0 Hz; free-running: 43.8-44.0 kHz throughout |
| MCLK pin (PE2) | **0 edges** | ~2430 edges streaming; gated: 0 after DROP; free-running: keeps running |

## Sync loopback image

| Word size | `main` | Change |
|---|---|---|
| 16 | counters in order but **every sample L/R swapped**; 1 error after each restart | 0 errors, 0 swapped (3 rounds + after DROP) |
| 24 | **12672 of 12673 samples wrong**, about half swapped | 0 errors, 0 swapped |
| 32 | same as 24 | 0 errors, 0 swapped |
| Slab after STOP | **leak** (9 times) | no leak |

At 24/32 bit, `main` takes the DMA width from the devicetree (`STM32_DMA_16BITS`), while the
SAI data register takes 32-bit words. The change uses the word size (4 bytes) instead. The
other defects are the same as on the L432KC and G431KB (see `nucleo_l432kc.md`).

## Not tested

- TX as synchronous slave, mono, formats other than I2S standard, rates other than 44.1 kHz
- DMA v1 FIFO mode and bursts (the driver uses single transfers, so FIFO mode stays off)

## Files

- `logs/nucleo_f429zi/uart_{async,sync}-new.log`: change
- `logs/nucleo_f429zi/uart_{async,sync}-old.log`: `main`
