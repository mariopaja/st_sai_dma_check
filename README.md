# sai_dma_check: STM32 SAI hardware check

A Zephyr application that tests the STM32 SAI I2S driver (`drivers/i2s/i2s_stm32_sai.c`) on
real boards. The check covers the DMA paths, the I2S trigger state machine, buffer handling,
the clock pins and received data. It was written to validate the move of the driver from the
STM32 HAL DMA layer to the Zephyr DMA API, and runs unchanged against the old driver for
comparison. Results per board are in [reports/](reports/README.md).

The app prints one `FAIL line <n>: ...` per failed check and ends with
`RESULT: PASS|FAIL (<passed> passed, <failed> failed)`.

## Requirements

- A west workspace with the Zephyr tree under test (the SAI driver on the Zephyr DMA API) and the
  `hal_stm32` module.
- STM32CubeProgrammer (`STM32_Programmer_CLI`) for flashing and for the helper scripts.
- One of the boards below, and a jumper between the SAI data pins for the sync image.

## Layout

| Path | Content |
|---|---|
| `src/main.c` | The check application |
| `boards/` | Per-board overlays (`<board>.overlay`, `<board>_sync.overlay`) and `.conf` files |
| `variants/` | Optional overlays: DMAMUX/BDMA routing, role swap, FIFO threshold, `nocache.conf` |
| `scripts/` | `build.sh` (new and baseline images) and `run.sh` (flash, capture the console) |
| `reports/` | Hardware test matrix, per-board reports and raw console logs |

## Images

The app builds as one of two images, selected by devicetree:

| Image | Setup | Wiring | Checks |
|---|---|---|---|
| async (default) | SAI A sub-block = TX master, SAI B sub-block = RX master, each on its own FS/SCK; RX data is not checked | none needed | about 1000 |
| sync loopback | SAI B = synchronous RX slave of SAI A (`synchronous` in the sync overlay) | SD_A wired to SD_B | 65 |

The sync image is chosen when the RX node has the `synchronous` property.

## What the async image tests

The whole set runs for word sizes 16, 24 and 32 bit, first with `I2S_OPT_BIT_CLK_GATED` and
then with a free-running bit clock. Stereo, I2S standard format, 44.1 kHz, 64 frames per
block, a slab of 8 blocks.

### TX (`test_tx`)

| Case | Pass condition |
|---|---|
| Stream + DRAIN | 300 blocks written and drained; all slab blocks back; elapsed time 0.9x to 1.1x of 300 x 64 / 44100 s (DMA paced by the frame clock) |
| Underrun | START with a single block; the driver returns to READY on its own and frees the block |
| STOP then DROP | 4 blocks queued, START, STOP, DROP: all blocks back |
| DROP while running, 20 rounds | 4 blocks queued, START, wait 0.3-2.1 ms, DROP (aborts the in-flight DMA block): all blocks back within 100 ms |
| Stream after the DROP rounds | 4 blocks, START, DRAIN: completes and takes at least 4 block times |

### RX (`test_rx`)

| Case | Pass condition |
|---|---|
| Stream + STOP | 300 blocks read with the expected size and timing; after STOP the queue drains and all blocks are back |
| DROP while running, 20 rounds | START, wait 0.3-4.3 ms, DROP: all blocks back within 50 ms |
| Read after the DROP rounds | START, then 4 blocks are delivered |
| Overrun + PREPARE | no reads for 50 ms; the queue fills and `i2s_read()` returns `-EIO`; PREPARE returns all blocks; START works again |

### TX and RX together (`test_concurrent`)

A writer thread streams 300 TX blocks while the main thread reads 300 RX blocks. Both finish in
time, the writer reports no error and both slabs are returned.

### Clock pins (`test_pins`)

The FS pin (and MCLK where routed) is read through the GPIO input register while it stays in
SAI alternate function mode. Edges are counted for 5 ms with interrupts locked.

| Point | Gated bit clock | Free-running bit clock |
|---|---|---|
| After configure, before START | FS 0 Hz | FS 44.1 kHz +-2 % |
| While streaming | FS 44.1 kHz +-2 %; MCLK toggles | same |
| After DROP | FS 0 Hz; MCLK stopped, except where the SAI has `MCKEN` (G4, H7, U5), where MCLK keeps running | FS and MCLK keep running |

MCLK (about 11 MHz) is faster than the polling loop, so only level changes are counted, not
its frequency.

### Slab accounting

