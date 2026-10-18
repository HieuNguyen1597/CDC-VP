// tb_oracle_top.sv -- replays the table-load sequence written by tb_rtl_ref_gvu_bdump (lutload_v1.txt, lutload.txt,
// lutload_mem.txt) on gvu_top.sv: the rows of vbank1 are placed in the memory by hierarchical assignment, the softmax
// opcode is started, and in every cycle up to the first softmax state the controller state, the table write strobe, the
// table select, the done flag, the write address and counter of the table memory and the vbank1 access of the load /
// store unit are compared with the ported sequence (has/rtl_ref_gvu_ctrl.h); at the end the four LUT tables.
// Prints "ORACLE lut_load cycles <n> mismatches <m>". shift_reg comes from shift_reg_standin.sv.
// Built with Verilator 5 (--binary --timing). Plusarg +dir=<vector directory>.
`timescale 1ns/1ps

module tb_t_lutload;
  import gvu_pkg::*;
  string dir;
  integer fd, rc, n, bad, bad_mem;
  logic clk = 0, rstn = 0, en = 0;
  always #5 clk = ~clk;
  gvu_top u (.i_clk(clk), .i_rstn(rstn), .i_pipeline_en(en), .i_gvu_op(GVU_SOFTMAX), .i_psum_valid(1'b0), .o_psum_ready(), .i_psum_data('0),
    .o_softmax_lut_init_done(), .o_softmax_pipe_done(), .i_num_tile(9'd1), .i_token_len(9'd8), .i_attn_z(8'sd0), .i_attn_scale(32'sd1), .i_attn_shift(8'd0),
    .i_sbank_buf_sel(1'b0), .i_vbank0_buf_sel(1'b0), .i_vbank1_buf_sel(1'b0), .i_vbank2_buf_sel(1'b0),
    .i_scratch_dma_en(1'b0), .i_scratch_dma_wen(1'b0), .i_scratch_dma_addr('0), .i_scratch_dma_wdata('0), .i_scratch_dma_wmask('0), .o_scratch_dma_rdata());
  logic [1023:0] row; logic [31:0] e [0:8], t0, t1, t2, t3;
  logic bad_now;
  initial begin
    if (!$value$plusargs("dir=%s", dir)) dir = ".";
    fd = $fopen({dir, "/lutload_v1.txt"}, "r");
    for (int k = 0; k < 32; k++) begin rc = $fscanf(fd, "%h\n", row); u.i_scratchpad_wrapper.i_vbank1_buf0.mem[k] = row; end
    $fclose(fd);
    repeat (4) @(negedge clk); rstn = 1; @(negedge clk); en = 1;
    fd = $fopen({dir, "/lutload.txt"}, "r"); n = 0; bad = 0;
    while (fd != 0 && !$feof(fd)) begin
      rc = $fscanf(fd, "%h %h %h %h %h %h %h %h %h\n", e[0], e[1], e[2], e[3], e[4], e[5], e[6], e[7], e[8]); if (rc != 9) break;
      #1; n++;
      bad_now = u.gvu_pipe_ctrl_state !== control_fsm_t'(e[0][4:0]);
      if (e[0] != 5) begin   // up to the first softmax state every load signal is compared
        if (u.pipe_ctrl_lut_init_wr !== e[1][0] || u.pipe_ctrl_lut_init_sel !== lut_type_e'(e[2][1:0]) || u.pipe_ctrl_lut_init_done !== e[3][0] ||
            u.i_gvu_vlut.i_lut_mem.lut_waddr_r !== e[4][8:0] || u.i_gvu_vlut.i_lut_mem.wr_cnt_r !== e[5][2:0] || u.vbank1_en_0 !== e[6][0] || u.vbank1_addr_0 !== e[7][VBANK1_AW-1:0] ||
            u.o_softmax_lut_init_done !== e[8][0]) bad_now = 1;
      end
      if (bad_now) begin
        bad++;
        if (bad <= 8) $display("FIRST lut_load cycle %0d: rtl state %0d wr %0d sel %0d done %0d addr %0d cnt %0d vbank1 en %0d addr %0d; file %0d %0d %0d %0d %0d %0d %0d %0d", n - 1, u.gvu_pipe_ctrl_state, u.pipe_ctrl_lut_init_wr,
          u.pipe_ctrl_lut_init_sel, u.pipe_ctrl_lut_init_done, u.i_gvu_vlut.i_lut_mem.lut_waddr_r, u.i_gvu_vlut.i_lut_mem.wr_cnt_r, u.vbank1_en_0, u.vbank1_addr_0, e[0], e[1][0], e[2][1:0], e[3][0], e[4][8:0], e[5][2:0], e[6][0], e[7][4:0]);
      end
      @(negedge clk);
    end
    $fclose(fd);
    fd = $fopen({dir, "/lutload_mem.txt"}, "r"); bad_mem = 0;
    for (int k = 0; k < 512; k++) begin
      rc = $fscanf(fd, "%h %h %h %h\n", t0, t1, t2, t3);
      if (k < 256 && (u.i_gvu_vlut.i_lut_mem.lut_act_mem[k] !== t0[7:0] || u.i_gvu_vlut.i_lut_mem.lut_exp_mem[k] !== t1[7:0] || u.i_gvu_vlut.i_lut_mem.lut_recip_mem[k] !== t2[15:0])) bad_mem++;
      if (u.i_gvu_vlut.i_lut_mem.lut_rsqrt_mem[k] !== t3[15:0]) bad_mem++;
    end
    $display("ORACLE lut_load cycles %0d mismatches %0d", n, bad + bad_mem);
    $display("DETAIL lut_load state_or_load_signals %0d table_entries %0d; cycles from the start to the first softmax state: %0d", bad, bad_mem, n - 1);
    $finish;
  end
endmodule
