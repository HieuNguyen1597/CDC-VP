// tb_gvu_top_gemm_fused.cpp -- SystemC & C++ reference testbench for GVU top (gvu_top)
// validating the GEMM Fused Post-Processing Pipeline (Requantization + Activation LUT)
// conforming to MAS Section 5.2.1 & 5.2.2 and the exact verification methodology
// from tb_gvu_top.cpp / tb_gvu_top.sv.
//
// Dual-use architecture:
// 1. When compiled with SystemC (<systemc.h>), instantiates ScGvuTop with sc_clock,
//    sc_signal, SC_THREADs for reset, stimulus, monitor, and timeout watchdog.
// 2. When compiled in standalone C++17 mode (without SystemC library installed),
//    executes cycle-by-cycle on GvuTop reproducing the exact same clock cycles,
//    stimulus sequences, latency assertions, and timeout protection.
//
// Build:
//   make tb_gvu_top_gemm_fused
//   or (standalone syntax / compile):
//   g++ -std=c++17 -O2 -Wall -I. tb_gvu_top_gemm_fused.cpp -o tb_gvu_top_gemm_fused
//
// Usage:
//   ./tb_gvu_top_gemm_fused [tile|vec]
//     tile : 64 rows, 64 token length, 2 tiles x 32 lanes = 4,096 elements (default)
//     vec  : 1 row, 64 token length, 1 tile x 1 lane = 64 elements

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
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <cstdint>
#include <cmath>
#include "has/rtl_ref_gvu_top.h"

// Hardware parameters matching tb_gvu_top.sv
static constexpr unsigned NUM_LANE      = 32;
static constexpr unsigned LUT_INIT_DW   = 32 * NUM_LANE; // 1024-bit lines
static constexpr double   ClockCycle    = 10.0;          // 10 ns (100 MHz)
static constexpr double   ResetDuration = 50.0;          // 50 ns (5 cycles)
static constexpr unsigned MaxCycle      = 20000;
static constexpr unsigned MinCycle      = 10;
static constexpr rtl_ref_gvu::GvuOpcode GVU_OP = rtl_ref_gvu::GVU_OP_GEMM_FUSED;

// Base paths to test datasets
static const std::string BASE_DATA_DIR = "target/test/gemm_fused/";

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
    case rtl_ref_gvu::GEMM_FUSED_ST1: return "GEMM_FUSED_ST1";
    case rtl_ref_gvu::RESIDUAL_ST1:   return "RESIDUAL_ST1";
    case rtl_ref_gvu::RESIDUAL_ST2:   return "RESIDUAL_ST2";
    default:                          return "UNKNOWN";
    }
}

// Structure to hold dataset vectors for GEMM Fused (MAS Section 5.2.1 & 5.2.2)
struct GemmFusedDataset
{
    std::string mode_str{"tile"};
    bool tile_mode{true};
    unsigned num_token{64};
    unsigned token_len{64};
    unsigned gemm_tile{2};
    unsigned tile_lane{32};

    // Quantization parameters matching MAS Section 5.2.2
    int32_t gemm_scale{320};
    int8_t  gemm_shift{8};
    int8_t  gemm_zp{-3};

    std::vector<uint8_t> lut_act_mem;
    std::vector<int32_t> input_mem;
    std::vector<int8_t>  golden_output;

