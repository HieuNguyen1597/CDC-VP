// gvu_top_bridge.h -- optional execution of the vector-unit work of the instruction path on GvuTop (has/rtl_ref_gvu_top.h),
// the port of the top level of the vector unit. Off unless the environment variable HAS_GVU_TOP is set to a non-zero
// value; with it off nothing computed by the model changes. When on it takes the place of HAS_OBP_RTL / HAS_ELEM_RTL.
//   HAS_GVU_TOP=1
//     GEMM_FUSED epilogue  every beat of the PSM -> SRAM-C write path (has::HasObp::apply_inline) goes through GvuTop in
//                          stream mode: the model first computes its own result, then the beat (32 psums, lane mask,
//                          word address, Out_Scale / Out_Shift of its output channel) is run through the 9-cycle pipeline
//                          to completion inside the call, so the value lands in the cycle of the PSM write as in the model.
//                          The activation table of the layer is loaded through GvuTop's own path: the 256 bytes in rows
//                          0 and 1 of vbank1 buffer 1, then LUT_ACT_INIT (i_gemm_lut_reload). The tail behind the last
//                          write of a core pass is the stream depth (kDepth).
//     residual add         every chunk of has::HasElemwise::add runs on GvuTop in runs of at most 128 rows (the int8 rows
//                          of vbank1), the lanes past the tensor masked in the last row; the compute cycles of the chunk are
//                          GvuTop's, from the start of a run to its done output.
//     max pool             stays on GvuMaxPoolPipe (has::RtlElem::maxpool): GvuTop has no max pool state.
//   HAS_GVU_TOP=2          as 1, and in has/has_npu_top.h the activation table of a layer costs the cycles of that path (a
//                          DMA of 256 bytes into vbank1 plus LUT_ACT_INIT, table_cycles()) instead of 32 copies over the
//                          host port of the model's epilogue.
// The Scratchpad DMA port of GvuTop does nothing (as in the RTL), so the operands are written straight into its banks and
// the DMA cycles stay those of the model. Every result is compared with the model's and counted; values outside the
// containers of GvuTop keep the model result and are counted as "not expressible". One GvuTop serves both instructions
// (they run one after the other): before an add the epilogue run is stopped, before a beat the controller is put back
// into GEMM_FUSED_ST1. A table is printed when the program exits ("[gvu-top] ..." lines).
#ifndef HAS_GVU_TOP_BRIDGE_H
#define HAS_GVU_TOP_BRIDGE_H

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "has/rtl_ref_gvu_top.h"

namespace has
{
    class GvuTopBridge
    {
    public:
        static constexpr int kDepth = 9;                 // stream depth of GEMM_FUSED in GvuTop (register cuts of the MAS)

        static bool on() { return inst().level_ > 0; }
        static int level() { return inst().level_; }
        // Cycles of loading one activation table through GvuTop: DMA of the 256 bytes (dma_cycles, from the caller's DMA
        // estimate) plus the controller from IDLE through LUT_ACT_INIT into GEMM_FUSED_ST1 (measured once on a spare unit).
        static uint64_t table_cycles(uint64_t dma_cycles) { return dma_cycles + uint64_t(inst().init_cycles_); }

