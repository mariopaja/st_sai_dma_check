# NUCLEO-G431KB: STM32 SAI Zephyr DMA test report

| | |
|---|---|
| Date | 2026-10-22 |
| Board | NUCLEO-G431KB (STM32G431KB), ST-LINK V3 (SN 002B003C3233510F39363634) |
| Change | driver revision before the `SAIEN` wait in `sai_sub_disable()`; otherwise as the final revision (Zephyr commit `3a2a6ecfead`) |
| Baseline | `main` at `b70dbaf045e` (HAL DMA driver) |
| DMA | DMA1 + DMAMUX1 (`st,stm32-dma-v2`, `st,stm32-dmamux`) |
| Clock | PLL Q /8, 44.1 kHz frame clock (-0.03 %) |
| Wiring | D0 (PA10, SD_A) to D11 (PB5, SD_B), used by the sync image only |

Each image was built with `west build -p` and flashed with `STM32_Programmer_CLI -w <hex> -v`
(`scripts/run.sh`), never with `west flash`. Before flashing, the ELF symbols were checked:
`HAL_DMA_Start_IT`/`HAL_SAI_TxCpltCallback` in the baseline, `sai_sub_dma_reload` in the change.

## Summary

| Image | DMA routing | `main` | Change |
|---|---|---|---|
| async | `&dma1` ch 1/2 (SoC dtsi before the DMAMUX change) | **FAIL**, 315 failures before the 150 s timeout | **PASS** 995/995 |
| sync loopback | `&dma1` ch 1/2 (SoC dtsi before the DMAMUX change) | **FAIL** 40/64 | **PASS** 64/64 |
| async | `&dmamux1` mux 0/1 (test overlay; now the `stm32g4.dtsi` default) | not run | **PASS** 995/995 |
| sync loopback | `&dmamux1` mux 0/1 (test overlay; now the `stm32g4.dtsi` default) | not run | **PASS** 64/64 |

The async image has 8 fewer checks than on the L432KC because this board has no MCLK pin on
the header.

## DMA routing on G4

At the time of these runs, `stm32g4.dtsi` gave SAI1 `dmas = <&dma1 1 108 ...>` / `<&dma1 2 109 ...>`;
it now uses `<&dmamux1 0 108 ...>` / `<&dmamux1 1 109 ...>`, the form tested in the overlay rows above.
With the `&dma1` form, `dma_stm32.c`
writes the request into `PeriphRequest`, and the G4 `LL_DMA_Init()` writes it to the DMAMUX
channel register, so requests do go through the DMAMUX. The other G4 users in the tree (UART,
SPI, ADC) reference `&dmamux1` instead. That goes through the DMAMUX driver, which owns the
channel allocation. Both forms pass on hardware. With the `&dma1` form, another peripheral
that allocates through `dmamux1` could be given the same channel.

## Async image

TX on SAI1_A (PA8 SCK, PA9 FS, PA10 SD), RX on SAI1_B as an unwired master (PB3 SCK, PB6 FS,
PB5 SD). Word sizes 16, 24 and 32, bit clock gated and then free-running. The cases and pass
conditions are listed in `nucleo_l432kc.md`.

| Case | `main` | Change |
|---|---|---|
| TX/RX stream timing | 434 ms at 16 bit, then fails once the slab is exhausted | 433-435 ms, all sizes |
| TX/RX DROP while running | **one slab block lost per DROP** | PASS |
| RX STOP, PREPARE | **slab leak** | PASS |
| TX+RX together | not reached | 433-434 ms both |
| FS pin, gated | `FS frequency 0 Hz` (TX never started, slab exhausted) | 0 Hz idle, 44.2 kHz streaming, 0 Hz after DROP |
| FS pin, free-running | not reached | 44.2-44.4 kHz throughout |

The first final-revision run failed 2 checks: with the bit clock gated, FS kept running after
`I2S_TRIGGER_DROP`. DROP stopped the DMA but left the SAI enabled. `main` behaves the same.
`sai_sub_dma_stop()` now also calls `sai_sub_disable()`, which still keeps free-running and
synchronous sub-blocks enabled. After that change: 995/995.

## Sync loopback image

| Word size | `main` | Change |
|---|---|---|
| 16 | rounds pass on counters but **all L/R swapped**; after DROP 1 error | 0 errors, 0 swapped (3 rounds + after DROP) |
| 24 | **12672 errors, ~6500 swapped per round**, slab leaks after STOP | 0 errors, 0 swapped |
| 32 | same as 24 | 0 errors, 0 swapped |

Each defect and its fix is described in `nucleo_l432kc.md`.

## Not tested

- MCLK output (no MCLK pin on the Arduino Nano header)
- TX as synchronous slave, mono, formats other than I2S standard, rates other than 44.1 kHz

## Files

- `logs/nucleo_g431kb/uart_{async,sync}-new.log`: change, `&dma1` routing
- `logs/nucleo_g431kb/uart_{async,sync}-mux.log`: change, `&dmamux1` routing
- `logs/nucleo_g431kb/uart_{async,sync}-old.log`: `main`
- `logs/nucleo_g431kb/first_revision/`: earlier app and driver revision (pacing and leak checks only)