Every case checks that all 8 slab blocks are free again at its end, so a leaked block shows
up as a failure at the case that leaked it.

## What the sync loopback image tests

TX runs free-running (the synchronous slave needs the master clocks) and a writer thread
streams a counter. Sample `n` is `(n XOR 0xA5C3965A)` masked to the word size, so every bit
toggles. RX decodes every sample:

| Check | Fails on |
|---|---|
| Counter increments by one on every sample | lost, repeated or corrupted samples (FIFO overrun, wrong DMA width, stale cache lines) |
| Counter parity matches the slot (left = even) | left/right channel swap |
| Lock within the first 2 blocks | no data on the wire (missing jumper) or no RX clock |

For each word size (16, 24, 32): 3 rounds of RX START, 100 blocks checked and STOP while TX
keeps streaming, then DROP mid-transfer, START and 100 more blocks checked. Mismatches are
stored and printed after the read loop, so printing cannot overrun the RX queue.

## Boards and wiring

| Board | TX (SAI A) | RX (SAI B) | Loopback jumper | FS / MCLK probe |
|---|---|---|---|---|
| `nucleo_g431kb` | SAI1_A: SCK PA8, FS PA9, SD PA10 | SAI1_B: SCK PB3, FS PB6, SD PB5 | D0 (PA10) to D11 (PB5) | PA9 / - |
| `nucleo_l432kc` | SAI1_A: MCLK PA3, SCK PA8, FS PA9, SD PA10 | SAI1_B: SCK PB3, FS PB6, SD PB5 | D0 (PA10) to D11 (PB5) | PA9 / PA3 |
| `nucleo_f429zi` | SAI1_A: MCLK PE2, FS PE4, SCK PE5, SD PE6 | SAI1_B: SD PE3, SCK PF8, FS PF9 | PE6 to PE3 | PE4 / PE2 |
| `nucleo_f767zi` | as F429ZI | as F429ZI | PE6 to PE3 | PE4 / PE2 |
| `nucleo_h745zi_q/stm32h745xx/m7` | as F429ZI (SAI1, or SAI4 with the BDMA overlay) | as F429ZI | PE6 to PE3 | PE4 / PE2 |
| `nucleo_u575zi_q` | as F429ZI | as F429ZI | PE6 to PE3 | PE4 / PE2 |
| `nucleo_n657x0_q` | SAI2_A: MCLK PE0, SCK PD13, FS PG13, SD PF5 | SAI2_B: SD PE11, FS PE13, SCK PE12 | PF5 (CN3-28) to PE11 (D7) | PG13 / - (see below) |

The jumper is only used by the sync image. Each board needs `boards/<board>.overlay` (aliases
`i2s-tx` / `i2s-rx`, pins, SAI kernel clock and `zephyr,user` probe pins). The sync image also
needs `boards/<board>_sync.overlay`; boards without one use `variants/sync.overlay` (Nucleo-32 pins).

## Overlays and configuration

| File | Purpose |
|---|---|
| `boards/<board>.overlay` | Board setup for the async image |
| `boards/<board>_sync.overlay`, `variants/sync.overlay` | Sync image: RX sub-block `synchronous`, RX pinctrl reduced to SD |
| `boards/nucleo_f767zi.conf`, `boards/nucleo_h745zi_q_stm32h745xx_m7.conf` | `CONFIG_NOCACHE_MEMORY=y`: the driver does not invalidate RX blocks, so on SoCs with a data cache the slab buffers must be non-cacheable |
| `variants/h7_sai4_bdma.overlay` | H745ZI-Q: adds a SAI4 node (not in the SoC dtsi) on BDMA through DMAMUX2; slab buffers in SRAM4, which is marked non-cacheable |
| `variants/h7_sai4_bdma_sync.overlay` | Sync image for SAI4 |
| `variants/h7_sai4_bdma_direct.overlay` | SAI4 referencing `&bdma1` directly |
| `variants/n6_sai2b_tx.overlay`, `variants/n6_sai2b_tx_sync.overlay` | N657: swap the roles (TX on SAI2_B, MCLK on PE14, SD PE11; RX on SAI2_A, SD PF5); same jumper |
| `boards/nucleo_n657x0_q.conf` | `CONFIG_NOCACHE_MEMORY=y` (Cortex-M55 D-cache) |
| `variants/rx_fifo_empty.overlay` | RX `fifo-threshold = "empty"` (used while investigating the FIFO overrun) |
| `variants/nocache.conf` | `CONFIG_NOCACHE_MEMORY=y` for any board |
| `variants/isr_timing.conf` | Instead of the checks, time the SAI DMA interrupts (DWT cycle counter in the user tracing ISR hooks) while TX and RX stream; GPDMA boards only. See `reports/isr_timing_nucleo_u575zi_q.md` |