        // One beat of the GEMM_FUSED epilogue. out holds the model result on entry and GvuTop's result on return (masked
        // lanes). lut: the layer's table (256 entries, index = int8 value + 128); table_new: it changed since the last beat.
        // round_mode: has::RoundMode. Returns false (out untouched) when the parameters do not fit GvuTop.
        template <class Vec>
        static bool gemm_beat(const Vec &in, Vec &out, uint32_t mask, uint32_t addr, int64_t S, int s, int zp, int round_mode, bool sat16,
                              bool act, const int8_t *lut, bool table_new)
        {
            namespace R = rtl_ref_gvu;
            GvuTopBridge &b = inst();
            Stat &st = b.st_[0];
            st.calls++;
            if (!sat16 || !fit(S, 32) || s < 0 || s > 63 || !fit(zp, 16) || round_mode < 0 || round_mode > 6) { st.not_expr++; return false; }
            if (table_new || !b.table_loaded_) b.load_table(lut);
            b.enter_gemm(map_round(round_mode), zp, act);
            R::GvuTop::In &i = b.gin_;
            i.i_psum_valid = true;
            for (size_t l = 0; l < size_t(R::GVU_LANES); l++) i.i_psum_vec[l] = static_cast<int32_t>(in[l]);
            i.i_psum_scale = static_cast<int32_t>(S); i.i_psum_shift = static_cast<uint8_t>(s); i.i_psum_mask = mask; i.i_psum_addr = addr;
            b.top_.tick(i, true);
            i.i_psum_valid = false;
            R::GvuTop::Out o;
            int n = 0;
            do { o = b.top_.tick(i, true); n++; } while (!o.o_gemm_fused_valid && n < 4 * kDepth);
            if (!o.o_gemm_fused_valid) { st.no_out++; st.not_expr++; return false; }
            if (n != kDepth) st.lat_other++;
            if (o.o_gemm_fused_mask != mask || o.o_gemm_fused_addr != addr) st.meta_bad++;
            for (size_t l = 0; l < size_t(R::GVU_LANES); l++)
            {
                if (!((mask >> l) & 1u)) continue;
                const int h = o.o_gemm_fused_data[l], m = static_cast<int>(out[l]);
                st.lanes++;
                if (h != m) st.miss("x=%d S=%lld s=%d zp=%d act=%d model=%d GvuTop=%d", static_cast<int>(in[l]), static_cast<long long>(S), s, zp, int(act), m, h);
                out[l] = h;
            }
            st.cycles += uint64_t(n);
            return true;
        }

        // Residual add of one chunk of n elements, out holds the model result on entry and GvuTop's on return. cycles: the
        // compute cycles of GvuTop for the chunk. Returns false (out untouched) when the parameters do not fit GvuTop.
        static bool add(const int8_t *A, const int8_t *B, int8_t *out, size_t n, int zpA, int zpB, int zpO, int64_t SA, int64_t SB, int64_t SO,
                        int sA, int sB, int sO, int round_mode, bool sub_before, bool sat16, uint64_t model_cycles, uint64_t *cycles)
        {
            namespace R = rtl_ref_gvu;
            GvuTopBridge &b = inst();
            Stat &st = b.st_[1];
            st.calls++;
            if (!sub_before || !sat16 || !fit(SA, 32) || !fit(SB, 32) || !fit(SO, 32) || !fit(zpA, 8) || !fit(zpB, 8) || !fit(zpO, 8) ||
                sA < 0 || sA > 63 || sB < 0 || sB > 63 || sO < 0 || sO > 63 || round_mode < 0 || round_mode > 6)
            { st.not_expr++; return false; }
            b.leave_gemm();
            const size_t rows_total = (n + 31) / 32, per_run = 128;
            uint64_t cyc = 0;
            for (size_t r0 = 0; r0 < rows_total; r0 += per_run)
            {
                const size_t rows = std::min(per_run, rows_total - r0);
                std::vector<R::SpBytes> v1((rows + 3) / 4, R::SpBytes(128, 0));
                for (size_t r = 0; r < rows; r++)
                {
                    R::SpBytes ra(32, 0);
                    for (size_t l = 0; l < 32; l++)
                    {
                        const size_t e = (r0 + r) * 32 + l;
                        if (e >= n) break;
                        ra[l] = static_cast<uint8_t>(A[e]);
                        v1[r / 4][32 * (r % 4) + l] = static_cast<uint8_t>(B[e]);
                    }
                    b.top_.scratchpad().sram(0, 0).poke(static_cast<unsigned>(r), ra);
                }
                for (size_t k = 0; k < v1.size(); k++) b.top_.scratchpad().sram(1, 0).poke(static_cast<unsigned>(k), v1[k]);
                R::GvuTop::In i;
                i.i_gvu_op = R::GVU_OP_RESIDUAL; i.i_num_tile = 1; i.i_token_len = static_cast<uint32_t>(rows);
                i.i_res_scale_a = static_cast<int32_t>(SA); i.i_res_shift_a = static_cast<uint8_t>(sA); i.i_res_zp_a = static_cast<int8_t>(zpA);
                i.i_res_scale_b = static_cast<int32_t>(SB); i.i_res_shift_b = static_cast<uint8_t>(sB); i.i_res_zp_b = static_cast<int8_t>(zpB);
                i.i_res_scale_out = static_cast<int32_t>(SO); i.i_res_shift_out = static_cast<uint8_t>(sO); i.i_res_zp_out = static_cast<int8_t>(zpO);
                i.i_res_round = map_round(round_mode); i.i_res_out_int16 = false;
                const size_t tail = n - (r0 + rows - 1) * 32;
                i.i_res_last_lane_mask = (r0 + rows == rows_total && tail < 32) ? ((1u << tail) - 1u) : 0xFFFFFFFFu;
                i.i_pipeline_en = true;
                bool done = false;
                uint64_t c = 0;
                for (; c < 4 * rows + 400 && !done; c++) done = b.top_.tick(i, true).o_residual_pipe_done;
                i.i_pipeline_en = false;
                for (int k = 0; k < 4 && b.top_.state() != R::IDLE; k++) b.top_.tick(i, true);
                if (!done || b.top_.state() != R::IDLE) { st.no_out++; st.not_expr++; b.reset(); return false; }
                cyc += c;
                st.runs++;
                for (size_t r = 0; r < rows; r++)
                {
                    const R::SpBytes orow = b.top_.scratchpad().sram(0, 0).word(static_cast<unsigned>(r));
                    for (size_t l = 0; l < 32; l++)
                    {
                        const size_t e = (r0 + r) * 32 + l;
                        if (e >= n) break;
                        const int8_t h = static_cast<int8_t>(orow[l]);
                        st.lanes++;
                        if (h != out[e]) st.miss("element %zu a=%d b=%d model=%d GvuTop=%d", e, A[e], B[e], out[e], h);
                        out[e] = h;
                    }
                }
            }
            st.cycles += cyc; st.model_cycles += model_cycles;
            *cycles = cyc;
            return true;
        }
        // Max pool chunks handed to GvuMaxPoolPipe instead (no max pool state in GvuTop): counted for the report only.
        static void maxpool_elsewhere() { inst().st_[2].calls++; }

