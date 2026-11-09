# CLAUDE.md

Instructions for Claude Code in this repository. It holds hardware tests for the STM32 SAI I2S
driver of Zephyr (`drivers/i2s/i2s_stm32_sai.c`). Humans: see README.md; this file is the short
version for an agent that has to run the tests on a machine it has not seen before.

## What is here

| Path | Content |
|---|---|
| `src/`, `boards/`, `variants/` | The check app: async, sync loopback and DROP storm images (README.md) |
| `tests/review/` | Regression tests for each bug fixed in the SAI DMA migration, wiring probe, TX-target suite (`-DTX_TARGET_ONLY=1`) |
| `tests/stress/` | 30 s soak, 200 random configure/trigger cycles, overrun and underrun recovery |
| `tests/tdm/` | TDM loopback. Disabled for now: only runs with `--tests tdm,tdm-nodiv`, and needs a driver with TDM |
| `tests/spdif-tx/`, `tests/spdifrx/` | S/PDIF output of SAI1_A, decoded by SAI1_B (U575) or looped into SPDIFRX (H745, F767). Disabled for now: run with `--tests spdif-tx` or `--tests spdifrx`, needs the `st/spdif` branch (the suite skips them on a tree without it) |
| `suite/suite.sh` | Builds, flashes and runs all of the above for one board and prints one table |
| `suite/boards/<board>.sh` | Per-board settings: wiring, flash quirks, expected failures |
| `suite/KNOWN_ISSUES.md` | Open issues. The numbers appear in the board files |
| `reports/` | Per-board hardware reports of the DMA migration |

## Run the suite

```sh
# from this directory, in the west workspace with the Python environment active
suite/suite.sh <board> --phase wired      # jumpers fitted
suite/suite.sh <board> --phase unwired    # SCK and FS jumpers removed
suite/suite.sh <board> --list             # what would run and why something is skipped
suite/suite.sh --help
```

- `ZEPHYR_BASE` defaults to `<west topdir>/zephyr`. The Zephyr tree under test is whatever is
  checked out there: the summary prints its branch and commit. **Do not switch branches or modify
  that tree without asking the user**: they usually work on other branches in it. Ask which
  branch to test (the SAI work is on `st_sai/dma/zephyr_api_migration`, `st_sai/tdm`,
  `st/spdif`).
- `--phase all` stops between the phases to let the user move jumpers and needs a terminal. As an
  agent run `--phase wired`, ask the user to remove the two jumpers, then run `--phase unwired`.
- Several probes connected: `--probe <ST-Link serial>`, and `--port <tty>` if the console device
  cannot be chosen automatically (macOS: `/dev/cu.usbmodem*`, Linux: `/dev/serial/by-id/*`).
- `--build-only` builds without flashing. `--from-log <image> <file>` judges an existing console
  log, without a board.
- Exit status: 0 pass, 1 a test failed, 2 setup error, 3 wiring check failed, 130 aborted.

## Wiring safety (hardware)

On the Nucleo-144 boards (`WIRING=full3`) SAI1_A and SAI1_B are wired together by three jumpers:
PE5-PF8 (SCK), PE4-PF9 (FS), PE6-PE3 (SD).

- **Never configure SAI1_B as a clock controller while the SCK or FS jumper is fitted**: both
  sub-blocks would drive the same wire. The async and DROP storm images do exactly that, so they
  only run in the `unwired` phase. The TX-target suite is safe: it makes SAI1_A a target first.
- Each phase starts with a wiring probe image that reads the jumpers and stops the phase if they
  do not match. Do not remove or bypass that check.
- Nucleo-32 boards (`WIRING=sd_only`, L432KC and G431KB) use a single SD jumper; no phases.
- `spdifrx` runs in its own phase with a PE6-PD7 jumper. There is no probe check for it: PD7 is an
  input. Ask the user to move the jumper, as for the other phases.

## Claude Code sandbox

The sandbox blocks USB and serial devices, so flashing and the console fail inside it.
Add these to `sandbox.excludedCommands` in `~/.claude/settings.json` (ask the user first):

```json
"sandbox": { "excludedCommands": ["suite/suite.sh *", "STM32_Programmer_CLI *", "west flash *"] }
```

- An excluded command must be the whole Bash command. A `cd`, pipe, redirect or `&&` around
  `suite/suite.sh` puts it back in the sandbox and the run then fails or shows an old log. Run
  it alone, then read `suite-out/<board>/summary.txt` and the logs.
- Builds inside the sandbox need `CCACHE_DISABLE=1` and `SUITE_CMAKE_ARGS=-DUSE_CCACHE=0`.
- `mktemp` and `strings` do not work in the sandbox; the scripts avoid them.
- Console logs are immediate: a test that prints between an RX START and the first read can
  overflow the 4-block RX queue. Print after the stream (see `suite/KNOWN_ISSUES.md`).

## Reading the results

- `suite-out/<board>/summary.txt` is the table, `suite-out/<board>/<image>/uart.log` the console.
- A cell is `PASS`, `FAIL`, `NORESULT` (no end line before the timeout), `BUILDFAIL`, `FLASHFAIL`
  or `SKIP` with the reason. `expected failure(s)` in the note column are the cases the board file
  lists as known (`XFAIL_*`): they do not fail the suite. A change in that count is news.
- Before calling something a driver bug, rule out the test: see "Test-app pitfalls" in
  `suite/KNOWN_ISSUES.md`.
- Write what was tested (branch, commit, board, result) in `reports/<board>.md` when the user asks
  for a report.

## Adding a board

1. `boards/<board>.overlay` (+ `_sync.overlay`, `.conf`) for the check app, as for the others.
2. `suite/boards/<board with / replaced by _>.sh`, copying the closest board file.
3. For the `tests/*` apps add `tests/<app>/boards/<board>.overlay`; without it they are skipped.

## Conventions

- Apache-2.0 header with `Copyright (c) 2026 Mario Paja` in new files.
- Do not commit `suite-out/`, build directories, `*.hex` or `*.raw` (already in `.gitignore`).
