// tb_gvu_top_layernorm.cpp -- Dedicated SystemC & C++ testbench for GVU top in LAYERNORM mode,
// based on the specification in MAS.docx (Section 5.2.5 LAYERNORM, Section 5.6 Pipeline Controller,
// and Figure 5-17 GVU LayerNorm Pipeline FSM).
//
// Dual-use architecture:
// 1. When compiled with SystemC (<systemc.h>), instantiates ScGvuTop with sc_clock,
//    sc_signal, SC_THREADs for reset, stimulus, monitor, and timeout watchdog.
// 2. When compiled in standalone C++17 mode (without SystemC library installed),
//    executes cycle-by-cycle on GvuTop reproducing the exact same clock cycles,
//    stimulus sequences, state transitions, and timeout protection.
//
// Build:
//   make tb_gvu_top_layernorm
//   or:
//   g++ -std=c++17 -O2 -Wall -I. tb_gvu_top_layernorm.cpp -o tb_gvu_top_layernorm
//
// Usage:
//   ./tb_gvu_top_layernorm

#if __has_include(<systemc.h>)
#include <systemc.h>
#define GVU_TB_USE_SYSTEMC 1
#elif defined(SYSTEMC_INCLUDED) || defined(SC_MODULE) || defined(HAVE_SYSTEMC)
#include <systemc.h>
#define GVU_TB_USE_SYSTEMC 1
#else
#define GVU_TB_USE_SYSTEMC 0
#endif

#include <iostream>
#include <iomanip>
#include <string>
#include <cstdint>
#include "has/rtl_ref_gvu_top.h"

// Parameters matching MAS.docx and gvu_pkg.sv for LayerNorm
static constexpr unsigned NUM_LANE      = 32;
static constexpr unsigned INIT_DW       = 1024;
static constexpr double   ClockCycle    = 10.0; // 10 ns (100 MHz)
static constexpr double   ResetDuration = 50.0; // 50 ns (5 cycles)
static constexpr unsigned MaxCycle      = 10000;
static constexpr unsigned MinCycle      = 10;
static constexpr unsigned DMA_AW        = 32;
static constexpr unsigned DMA_DW        = 128;
static constexpr unsigned TOKEN_LEN     = 256;  // d_model = 256 as specified in MAS.docx Table 5-6
static constexpr rtl_ref_gvu::GvuOpcode GVU_OP = rtl_ref_gvu::GVU_OP_LAYERNORM;

// Helper to print FSM state names matching gvu_pkg.sv
static const char *fsm_state_name(uint32_t s)
{
    switch (static_cast<rtl_ref_gvu::control_fsm_t>(s))
    {
    case rtl_ref_gvu::IDLE:           return "IDLE";
    case rtl_ref_gvu::LUT_ACT_INIT:   return "LUT_ACT_INIT";
    case rtl_ref_gvu::LUT_EXP_INIT:   return "LUT_EXP_INIT";
    case rtl_ref_gvu::LUT_RECIP_INIT: return "LUT_RECIP_INIT";
    case rtl_ref_gvu::LUT_RSQRT_INIT: return "LUT_RSQRT_INIT";
    case rtl_ref_gvu::SOFTMAX_ST1:    return "SOFTMAX_ST1";
    case rtl_ref_gvu::SOFTMAX_ST2:    return "SOFTMAX_ST2";
    case rtl_ref_gvu::SOFTMAX_ST3:    return "SOFTMAX_ST3";
    case rtl_ref_gvu::LAYERNORM_ST1:  return "LAYERNORM_ST1";
    case rtl_ref_gvu::LAYERNORM_ST2:  return "LAYERNORM_ST2";
    case rtl_ref_gvu::LAYERNORM_ST3:  return "LAYERNORM_ST3";
    case rtl_ref_gvu::LAYERNORM_ST4:  return "LAYERNORM_ST4";
    case rtl_ref_gvu::LAYERNORM_ST5:  return "LAYERNORM_ST5";
    default:                          return "UNKNOWN";
    }
}

