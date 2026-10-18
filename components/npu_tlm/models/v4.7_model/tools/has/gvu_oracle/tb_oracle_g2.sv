// tb_oracle_g2.sv -- replays, cycle by cycle, the file written by tb_rtl_ref_gvu_g2dump on the RTL of the register file
// (vrf.sv) and counts the cycles whose RTL outputs differ from the outputs of the ported C++ block stored in the file.
// The top prints "ORACLE vrf cycles <n> mismatches <m>". The LUT unit and the Scratchpad are replayed by tb_oracle_b.sv.
// Built with Verilator 5 (--binary --timing). Plusarg +dir=<vector directory>.
`timescale 1ns/1ps

// vrf.sv with the default parameters: the nine write ports are applied before the edge, the nine read ports are compared
// after it.
module tb_oracle_vrf;
  string dir;
  integer fd, n, rc, bad, ok_scan;
  logic clk = 0;
  always #5 clk = ~clk;
  logic [1:0] w8; logic [1:0][4:0] wa8, ra8; logic [1:0][255:0] wd8, rd8;
  logic [1:0] w16; logic [1:0][3:0] wa16, ra16; logic [1:0][511:0] wd16, rd16;
  logic [2:0] w32; logic [2:0][2:0] wa32, ra32; logic [2:0][1023:0] wd32, rd32;
  logic [1:0] w64; logic [1:0][1:0] wa64, ra64; logic [1:0][2047:0] wd64, rd64;
  vrf u (.i_clk(clk), .i_int8_wen(w8), .i_int8_waddr(wa8), .i_int8_wdata(wd8), .i_int8_raddr(ra8), .o_int8_rdata(rd8),
    .i_int16_wen(w16), .i_int16_waddr(wa16), .i_int16_wdata(wd16), .i_int16_raddr(ra16), .o_int16_rdata(rd16),
    .i_int32_wen(w32), .i_int32_waddr(wa32), .i_int32_wdata(wd32), .i_int32_raddr(ra32), .o_int32_rdata(rd32),
    .i_int64_wen(w64), .i_int64_waddr(wa64), .i_int64_wdata(wd64), .i_int64_raddr(ra64), .o_int64_rdata(rd64));
  logic [31:0] we [0:8], wa [0:8], ra [0:8]; logic [2047:0] wd [0:8], er [0:8];
  initial begin
    if (!$value$plusargs("dir=%s", dir)) dir = ".";
    w8 = '0; w16 = '0; w32 = '0; w64 = '0;
    fd = $fopen({dir, "/vrf.txt"}, "r"); n = 0; bad = 0;
    while (fd != 0 && !$feof(fd)) begin
      ok_scan = 1;
      for (int k = 0; k < 9; k++) begin rc = $fscanf(fd, "%h %h %h", we[k], wa[k], wd[k]); if (rc != 3) ok_scan = 0; end
      for (int k = 0; k < 9; k++) begin rc = $fscanf(fd, "%h", ra[k]); if (rc != 1) ok_scan = 0; end
      for (int k = 0; k < 9; k++) begin rc = $fscanf(fd, "%h", er[k]); if (rc != 1) ok_scan = 0; end
      rc = $fscanf(fd, "\n");
      if (!ok_scan) break;
      @(negedge clk);
      for (int j = 0; j < 2; j++) begin w8[j] = we[j][0]; wa8[j] = wa[j][4:0]; wd8[j] = wd[j][255:0]; end
      for (int j = 0; j < 2; j++) begin w16[j] = we[2+j][0]; wa16[j] = wa[2+j][3:0]; wd16[j] = wd[2+j][511:0]; end
      for (int j = 0; j < 3; j++) begin w32[j] = we[4+j][0]; wa32[j] = wa[4+j][2:0]; wd32[j] = wd[4+j][1023:0]; end
      for (int j = 0; j < 2; j++) begin w64[j] = we[7+j][0]; wa64[j] = wa[7+j][1:0]; wd64[j] = wd[7+j]; end
      @(posedge clk); #1;
      w8 = '0; w16 = '0; w32 = '0; w64 = '0;
      for (int j = 0; j < 2; j++) ra8[j] = ra[j][4:0];
      for (int j = 0; j < 2; j++) ra16[j] = ra[2+j][3:0];
      for (int j = 0; j < 3; j++) ra32[j] = ra[4+j][2:0];
      for (int j = 0; j < 2; j++) ra64[j] = ra[7+j][1:0];
      #1; n++;
      if (rd8[0] !== er[0][255:0] || rd8[1] !== er[1][255:0] || rd16[0] !== er[2][511:0] || rd16[1] !== er[3][511:0] ||
          rd32[0] !== er[4][1023:0] || rd32[1] !== er[5][1023:0] || rd32[2] !== er[6][1023:0] || rd64[0] !== er[7] || rd64[1] !== er[8]) bad++;
    end
    $display("ORACLE vrf cycles %0d mismatches %0d", n, bad);
    $finish;
  end
endmodule
