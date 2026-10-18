// rtl_ref_gvu_ctrl.h -- table-load sequence of the vector unit, ported from the RTL (neo_core/gvu): the part of
// gvu_pipe_controller.sv that loads the LUT tables before a softmax (states IDLE, LUT_RECIP_INIT, LUT_EXP_INIT), together
// with the blocks gvu_top.sv connects to it: the load / store unit (has/rtl_ref_gvu_vlsu.h), the Scratchpad
// (has/rtl_ref_gvu_scratchpad.h) and the table memory of the LUT unit (has/rtl_ref_gvu_lut.h) with INIT_DW = 1024.
// Pure C++17, no SystemC; nothing here is used by the instruction path. The softmax stages of the controller are not
// ported: in the RTL delivered their data path is not connected (the state SOFTMAX_ST2 is never left).
// Sequence as the RTL is written (cycle numbers measured on the RTL simulation, cycle 0 = first cycle with i_pipeline_en):
//   - the tables are read from vbank1 port 0 (rows of 1024 bits): reciprocal table rows 0..3, exponential table rows 4..5;
//   - the read request stays high from LUT_RECIP_INIT into LUT_EXP_INIT; in the cycle before a state change the
//     controller raises the address reset of the load / store unit, which loads the start address presented in that cycle;
//   - a table write follows its read by two clocks (the read latency of the Scratchpad): o_lut_init_wr is the read
//     request delayed by a two-stage shift register and gated by the load states;
//   - the state changes one clock after the done flag of the table memory (write counter == number of writes - 1).
//   - exponential table: one write carries 128 bytes; the two writes fill the lower and the upper half of the table.
#ifndef RTL_REF_GVU_CTRL_H
#define RTL_REF_GVU_CTRL_H

#include <cstdint>
#include "has/rtl_ref_gvu_lut.h"
#include "has/rtl_ref_gvu_scratchpad.h"
#include "has/rtl_ref_gvu_vlsu.h"

namespace rtl_ref_gvu
{
    // control_fsm_t of gvu_pkg.sv
    enum CtrlState : int { ST_IDLE = 0, ST_LUT_ACT_INIT = 1, ST_LUT_EXP_INIT = 2, ST_LUT_RECIP_INIT = 3, ST_LUT_RSQRT_INIT = 4, ST_SOFTMAX_ST1 = 5, ST_SOFTMAX_ST2 = 6, ST_SOFTMAX_ST3 = 7 };

    class LutInitSequence
    {
    public:
        static constexpr int kInitDw = 1024;                              // LUT_INIT_DW = 32 * NUM_LANE
        struct Out
        {
            CtrlState state;                                              // o_gvu_pipe_ctrl_state
            bool o_lut_init_wr; LutType o_lut_init_sel; bool i_lut_init_done;
            unsigned lut_waddr_r, wr_cnt_r;                               // registers of gvu_lut_mem
            bool o_vbank1_en_0; unsigned o_vbank1_addr_0;                 // load / store unit outputs
            bool o_softmax_lut_init_done;
        };
        LutInitSequence() : mem(params()), scratchpad(), vlsu() {}
        LutMem mem;
        GvuScratchpad scratchpad;
        GvuVlsu vlsu;
        bool done() const { return state_ == ST_SOFTMAX_ST1; }

        // Outputs of the current cycle (i_gvu_op = GVU_SOFTMAX).
        Out eval() const { Wires w; return comb(w); }
        // One clock edge.
        void tick(bool i_pipeline_en = true)
        {
            Wires w;
            const Out o = comb(w);
            // next state, fsm_nxt_comb
            CtrlState nxt = state_;
            bool recip_nxt = is_lut_recip_init_, exp_nxt = is_lut_exp_init_;
            if (state_ == ST_IDLE) nxt = !is_lut_recip_init_ ? ST_LUT_RECIP_INIT : (!is_lut_exp_init_ ? ST_LUT_EXP_INIT : ST_SOFTMAX_ST1);
            else if (state_ == ST_LUT_RECIP_INIT) { if (o.i_lut_init_done) { recip_nxt = true; nxt = !is_lut_exp_init_ ? ST_LUT_EXP_INIT : ST_SOFTMAX_ST1; } }
            else if (state_ == ST_LUT_EXP_INIT) { if (o.i_lut_init_done) { exp_nxt = true; nxt = ST_SOFTMAX_ST1; } }
            mem.tick(true, o.o_lut_init_wr, o.o_lut_init_sel, w.rdata.data());
            scratchpad.tick(w.sp_in);
            w.vlsu_in.i_agu_addr_reset = nxt != state_;                    // o_agu_addr_reset = pipe_ctrl_fsm_nxt != pipe_ctrl_fsm
            vlsu.tick(w.vlsu_in);
            rd_shift_[1] = rd_shift_[0]; rd_shift_[0] = w.vlsu_in.i_vbank1_rd;
            if (i_pipeline_en) state_ = nxt;
            is_lut_recip_init_ = recip_nxt; is_lut_exp_init_ = exp_nxt;
        }

