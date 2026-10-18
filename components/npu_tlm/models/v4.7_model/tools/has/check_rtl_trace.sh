#!/bin/bash
# Checks, without a Verilog simulator, that the blocks ported from the RTL of the vector unit (has/rtl_ref_gvu_*.h) still
# produce the outputs that the RTL simulation produced: regenerates the vector files and compares their digest with
# tools/has/gvu_oracle/rtl_trace_digest.txt (stored by tools/has/gvu_oracle/run_all.sh after a replay on the RTL without a
# mismatch; the header of that file names the replay). Needs g++ only; about 2 minutes and 500 MB in the work directory.
# Run from the repository root:
#   bash tools/has/check_rtl_trace.sh [work dir, default /tmp/rtl_trace_check]
set -u
W=${1:-/tmp/rtl_trace_check}
D=tools/has/gvu_oracle/rtl_trace_digest.txt
[ -f "$D" ] || { echo "$D not found"; echo "RESULT: FAIL"; exit 1; }
mkdir -p "$W/bin" "$W/vec"
rm -f "$W"/vec/*.txt
for t in tb_rtl_ref_gvu_fu tb_rtl_ref_gvu_g2dump tb_rtl_ref_gvu_bdump tb_rtl_ref_gvu_trace; do
  g++ -std=c++17 -O2 -I. tools/has/$t.cpp -o "$W/bin/$t" 2> "$W/bin/$t.build.log" || { echo "BUILD FAIL $t (see $W/bin/$t.build.log)"; echo "RESULT: FAIL"; exit 1; }
done
"$W/bin/tb_rtl_ref_gvu_fu" --dump "$W/vec" > "$W/bin/fu.log" 2>&1 || { echo "tb_rtl_ref_gvu_fu failed (see $W/bin/fu.log)"; echo "RESULT: FAIL"; exit 1; }
"$W/bin/tb_rtl_ref_gvu_g2dump" "$W/vec" > /dev/null
"$W/bin/tb_rtl_ref_gvu_bdump" "$W/vec" > /dev/null
grep "^#" "$D"
"$W/bin/tb_rtl_ref_gvu_trace" check "$W/vec" "$D"
rc=$?
rm -f "$W"/vec/*.txt
exit $rc
