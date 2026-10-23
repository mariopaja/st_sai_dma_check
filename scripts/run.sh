#!/bin/bash
# Copyright (c) 2026 Mario Paja
# SPDX-License-Identifier: Apache-2.0
# run.sh <dir> <image-name> <max-seconds> <probe-sn>
# Flashes <dir>/<image>.hex on the probe with serial <probe-sn> and captures its VCP until RESULT.
B=$1; img=$2; max=$3; SN=$4
TTY=$(ls /dev/serial/by-id/*${SN}*-if0[0-9] 2>/dev/null | head -1)
[ -z "$SN" ] || [ -z "$TTY" ] && { echo "probe $SN / tty not found"; exit 1; }
CONN="port=SWD sn=$SN"
stty -F $TTY 115200 raw -echo
rm -f $B/uart_$img.raw
cat $TTY > $B/uart_$img.raw & CPID=$!
sleep 1
if [ "${RAM_LOAD:-0}" = 1 ]; then
	# STM32N6 development boot (BOOT1=1): load the RAM image over SWD and start it at its
	# vector table (vector_base, default 0x34180400): MSP = word 0, PC = word 1
	VT=${VECTOR_BASE:-0x34180400}
	read MSP PC < <(python3 - $B/$img.hex $VT <<'PY'
import sys
base = int(sys.argv[2], 16); mem = {}; ext = 0
for line in open(sys.argv[1]):
	n, a, t = int(line[1:3], 16), int(line[3:7], 16), int(line[7:9], 16)
	d = bytes.fromhex(line[9:9 + 2 * n])
	if t == 4:
		ext = int.from_bytes(d, 'big') << 16
	elif t == 0:
		for i, b in enumerate(d):
			mem[ext + a + i] = b
w = lambda x: int.from_bytes(bytes(mem[x + i] for i in range(4)), 'little')
print("0x%08X 0x%08X" % (w(base), w(base + 4) & ~1))
PY
)
	STM32_Programmer_CLI -c $CONN mode=UR reset=HWrst ap=1 -halt -w $B/$img.hex -v -coreReg MSP=$MSP PC=$PC xPSR=0x01000000 -run 2>&1 | sed 's/\x1b\[[0-9;]*m//g' | grep -E "verified|Error" | head -1
	echo "started from RAM: MSP=$MSP PC=$PC"
else
STM32_Programmer_CLI -c $CONN mode=UR -w $B/$img.hex -v 2>&1 | sed 's/\x1b\[[0-9;]*m//g' | grep -E "verified|Error" | head -1
# STM32L4/G4: PEMPTY set after programming makes the next reset boot the ROM loader; writing 1 toggles it
if [ "${PEMPTY_FIX:-0}" = 1 ]; then
	sr=$(STM32_Programmer_CLI -c $CONN mode=HOTPLUG -r32 0x40022010 4 2>&1 | sed 's/\x1b\[[0-9;]*m//g' | awk '/^0x40022010 :/ {print $3}')
	if [ -n "$sr" ] && [ $(( 0x$sr & 0x20000 )) -ne 0 ]; then
		STM32_Programmer_CLI -c $CONN mode=HOTPLUG -w32 0x40022010 0x00020000 >/dev/null 2>&1
		echo "PEMPTY cleared (SR was 0x$sr)"
	fi
fi
STM32_Programmer_CLI -c $CONN mode=HOTPLUG -rst >/dev/null 2>&1
fi
for i in $(seq 1 $max); do grep -q RESULT $B/uart_$img.raw && break; sleep 1; done
sleep 1; kill $CPID 2>/dev/null; wait $CPID 2>/dev/null
sed 's/\x1b\[[0-9;]*m//g' $B/uart_$img.raw > $B/uart_$img.log
echo "== $img [$SN]: $(grep RESULT $B/uart_$img.log || echo 'no RESULT (timeout)')"
