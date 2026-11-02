# NUCLEO-N657X0-Q: STM32 SAI Zephyr DMA test report

| | |
|---|---|
| Date | 2026-10-23 |
| Board | NUCLEO-N657X0-Q (STM32N657X0H3Q, Cortex-M55), ST-LINK V3EC (SN 002800224142500220353451) |
| Change | final driver revision (Zephyr commit `3a2a6ecfead`, "drivers: i2s: stm32_sai: migrate dma from stm32_hal to zephyr api") |
| Baseline | `main` at `b70dbaf045e` (HAL DMA driver), built from a separate worktree |
| Build | `nucleo_n657x0_q`: secure firmware (`CONFIG_ARM_SECURE_FIRMWARE=y`), D-cache on, `CONFIG_NOCACHE_MEMORY=y` (`boards/nucleo_n657x0_q.conf`), slabs in `.nocache` |
| DMA | GPDMA1 (`st,stm32u5-dma`), secure channel attributes. SAI2_A: ch 3 / req 93. SAI2_B: ch 2 / req 94 |
| Clock | PLL2 / IC7 as SAI2 kernel clock, 44.084 kHz frame clock (-0.03 %), from `samples/drivers/i2s/output` |
| Wiring | PF5 (SAI2_SD_A, CN3 pin 28) to PE11 (SAI2_SD_B, Arduino D7 / CN15 pin 23), sync images only |

The board has no internal flash. All images were loaded into RAM in development boot mode
(BOOT1 = 1): `scripts/run.sh` with `RAM_LOAD=1` connects under hardware reset (`mode=UR reset=HWrst
ap=1`), writes the hex, sets MSP and PC from the vector table at 0x34180400 and runs. No
external flash programming or jumper changes between images.

## Pin layouts

| Layout | TX | RX | Overlays |
|---|---|---|---|
| default | SAI2_A: MCLK PE0, SCK PD13, FS PG13, SD PF5 | SAI2_B: SD PE11, FS PE13, SCK PE12 | `boards/nucleo_n657x0_q.overlay` (+ `_sync.overlay`) |
| swapped | SAI2_B: MCLK PE14, SCK PE12, FS PE13, SD PE11 | SAI2_A: SD PF5, FS PG13, SCK PD13 | + `variants/n6_sai2b_tx.overlay` (+ `variants/n6_sai2b_tx_sync.overlay`) |

I2C4 is disabled (PE12/PE13). The same jumper serves both layouts; only the direction changes.

## Summary

| Image | `main` | Change |
|---|---|---|
| async, default layout | **FAIL**, 304 failures before the 150 s timeout | **PASS** 1056/1056 |
| async, swapped layout | not run | **PASS** 1056/1056 |
| sync loopback, default layout | **FAIL** 41/65 | **PASS** 65/65 |
| sync loopback, swapped layout | not run | **PASS** 65/65 |

`main` fails as on the other boards: slab leaks on DROP/STOP/PREPARE that end in a cascade, L/R
swapped at 16 bit, and 12672 of 12673 samples wrong at 24/32 bit (16-bit DMA width).

This is the only tested board running secure firmware, so it is the only hardware coverage of
the GPDMA secure channel attributes.

## MCLK

With `sai-mclk-gpios` set, the MCLK probe read 0 on every sample, on PE0 and on PE14, with
`slew-rate = "very-high-speed"` on the MCLK pins (scratch overlay, not kept) too. The FS probe on the same ports
read 44.0-44.2 kHz.

- A register dump while streaming (scratch diagnostic, `logs/.../uart_pins-diag-*.log`) shows the
  SAI set up for MCLK output: `CR1 = 0x08430280` (`MCKEN=1`, `SAIEN=1`, `MCKDIV=4`, i.e.
  45.1 MHz / 4 = 11.29 MHz), pin in alternate function AF10. `main` writes the same `CR1`.
- A logic analyser showed MCLK on both pins, with an image that streams with a free-running bit
  clock without stopping: PE14 in the swapped layout (with SCK PE12, FS PE13, data PE11), and
  PE0 in the default layout (data visible on PE11 through the jumper; the SAI2_B pins went idle
  when that image started).

So MCLK is output; polling GPIO IDR does not see an 11.3 MHz signal on this SoC. The overlays no
longer define `sai-mclk-gpios` for this board, so the MCLK checks are skipped (8 fewer checks
than on the F7/H7/U5 boards).

## Not tested

- Boot from the external flash (images ran from RAM in development boot)
- MCLK frequency (activity only was checked on the analyser)
- TX as synchronous slave, mono, formats other than I2S standard, rates other than 44.1 kHz

## Files

- `logs/nucleo_n657x0_q/uart_{async,sync}-new.log`: change, default layout
- `logs/nucleo_n657x0_q/uart_{async,sync}-new-sai2b_tx.log`: change, swapped layout
- `logs/nucleo_n657x0_q/uart_{async,sync}-old.log`: `main`
- `logs/nucleo_n657x0_q/uart_pins-diag-{new,old}.log`: clock-pin cases with the register dump

## Update 2026-11-02: HPDMA1 and PR 123259 after review

Driver from PR 123259 after the review fixes, rebased on `main` (Zephyr commit `7bdffd3af34`),
secure firmware, RAM load as above, PF5-PE11 jumper. `variants/n6_sai2_hpdma.overlay` moves
SAI2 from GPDMA1 to HPDMA1 with the same channels and requests (SAI2_A channel 3 / request 93,
SAI2_B channel 2 / request 94); slabs in `.nocache` AXISRAM (0x3418xxxx).

HPDMA1 uses the same driver (`dma_stm32u5.c`) but its own code there: in secure firmware every
HPDMA channel gets static CID1 isolation at init (without it, transfers complete without moving
data), and the source/destination port allocation is left at the reset value (it is only set
for H7RS). Nothing in the tree used HPDMA1 on the N6 before.

| Image | GPDMA1 | HPDMA1 |
|---|---|---|
| sync loopback | **PASS** 65/65 | **PASS** 65/65: 12800 samples per round, 0 errors, 0 L/R swapped |
| async | **PASS** 1056/1056 | **PASS** 1056/1056 |
| DROP storm (`variants/drop_storm.conf`) | not run | **PASS**, 6 x 1000 DROPs, no log line |

So the request numbers 93/94 select SAI2 on HPDMA1 as on GPDMA1, and the default port
allocation reaches both the SAI and AXISRAM. Async logs only contain the expected overrun
lines. Logs: `logs/nucleo_n657x0_q/uart_{synchp,asynchp,stormhp,sync,async}-review.log`.
