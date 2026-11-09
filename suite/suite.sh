#!/bin/bash
# Copyright (c) 2026 Mario Paja
# SPDX-License-Identifier: Apache-2.0
#
# Builds, flashes and runs every SAI hardware test for one board, then prints one summary.
# Run suite.sh --help for the options. Works on Linux and macOS (bash 3.2).

SUITE_DIR=$(cd "$(dirname "$0")" && pwd)
APP=$(cd "$SUITE_DIR/.." && pwd)

usage() {
cat <<'EOF'
Usage: suite.sh <board> [options]

  <board>              west board target, or its file name form, for example
                       nucleo_u575zi_q or nucleo_h745zi_q/stm32h745xx/m7. There must be
                       suite/boards/<board with / replaced by _>.sh.

Options:
  --list               show the images selected for the board and why others are skipped
  --phase wired|unwired|spdifrx|all
                       wired:   loopback jumpers fitted (sync, review, TX target, stress, TDM,
                                S/PDIF output)
                       unwired: SCK and FS jumpers removed (async, DROP storm)
                       spdifrx: PE6-PD7 fitted (SPDIFRX loopback)
                       all (default): wired, then ask to change the jumpers, then unwired,
                                then spdifrx
                       Boards with an SD-only loopback have a single phase.
  --tests a,b,...      only these images: check-sync review review-txtgt stress tdm tdm-nodiv
                       spdif-tx spdifrx check-async check-storm (or the group "check").
                       tdm, tdm-nodiv, spdif-tx and spdifrx are disabled for now and only
                       run when named here.
  --probe <sn>         ST-Link serial number, when several probes are connected
  --port <tty>         console device (default: the only /dev/cu.usbmodem* or by-id device)
  --out <dir>          output directory (default: suite-out/<board>)
  --build-only         build the images, do not flash
  --no-build           reuse the images already in the output directory
  --yes                do not stop at the jumper prompts
  --from-log <image> <file>
                       judge an existing console log as <image>, without a board

Environment:
  ZEPHYR_BASE          Zephyr tree under test (default: <west topdir>/zephyr)
  SUITE_CMAKE_ARGS     extra CMake arguments for every build, for example -DUSE_CCACHE=0
  PROBE_SN, SAI_TTY    same as --probe, --port

Exit status: 0 all images passed (expected failures from the board file excluded),
1 a test failed or did not finish, 2 setup error, 3 wiring check failed, 130 aborted.
EOF
}

die() { echo "suite: $*" >&2; exit 2; }

# --- arguments --------------------------------------------------------------------------------
BOARD=""; TESTS=""; PHASE=all; OUT=""; BUILD=1; FLASH=1; YES=0; LIST=0
FROM_IMG=""; FROM_LOG=""
PROBE_SN=${PROBE_SN:-}; SAI_TTY=${SAI_TTY:-}
while [ $# -gt 0 ]; do
	case $1 in
	-h|--help) usage; exit 0;;
	--list) LIST=1; shift;;
	--phase) PHASE=$2; shift 2;;
	--tests) TESTS=$2; shift 2;;
	--probe) PROBE_SN=$2; shift 2;;
	--port) SAI_TTY=$2; shift 2;;
	--out) OUT=$2; shift 2;;
	--build-only) FLASH=0; shift;;
	--no-build) BUILD=0; shift;;
	--yes) YES=1; shift;;
	--from-log) FROM_IMG=$2; FROM_LOG=$3; BUILD=0; FLASH=0; shift 3;;
	-*) die "unknown option $1 (see --help)";;
	*) BOARD=$1; shift;;
	esac
done
[ -n "$BOARD" ] || { usage; exit 2; }
case $PHASE in wired|unwired|spdifrx|all) ;; *) die "--phase must be wired, unwired, spdifrx or all";; esac
export PROBE_SN SAI_TTY

