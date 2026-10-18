#!/bin/bash
# RTL oracle of the vector-unit functional units: builds tb_oracle_fu.sv against the RTL with Verilator 5 and replays the
# vectors written by tb_rtl_ref_gvu_fu --dump. Verilator 5 is required (run with 5.052); iverilog 11 rejects the RTL at the
# first "parameter int unsigned" (probe_iverilog.sh). tb_oracle_mulb checks the modes 40x32 and 32x8 of gvu_vmul48x40; the
# four modes through their own interfaces are checked by run_verilator_b.sh (tb_b_mulb).
# usage: bash run_verilator.sh <rtl gvu dir> <vector dir> <work dir>
#   <rtl gvu dir>  copy of neo_core/gvu (read-only source of the hardware team)
#   <vector dir>   output of: tb_rtl_ref_gvu_fu --dump <vector dir>
set -u
R=${1:?rtl gvu dir}; V=${2:?vector dir}; W=${3:?work dir}
HERE=$(cd "$(dirname "$0")" && pwd)
command -v verilator > /dev/null || { echo "verilator not found (version 5 or later is needed for --binary --timing)"; exit 2; }
verilator --version
mkdir -p "$W"
F=$R/FUs
SRC="$R/gvu_pkg.sv $R/pipeline_reg.sv $F/clz.sv $F/gvu_full_adder.sv $F/gvu_adder32_lane.sv $F/gvu_adder64_lane.sv $F/gvu_vcmp32.sv
     $F/gvu_vector_clamp.sv $F/gvu_vector_shift.sv $F/gvu_vroundshift.sv $F/gvu_vroundshift64.sv $F/gvu_vroundshift72.sv
     $F/gvu_vmul32x32.sv $F/gvu_vmul48x40.sv"
rc=0
for top in ${TOPS:-tb_oracle_comb tb_oracle_roundshift tb_oracle_mula tb_oracle_mulb}; do
  nice -n 19 verilator --binary --timing -sv -Wno-fatal -Wno-ENUMVALUE --top-module $top --Mdir "$W/$top" $SRC "$HERE/tb_oracle_fu.sv" > "$W/$top.build.log" 2>&1 \
    || { echo "BUILD FAIL $top (see $W/$top.build.log)"; rc=1; continue; }
  "$W/$top/V$top" +dir="$V" 2>&1 | tee "$W/$top.log" | grep -E "ORACLE|KNOWN|DETAIL|LATENCY|Error|FATAL"
done
grep -h "ORACLE" "$W"/*.log 2>/dev/null | grep -v "mismatches 0" && rc=1
exit $rc