Slab buffers are 32-byte aligned. They go into the region named by the `zephyr,user` property
`slab-memory-region` if set (SRAM4 for BDMA), else into `.nocache` when
`CONFIG_NOCACHE_MEMORY=y`, else into normal RAM.

## Build and run

From the west workspace:

```sh
# async image
west build -p -b nucleo_u575zi_q sai_dma_check -d build/sai_async

# sync loopback image
west build -p -b nucleo_u575zi_q sai_dma_check -d build/sai_sync -- \
	-DEXTRA_DTC_OVERLAY_FILE=$PWD/sai_dma_check/boards/nucleo_u575zi_q_sync.overlay

# H745 SAI4 on BDMA, sync image
west build -p -b nucleo_h745zi_q/stm32h745xx/m7 sai_dma_check -d build/sai_bdma -- \
	"-DEXTRA_DTC_OVERLAY_FILE=$PWD/sai_dma_check/variants/h7_sai4_bdma.overlay;$PWD/sai_dma_check/variants/h7_sai4_bdma_sync.overlay"
```

Flash, then read the console at 115200 baud. A run takes well under 30 s with the change. With
the old driver it starts failing after the first leaked block and then runs into timeouts.

`scripts/build.sh` and `scripts/run.sh` are the helpers used for the reports:

- `scripts/build.sh <out-dir> <board> <async|sync|syncfifo>...` builds each image for the tree in
  `ZEPHYR_BASE` (`new`) and for the baseline driver (`old`, from a separate git worktree, so the
  working tree is never modified). Create the worktree once with
  `git -C $ZEPHYR_BASE worktree add --detach <out-dir>/zephyr-base <baseline-commit>` (or set
  `WT`). `VARIANTS=new` builds only the current tree. It
  prints, per image, whether the ELF contains the HAL DMA driver (`HAL_DMA_Start_IT`) or the
  Zephyr DMA one (`sai_sub_dma_reload`).
- `scripts/run.sh <out-dir> <image> <max-seconds> <probe-sn>` flashes `<out-dir>/<image>.hex` with
  STM32_Programmer_CLI on the probe with that serial number, captures its virtual COM port
  (`/dev/serial/by-id`) until the `RESULT` line, and writes `uart_<image>.log`.
  `PEMPTY_FIX=1` (STM32L4/G4 only) clears FLASH_SR.PEMPTY before reset; otherwise the board
  boots the ROM loader after programming. `RAM_LOAD=1` (STM32N6, board in development boot,
  BOOT1 = 1) connects under hardware reset, writes the RAM image, sets MSP/PC from the vector
  table (`VECTOR_BASE`, default 0x34180400) and runs it, so no external flash programming or
  jumper change is needed between images.

Do not use `west flash` to compare drivers: it rebuilds the build directory from the current
tree first.

## Limits

- Only the I2S standard format, stereo, 44.1 kHz and the SAI controller (master) mode for TX
  are used. Mono, other formats and rates, TX as a synchronous slave and MCLK frequency are not
  checked.
- The async image does not check RX data. TX and RX are separate masters: their frames have the
  same rate but an arbitrary phase, so even with the loopback jumper fitted RX samples TX data
  bit-shifted. Data is only checked by the sync image, where RX uses the TX FS and SCK. (A
  data check with independent sub-blocks would need RX as an asynchronous slave with FS and
  SCK wired from TX as well; not implemented.)
- On the N657 the MCLK probe is not used: polling GPIO IDR reads MCLK as constant 0 there,
  while a logic analyser shows it running.
- Timing checks have a 10 % window; they find a stalled or unpaced DMA, not jitter.
- Logging uses `CONFIG_LOG_MODE_IMMEDIATE`; the expected error lines of the overrun case
  (`RX queue full`, `RX invalid state: 4`) are printed from the DMA interrupt.

## License

Copyright (c) 2026 Mario Paja

Licensed under the Apache License, Version 2.0 (SPDX-License-Identifier: Apache-2.0), the same
license as Zephyr. See [LICENSE](LICENSE).
