#!/bin/bash
# Copyright (c) 2026 Mario Paja
# SPDX-License-Identifier: Apache-2.0
#
# flash_capture.sh <hex> <log> <max-seconds> <end-pattern>
#
# Flashes <hex> with STM32_Programmer_CLI and captures the board's virtual COM port into <log>
# until a line matching the extended regex <end-pattern> appears (or <max-seconds> pass).
# Works on Linux and macOS (bash 3.2). Needs STM32_Programmer_CLI and perl.
#
# Environment:
#   PROBE_SN    ST-Link serial number (needed when several probes are connected)
#   SAI_TTY     console device, overrides the automatic choice
#   PEMPTY_FIX  1 on STM32L4/G4: clear FLASH_SR.PEMPTY after programming, otherwise the next
#               reset boots the ROM loader (same sequence as scripts/run.sh)
#
# Exit status: 0 end pattern seen, 1 setup or flash error, 2 timeout without the end pattern.

HEX=$1; LOG=$2; MAX=$3; END=$4
SN=${PROBE_SN:-}
[ -f "$HEX" ] || { echo "flash_capture: $HEX not found"; exit 1; }
command -v STM32_Programmer_CLI >/dev/null || { echo "flash_capture: STM32_Programmer_CLI not in PATH"; exit 1; }
command -v perl >/dev/null || { echo "flash_capture: perl not found"; exit 1; }

# --- console device -------------------------------------------------------------------------
find_tty() {
	if [ -n "${SAI_TTY:-}" ]; then echo "$SAI_TTY"; return; fi
	local c
	if [ "$(uname)" = Darwin ]; then
		c=$(ls /dev/cu.usbmodem* 2>/dev/null)
	elif [ -n "$SN" ]; then
		c=$(ls /dev/serial/by-id/*"$SN"*-if0[0-9] 2>/dev/null | head -1)
	else
		c=$(ls /dev/serial/by-id/*STLink*-if0[0-9] /dev/serial/by-id/*ST-LINK*-if0[0-9] 2>/dev/null)
	fi
	if [ -z "$c" ]; then echo ""; return; fi
	if [ "$(echo "$c" | wc -l)" -ne 1 ]; then
		echo "AMBIGUOUS:$(echo $c)"
		return
	fi
	echo "$c"
}
TTY=$(find_tty)
case "$TTY" in
"") echo "flash_capture: no console device found (set SAI_TTY, or PROBE_SN on Linux)"; exit 1;;
AMBIGUOUS:*) echo "flash_capture: several console devices (${TTY#AMBIGUOUS:}): set SAI_TTY"; exit 1;;
esac

CONN="port=SWD"
[ -n "$SN" ] && CONN="$CONN sn=$SN"

if [ "$(uname)" = Darwin ]; then stty -f "$TTY" 115200 raw -echo; else stty -F "$TTY" 115200 raw -echo; fi
RAW="$LOG.raw"
: > "$RAW"
cat "$TTY" > "$RAW" &
CPID=$!
sleep 1

# --- flash ------------------------------------------------------------------------------------
clean() { perl -pe 's/\e\[[0-9;]*m//g; s/\r//g'; }
if [ "${PEMPTY_FIX:-0}" = 1 ]; then
	OUTF=$(STM32_Programmer_CLI -c $CONN mode=UR -w "$HEX" -v 2>&1 | clean | grep -E "verified|Error" | head -1)
	sr=$(STM32_Programmer_CLI -c $CONN mode=HOTPLUG -r32 0x40022010 4 2>&1 | clean | awk '/^0x40022010 :/ {print $3}')
	if [ -n "$sr" ] && [ $(( 0x$sr & 0x20000 )) -ne 0 ]; then
		STM32_Programmer_CLI -c $CONN mode=HOTPLUG -w32 0x40022010 0x00020000 >/dev/null 2>&1
		echo "PEMPTY cleared (SR was 0x$sr)"
	fi
	STM32_Programmer_CLI -c $CONN mode=HOTPLUG -rst >/dev/null 2>&1
else
	OUTF=$(STM32_Programmer_CLI --connect $CONN reset=HWrst --download "$HEX" -rst 2>&1 | clean | grep -E "verified|Error|Download verified" | head -1)
fi
case "$OUTF" in
*Error*) kill $CPID 2>/dev/null; wait $CPID 2>/dev/null; echo "flash_capture: flash failed: $OUTF"; exit 1;;
esac

# --- capture ----------------------------------------------------------------------------------
seen=0
for i in $(seq 1 "$MAX"); do
	if grep -qE "$END" "$RAW" 2>/dev/null; then seen=1; break; fi
	sleep 1
done
sleep 1
kill $CPID 2>/dev/null; wait $CPID 2>/dev/null
clean < "$RAW" > "$LOG"
[ "$seen" = 1 ] && exit 0
exit 2
