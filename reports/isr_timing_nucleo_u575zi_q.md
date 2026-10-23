# NUCLEO-U575ZI-Q: SAI DMA interrupt time

| | |
|---|---|
| Date | 2026-10-23 |
| Board | NUCLEO-U575ZI-Q, core clock 160 MHz |
| Image | async image with `variants/isr_timing.conf` (`CONFIG_SAI_CHECK_ISR_TIMING`) |
| DMA | GPDMA1: SAI1_A TX on channel 1 (IRQ 30), SAI1_B RX on channel 0 (IRQ 29) |
| Workload | TX and RX stream at the same time, 2000 blocks each (64 stereo frames, 44.1 kHz), free-running bit clock |

Method: the user tracing hooks (`sys_trace_isr_enter_user()` / `sys_trace_isr_exit_user()`,
called by the Cortex-M ISR wrapper) read `DWT->CYCCNT` on entry and exit of the two SAI DMA
interrupts. The time therefore covers the DMA driver ISR and the SAI callback, plus the ISR
wrapper and the hooks themselves (a few tens of cycles). Nested interrupts are not excluded. The
CPU load uses the system uptime for the elapsed time (CYCCNT may stop while the core sleeps).
Neither driver was modified for the measurement.

## 16-bit words

| Per direction | `main` (HAL DMA) | Change (Zephyr DMA) |
|---|---|---|
| TX interrupts per block | 2 (half and full transfer) | 1 |
| TX time per interrupt, avg / max | 7.33 / 12.68 us | 10.78 / 10.80 us |
| TX CPU time per block | 14.7 us | 10.8 us (-27 %) |
| TX CPU load | 1.010 % | 0.742 % |
| RX interrupts per block | 2 | 1 |
| RX time per interrupt, avg / max | 7.62 / 14.09 us | 13.03 / 13.26 us |
| RX CPU time per block | 15.2 us | 13.0 us (-15 %) |
| RX CPU load | 1.049 % | 0.897 % |

`main` runs the HAL DMA handler with the half-transfer interrupt enabled, so every block costs two
interrupts. The Zephyr DMA driver enables the half-transfer interrupt only in cyclic mode. The
longest interrupt in both drivers is the transfer-complete one that restarts the DMA; it is
1.9 us (TX) and 0.8 us (RX) shorter with the change. That is the time the SAI FIFO has to cover
between two blocks.

## 32-bit words

The change gives the same figures as at 16 bit (TX 10.79 us, RX 13.03 us per block). `main` is not
comparable: its 2000 blocks took 5807 ms instead of 2904 ms, because it moves 16-bit halves of the
32-bit words (DMA width taken from the devicetree), so each block needs twice as many SAI slots.

## Logs

- `logs/nucleo_u575zi_q/uart_isr_timing-new.log`, `uart_isr_timing-old.log`
