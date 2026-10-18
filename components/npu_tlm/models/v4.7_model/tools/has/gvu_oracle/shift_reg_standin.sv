// shift_reg_standin.sv -- stand-in for the module shift_reg that gvu_pipe_controller.sv instantiates and the RTL delivery
// does not contain. Written from the way it is instantiated (parameters dtype and Depth, ports clk_i, rst_ni, d_i, d_o):
// Depth registers in series, cleared by the asynchronous active-low reset; Depth = 0 is a wire. This is the behaviour of
// the module of the same name in the common_cells library; whether the hardware team uses that one is an open question.
module shift_reg #(
  parameter type dtype = logic,
  parameter int unsigned Depth = 1
) (
  input  logic clk_i,
  input  logic rst_ni,
  input  dtype d_i,
  output dtype d_o
);
  if (Depth == 0) begin : gen_wire
    assign d_o = d_i;
  end else begin : gen_regs
    dtype [Depth-1:0] q;
    always_ff @(posedge clk_i or negedge rst_ni) begin
      if (!rst_ni) q <= '0;
      else begin
        q[0] <= d_i;
        for (int unsigned k = 1; k < Depth; k++) q[k] <= q[k-1];
      end
    end
    assign d_o = q[Depth-1];
  end
endmodule
