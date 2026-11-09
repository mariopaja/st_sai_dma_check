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

TARGET=nucleo_l432kc
WIRING=sd_only
PEMPTY_FIX=1
JUMPERS_WIRED="D0 (PA10) to D11 (PB5)"
# Only the check app has overlays for this board: no tests/review, stress or tdm.
