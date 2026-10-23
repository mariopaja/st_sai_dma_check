# NUCLEO-H745ZI-Q (M7): SAI DMA interrupt time

| | |
|---|---|
| Date | 2026-10-23 |
| Board | NUCLEO-H745ZI-Q, Cortex-M7 core, core clock 400 MHz, D-cache on, slabs `__nocache` |
| Image | async image with `variants/isr_timing.conf` (`CONFIG_SAI_CHECK_ISR_TIMING`) |
| DMA, change | SAI1 through `&dmamux1` (SoC dtsi): TX mux 0 = DMA1 stream 0 (IRQ 11), RX mux 1 = DMA1 stream 1 (IRQ 12) |
| DMA, `main` | SAI1 on `&dma1 2` / `&dma1 3` (SoC dtsi of the baseline): DMA1 stream 1 (IRQ 12) / stream 2 (IRQ 13) |
| Workload | TX and RX stream at the same time, 2000 blocks each (64 stereo frames, 44.1 kHz), free-running bit clock |

Method as in `isr_timing_nucleo_u575zi_q.md`: `DWT->CYCCNT` is read in the user tracing ISR
hooks on entry and exit of the two SAI DMA interrupts (DWT unlocked through `DWT->LAR` on the
Cortex-M7). The CPU load uses the system uptime for the elapsed time, because CYCCNT stops
while the core sleeps.

## 16-bit words

| Per direction | `main` (HAL DMA) | Change (Zephyr DMA) |
|---|---|---|
| TX interrupts per block | 2 (half and full transfer) | 1 |
| TX time per interrupt, avg / max | 3.49 / 6.57 us | 4.91 / 5.40 us |
| TX CPU time per block | 6.98 us | 4.91 us (-30 %) |
| TX CPU load | 0.481 % | 0.338 % |
| RX interrupts per block | 2 | 1 |
| RX time per interrupt, avg / max | 3.44 / 6.33 us | 5.48 / 5.73 us |
| RX CPU time per block | 6.88 us | 5.48 us (-20 %) |
| RX CPU load | 0.474 % | 0.377 % |

## 32-bit words

Within a few percent of the 16-bit figures for both drivers (`main` TX 3.52 us x 2, RX 3.44 us x 2;
change TX 5.05 us, RX 5.49 us). Both streams took 2902 ms.

## Logs

- `logs/nucleo_h745zi_q_stm32h745xx_m7/uart_isr_timing-new.log`, `uart_isr_timing-old.log`
