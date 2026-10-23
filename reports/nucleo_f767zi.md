# NUCLEO-F767ZI: STM32 SAI Zephyr DMA test report

| | |
|---|---|
| Date | 2026-10-22 |
| Board | NUCLEO-F767ZI (STM32F767ZI, Cortex-M7), ST-LINK V2-1 (SN 0671FF555567894967085923) |
| Change | final driver revision (Zephyr commit `3a2a6ecfead`, "drivers: i2s: stm32_sai: migrate dma from stm32_hal to zephyr api") |
| Baseline | `main` at `b70dbaf045e` (HAL DMA driver), built from a separate worktree |
| DMA | DMA2 (`st,stm32-dma-v1`). SAI1_A TX: stream 1 / channel 0. SAI1_B RX: stream 5 / channel 0 |
| Cache | `CONFIG_DCACHE=y`. Slabs in SRAM1 (0x20021E00 / 0x20022E00, cached), aligned to 32 bytes |
| Clock | PLLSAI from HSE, 44.27 kHz frame clock (+0.38 %), from `samples/drivers/i2s/output` |
| Wiring | PE6 (SD_A) to PE3 (SD_B), used by the sync image only |

Pins: SAI1_A MCLK PE2, FS PE4, SCK PE5, SD PE6. SAI1_B SD PE3, FS PF9, SCK PF8 (async image).
The sync image uses `boards/nucleo_f767zi_sync.overlay`. This is the first tested board with a
data cache.

## Summary

| Image | `main` | Change | Change without RX invalidate (control) |
|---|---|---|---|
| async | **FAIL**, 310 failures before the 150 s timeout | **PASS** 1063/1063 | not run |
| sync loopback | **FAIL** 40/64 | **PASS** 64/64 | **FAIL** 52/64 |

## Cache maintenance

The control image is the change with `sys_cache_data_invd_range()` removed from the RX
callback. It shows that the loopback detects missing cache maintenance:

| Image | Errors per round (16 bit) | Pattern |
|---|---|---|
| Change | 0 | - |
| Control | ~1050 | from block 1 on, the first samples of a block hold old data: the CPU reads lines cached during an earlier use of the same slab block |
| `main` | 1731-1833 (24/32 bit: ~6000) | same stale-line pattern, plus the L/R swap and 16-bit DMA width |

On the F429ZI (no D-cache), `main` had intact 16-bit counters. Here it fails at 16 bit too:
`main` never invalidated the RX buffer.

The slab alignment matters. With 4-byte alignment, an RX block can share a cache line with
another block or variable, and an invalidate can discard that data. The check app aligns its
slabs to 32 bytes, as `samples/drivers/i2s/output` does.

## Async image (change)

| Case | Result |
|---|---|
| TX/RX stream timing | 433-434 ms (expected 434 ms at 44.27 kHz), all sizes, both clock modes |
| TX+RX together | 433-434 ms both |
| DROP rounds, then read/stream again | PASS |
| FS pin (PE4) | gated: 0 Hz idle, 44.2 kHz streaming, 0 Hz after DROP; free-running: 44.2-44.4 kHz throughout |
| MCLK pin (PE2) | ~1860-3200 edges while streaming; gated: 0 after DROP (F7 has no `MCKEN`); free-running: keeps running |

## Not tested

- Buffers in DTCM
- TX as synchronous slave, mono, formats other than I2S standard, rates other than 44.1 kHz

## Files

- `logs/nucleo_f767zi/uart_{async,sync}-new.log`: change
- `logs/nucleo_f767zi/uart_sync-noinv.log`: control without RX invalidate
- `logs/nucleo_f767zi/uart_{async,sync}-old.log`: `main`

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
