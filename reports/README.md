# STM32 SAI on the Zephyr DMA API: hardware test matrix

Updated 2026-10-23. Change: the SAI driver on the Zephyr DMA API (Zephyr commit `3a2a6ecfead`),
compared with the HAL DMA driver on `main` at `b70dbaf045e`.

Hardware status means the check app (`sai_dma_check`) ran on that board: the async image
(trigger paths, pacing, slab accounting, TX+RX together, FS/MCLK pins, 16/24/32-bit,
gated and free-running) and the sync loopback image (SAI1_B synchronous RX slave, D0-D11
jumper, per-sample data and L/R check).

| DMA controller (driver) | MCU series with SAI | Board | Hardware result (final change) | `main` (HAL DMA) on the same test |
|---|---|---|---|---|
| DMA v1 (`dma_stm32.c`) | F4 (F413, F427/F429, F446) | NUCLEO-F429ZI | **PASS** (\*): async 1003/1003, sync 64/64 | FAIL (leaks, L/R swap, data errors at 24/32 bit) |
| DMA v1 (`dma_stm32.c`) | F7 | NUCLEO-F767ZI | **PASS**: async 1064/1064, sync 65/65; D-cache on, slabs `__nocache` | FAIL (leaks, L/R swap; stale cache data with cached slabs) |
| DMA v1 + DMAMUX (`dma_stm32.c`), BDMA (`dma_stm32_bdma.c`) | H7 (H742, H743/H753, H745/H755, H7A3/H7B3) | NUCLEO-H745ZI-Q (M7) | **PASS**: async 1064/1064, sync 65/65 each for SAI1 on `&dma1` and on `&dmamux1`, and SAI4 on BDMA (`&dmamux2` and `&bdma1`); D-cache on, slabs `__nocache` / SRAM4 | FAIL (leaks, L/R swap; stale cache data with cached slabs; SAI4/BDMA does not start) |
| DMA v2, no DMAMUX (`dma_stm32.c`) | L4 (L431, L432, L451, L471) | NUCLEO-L432KC | **PASS** (\*): async 1003/1003, sync 64/64 | FAIL (leaks, L/R swap, lost frames) |
| DMA v2 + DMAMUX (`dma_stm32.c`) | L4+ (L4P5) | - | **not tested** | - |
| DMA v2 + DMAMUX (`dma_stm32.c`) | G4 | NUCLEO-G431KB | **PASS** (\*): async 995/995, sync 64/64, via `&dma1` and via `&dmamux1` | FAIL (leaks, L/R swap, data errors at 24/32 bit) |
| DMA v2 + DMAMUX (`dma_stm32.c`) | L5 | - | **not tested** | - |
| GPDMA (`dma_stm32u5.c`) | U5 (U575, U595) | NUCLEO-U575ZI-Q | **PASS**: async 1063/1063, sync 64/64 | FAIL (leaks, L/R swap, data errors at 24/32 bit) |
| GPDMA (`dma_stm32u5.c`) | U3 | - | **not tested** | - |
| GPDMA (`dma_stm32u5.c`) | H5 (H562/H563) | - | **not tested** | - |
| GPDMA (`dma_stm32u5.c`) | H7RS | - | **not tested** | - |
| GPDMA (`dma_stm32u5.c`) | WBA (WBA55) | - | **not tested** | - |
| GPDMA (`dma_stm32u5.c`) | N6 | NUCLEO-N657X0-Q | **PASS**: async 1056/1056, sync 65/65, SAI2 in both A/B role layouts; secure firmware, D-cache on, slabs `__nocache` | FAIL (leaks, L/R swap, data errors at 24/32 bit) |

(\*) Tested with the driver revision before the `SAIEN` wait in `sai_sub_disable()` and the removal
of the RX cache invalidate (no effect on these cacheless parts), and before
the two "still works after the DROP rounds" checks were added; to be rerun. The SAIEN race needs
a START within one frame of a gated stop, and these boards passed the overrun case that exposed
it on the U575.

## Coverage by DMA controller

Every DMA controller type an STM32 SAI can use is tested on hardware at least once.

