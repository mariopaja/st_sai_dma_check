# Copyright (c) 2026 Mario Paja
# SPDX-License-Identifier: Apache-2.0
#
# Board file for suite.sh, sourced by it. Variables:
#   TARGET           west board target (default: the name given to suite.sh)
#   WIRING           full3  : Nucleo-144 loopback, SAI1_A to SAI1_B over SCK, FS and SD
#                    sd_only: single SD jumper (Nucleo-32), one phase
#   PEMPTY_FIX       1 on STM32L4/G4 (see flash_capture.sh)
#   JUMPERS_WIRED / JUMPERS_UNWIRED   text shown at the jumper prompts
#   SKIP_<image>     reason; the image is not run (dashes in the image name become _)
#   XFAIL_<image>    known failing cases, "|" separated substrings of the [FAIL] lines
#                    (tests/review, stress and tdm only). They do not fail the suite.

TARGET=nucleo_f767zi
WIRING=full3
JUMPERS_WIRED="PE5-PF8 (SCK), PE4-PF9 (FS), PE6-PE3 (SD)"
JUMPERS_UNWIRED="PE5-PF8 and PE4-PF9 removed (PE6-PE3 may stay)"

# 16 slots of 16 bit at 44.1 kHz loses a frame at the first block boundary, KNOWN_ISSUES.md issue 6.
XFAIL_tdm="16-bit x16 @44100"
# On F4/F7, mclk-divider "no-div" ignores frame_clk_freq (the HAL assumes MCLK = 256 x FS),
# KNOWN_ISSUES.md issue 9.
SKIP_tdm_nodiv="F4/F7: no-div ignores frame_clk_freq (KNOWN_ISSUES.md issue 9)"

# S/PDIF loopback (tests/spdifrx): SAI1_A SD (PE6) to the SPDIFRX input
JUMPERS_SPDIFRX="PE6-PD7"