# --- board manifest ---------------------------------------------------------------------------
bn=${BOARD//\//_}
MAN=$SUITE_DIR/boards/$bn.sh
[ -f "$MAN" ] || die "no board file $MAN (copy an existing one in suite/boards/)"
TARGET=$BOARD; WIRING=full3; PEMPTY_FIX=0
JUMPERS_WIRED=""; JUMPERS_UNWIRED=""
. "$MAN"
export PEMPTY_FIX
[ -n "$OUT" ] || OUT=$APP/suite-out/$bn
mkdir -p "$OUT"

# --- Zephyr tree ------------------------------------------------------------------------------
if [ -z "${ZEPHYR_BASE:-}" ]; then
	TOP=$(cd "$APP" && west topdir 2>/dev/null)
	[ -n "$TOP" ] && ZEPHYR_BASE=$TOP/zephyr
fi
if [ "$FROM_LOG" = "" ]; then
	[ -d "${ZEPHYR_BASE:-/nonexistent}/drivers/i2s" ] || die "set ZEPHYR_BASE to the Zephyr tree under test"
	export ZEPHYR_BASE
	command -v west >/dev/null || die "west not found: activate the Python environment of the workspace"
	if [ $FLASH = 1 ]; then
		command -v STM32_Programmer_CLI >/dev/null || die "STM32_Programmer_CLI not in PATH (or use --build-only)"
	fi
fi
DRV=${ZEPHYR_BASE:-.}/drivers/i2s/i2s_stm32_sai.c
HAS_TDM=0; HAS_SPDIF=0; HAS_SPDIFRX=0
grep -q stm32_sai_sub_frame_conf "$DRV" 2>/dev/null && HAS_TDM=1
grep -q 'st,sai-spdif' "${ZEPHYR_BASE:-.}/dts/bindings/i2s/st,stm32-sai.yaml" 2>/dev/null && HAS_SPDIF=1
[ -f "${ZEPHYR_BASE:-.}/dts/bindings/i2s/st,stm32-spdifrx.yaml" ] && HAS_SPDIFRX=1

# --- images -----------------------------------------------------------------------------------
sync_ovl() {
	if [ -f "$APP/boards/${bn}_sync.overlay" ]; then echo "$APP/boards/${bn}_sync.overlay"
	else echo "$APP/variants/sync.overlay"; fi
}

# image_spec <name>: sets IMG_APP IMG_KIND IMG_END IMG_TIMEOUT and the array IMG_ARGS
image_spec() {
	IMG_ARGS=()
	IMG_KIND=done; IMG_END='^=== DONE'; IMG_TIMEOUT=90
	case $1 in
	probe-wired|probe-unwired)
		IMG_APP=$APP/tests/review; IMG_ARGS=(-DPROBE_ONLY=1); IMG_KIND=probe; IMG_TIMEOUT=40;;
	check-sync)
		IMG_APP=$APP; IMG_ARGS=("-DEXTRA_DTC_OVERLAY_FILE=$(sync_ovl)")
		IMG_KIND=result; IMG_END='^RESULT'; IMG_TIMEOUT=90;;
	check-async)
		IMG_APP=$APP; IMG_KIND=result; IMG_END='^RESULT'; IMG_TIMEOUT=120;;
	check-storm)
		IMG_APP=$APP; IMG_ARGS=("-DEXTRA_CONF_FILE=$APP/variants/drop_storm.conf")
		IMG_KIND=result; IMG_END='^RESULT'; IMG_TIMEOUT=150;;
	review)
		IMG_APP=$APP/tests/review; IMG_ARGS=(-DTX_TARGET_ONLY=0);;
	review-txtgt)
		IMG_APP=$APP/tests/review; IMG_ARGS=(-DTX_TARGET_ONLY=1);;
	stress)
		IMG_APP=$APP/tests/stress; IMG_TIMEOUT=170;;
	tdm)
		IMG_APP=$APP/tests/tdm
		IMG_ARGS=("-DEXTRA_DTC_OVERLAY_FILE=$APP/tests/common/fifo_full.overlay");;
	tdm-nodiv)
		IMG_APP=$APP/tests/tdm
		IMG_ARGS=("-DEXTRA_DTC_OVERLAY_FILE=$APP/tests/tdm/no_div.overlay;$APP/tests/common/fifo_full.overlay");;
	spdif-tx)
		IMG_APP=$APP/tests/spdif-tx; IMG_KIND=bars; IMG_END='^==== [0-9]+ passed'; IMG_TIMEOUT=90;;
	spdifrx)
		IMG_APP=$APP/tests/spdifrx; IMG_KIND=bars; IMG_END='^==== [0-9]+ passed'; IMG_TIMEOUT=90;;
	*) die "unknown image $1";;
	esac
}

