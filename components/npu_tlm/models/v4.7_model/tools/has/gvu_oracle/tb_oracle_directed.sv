// tb_oracle_directed.sv -- directed checks on the RTL for behaviours the ported blocks reproduce or flag:
//   tb_dir_vstream   vstream_reg_int32 in the 4 x v32int8 mode: does a register load with i_vint8_ready = 1 while
//                    i_vint32_ready = 0, and with the two swapped?
//   tb_dir_cmp       gvu_vcmp32: accumulate window with OUT_PIPE = 0 and 1; then an emitting beat while out_ready = 0
//                    (OUT_PIPE = 0: in_ready follows out_ready)
// Built with Verilator 5 (--binary --timing). Each top prints DIRECTED lines.
`timescale 1ns/1ps

module tb_dir_vstream;
  logic clk = 0, rstn = 0;
  always #5 clk = ~clk;
  logic [1:0] mode; logic v32, r32, ov32, or32; logic [1023:0] d32, od32;
  logic [1:0] v16, r16, ov16, or16; logic [1:0][511:0] d16, od16;
  logic [3:0] v8, r8, ov8, or8; logic [3:0][255:0] d8, od8;
  vstream_reg_int32 u (.i_clk(clk), .i_rstn(rstn), .i_cfg_mode(mode),
    .i_vint32_flush(1'b0), .i_vint32_valid(v32), .o_vint32_ready(or32), .i_vint32_data(d32), .o_vint32_valid(ov32), .i_vint32_ready(r32), .o_vint32_data(od32),
    .i_vint16_flush(2'b0), .i_vint16_valid(v16), .o_vint16_ready(or16), .i_vint16_data(d16), .o_vint16_valid(ov16), .i_vint16_ready(r16), .o_vint16_data(od16),
    .i_vint8_flush(4'b0), .i_vint8_valid(v8), .o_vint8_ready(or8), .i_vint8_data(d8), .o_vint8_valid(ov8), .i_vint8_ready(r8), .o_vint8_data(od8));
  initial begin
    mode = 2; v32 = 0; r32 = 0; d32 = '0; v16 = 0; r16 = 0; d16 = '0; v8 = 0; r8 = 0; d8 = '0;
    repeat (3) @(negedge clk); rstn = 1;
    // case A: v32int8 mode, int8 side valid and ready, int32 ready low
    @(negedge clk); v8 = 4'hF; r8 = 4'hF; r32 = 0; for (int i = 0; i < 4; i++) d8[i] = {32{8'(i + 1)}};
    #1; $display("DIRECTED vstream_int32 mode 2, vint8_ready 1, vint32_ready 0: o_vint8_ready %b before the edge", or8);
    @(negedge clk); v8 = 0;
    #1; $display("DIRECTED vstream_int32 mode 2, vint8_ready 1, vint32_ready 0: loaded o_vint8_valid %b o_vint32_valid %b", ov8, ov32);
    // case B: the same with int8 ready low and int32 ready high
    @(negedge clk); v8 = 4'hF; r8 = 4'h0; r32 = 1;
    @(negedge clk); v8 = 0;
    #1; $display("DIRECTED vstream_int32 mode 2, vint8_ready 0, vint32_ready 1: loaded o_vint8_valid %b o_vint32_valid %b data0 %h", ov8, ov32, od8[0][7:0]);
    $finish;
  end
endmodule

module tb_dir_cmp;
  import gvu_pkg::*;
  logic clk = 0, rst_n = 0;
  always #5 clk = ~clk;
  logic v, first, last, ordy, irdy1, ov1, irdy0, ov0; cmp_mode_e cm; logic [0:0][31:0] s0, s1, r1, r0;
  gvu_vcmp32 #(.OUT_PIPE(1'b1), .LANES(1)) u1 (.clk, .rst_n, .in_valid(v), .in_ready(irdy1), .in_cmp_mode(cm), .in_op(CMP_MAX), .in_elem_w(EW_32), .in_signed(1'b1),
    .in_src1_bcast(1'b0), .in_first(first), .in_last(last), .in_src0(s0), .in_src1(s1), .out_valid(ov1), .out_ready(ordy), .out_res(r1));
  gvu_vcmp32 #(.OUT_PIPE(1'b0), .LANES(1)) u0 (.clk, .rst_n, .in_valid(v), .in_ready(irdy0), .in_cmp_mode(cm), .in_op(CMP_MAX), .in_elem_w(EW_32), .in_signed(1'b1),
    .in_src1_bcast(1'b0), .in_first(first), .in_last(last), .in_src0(s0), .in_src1(s1), .out_valid(ov0), .out_ready(ordy), .out_res(r0));
  initial begin
    v = 0; first = 0; last = 0; ordy = 1; cm = CMP_ACCUMULATE; s0 = '0; s1 = '0;
    repeat (3) @(negedge clk); rst_n = 1;
    @(negedge clk); v = 1; first = 1; last = 0; s0[0] = 32'd5;
    @(negedge clk); first = 0; s0[0] = -32'sd7;
    @(negedge clk); last = 1; s0[0] = 32'd3;
    #1; $display("DIRECTED vcmp32 OUT_PIPE 0 accumulate max(5, -7, 3): out_valid %b result %0d on the last beat", ov0, $signed(r0[0]));
    @(negedge clk); v = 0; last = 0;
    #1; $display("DIRECTED vcmp32 OUT_PIPE 1 accumulate max(5, -7, 3): out_valid %b result %0d one cycle after the last beat", ov1, $signed(r1[0]));
    // emitting beat while the consumer is not ready
    @(negedge clk); cm = CMP_ELEMENTWISE; v = 1; ordy = 0; s0[0] = 32'd1; s1[0] = 32'd2;
    #1; $display("DIRECTED vcmp32 OUT_PIPE 0, emitting beat, out_ready 0: in_ready %b out_valid %b", irdy0, ov0);
    #1; $display("DIRECTED vcmp32 OUT_PIPE 1, emitting beat, out_ready 0: in_ready %b out_valid %b", irdy1, ov1);
    $finish;
  end
endmodule