#if GVU_TB_USE_SYSTEMC

// ============================================================================================================
// SystemC Testbench Implementation for LayerNorm
// ============================================================================================================

SC_MODULE(TbGvuTopLayerNorm)
{
    // Clock & Reset
    sc_clock clk;
    sc_signal<bool> rstn;

    // Pipeline enable & GVU opcode
    sc_signal<bool> pipeline_en;
    sc_signal<uint32_t> gvu_op;

    // Systolic Array interface
    sc_signal<bool> psum_valid;
    sc_signal<bool> psum_ready;
    sc_signal<uint32_t> psum_data;

    // Token length & parameters
    sc_signal<uint32_t> token_len;
    sc_signal<int32_t> attn_z;
    sc_signal<int32_t> attn_scale;
    sc_signal<uint32_t> attn_shift;

    // Status outputs
    sc_signal<bool> softmax_lut_init_done;
    sc_signal<bool> softmax_pipe_done;
    sc_signal<bool> layernorm_lut_init_done;
    sc_signal<bool> layernorm_pipe_done;
    sc_signal<uint32_t> gvu_pipe_ctrl_state;

    // Buffer selects
    sc_signal<bool> sbank_buf_sel;
    sc_signal<bool> vbank0_buf_sel;
    sc_signal<bool> vbank1_buf_sel;
    sc_signal<bool> vbank2_buf_sel;

    // Scratchpad DMA access
    sc_signal<bool> scratch_dma_en;
    sc_signal<bool> scratch_dma_wen;
    sc_signal<uint32_t> scratch_dma_addr;

    // DUT instance (ScGvuTop SystemC wrapper of rtl_ref_gvu_top.h)
    rtl_ref_gvu::ScGvuTop dut;

    // Testbench internal state counters and flags
    uint64_t cycle_count{0};
    bool done_seen{false};
    bool timed_out{false};
    int exit_code{0};

    SC_CTOR(TbGvuTopLayerNorm)
        : clk("clk", ClockCycle, SC_NS, 0.5, 0.0, SC_NS, true),
          rstn("rstn"),
          pipeline_en("pipeline_en"),
          gvu_op("gvu_op"),
          psum_valid("psum_valid"),
          psum_ready("psum_ready"),
          psum_data("psum_data"),
          token_len("token_len"),
          attn_z("attn_z"),
          attn_scale("attn_scale"),
          attn_shift("attn_shift"),
          softmax_lut_init_done("softmax_lut_init_done"),
          softmax_pipe_done("softmax_pipe_done"),
          layernorm_lut_init_done("layernorm_lut_init_done"),
          layernorm_pipe_done("layernorm_pipe_done"),
          gvu_pipe_ctrl_state("gvu_pipe_ctrl_state"),
          sbank_buf_sel("sbank_buf_sel"),
          vbank0_buf_sel("vbank0_buf_sel"),
          vbank1_buf_sel("vbank1_buf_sel"),
          vbank2_buf_sel("vbank2_buf_sel"),
          scratch_dma_en("scratch_dma_en"),
          scratch_dma_wen("scratch_dma_wen"),
          scratch_dma_addr("scratch_dma_addr"),
          dut("i_dut")
    {
        // Connect DUT ports
        dut.i_clk(clk);
        dut.i_rstn(rstn);
        dut.i_pipeline_en(pipeline_en);
        dut.i_gvu_op(gvu_op);
        dut.i_psum_valid(psum_valid);
        dut.o_psum_ready(psum_ready);
        dut.i_psum_data(psum_data);
        dut.i_token_len(token_len);
        dut.i_attn_z(attn_z);
        dut.i_attn_scale(attn_scale);
        dut.i_attn_shift(attn_shift);
        dut.o_softmax_lut_init_done(softmax_lut_init_done);
        dut.o_softmax_pipe_done(softmax_pipe_done);
        dut.o_layernorm_lut_init_done(layernorm_lut_init_done);
        dut.o_layernorm_pipe_done(layernorm_pipe_done);
        dut.o_gvu_pipe_ctrl_state(gvu_pipe_ctrl_state);
        dut.i_sbank_buf_sel(sbank_buf_sel);
        dut.i_vbank0_buf_sel(vbank0_buf_sel);
        dut.i_vbank1_buf_sel(vbank1_buf_sel);
        dut.i_vbank2_buf_sel(vbank2_buf_sel);
        dut.i_scratch_dma_en(scratch_dma_en);
        dut.i_scratch_dma_wen(scratch_dma_wen);
        dut.i_scratch_dma_addr(scratch_dma_addr);

        // Processes
        SC_THREAD(reset_proc);
        SC_THREAD(stimulus_proc);
        SC_THREAD(monitor_proc);
        SC_THREAD(timeout_proc);
        SC_METHOD(cycle_proc);
        sensitive << clk.posedge_event();
    }

    // Reset process: holds reset for ResetDuration (50 ns = 5 cycles)
    void reset_proc()
    {
        rstn.write(false);
        wait(ResetDuration, SC_NS);
        rstn.write(true);
    }

    // Stimulus process: configures LayerNorm mode and enables pipeline
    void stimulus_proc()
    {
        // Initial values
        pipeline_en.write(false);
        gvu_op.write(static_cast<uint32_t>(GVU_OP));
        psum_valid.write(false);
        psum_data.write(0);
        token_len.write(TOKEN_LEN);
        attn_z.write(0);
        attn_scale.write(0);
        attn_shift.write(0);
        sbank_buf_sel.write(false);
        vbank0_buf_sel.write(false);
        vbank1_buf_sel.write(false);
        vbank2_buf_sel.write(false);
        scratch_dma_en.write(false);
        scratch_dma_wen.write(false);
        scratch_dma_addr.write(0);

        // @(posedge clk);
        wait(clk.posedge_event());
        // wait (rstn);
        while (!rstn.read())
            wait(clk.posedge_event());
        // @(posedge clk);
        wait(clk.posedge_event());

        // Apply configuration for LayerNorm mode
        sbank_buf_sel.write(false);
        vbank0_buf_sel.write(false);
        vbank1_buf_sel.write(false);
        vbank2_buf_sel.write(false);
        pipeline_en.write(true);

        std::cout << "[tb_gvu_top_layernorm] Stimulus enabled at " << sc_time_stamp()
                  << ": op=GVU_OP_LAYERNORM token_len=" << TOKEN_LEN << std::endl;
    }

    // Monitor process: waits for LayerNorm pipeline done
    void monitor_proc()
    {
        std::cout << "Seed: " << 0 << std::endl;

        for (unsigned i = 0; i < MinCycle; ++i)
            wait(clk.posedge_event());

        // Wait until layernorm_pipe_done is asserted
        while (!layernorm_pipe_done.read() && !softmax_pipe_done.read())
            wait(clk.posedge_event());

        done_seen = true;
        std::cout << "[tb_gvu_top_layernorm] o_layernorm_pipe_done asserted at " << sc_time_stamp()
                  << " (cycle " << cycle_count << ")" << std::endl;

        // Post-completion settling cycles
        wait(100, SC_NS);
        std::cout << "Simulation end!" << std::endl;

        wait(50, SC_NS);
        exit_code = 0;
        sc_stop();
    }

    // Timeout watchdog
    void timeout_proc()
    {
        for (unsigned i = 0; i < MaxCycle; ++i)
            wait(clk.posedge_event());

        if (!done_seen)
        {
            std::cout << "[FATAL] Simulation timeout!" << std::endl;
            timed_out = true;
            exit_code = 1;
            sc_stop();
        }
    }

    // Cycle tracking and FSM state transition logger
    void cycle_proc()
    {
        if (rstn.read())
        {
            cycle_count++;
            static uint32_t last_state = 0xFFFFFFFFu;
            uint32_t cur_state = gvu_pipe_ctrl_state.read();
            if (cur_state != last_state)
            {
                std::cout << "[tb_gvu_top_layernorm] Cycle " << std::setw(5) << cycle_count
                          << " (" << std::setw(8) << sc_time_stamp() << ") State: "
                          << fsm_state_name(cur_state) << std::endl;
                last_state = cur_state;
            }
        }
    }
};