# skip_reason <name>: prints why an image cannot run on this board, nothing if it can
skip_reason() {
	local name=$1 t v
	# TDM is not merged yet: the images are kept but only run when asked for with --tests
	case $name in
	tdm|tdm-nodiv)
		[ -z "$TESTS" ] && { echo "disabled for now: TDM is not merged (enable with --tests $name)"; return; };;
	spdif-tx|spdifrx)
		[ -z "$TESTS" ] && { echo "disabled for now: S/PDIF is not merged (enable with --tests $name)"; return; };;
	esac
	# reasons given in the board file come before the missing-overlay checks
	v=SKIP_${name//-/_}
	[ -n "${!v:-}" ] && { echo "${!v}"; return; }
	case $name in
	check-*) [ -f "$APP/boards/$bn.overlay" ] || { echo "no boards/$bn.overlay in the check app"; return; };;
	probe-*|review|review-txtgt) t=review;;
	stress) t=stress;;
	tdm|tdm-nodiv) t=tdm;;
	spdif-tx) t=spdif-tx;;
	spdifrx) t=spdifrx;;
	esac
	if [ -n "${t:-}" ] && [ ! -f "$APP/tests/$t/boards/$bn.overlay" ]; then
		echo "no tests/$t/boards/$bn.overlay"; return
	fi
	case $name in
	tdm|tdm-nodiv) [ $HAS_TDM = 1 ] || { echo "the driver under test has no TDM"; return; };;
	spdif-tx) [ $HAS_SPDIF = 1 ] || { echo "the Zephyr tree has no S/PDIF output (st/spdif branch)"; return; };;
	spdifrx) [ $HAS_SPDIFRX = 1 ] || { echo "the Zephyr tree has no SPDIFRX driver (st/spdif branch)"; return; };;
	esac
}

phase_images() {
	if [ "$WIRING" = sd_only ]; then
		[ "$1" = wired ] && echo "check-sync check-async check-storm"
		return
	fi
	case $1 in
	wired) echo "probe-wired check-sync review review-txtgt stress tdm tdm-nodiv spdif-tx";;
	unwired) echo "probe-unwired check-async check-storm";;
	spdifrx) echo "spdifrx";;
	esac
}

selected() { # selected <image>: is it wanted by --tests
	[ -z "$TESTS" ] && return 0
	case ",$TESTS," in
	*",$1,"*) return 0;;
	*",check,"*) case $1 in check-*) return 0;; esac;;
	esac
	return 1
}

# --- result evaluation ------------------------------------------------------------------------
# evaluate <image> <kind> <log>: sets R_STATUS (PASS FAIL NORESULT) R_PASS R_FAIL R_NOTE
evaluate() {
	local name=$1 kind=$2 log=$3 line v pats f xf tmp
	R_STATUS=NORESULT; R_PASS=-; R_FAIL=-; R_NOTE=""
	[ -f "$log" ] || { R_NOTE="no console log"; return; }
	case $kind in
	probe)
		line=$(grep -E '^Wiring:' "$log" | tail -1)
		[ -n "$line" ] || { R_NOTE="no Wiring line"; return; }
		local sck fs sd
		sck=$(echo "$line" | sed -E 's/.*SCK [A-Z0-9-]+ (yes|no).*/\1/')
		fs=$(echo "$line" | sed -E 's/.*FS [A-Z0-9-]+ (yes|no).*/\1/')
		sd=$(echo "$line" | sed -E 's/.*SD [A-Z0-9-]+ (yes|no).*/\1/')
		R_NOTE="SCK $sck, FS $fs, SD $sd"
		if [ "${name#probe-}" = wired ]; then
			[ "$sck $fs $sd" = "yes yes yes" ] && R_STATUS=PASS || R_STATUS=FAIL
		else
			[ "$sck $fs" = "no no" ] && R_STATUS=PASS || R_STATUS=FAIL
		fi
		return;;
	bars)
		line=$(grep -E '^==== [0-9]+ passed' "$log" | tail -1)
		[ -n "$line" ] || { R_NOTE="no result line (timeout)"; return; }
		R_PASS=$(echo "$line" | sed -E 's/.*==== ([0-9]+) passed, ([0-9]+) failed.*/\1/')
		R_FAIL=$(echo "$line" | sed -E 's/.*==== ([0-9]+) passed, ([0-9]+) failed.*/\2/')
		[ "$R_FAIL" = 0 ] && R_STATUS=PASS || R_STATUS=FAIL
		return;;
	result)
		line=$(grep -E '^RESULT:' "$log" | tail -1)
		[ -n "$line" ] || { R_NOTE="no RESULT line (timeout)"; return; }
		R_PASS=$(echo "$line" | sed -E 's/.*\(([0-9]+) passed, ([0-9]+) failed\).*/\1/')
		R_FAIL=$(echo "$line" | sed -E 's/.*\(([0-9]+) passed, ([0-9]+) failed\).*/\2/')
		[ "$R_FAIL" = 0 ] && R_STATUS=PASS || R_STATUS=FAIL
		return;;
	done)
		line=$(grep -E '^=== DONE: [0-9]+ passed' "$log" | tail -1)
		[ -n "$line" ] || { R_NOTE="no DONE line (timeout)"; return; }
		R_PASS=$(echo "$line" | sed -E 's/.*DONE: ([0-9]+) passed, ([0-9]+) failed.*/\1/')
		R_FAIL=$(echo "$line" | sed -E 's/.*DONE: ([0-9]+) passed, ([0-9]+) failed.*/\2/')
		v=XFAIL_${name//-/_}; pats=${!v:-}
		xf=0
		if [ -n "$pats" ] && [ "$R_FAIL" -gt 0 ]; then
			tmp=$OUT/.xfail.$$
			printf '%s\n' "$pats" | tr '|' '\n' > "$tmp"
			xf=$(grep '^\[FAIL\]' "$log" | grep -cFf "$tmp")
			rm -f "$tmp"
		fi
		f=$((R_FAIL - xf))
		if [ $f -le 0 ]; then
			R_STATUS=PASS
			[ $xf -gt 0 ] && R_NOTE="$xf expected failure(s)"
		else
			R_STATUS=FAIL
			[ $xf -gt 0 ] && R_NOTE="$xf expected failure(s), $f unexpected"
		fi
		return;;
	esac
}

