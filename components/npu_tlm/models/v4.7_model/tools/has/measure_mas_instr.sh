#!/bin/bash
# Measure, one instruction at a time, the instructions whose cycle count depends on the MAS knobs of the vector unit
# (ELEM_WISE, LAYERNORM, and a few FUSED_ATTN), under three configurations:
#   ref  HAS_SPEC=v47ref (the configuration before the MAS: 4 / 4 / 16 KB banks, 32 rows per group, 257-entry tables)
#   def  no knob (default configuration, Knobs::spec_default())
#   mas  HAS_SPEC=mas10 (scratchpad preset mas128k, rows per group limited by it, LUT depth 6, LUT tables 256 / 512)
# Every run starts from the golden DRAM image (--first I --count 1), so the runs are independent and the difference
# between two configurations of the same instruction is the cycle difference of that instruction.
# usage (repository root, Linux machine):
#   bash tools/has/measure_mas_instr.sh <binary> <program dir> <out csv> [max FUSED_ATTN instructions, default 2]
set -u
BIN=${1:?tb_has_npu_top binary}; PROG=${2:?program dir}; CSV=${3:?output csv}; NATTN=${4:-2}
export LD_LIBRARY_PATH=/usr/lib/x86_64-linux-gnu:${LD_LIBRARY_PATH:-}
LOGD=$(dirname "$CSV")/logs_$(basename "$PROG"); mkdir -p "$LOGD"
echo "program,index,kind,config,pass,sim_cycles,rce_groups,sbank_short" > "$CSV"
nattn=0
grep "^# \[" "$PROG/mmio.txt" | while read -r line; do
  idx=$(echo "$line" | sed 's/^# \[\([0-9]*\)\].*/\1/')
  case "$line" in
    *ELEM_WISE*) kind=$(echo "$line" | grep -o "ELEM_WISE [A-Z_]*" | tr ' ' '_');;
    *LAYERNORM*) kind=LAYERNORM;;
    *FUSED_ATTN*) nattn=$((nattn+1)); [ "$nattn" -gt "$NATTN" ] && continue; kind=FUSED_ATTN;;
    *) continue;;
  esac
  for cfg in ref def mas; do
    log="$LOGD/i${idx}_$cfg.log"
    case $cfg in
      ref) HAS_SPEC=v47ref "$BIN" "$PROG" --first "$idx" --count 1 > "$log" 2>&1;;
      def) "$BIN" "$PROG" --first "$idx" --count 1 > "$log" 2>&1;;
      mas) HAS_SPEC=mas10 "$BIN" "$PROG" --first "$idx" --count 1 > "$log" 2>&1;;
    esac
    pass=0; grep -q "RESULT: PASS" "$log" && pass=1
    cyc=$(grep -o "sim cycles [0-9]*" "$log" | tail -1 | awk '{print $3}')
    grp=$(grep -o "RCE row groups [0-9]*" "$log" | awk '{print $4}')
    short=$(grep -o "scalar bank too small [0-9]*" "$log" | awk '{print $5}')
    echo "$(basename "$PROG"),$idx,$kind,$cfg,$pass,${cyc:-},${grp:-},${short:-}" >> "$CSV"
  done
done
echo "done: $(($(wc -l < "$CSV") - 1)) runs in $CSV"