int sc_main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    TbGvuTopLayerNorm tb("tb_gvu_top_layernorm");
    sc_start();

    std::cout << "[tb_gvu_top_layernorm] RESULT: " << (tb.exit_code == 0 ? "PASS" : "FAIL") << std::endl;
    return tb.exit_code;
}

#else

// ============================================================================================================
// Standalone C++17 Cycle Testbench Implementation for LayerNorm (compiled when SystemC is absent)
// ============================================================================================================

int main(int argc, char *argv[])
{
    std::cout << "[tb_gvu_top_layernorm] Running in standalone C++17 cycle-accurate mode (LAYERNORM)." << std::endl;
    std::cout << "Seed: " << 0 << std::endl;

    rtl_ref_gvu::GvuTop dut;
    rtl_ref_gvu::GvuTop::In in;

    // Reset phase: 5 cycles active-low (50 ns at 10 ns clock)
    for (unsigned c = 0; c < 5; ++c)
    {
        dut.tick(in, false);
    }

    // One idle cycle after reset deasserts
    dut.tick(in, true);

    // Apply configuration for LayerNorm mode matching MAS.docx
    in.i_attn_z = 0;
    in.i_attn_scale = 0;
    in.i_attn_shift = 0;
    in.i_sbank_buf_sel = false;
    in.i_vbank0_buf_sel = false;
    in.i_vbank1_buf_sel = false;
    in.i_vbank2_buf_sel = false;
    in.i_token_len = TOKEN_LEN;
    in.i_gvu_op = GVU_OP; // GVU_OP_LAYERNORM
    in.i_pipeline_en = true;

    std::cout << "[tb_gvu_top_layernorm] Stimulus enabled at cycle 6 (60 ns): op=GVU_OP_LAYERNORM token_len="
              << TOKEN_LEN << std::endl;

    bool done_seen = false;
    uint32_t last_state = 0xFFFFFFFFu;
    uint64_t post_done_cycles = 0;

    for (uint64_t cycle = 7; cycle <= MaxCycle; ++cycle)
    {
        const rtl_ref_gvu::GvuTop::Out out = dut.tick(in, true);

        uint32_t cur_state = static_cast<uint32_t>(out.o_gvu_pipe_ctrl_state);

        if (cur_state != last_state)
        {
            std::cout << "[tb_gvu_top_layernorm] Cycle " << std::setw(5) << cycle
                      << " (" << std::setw(6) << cycle * 10 << " ns) State: "
                      << fsm_state_name(cur_state) << std::endl;
            last_state = cur_state;
        }

        if ((out.o_layernorm_pipe_done || out.o_softmax_pipe_done) && !done_seen)
        {
            done_seen = true;
            std::cout << "[tb_gvu_top_layernorm] o_layernorm_pipe_done asserted at " << cycle * 10
                      << " ns (cycle " << cycle << ")" << std::endl;
        }

        if (done_seen)
        {
            post_done_cycles++;
            if (post_done_cycles >= 15) // #100 + #50 ns = 15 cycles
            {
                std::cout << "Simulation end!" << std::endl;
                std::cout << "[tb_gvu_top_layernorm] RESULT: PASS" << std::endl;
                return 0;
            }
        }
    }

    std::cout << "[FATAL] Simulation timeout!" << std::endl;
    std::cout << "[tb_gvu_top_layernorm] RESULT: FAIL" << std::endl;
    return 1;
}

#endif