# --- summary rows -----------------------------------------------------------------------------
ROW_IMG=(); ROW_ST=(); ROW_P=(); ROW_F=(); ROW_N=()
add_row() { ROW_IMG[${#ROW_IMG[@]}]=$1; ROW_ST[${#ROW_ST[@]}]=$2; ROW_P[${#ROW_P[@]}]=$3
	ROW_F[${#ROW_F[@]}]=$4; ROW_N[${#ROW_N[@]}]=$5; }
FAILED=0; WIRING_BAD=0

print_summary() {
	local i hdr
	hdr="Board $TARGET ($WIRING loopback), $(date '+%Y-%m-%d %H:%M')
Zephyr $(git -C "${ZEPHYR_BASE:-.}" rev-parse --abbrev-ref HEAD 2>/dev/null) $(git -C "${ZEPHYR_BASE:-.}" log -1 --format='%h %s' 2>/dev/null)
Driver TDM support: $([ $HAS_TDM = 1 ] && echo yes || echo no)"
	{
		echo "$hdr"; echo
		printf '%-16s %-9s %7s %7s  %s\n' image result passed failed note
		for ((i = 0; i < ${#ROW_IMG[@]}; i++)); do
			printf '%-16s %-9s %7s %7s  %s\n' "${ROW_IMG[$i]}" "${ROW_ST[$i]}" "${ROW_P[$i]}" "${ROW_F[$i]}" "${ROW_N[$i]}"
		done
	} | tee "$OUT/summary.txt"
	echo; echo "logs: $OUT/<image>/uart.log   summary: $OUT/summary.txt"
}

# --- one image --------------------------------------------------------------------------------
run_image() {
	local name=$1 bdir log hex rc
	image_spec "$name"
	bdir=$OUT/$name; log=$bdir/uart.log; hex=$bdir/build/zephyr/zephyr.hex
	mkdir -p "$bdir"
	if [ $BUILD = 1 ]; then
		echo "== $name: build"
		( cd "$ZEPHYR_BASE" && west build -p always -b "$TARGET" "$IMG_APP" -d "$bdir/build" -- \
			$SUITE_CMAKE_ARGS ${IMG_ARGS[@]+"${IMG_ARGS[@]}"} ) > "$bdir/build.log" 2>&1
		if [ $? -ne 0 ] || [ ! -f "$hex" ]; then
			echo "   build failed, see $bdir/build.log"; tail -5 "$bdir/build.log" | sed 's/^/   /'
			add_row "$name" BUILDFAIL - - "$bdir/build.log"; FAILED=1; return 1
		fi
	fi
	if [ $FLASH = 0 ]; then
		[ -f "$hex" ] && add_row "$name" BUILT - - "" || add_row "$name" "NOT BUILT" - - ""
		return 0
	fi
	echo "== $name: flash and run (up to ${IMG_TIMEOUT}s)"
	"$SUITE_DIR/flash_capture.sh" "$hex" "$log" "$IMG_TIMEOUT" "$IMG_END"
	rc=$?
	if [ $rc -eq 1 ]; then add_row "$name" "FLASHFAIL" - - "see output above"; FAILED=1; return 1; fi
	evaluate "$name" "$IMG_KIND" "$log"
	[ "$R_STATUS" = PASS ] || FAILED=1
	add_row "$name" "$R_STATUS" "$R_PASS" "$R_FAIL" "$R_NOTE"
	echo "   $R_STATUS ${R_NOTE}"
	[ "$IMG_KIND" = probe ] && [ "$R_STATUS" != PASS ] && return 3
	return 0
}

confirm() { # confirm <text>
	[ $YES = 1 ] && return 0
	if [ ! -t 0 ]; then return 0; fi
	echo; echo "$1"
	read -r -p "Press Enter when done, q to quit: " ans
	[ "$ans" = q ] && { echo "aborted"; exit 130; }
	return 0
}

# --- main -------------------------------------------------------------------------------------
if [ -n "$FROM_LOG" ]; then
	image_spec "$FROM_IMG"
	evaluate "$FROM_IMG" "$IMG_KIND" "$FROM_LOG"
	add_row "$FROM_IMG" "$R_STATUS" "$R_PASS" "$R_FAIL" "$R_NOTE"
	print_summary
	[ "$R_STATUS" = PASS ] && exit 0 || exit 1
fi

PHASES=$PHASE; [ $PHASE = all ] && PHASES="wired unwired spdifrx"
if [ $PHASE = all ] && [ "$WIRING" = full3 ] && [ $YES = 0 ] && [ ! -t 0 ] && [ $LIST = 0 ] && [ $FLASH = 1 ]; then
	die "--phase all changes the jumpers in between and needs a terminal: run --phase wired and --phase unwired separately, or pass --yes if the wiring is handled by other means"
fi

for ph in $PHASES; do
	run_list=""
	for i in $(phase_images $ph); do
		case $i in probe-*) continue;; esac
		selected "$i" || continue
		r=$(skip_reason "$i")
		if [ -n "$r" ]; then add_row "$i" SKIP - - "$r"; continue; fi
		run_list="$run_list $i"
	done
	[ -z "$run_list" ] && continue
	# the wiring probe goes first: it stops the phase when the jumpers do not match
	if [ "$WIRING" = full3 ] && [ $ph != spdifrx ] && [ -z "$(skip_reason probe-$ph)" ]; then
		run_list="probe-$ph$run_list"
	fi
	if [ $LIST = 1 ]; then echo "phase $ph: $run_list"; continue; fi
	if [ "$WIRING" = full3 ]; then
		if [ $ph = wired ]; then
			confirm "Fit the loopback jumpers: ${JUMPERS_WIRED:-the loopback wires of the board}."
		elif [ $ph = spdifrx ]; then
			confirm "Fit the S/PDIF jumper: ${JUMPERS_SPDIFRX:-the SAI1_A SD pin to the SPDIFRX input}."
		else
			confirm "Remove the SCK and FS jumpers: ${JUMPERS_UNWIRED:-the clock wires between SAI A and SAI B}. SAI B becomes a clock controller in these images."
		fi
	elif [ -n "${JUMPERS_WIRED:-}" ]; then
		confirm "Fit the loopback jumper: $JUMPERS_WIRED."
	fi
	for i in $run_list; do
		run_image "$i"
		if [ $? -eq 3 ]; then
			WIRING_BAD=1
			echo "   wiring does not match the $ph phase: not running the rest of it"
			break
		fi
	done
done

if [ $LIST = 1 ]; then
	for ((k = 0; k < ${#ROW_IMG[@]}; k++)); do echo "skip ${ROW_IMG[$k]}: ${ROW_N[$k]}"; done
	exit 0
fi

print_summary
[ $WIRING_BAD = 1 ] && exit 3
[ $FAILED = 1 ] && exit 1
exit 0