    bool load(bool is_tile)
    {
        tile_mode = is_tile;
        mode_str = is_tile ? "tile" : "vec";
        num_token = is_tile ? 64 : 1;
        token_len = 64;
        gemm_tile = num_token / NUM_LANE + (num_token % NUM_LANE != 0);
        tile_lane = is_tile ? NUM_LANE : 1;

        lut_act_mem.assign(256, 0);
        input_mem.assign(num_token * token_len, 0);
        golden_output.assign(num_token * token_len, 0);

        const std::string dir = BASE_DATA_DIR + mode_str + "/";
        std::cout << "[tb_gvu_top_gemm_fused] Loading dataset from: " << dir << std::endl;

        // 1. Load lut_act.txt (256 uint8)
        {
            std::ifstream f(dir + "lut_act.txt");
            if (!f.is_open())
            {
                std::cerr << "[tb_gvu_top_gemm_fused] ERROR: Could not open " << dir << "lut_act.txt" << std::endl;
                return false;
            }
            int val;
            for (size_t i = 0; i < 256 && (f >> val); ++i)
                lut_act_mem[i] = static_cast<uint8_t>(val);
        }

        // 2. Load input.txt (int32 psum values)
        {
            std::ifstream f(dir + "input.txt");
            if (!f.is_open())
            {
                std::cerr << "[tb_gvu_top_gemm_fused] ERROR: Could not open " << dir << "input.txt" << std::endl;
                return false;
            }
            int64_t val;
            for (size_t i = 0; i < input_mem.size() && (f >> val); ++i)
                input_mem[i] = static_cast<int32_t>(val);
        }

        // 3. Load golden_output.txt (int8 output values)
        {
            std::ifstream f(dir + "golden_output.txt");
            if (!f.is_open())
            {
                std::cerr << "[tb_gvu_top_gemm_fused] ERROR: Could not open " << dir << "golden_output.txt" << std::endl;
                return false;
            }
            int val;
            for (size_t i = 0; i < golden_output.size() && (f >> val); ++i)
                golden_output[i] = static_cast<int8_t>(val);
        }

        std::cout << "[tb_gvu_top_gemm_fused] Successfully loaded " << num_token << " tokens ("
                  << num_token * token_len << " elements), " << gemm_tile << " tiles ("
                  << tile_lane << " lanes/tile)." << std::endl;
        return true;
    }

    // Preloads scratchpad memories matching MAS Section 5.2.1 & Table 5-4:
    // 1. LUT act: start addr = 0 in vbank1 buffer 1, 2 writes of 128 bytes (256 uint8 entries)
    // 2. Input psum (int32): vbank2 buffer 0, row k*token_len + i, lane j (128 bytes per row)
    void preload_scratchpad(rtl_ref_gvu::GvuTop &dut) const
    {
        // 1. LUT act: 2 writes of 128 bytes in vbank1 buffer 1
        const unsigned LUT_ACT_NUM_WR = 8 * 256 / LUT_INIT_DW; // 2
        const unsigned LUT_ACT_EL_PER_W = 32 * NUM_LANE / 8;   // 128
        for (unsigned i = 0; i < LUT_ACT_NUM_WR; ++i)
        {
            std::vector<uint8_t> word(128, 0);
            for (unsigned j = 0; j < LUT_ACT_EL_PER_W; ++j)
            {
                word[j] = lut_act_mem[i * LUT_ACT_EL_PER_W + j];
            }
            dut.scratchpad().sram(1, 1).poke(i, word);
        }

        // 2. Input psum data (int32): vbank2 buffer 0
        for (unsigned k = 0; k < gemm_tile; ++k)
        {
            for (unsigned i = 0; i < token_len; ++i)
            {
                std::vector<uint8_t> row(4 * NUM_LANE, 0);
                for (unsigned j = 0; j < tile_lane; ++j)
                {
                    const uint32_t u = static_cast<uint32_t>(input_mem[k * NUM_LANE * token_len + j * token_len + i]);
                    row[4 * j + 0] = static_cast<uint8_t>(u & 0xFFu);
                    row[4 * j + 1] = static_cast<uint8_t>((u >> 8) & 0xFFu);
                    row[4 * j + 2] = static_cast<uint8_t>((u >> 16) & 0xFFu);
                    row[4 * j + 3] = static_cast<uint8_t>((u >> 24) & 0xFFu);
                }
                dut.scratchpad().sram(2, 0).poke(k * token_len + i, row);
            }
        }
    }

