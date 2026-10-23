#!/bin/bash
# Copyright (c) 2026 Mario Paja
# SPDX-License-Identifier: Apache-2.0
# build.sh <dir> <board> <variants...>   variants: async sync syncfifo; builds new + original driver
B=$1; brd=$2; shift 2
bn=${brd//\//_}   # board target with "/" replaced, for file names
# Run from a west workspace with the Python environment active and ZEPHYR_BASE set.
Z=${ZEPHYR_BASE:?set ZEPHYR_BASE to the zephyr tree under test}
A=$(cd "$(dirname "$0")/.." && pwd)   # application root
cd "$Z"
sync_ovl() { [ -f $A/boards/${bn}_sync.overlay ] && echo $A/boards/${bn}_sync.overlay || echo $A/variants/sync.overlay; }
FILES="drivers/i2s/i2s_stm32_sai.c drivers/i2s/Kconfig.stm32"
# Baseline worktree: git -C $Z worktree add --detach $WT b70dbaf045e
WT=${WT:-$B/zephyr-base}
extra() { case $1 in async) echo "";; sync) echo "-DEXTRA_DTC_OVERLAY_FILE=$(sync_ovl)";; syncfifo) echo "-DEXTRA_DTC_OVERLAY_FILE=$(sync_ovl);$A/variants/rx_fifo_empty.overlay";; esac; }
for v in ${VARIANTS:-new old}; do
	for m in "$@"; do
		# 'old' builds the baseline driver from a separate worktree (WT), never touching this tree
		if [ $v = old ]; then ZB=$WT; MOD="-DZEPHYR_MODULES=$(cd $Z/.. && west list -f '{abspath}' | grep -E 'modules/hal/(stm32|cmsis)' | paste -sd ';')"; else ZB=$Z; MOD=""; fi
		ZEPHYR_BASE=$ZB west build -p -b $brd $A -d $B/$bn-$m-$v -- -DZEPHYR_BASE=$ZB ${MOD:+"$MOD"} $(extra $m) > $B/$bn-$m-$v.log 2>&1
		echo "$v $brd $m rc=$? warn=$(grep -cE 'warning|error:' $B/$bn-$m-$v.log)"
		cp $B/$bn-$m-$v/zephyr/zephyr.hex $B/$bn-$m-$v.hex
	done
done
echo "driver files in tree (unchanged by this script): $(git status --short $FILES | tr "\n" " ")"
NM=$(grep -m1 "CMAKE_NM:FILEPATH" $B/$bn-$1-new/CMakeCache.txt | cut -d= -f2)
for m in "$@"; do for v in ${VARIANTS:-new old}; do d=$B/$bn-$m-$v; echo "$(basename $d): HAL=$($NM $d/zephyr/zephyr.elf | grep -cE 'HAL_DMA_Start_IT|HAL_SAI_TxCpltCallback') new=$($NM $d/zephyr/zephyr.elf | grep -c sai_sub_dma_reload)"; done; done
