# Known issues and expected failures

Found while testing the STM32 SAI driver on the branches `st_sai/dma/zephyr_api_migration`
(PR #123259), `st_sai/tdm` and `st/spdif`. The board files in `suite/boards/` list the cases
below as expected failures, so `suite.sh` stays green on boards where they are understood.
Issue numbers are the ones used in those files.

## Open issues and limitations

None of these is critical: no crash, hang or data loss in a default, documented use.

| # | Issue | Type | Workaround / next step |
|---|---|---|---|
| 1 | **DMA v2 (L4, G4) not rerun on the final revision.** The 021 suite passed on NUCLEO-L432KC (plain L4, no DMAMUX) and NUCLEO-G431KB (G4, via `&dma1` and `&dmamux1`) with an earlier driver revision (`021_st_sai_dma_check/reports/`, marked "to be rerun"). The DROP storm never ran on them. L4+ (L4P5/L4R5) and L5 were never tested. | Test gap | Rerun the 021 async, sync and DROP storm images on both boards with `PEMPTY_FIX=1` (see 021's `scripts/run.sh`). 019/020/022 need the 3-wire loopback of a Nucleo-144 board. |
| 2 | **Free-running RX controller with a FIFO threshold above `"empty"` can swap channels on restart.** The controller stays enabled between transfers, so reception is not restarted on a frame boundary. With `"full"`, every second stereo transfer started on the right channel (H745, 7 of 15). The binding default is `"full"`. Probably the same on `main` (not verified). | Limitation | Use `fifo-threshold = "empty"` or a gated clock. The driver now logs a warning at configure. A real fix would restart the controller at START and pause its clocks for 1-2 frames. |
| 3 | **ISR busy-wait at stream end.** Bounded by the frame time: about 160 us at 44.1 kHz stereo, about 1.4 ms at 8 kHz mono. | Design choice | Inherent to keeping the last FIFO words. The alternative is a work item, at the cost of `-EIO` for a START right after a stream ends. |
| 4 | **RX with a threshold above `"empty"` leaves up to 7 words in the FIFO at stop.** | Inherent | They never form a complete block; the next START discards them. |
| 5 | **DROP from an interrupt with a higher priority than the DMA** can preempt the callback between its state check and the reload. | Limitation | Documented in a code comment. |
| 6 | **Throughput at high word rates.** Single-block DMA reload: with `fifo-threshold "empty"`, TX underruns at block boundaries above roughly 500k words/s (for example 16 slots x 16 bit at 44.1 kHz). With `"full"` the H745 (480 MHz) handles 16 x 16 at 44.1 kHz (706k words/s), the F767 (216 MHz) and the U575 (160 MHz) do not: one frame is lost at the first block boundary. 16 x 16 at 22.05 kHz passes on both. | Limitation | Use `"full"`. The ceiling depends on the CPU. Lifting it needs cyclic or double-buffered DMA. Mostly relevant to TDM. |
| 9 | **`mclk-divider "no-div"` on STM32F4/F7 ignores `frame_clk_freq`.** The F4/F7 HAL always computes MCKDIV for MCLK = 256 x FS, and on these SAIs NODIV removes the divider, so the bit clock equals the SAI kernel clock and FS = kernel clock / frame length. On the F767 with a 256 x 44.1 kHz kernel clock, 16-bit stereo ran 8x too fast. Only a 256-bit frame gets the configured rate. All other series compute MCKDIV from the frame length. Pre-existing, and the binding default is `"no-div"`. | Pre-existing bug | F4/F7 boards should set `div-256` or `div-512`, or set the PLL to FS x frame length. The driver could check the resulting rate and refuse a mismatch. Affects the `st_sai/tdm` `no-div` commit on F4/F7. |
| 10 | **U575 with `mclk-divider "no-div"` loses a frame at the first block boundary in cases that pass with `div-256`.** 16 x 8, 32 x 8 and PCM-L 16 x 8 at 44.1 kHz (353k words/s) and 16 x 12 fail with `no-div`, and pass divided. The H745 passes all of them with `no-div`. Not explained by the reload-latency ceiling alone, since the word rate is the same in both modes. Not investigated: the actual FS with `no-div` on the U575 has not been measured. | Open question | Measure FS (count FS edges against a timer) in both modes on the U575. Possibly related to how the U5 HAL derives MCKDIV with NODIV set. |
| 7 | **No SPDIFRX pinctrl nodes in hal_stm32.** A board has to define its own. | `st/spdif` | Add them to hal_stm32 or provide a board overlay or sample before the S/PDIF PR. |
| 8 | **S/PDIF and SPDIFRX not run on hardware.** Compiled on the H745 only (not the F7 node). | Test gap | Hardware test before the S/PDIF PR. |

## Test-app pitfalls (not driver bugs)

These looked like driver failures and were not:

- Printing to the UART right after an RX START (immediate log mode) delays the first
  reads enough to overflow the 4-block RX queue. Print after the stream instead.
- A loopback checker that locks on any value in the pattern range can lock on a stray
  word from an idle line. Lock only on the first expected value.
- Sampling a TX DMA counter once per RX block shows a "frozen" counter when the TX and
  RX block periods are equal: it is aliasing.
- In 019, the "left/right slot" of a stream is measured from the RX DMA's first word, so
  an RX-side misalignment looks like a TX-side one. Split the FIFO settings per
  sub-block to tell them apart.