    // Verifies vbank0 memory against golden_output matching tb_gvu_top.cpp
    unsigned verify_output(rtl_ref_gvu::GvuTop &dut) const
    {
        unsigned mismatches = 0;
        std::cout << "\n[tb_gvu_top_gemm_fused] ==================== VERIFICATION RESULTS ====================" << std::endl;
        std::cout << "[tb_gvu_top_gemm_fused] Comparing 100% of memory contents in vbank0 against golden reference..." << std::endl;

        for (unsigned k = 0; k < gemm_tile; ++k)
        {
            for (unsigned j = 0; j < tile_lane; ++j)
            {
                for (unsigned i = 0; i < token_len; ++i)
                {
                    const int8_t exp = golden_output[k * NUM_LANE * token_len + j * token_len + i];
                    const auto row = dut.scratchpad().sram(0, 0).word(k * token_len + i);
                    const int8_t act = static_cast<int8_t>(row[j]);
                    if (exp != act)
                    {
                        if (mismatches < 10)
                        {
                            std::cerr << "[MISMATCH] Tile: " << std::setw(2) << k
                                      << " Lane/Token: " << std::setw(2) << (k * NUM_LANE + j)
                                      << " Pos: " << std::setw(2) << i
                                      << " | Expected: " << std::setw(4) << static_cast<int>(exp)
                                      << " Received: " << std::setw(4) << static_cast<int>(act) << std::endl;
                        }
                        mismatches++;
                    }
                }
            }
        }

        // Print sample output comparison (first 8 positions of first 4 lanes)
        std::cout << "\n[tb_gvu_top_gemm_fused] Sample Output Comparison (First 8 positions for Lanes 0..3):" << std::endl;
        std::cout << "  Pos | Lane 0 (Exp/Act) | Lane 1 (Exp/Act) | Lane 2 (Exp/Act) | Lane 3 (Exp/Act)" << std::endl;
        std::cout << "  ----+------------------+------------------+------------------+------------------" << std::endl;
        for (unsigned i = 0; i < std::min(8u, token_len); ++i)
        {
            std::cout << "  " << std::setw(3) << i << " | ";
            for (unsigned j = 0; j < std::min(4u, tile_lane); ++j)
            {
                const int8_t exp = golden_output[j * token_len + i];
                const auto row = dut.scratchpad().sram(0, 0).word(i);
                const int8_t act = static_cast<int8_t>(row[j]);
                std::cout << std::setw(4) << static_cast<int>(exp) << " / " << std::setw(4) << static_cast<int>(act) << "   | ";
            }
            std::cout << std::endl;
        }

        const unsigned total_elements = num_token * token_len;
        std::cout << "\n[tb_gvu_top_gemm_fused] Total elements verified: " << total_elements << std::endl;
        std::cout << "[tb_gvu_top_gemm_fused] Total mismatches:        " << mismatches << " / " << total_elements << std::endl;
        std::cout << "[tb_gvu_top_gemm_fused] Accuracy pass rate:      " << std::fixed << std::setprecision(2)
                  << (100.0 * (total_elements - mismatches) / total_elements) << "%" << std::endl;

        return mismatches;
    }
};

static GemmFusedDataset g_dataset;

#if GVU_TB_USE_SYSTEMC

// ============================================================================================================
// SystemC Testbench Implementation
// ============================================================================================================