    private:
        struct Stat
        {
            uint64_t calls{0}, not_expr{0}, no_out{0}, lat_other{0}, meta_bad{0}, runs{0}, lanes{0}, differ{0}, cycles{0}, model_cycles{0};
            char first[160]{0};
            template <typename... A> void miss(const char *fmt, A... a) { if (!differ) std::snprintf(first, sizeof first, fmt, a...); differ++; }
        };
        int level_{0};
        int init_cycles_{0};
        bool table_loaded_{false};
        uint64_t tables_{0}, enters_{0}, stops_{0}, resets_{0};
        rtl_ref_gvu::GvuTop top_;
        rtl_ref_gvu::GvuTop::In gin_;     // inputs held between the beats of an epilogue run
        Stat st_[3];

        static GvuTopBridge &inst() { static GvuTopBridge b; return b; }
        GvuTopBridge()
        {
            const char *v = std::getenv("HAS_GVU_TOP");
            level_ = v ? std::atoi(v) : 0;
            if (level_ <= 0) { level_ = 0; return; }
            reset();
            // cycles of one table load on a spare unit: IDLE -> LUT_ACT_INIT -> GEMM_FUSED_ST1
            rtl_ref_gvu::GvuTop spare;
            rtl_ref_gvu::GvuTop::In i;
            for (int c = 0; c < 5; c++) spare.tick(i, false);
            spare.tick(i, true);
            i.i_gvu_op = rtl_ref_gvu::GVU_OP_GEMM_FUSED; i.i_gemm_stream = true; i.i_pipeline_en = true;
            while (spare.state() != rtl_ref_gvu::GEMM_FUSED_ST1 && init_cycles_ < 100) { spare.tick(i, true); init_cycles_++; }
            std::atexit(&GvuTopBridge::report);
        }
        void reset()
        {
            rtl_ref_gvu::GvuTop::In i;
            for (int c = 0; c < 5; c++) top_.tick(i, false);
            top_.tick(i, true);
            gin_ = rtl_ref_gvu::GvuTop::In();
            table_loaded_ = false;
            resets_++;
        }
        static bool fit(int64_t v, int bits) { const int64_t h = static_cast<int64_t>(1) << (bits - 1); return v >= -h && v < h; }
        static rtl_ref_gvu::RoundMode map_round(int m)
        {
            namespace R = rtl_ref_gvu;
            static const R::RoundMode t[7] = {R::RND_FLOOR, R::RND_NEAR_UP, R::RND_NEAR_AWAY, R::RND_NEAR_EVEN, R::RND_CEIL, R::RND_TRUNC, R::RND_AWAY};
            return t[m];
        }
        // Ends an epilogue run: stop, then IDLE once the pipeline is empty.
        void leave_gemm()
        {
            namespace R = rtl_ref_gvu;
            if (top_.state() == R::IDLE) return;
            gin_.i_psum_valid = false; gin_.i_gemm_stream_stop = true;
            for (int k = 0; k < 4 * kDepth && top_.state() != R::IDLE; k++) top_.tick(gin_, true);
            gin_.i_pipeline_en = false; gin_.i_gemm_stream_stop = false;
            top_.tick(gin_, true);
            stops_++;
            if (top_.state() != R::IDLE) reset();
        }
        // Puts the controller into GEMM_FUSED_ST1 with the held parameters of the layer (rounding, zero point, activation).
        void enter_gemm(rtl_ref_gvu::RoundMode rm, int zp, bool act)
        {
            namespace R = rtl_ref_gvu;
            gin_.i_gemm_round = rm; gin_.i_gemm_zp16 = zp; gin_.i_gemm_skip_act = !act;
            if (top_.state() == R::GEMM_FUSED_ST1) return;
            leave_gemm();
            gin_.i_gvu_op = R::GVU_OP_GEMM_FUSED; gin_.i_gemm_stream = true; gin_.i_gemm_stream_stop = false; gin_.i_psum_valid = false;
            gin_.i_pipeline_en = true;
            for (int k = 0; k < 100 && top_.state() != R::GEMM_FUSED_ST1; k++)
            {
                top_.tick(gin_, true);
                if (top_.state() == R::LUT_ACT_INIT) gin_.i_gemm_lut_reload = false;
            }
            gin_.i_gemm_lut_reload = false;
            enters_++;
        }
        // A new table: the 256 bytes into rows 0 and 1 of vbank1 buffer 1, then a pass through LUT_ACT_INIT.
        void load_table(const int8_t *lut)
        {
            leave_gemm();
            for (unsigned w = 0; w < 2; w++)
            {
                rtl_ref_gvu::SpBytes row(128);
                for (unsigned k = 0; k < 128; k++) row[k] = static_cast<uint8_t>(lut[128 * w + k]);
                top_.scratchpad().sram(1, 1).poke(w, row);
            }
            gin_.i_vbank1_buf_sel = true;
            gin_.i_gemm_lut_reload = table_loaded_;   // the first load needs no reload: the controller has not loaded a table yet
            table_loaded_ = true;
            tables_++;
        }
        static void report()
        {
            const GvuTopBridge &b = inst();
            static const char *fn[3] = {"GEMM epilogue", "residual add", "max pool"};
            std::printf("[gvu-top] vector unit on GvuTop (HAS_GVU_TOP=%d), stream depth %d, table load %d cycles + DMA: operation | calls | "
                        "not expressible | lanes | differ from the model | GvuTop cycles | model cycles | first difference\n", b.level_, kDepth, b.init_cycles_);
            for (int i = 0; i < 3; i++)
            {
                const Stat &s = b.st_[i];
                if (s.calls == 0) continue;
                if (i == 2) { std::printf("[gvu-top] %-13s | chunks %llu on GvuMaxPoolPipe (no max pool in GvuTop)\n", fn[i], static_cast<unsigned long long>(s.calls)); continue; }
                std::printf("[gvu-top] %-13s | calls %10llu | not expressible %6llu | lanes %12llu | differ %8llu | GvuTop cycles %12llu | model cycles %12llu | %s\n",
                            fn[i], static_cast<unsigned long long>(s.calls), static_cast<unsigned long long>(s.not_expr), static_cast<unsigned long long>(s.lanes),
                            static_cast<unsigned long long>(s.differ), static_cast<unsigned long long>(s.cycles), static_cast<unsigned long long>(s.model_cycles),
                            s.differ ? s.first : (s.no_out ? "a beat or run did not finish" : "-"));
            }
            std::printf("[gvu-top] epilogue beats with another depth %llu, address / mask not carried %llu; tables loaded %llu, runs entered %llu, stopped %llu, "
                        "unit resets %llu\n", static_cast<unsigned long long>(b.st_[0].lat_other), static_cast<unsigned long long>(b.st_[0].meta_bad),
                        static_cast<unsigned long long>(b.tables_), static_cast<unsigned long long>(b.enters_), static_cast<unsigned long long>(b.stops_),
                        static_cast<unsigned long long>(b.resets_));
            std::fflush(stdout);
        }
    };
} // namespace has

#endif // HAS_GVU_TOP_BRIDGE_H
