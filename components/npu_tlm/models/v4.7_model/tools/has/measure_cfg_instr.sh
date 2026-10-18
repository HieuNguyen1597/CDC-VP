#!/bin/bash
# Measure, one instruction at a time, the vector-unit instructions (ELEM_WISE, LAYERNORM, and a few FUSED_ATTN) under a
# list of configurations given on the command line. Every run starts from the golden DRAM image (--first I --count 1), so
# the runs are independent and the difference between two configurations of the same instruction is the cycle difference
# of that instruction.
# usage (repository root, Linux machine):
#   bash tools/has/measure_cfg_instr.sh <binary> <program dir> <out csv> <max FUSED_ATTN instructions> <name>[:VAR=value[,VAR=value...]] ...
# example: two configurations, the default one and the Scratchpad with the bank depths of the RTL
#   bash tools/has/measure_cfg_instr.sh tools/has/tb_has_npu_top fe_work/has/vit_full out.csv 2 def rtl:HAS_SP_PRESET=rtl
set -u
BIN=${1:?tb_has_npu_top binary}; PROG=${2:?program dir}; CSV=${3:?output csv}; NATTN=${4:?max FUSED_ATTN instructions}
shift 4
[ $# -ge 1 ] || { echo "no configuration given"; exit 2; }
export LD_LIBRARY_PATH=/usr/lib/x86_64-linux-gnu:${LD_LIBRARY_PATH:-}
LOGD=$(dirname "$CSV")/logs_$(basename "$CSV" .csv); mkdir -p "$LOGD"
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
  for cfg in "$@"; do
    name=${cfg%%:*}; vars=""
    case "$cfg" in *:*) vars=$(echo "${cfg#*:}" | tr ',' ' ');; esac
    log="$LOGD/i${idx}_$name.log"
    # shellcheck disable=SC2086
    env $vars "$BIN" "$PROG" --first "$idx" --count 1 > "$log" 2>&1
    pass=0; grep -q "RESULT: PASS" "$log" && pass=1
    cyc=$(grep -o "sim cycles [0-9]*" "$log" | tail -1 | awk '{print $3}')
    grp=$(grep -o "RCE row groups [0-9]*" "$log" | awk '{print $4}')
    short=$(grep -o "scalar bank too small [0-9]*" "$log" | awk '{print $5}')
    echo "$(basename "$PROG"),$idx,$kind,$name,$pass,${cyc:-},${grp:-},${short:-}" >> "$CSV"
  done
done
echo "done: $(($(wc -l < "$CSV") - 1)) runs in $CSV"