SC_MODULE(TbGvuTopGemmFused)
{
    sc_clock clk;
    sc_signal<bool> rstn;

    sc_signal<bool, SC_MANY_WRITERS> pipeline_en;   // written by stimulus_proc (start) and monitor_proc (stop)
    sc_signal<uint32_t> gvu_op;

    sc_signal<bool> psum_valid;
    sc_signal<bool> psum_ready;
    sc_signal<uint32_t> psum_data;

    sc_signal<uint32_t> num_tile;
    sc_signal<uint32_t> token_len;
    sc_signal<int32_t> attn_z;
    sc_signal<int32_t> attn_scale;
    sc_signal<uint32_t> attn_shift;

    sc_signal<bool> softmax_lut_init_done;
    sc_signal<bool> softmax_pipe_done;
    sc_signal<bool> gemm_fused_lut_init_done;
    sc_signal<bool> gemm_fused_pipe_done;
    sc_signal<uint32_t> gvu_pipe_ctrl_state;

    sc_signal<bool> sbank_buf_sel;
    sc_signal<bool> vbank0_buf_sel;
    sc_signal<bool> vbank1_buf_sel;
    sc_signal<bool> vbank2_buf_sel;

    sc_signal<bool> scratch_dma_en;
    sc_signal<bool> scratch_dma_wen;
    sc_signal<uint32_t> scratch_dma_addr;

    rtl_ref_gvu::ScGvuTop dut;

    uint64_t cycle_count{0};
    bool done_seen{false};
    bool timed_out{false};
    int exit_code{0};

    SC_CTOR(TbGvuTopGemmFused)
        : clk("clk", ClockCycle, SC_NS, 0.5, 0.0, SC_NS, true),
          rstn("rstn"),
          pipeline_en("pipeline_en"),
          gvu_op("gvu_op"),
          psum_valid("psum_valid"),
          psum_ready("psum_ready"),
          psum_data("psum_data"),
          num_tile("num_tile"),
          token_len("token_len"),
          attn_z("attn_z"),
          attn_scale("attn_scale"),
          attn_shift("attn_shift"),
          softmax_lut_init_done("softmax_lut_init_done"),
          softmax_pipe_done("softmax_pipe_done"),
          gemm_fused_lut_init_done("gemm_fused_lut_init_done"),
          gemm_fused_pipe_done("gemm_fused_pipe_done"),
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
        dut.i_clk(clk);
        dut.i_rstn(rstn);
        dut.i_pipeline_en(pipeline_en);
        dut.i_gvu_op(gvu_op);
        dut.i_psum_valid(psum_valid);
        dut.o_psum_ready(psum_ready);
        dut.i_psum_data(psum_data);
        dut.i_num_tile(num_tile);
        dut.i_token_len(token_len);
        dut.i_attn_z(attn_z);
        dut.i_attn_scale(attn_scale);
        dut.i_attn_shift(attn_shift);
        dut.o_softmax_lut_init_done(softmax_lut_init_done);
        dut.o_softmax_pipe_done(softmax_pipe_done);
        dut.o_gemm_fused_lut_init_done(gemm_fused_lut_init_done);
        dut.o_gemm_fused_pipe_done(gemm_fused_pipe_done);
        dut.o_gvu_pipe_ctrl_state(gvu_pipe_ctrl_state);
        dut.i_sbank_buf_sel(sbank_buf_sel);
        dut.i_vbank0_buf_sel(vbank0_buf_sel);
        dut.i_vbank1_buf_sel(vbank1_buf_sel);
        dut.i_vbank2_buf_sel(vbank2_buf_sel);
        dut.i_scratch_dma_en(scratch_dma_en);
        dut.i_scratch_dma_wen(scratch_dma_wen);
        dut.i_scratch_dma_addr(scratch_dma_addr);

        dut.config_gemm_fused(g_dataset.gemm_scale, g_dataset.gemm_shift, g_dataset.gemm_zp);
        g_dataset.preload_scratchpad(dut.core());

        SC_THREAD(reset_proc);
        SC_THREAD(stimulus_proc);
        SC_THREAD(monitor_proc);
        SC_THREAD(timeout_proc);
        SC_METHOD(cycle_proc);
        sensitive << clk.posedge_event();
    }

    void reset_proc()
    {
        rstn.write(false);
        wait(ResetDuration, SC_NS);
        rstn.write(true);
    }

    void stimulus_proc()
    {
        pipeline_en.write(false);
        gvu_op.write(static_cast<uint32_t>(GVU_OP));
        psum_valid.write(false);
        psum_data.write(0);
        num_tile.write(g_dataset.gemm_tile);
        token_len.write(g_dataset.token_len);
        attn_z.write(0);
        attn_scale.write(1);
        attn_shift.write(0);
        sbank_buf_sel.write(false);
        vbank0_buf_sel.write(false);
        vbank1_buf_sel.write(true); // Buffer 1 for LUT_ACT_INIT
        vbank2_buf_sel.write(false);
        scratch_dma_en.write(false);
        scratch_dma_wen.write(false);
        scratch_dma_addr.write(0);

        wait(clk.posedge_event());
        while (!rstn.read())
            wait(clk.posedge_event());
        wait(clk.posedge_event());

        pipeline_en.write(true);

        std::cout << "[tb_gvu_top_gemm_fused] Stimulus enabled at " << sc_time_stamp()
                  << ": mode=" << g_dataset.mode_str << " token_len=" << g_dataset.token_len
                  << " num_tile=" << g_dataset.gemm_tile << " gemm_scale=" << g_dataset.gemm_scale
                  << " gemm_shift=" << static_cast<int>(g_dataset.gemm_shift)
                  << " gemm_zp=" << static_cast<int>(g_dataset.gemm_zp) << std::endl;

        while (!gemm_fused_lut_init_done.read())
            wait(clk.posedge_event());
        wait(clk.posedge_event());
        vbank1_buf_sel.write(false);
        std::cout << "[tb_gvu_top_gemm_fused] o_gemm_fused_lut_init_done asserted at " << sc_time_stamp()
                  << "; switched vbank1_buf_sel to Buffer 0" << std::endl;
    }

    void monitor_proc()
    {
        std::cout << "Seed: " << 0 << std::endl;

        for (unsigned i = 0; i < MinCycle; ++i)
            wait(clk.posedge_event());

        while (!gemm_fused_pipe_done.read())
            wait(clk.posedge_event());

        done_seen = true;
        pipeline_en.write(false);
        std::cout << "\n[tb_gvu_top_gemm_fused] o_gemm_fused_pipe_done asserted at " << sc_time_stamp()
                  << " (cycle " << cycle_count << ")" << std::endl;

        wait(100, SC_NS);
        std::cout << "Simulation end!" << std::endl;

        unsigned mismatches = g_dataset.verify_output(dut.core());
        exit_code = (mismatches == 0) ? 0 : 1;

        wait(50, SC_NS);
        sc_stop();
    }

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

    void cycle_proc()
    {
        if (rstn.read())
        {
            cycle_count++;
            static uint32_t last_state = 0xFFFFFFFFu;
            uint32_t cur_state = gvu_pipe_ctrl_state.read();
            if (cur_state != last_state)
            {
                std::cout << "[tb_gvu_top_gemm_fused] Cycle " << std::setw(5) << cycle_count
                          << " (" << std::setw(8) << sc_time_stamp() << ") State: "
                          << std::setw(15) << std::left << fsm_state_name(cur_state)
                          << " | Tile " << dut.core().tile_cnt()
                          << " start_d=" << std::setw(3) << dut.core().start_d_cnt()
                          << " end_d=" << std::setw(3) << dut.core().end_d_cnt()
                          << std::endl;
                last_state = cur_state;
            }
        }
    }
};