| DMA controller | Driver | Tested on | Result |
|---|---|---|---|
| DMA v1, no DMAMUX | `dma_stm32.c` | NUCLEO-F429ZI, NUCLEO-F767ZI (D-cache) | PASS (F429ZI: \*) |
| DMA v1 + DMAMUX | `dma_stm32.c` | NUCLEO-H745ZI-Q M7, SAI1 via `&dma1` and `&dmamux1` | PASS |
| DMA v2, no DMAMUX | `dma_stm32.c` | NUCLEO-L432KC | PASS (\*) |
| DMA v2 + DMAMUX | `dma_stm32.c` | NUCLEO-G431KB, SAI1 via `&dma1` and `&dmamux1` | PASS (\*) |
| BDMA (+ DMAMUX2) | `dma_stm32_bdma.c` | NUCLEO-H745ZI-Q M7, SAI4 via `&dmamux2` and `&bdma1` | PASS |
| GPDMA | `dma_stm32u5.c` | NUCLEO-U575ZI-Q (non-secure), NUCLEO-N657X0-Q (secure channel attributes) | PASS |

Not covered: L4+, L5, U3, H5, H7RS, WBA (no hardware run; they use the same DMA drivers as the
tested boards).

## Driver defects found on hardware and fixed in the change

| Defect | Present in `main` | Fix |
|---|---|---|
| Slab block leaked on DROP while running (TX and RX), on RX STOP and on PREPARE | yes | `queue_drop()` frees the active block after the DMA is stopped |
| One stereo frame lost per RX block with the default `fifo-threshold = "full"` | yes (~56 % of blocks) | Callbacks restart the DMA before queueing or freeing the finished block |
| TX L/R swapped with a free-running bit clock | yes | Prime one word per channel instead of one word |
| 24/32-bit data corrupted: DMA width taken from the devicetree (16 bit) | yes (F4, L4, G4) | DMA width follows the word size |
| Stale FIFO words / L/R swap after a synchronous RX restart | yes | RX start disables, flushes and re-enables a synchronous slave |
| Bit clock kept running after DROP with `I2S_OPT_BIT_CLK_GATED` | yes | `sai_sub_dma_stop()` calls `sai_sub_disable()` |
| START right after a gated stop left the SAI disabled (`SAIEN` still read 1) | race; seen on U5 | `sai_sub_disable()` waits until `SAIEN` reads 0 |

## Data cache (unchanged from `main`, documented)

The driver flushes TX blocks before the DMA reads them and does not invalidate RX blocks: an
invalidation of a block that is not cache-line aligned would discard data sharing its edge
lines. On SoCs with a data cache (F7, H7), the RX slab buffer must be `__nocache` with
`CONFIG_NOCACHE_MEMORY=y`; a comment in `sai_sub_rx_callback()` says so. On the F767ZI and
H745ZI-Q with cached slabs, RX data is stale: about 1700-1900 wrong samples per 12800 for `main`
on both boards, and about 1050 for the change without an RX invalidate (F767ZI). With
`__nocache` slabs both boards pass.

## Open points (not changed)

- On the N657, polling GPIO IDR does not see the 11.3 MHz MCLK (always 0); a logic analyser shows
  MCLK on PE0 and PE14, so the check app skips the MCLK probe there (`nucleo_n657x0_q.md`).
- With `mclk-enable` on SAI blocks that have `MCKEN` (G4, U5, ...), MCLK keeps running after a
  gated stop: it is generated independently of `SAIEN`. Expected for codecs; documented, not changed.
- `sai_sub_init()` uses `k_msgq_alloc_init()`, so the driver needs `CONFIG_HEAP_MEM_POOL_SIZE`;
  the static `queue_##node` msgq defined per node is unused.
- Fixed separately: the SoC dtsi files of the series with a DMAMUX (G4, H7, L4+, L5) now route
  SAI through `&dmamux1` (SAI1 on mux channels 0/1, SAI2 on 2/3) instead of referencing the
  DMA controllers. The `&dma1` results above are from before that change.
- No STM32 dtsi file in the tree describes SAI4; the test adds it in
  `variants/h7_sai4_bdma.overlay`.
- The binding default `fifo-threshold = "full"` leaves RX no FIFO headroom; `"empty"` is safer.
