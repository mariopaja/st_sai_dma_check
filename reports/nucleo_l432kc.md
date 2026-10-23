# NUCLEO-L432KC: STM32 SAI Zephyr DMA test report

| | |
|---|---|
| Date | 2026-10-22 |
| Board | NUCLEO-L432KC (STM32L432KC), ST-LINK V2-1 (SN 0671FF485753667187213242) |
| Change | driver revision before the `SAIEN` wait in `sai_sub_disable()`; otherwise as the final revision (Zephyr commit `3a2a6ecfead`) |
| Baseline | `main` at `b70dbaf045e` (HAL DMA driver) |
| DMA | DMA2 (`st,stm32-dma-v2`, no DMAMUX). SAI1_A TX: ch 1 / req 1. SAI1_B RX: ch 2 / req 1 |
| Clock | PLLSAI1 from HSI, 44.27 kHz frame clock (+0.38 %) |
| Wiring | D0 (PA10, SD_A) to D11 (PB5, SD_B), used by the sync image only |

Each image was built with `west build -p` and flashed with `STM32_Programmer_CLI -w <hex> -v`
(`scripts/run.sh`), never with `west flash`. Before flashing, the ELF symbols were checked:
`HAL_DMA_Start_IT`/`HAL_SAI_TxCpltCallback` in the baseline, `sai_sub_dma_reload` in the change.

The L432 sets FLASH_SR.PEMPTY after programming, so the next reset boots the ROM loader.
`scripts/run.sh` with `PEMPTY_FIX=1` toggles the bit (write 1 to bit 17) before resetting. With
several boards attached, `scripts/run.sh` selects the probe by serial number (`sn=`) and the VCP by
`/dev/serial/by-id`.

## Summary

The change results are from the driver revision just before the `SAIEN` wait was added.

| Image | `main` | Change |
|---|---|---|
| async: TX/RX trigger paths, pacing, slab, TX+RX together, FS/MCLK pins | **FAIL**, leaks from the first DROP, cascade, did not finish in 150 s | **PASS** 1003/1003 |
| sync loopback, default `fifo-threshold` ("full") | **FAIL** 40/64 | **PASS** 64/64 |
| sync loopback, RX `fifo-threshold = "empty"` | **FAIL** 41/64 | not rerun on the final driver |

## Async image

TX on SAI1_A (PA8 SCK, PA9 FS, PA10 SD, PA3 MCLK), RX on SAI1_B as an unwired master
(PB3 SCK, PB6 FS, PB5 SD). Word sizes 16, 24 and 32, bit clock gated and then free-running.

| Case | Pass condition | `main` | Change |
|---|---|---|---|
| TX stream + DRAIN | 300 blocks, slab returned, 0.9x to 1.1x of 435 ms | 434 ms at 16 bit, then fails once the slab is exhausted | 434-435 ms |
| TX underrun, STOP+DROP | slab returned | fails after the leaks | PASS |
| TX DROP while running, 20 rounds | slab returned within 100 ms | **one block lost per round** | PASS |
| RX stream + STOP | 300 blocks, timing, slab returned | 441 ms at 16 bit | 434 ms |
| RX DROP while running, 20 rounds | slab returned | **one block lost per round** | PASS |
| RX overrun + PREPARE | `-EIO` in error state, all blocks back, restart works | **leak** | PASS |
| TX+RX together, 16/24/32 | both 300 blocks in time, no error | not reached | 433-434 ms both |
| FS pin (PA9) | gated: 0 Hz idle and after stop; free-running: ~44.1 kHz always | not reached | 0 Hz / 44.2-44.4 kHz |
| MCLK pin (PA3) | toggles while streaming; gated: stops after stop | not reached | ~100 edges / 0 edges gated / ~100 free-running |

FS is counted by polling GPIOA IDR for 5 ms with interrupts locked (200 Hz resolution).
MCLK (11.3 MHz) is faster than the polling loop, so only level changes are counted.

## Sync loopback image

SAI1_B is a synchronous RX slave of SAI1_A (`variants/sync.overlay`). TX runs free-running and streams
a scrambled counter. RX must receive every sample in order, with the counter parity matching
the slot (left/right). Each word size runs 3 RX start/stop rounds and one restart after DROP,
while TX keeps streaming.

### Defects found by the loopback, and what fixed them

| Defect | Seen on | Cause | Fix in the change |
|---|---|---|---|
| One stereo frame lost at sample 8 of every RX block | `main` (~56 of 100 blocks) and first change revision (~99 of 100) | With `fifo-threshold = "full"`, RX requests DMA only when the FIFO is full: no headroom while the callback restarts the DMA | Callbacks restart the DMA before queueing or freeing the finished block. 0 losses with the default threshold |
| Stale samples at the start of a round | `main` and change | A synchronous slave stays enabled while stopped; its FIFO keeps 8 old words | RX start disables the synchronous slave, flushes the FIFO and re-enables it |
| Left/right swapped on every round | `main` and change | `stm32_sai_sub_conf()` primed the TX FIFO with one word: TX data started on the right slot | Prime one word per channel (a silent frame) |
| Left/right swapped after a quick RX restart | change, before the resync | Restart before the FIFO overran resumed mid-frame | Same resync as for stale samples |
| Slab leak after RX STOP and after DROP | `main` | `queue_drop()` dropped the in-flight block | Change frees it after stopping the DMA |

### Final result (change)

| Word size | Rounds 0-2 | After DROP | Errors | L/R swapped |
|---|---|---|---|---|
| 16 | 3 x 12800 samples | 12800 samples | 0 | 0 |
| 24 | 3 x 12800 samples | 12800 samples | 0 | 0 |
| 32 | 3 x 12800 samples | 12800 samples | 0 | 0 |

## Not tested on this board

- TX as synchronous slave, or RX as master with TX as its synchronous slave
- Mono (1 channel), formats other than I2S standard, frame rates other than 44.1 kHz
- The `"empty"` threshold run on the final driver revision (the default now passes)

## Files

- `logs/nucleo_l432kc/uart_async-new.log`, `uart_sync-new.log`: change, final revision
- `logs/nucleo_l432kc/uart_async-old.log`, `uart_sync-old.log`, `uart_syncfifo-old.log`: `main`
- `../run.sh`, `../build.sh`: flash/capture and build helpers