int sc_main(int argc, char *argv[])
{
    bool is_tile = true;
    if (argc > 1 && (std::string(argv[1]) == "vec" || std::string(argv[1]) == "single"))
        is_tile = false;

    if (!g_dataset.load(is_tile))
        return 1;

    TbGvuTopGemmFused tb("tb_gvu_top_gemm_fused");
    sc_start();

    std::cout << "\n[tb_gvu_top_gemm_fused] RESULT: " << (tb.exit_code == 0 ? "PASS" : "FAIL") << std::endl;
    return tb.exit_code;
}

#else

// ============================================================================================================
// Standalone C++17 Cycle Testbench Implementation (compiled when SystemC is absent)
// ============================================================================================================

int main(int argc, char *argv[])
{
    std::cout << "[tb_gvu_top_gemm_fused] Running in standalone C++17 cycle-accurate mode (GEMM_FUSED)." << std::endl;
    std::cout << "Seed: " << 0 << std::endl;

    bool is_tile = true;
    if (argc > 1 && (std::string(argv[1]) == "vec" || std::string(argv[1]) == "single"))
        is_tile = false;

    if (!g_dataset.load(is_tile))
        return 1;

    rtl_ref_gvu::GvuTop dut;
    g_dataset.preload_scratchpad(dut);

    rtl_ref_gvu::GvuTop::In in;

    // Reset phase: 5 cycles active-low (50 ns at 10 ns clock)
    for (unsigned c = 0; c < 5; ++c)
    {
        dut.tick(in, false);
    }

    // One idle cycle after reset deasserts
    dut.tick(in, true);

    // Apply configuration matching MAS Section 5.2.1 & 5.2.2
    in.i_gemm_scale    = g_dataset.gemm_scale;
    in.i_gemm_shift    = g_dataset.gemm_shift;
    in.i_gemm_zp       = g_dataset.gemm_zp;
    in.i_gemm_skip_act = false;
    in.i_gemm_skip_zp  = false;

    in.i_sbank_buf_sel  = false;
    in.i_vbank0_buf_sel = false;
    in.i_vbank1_buf_sel = true; // Buffer 1 for LUT_ACT_INIT
    in.i_vbank2_buf_sel = false;
    in.i_num_tile       = g_dataset.gemm_tile;
    in.i_token_len      = g_dataset.token_len;
    in.i_gvu_op         = GVU_OP;
    in.i_pipeline_en    = true;

    std::cout << "[tb_gvu_top_gemm_fused] Stimulus enabled at cycle 6 (60 ns): mode=" << g_dataset.mode_str
              << " token_len=" << g_dataset.token_len << " num_tile=" << g_dataset.gemm_tile
              << " gemm_scale=" << g_dataset.gemm_scale
              << " gemm_shift=" << static_cast<int>(g_dataset.gemm_shift)
              << " gemm_zp=" << static_cast<int>(g_dataset.gemm_zp) << std::endl;

    bool done_seen = false;
    uint32_t last_state = 0xFFFFFFFFu;
    uint64_t post_done_cycles = 0;
    uint64_t state_start_cycle = 7;

    for (uint64_t cycle = 7; cycle <= MaxCycle; ++cycle)
    {
        const rtl_ref_gvu::GvuTop::Out out = dut.tick(in, true);
        uint32_t cur_state = static_cast<uint32_t>(out.o_gvu_pipe_ctrl_state);

        if (cur_state != last_state)
        {
            if (last_state != 0xFFFFFFFFu)
            {
                std::cout << "  --> Stage " << fsm_state_name(last_state)
                          << " completed after " << (cycle - state_start_cycle) << " cycles ("
                          << (cycle - state_start_cycle) * 10 << " ns)" << std::endl;
            }

            std::cout << "[tb_gvu_top_gemm_fused] Cycle " << std::setw(5) << cycle
                      << " (" << std::setw(6) << cycle * 10 << " ns) State: "
                      << std::setw(15) << std::left << fsm_state_name(cur_state)
                      << " | Tile " << dut.tile_cnt()
                      << " start_d=" << std::setw(3) << dut.start_d_cnt()
                      << " end_d=" << std::setw(3) << dut.end_d_cnt()
                      << std::endl;
            last_state = cur_state;
            state_start_cycle = cycle;
        }

        // When LUT_ACT init completes, switch to vbank1 buffer 0
        if (out.o_gemm_fused_lut_init_done && in.i_vbank1_buf_sel)
        {
            in.i_vbank1_buf_sel = false;
            std::cout << "[tb_gvu_top_gemm_fused] Cycle " << std::setw(5) << cycle
                      << ": o_gemm_fused_lut_init_done asserted! Switched vbank1_buf_sel to Buffer 0" << std::endl;
        }

        if (out.o_gemm_fused_pipe_done && !done_seen)
        {
            done_seen = true;
            in.i_pipeline_en = false;
            std::cout << "\n[tb_gvu_top_gemm_fused] o_gemm_fused_pipe_done asserted at " << cycle * 10
                      << " ns (cycle " << cycle << ")" << std::endl;
        }

        if (done_seen)
        {
            post_done_cycles++;
            if (post_done_cycles >= 15) // #100 + #50 ns = 15 cycles
            {
                std::cout << "Simulation end!" << std::endl;

                unsigned mismatches = g_dataset.verify_output(dut);
                if (mismatches == 0)
                {
                    std::cout << "\n[tb_gvu_top_gemm_fused] RESULT: PASS" << std::endl;
                    return 0;
                }
                else
                {
                    std::cout << "\n[tb_gvu_top_gemm_fused] RESULT: FAIL" << std::endl;
                    return 1;
                }
            }
        }
    }

    std::cout << "[FATAL] Simulation timeout!" << std::endl;
    std::cout << "[tb_gvu_top_gemm_fused] RESULT: FAIL" << std::endl;
    return 1;
}

#endif