    private:
        struct Wires { GvuVlsu::In vlsu_in; GvuScratchpad::In sp_in; SpBytes rdata; };
        CtrlState state_{ST_IDLE};
        bool is_lut_recip_init_{false}, is_lut_exp_init_{false};
        bool rd_shift_[2] = {false, false};                               // i_vbank1_rd_shift, Depth 2
        static LutParams params() { LutParams p; p.INIT_DW = kInitDw; return p; }
        Out comb(Wires &w) const
        {
            const bool lut_init_phase = state_ == ST_LUT_RSQRT_INIT || state_ == ST_LUT_RECIP_INIT || state_ == ST_LUT_EXP_INIT || state_ == ST_LUT_ACT_INIT;
            // vlsu_ctrl_comb: LUT init from vbank1 port 0
            w.vlsu_in.i_vbank1_port_sel = 0;
            w.vlsu_in.i_vbank1_rd = state_ == ST_LUT_RECIP_INIT || state_ == ST_LUT_EXP_INIT;
            w.vlsu_in.i_vbank1_start_addr = state_ == ST_LUT_EXP_INIT ? static_cast<unsigned>(16 * 256 / kInitDw) : 0u;
            w.vlsu_in.i_vbank1_lane_wmask = 0xFFFFFFFFu; w.vlsu_in.i_vbank0_lane_wmask = 0xFFFFFFFFu; w.vlsu_in.i_vbank2_lane_wmask = 0xFFFFFFFFu;
            w.vlsu_in.i_sbank_wmask_0 = 0xFu; w.vlsu_in.i_sbank_wmask_1 = true;
            const GvuVlsu::Out v = vlsu.eval(w.vlsu_in);
            w.sp_in.i_vbank1_port_sel = v.o_vbank1_port_sel;
            w.sp_in.i_vbank1_en_0 = v.o_vbank1_en_0; w.sp_in.i_vbank1_wen_0 = v.o_vbank1_wen_0; w.sp_in.i_vbank1_addr_0 = v.o_vbank1_addr_0; w.sp_in.i_vbank1_wmask_0 = v.o_vbank1_wmask_0;
            w.sp_in.i_vbank1_wdata_0.assign(128, 0);
            w.rdata = scratchpad.out(w.sp_in).o_vbank1_rdata_0;              // pipe_ctrl_lut_rdata = vbank1_rdata_0
            Out o;
            o.state = state_;
            o.o_lut_init_wr = rd_shift_[1] && lut_init_phase;
            o.o_lut_init_sel = state_ == ST_LUT_RSQRT_INIT ? LUT_RSQRT : (state_ == ST_LUT_RECIP_INIT ? LUT_RECIP : (state_ == ST_LUT_EXP_INIT ? LUT_EXP : LUT_ACT));
            o.i_lut_init_done = mem.o_init_done(o.o_lut_init_sel);
            o.lut_waddr_r = mem.lut_waddr(); o.wr_cnt_r = mem.wr_cnt();
            o.o_vbank1_en_0 = v.o_vbank1_en_0; o.o_vbank1_addr_0 = v.o_vbank1_addr_0;
            o.o_softmax_lut_init_done = is_lut_recip_init_ && is_lut_exp_init_;
            return o;
        }
    };
} // namespace rtl_ref_gvu

#endif // RTL_REF_GVU_CTRL_H
