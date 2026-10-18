// rtl_ref_gvu_lut.h -- LUT unit of the vector unit, ported line by line from the RTL (neo_core/gvu/FUs): gvu_lut_mem.sv,
// gvu_lut_lookup_lane.sv, gvu_lut_unit.sv. Pure C++17, no SystemC. The functional model of the same unit is has/gvu_lut.h
// (has::LutIndirect = the PWL mode, has::LutLogScale = the log-scale mode); nothing here is used by the instruction path.
// Follows the RTL in which gvu_lut_mem counts its own write address (ports o_init_done, i_lut_compens, o_pipe_almost_done).
// The lane is a chain of five pipeline_reg (pipe0 .. pipe4); pipe3 exists when LERP_MUL_PIPE = 1 (default) and pipe4 when
// LERP_SHIFT_PIPE = 1 (default 0). Latency with the defaults (lat_lut()): direct (act / exp) 0, log-scale 2, PWL 4.
// The control inputs (table select, index mode, base, W, lerp shift) are not pipelined: every stage uses the value present
// in the cycle it works, so they must be held while a lookup is in flight.
// Zero input: the all-zeros flag of the leading-one detector travels with the beat through the five pipeline registers.
// A zero beat takes the same number of cycles as any other beat and its 16-bit result is forced to 0 at the output
// (log-scale: by the flag held in pipe1, PWL: by the flag held in pipe4). E is 0 for a zero input, so o_e_shift is 0.
// The unit has no zero-flag output.
// DOUBLE_PIPE_REG = 1 (spill register) is not ported, see has/rtl_ref_gvu_fu.h.
// Table load, as the RTL is written: gvu_lut_mem keeps its own write address (lut_waddr_r) and a write counter
// (wr_cnt_r, 3 bits). max_wr_cnt is the number of writes of a whole table minus one (15 / 31 / 63 for INIT_DW = 128)
// assigned to 3 bits, so it is 7 for every table: o_init_done rises after 7 writes and the address returns to 0 after the
// 8th. Only the first 8 * INIT_DW bits of a table can be written through the port (128 of the 256 act / exp entries, 64 of
// the 256 recip and of the 512 rsqrt entries). LutMem::tick reproduces it; load8 / load16 fill a table directly.
#ifndef RTL_REF_GVU_LUT_H
#define RTL_REF_GVU_LUT_H

#include <array>
#include <cstdint>
#include <vector>
#include "has/rtl_ref_gvu_fu.h"

namespace rtl_ref_gvu
{
    enum LutType : int { LUT_ACT = 0, LUT_EXP = 1, LUT_RECIP = 2, LUT_RSQRT = 3 };   // lut_type_e
    enum LutIdxMode : int { LUT_LOG_SCALE = 0, LUT_PWL = 1 };                        // lut_index_type_e

    struct LutParams
    {
        bool DOUBLE_PIPE_REG{false};
        bool LERP_MUL_PIPE{true};    // pipeline register after the multiplication of the interpolation
        bool LERP_SHIFT_PIPE{false}; // pipeline register after the shift of the interpolation
        int INIT_DW{128};
    };
    inline bool lut_is_direct(LutType t) { return t == LUT_ACT || t == LUT_EXP; }
    inline int lat_lut(LutType t, LutIdxMode m, const LutParams &p = LutParams())
    {
        if (lut_is_direct(t)) return 0;
        if (m == LUT_LOG_SCALE) return 2;                               // pipe0, pipe1
        return 3 + (p.LERP_MUL_PIPE ? 1 : 0) + (p.LERP_SHIFT_PIPE ? 1 : 0); // pipe0, pipe1, pipe2 (+ pipe3, pipe4)
    }

    // ---- gvu_lut_mem.sv ------------------------------------------------------------------------------------------------
    // Four flip-flop tables: act 256 x 8, exp 256 x 8, recip 256 x 16, rsqrt 512 x 16. No reset (zero here).
    // Write port: INIT_DW bits per write = INIT_DW/8 bytes (act / exp) or INIT_DW/16 words (recip / rsqrt) starting at the
    // internal word address lut_waddr_r. An element whose address passes the end of the table wraps to its start: the
    // index waddr + i is used on the address width of the table (8 bits, 9 bits for rsqrt), as the RTL simulation shows.
    // lut_waddr_proc (reset to 0): with i_lut_wen the address advances by the words of one write of the table selected
    // in that cycle and wr_cnt_r by one; when wr_cnt_r == max_wr_cnt (before the edge) both return to 0, with or without
    // a write. max_wr_cnt and o_init_done follow i_lut_wsel combinationally.
    // Read, combinational: lut[i] and lut[i+1]; the address of lut[i+1] is held at 255 (recip) / 511 (rsqrt). The 8-bit
    // data output selects act when i_lut_rsel == LUT_ACT and exp otherwise; the 16-bit outputs select recip when
    // i_lut_rsel == LUT_RECIP and rsqrt otherwise.
    class LutMem
    {
    public:
        explicit LutMem(const LutParams &p = LutParams()) : P_(p) { act_.fill(0); exp_.fill(0); recip_.fill(0); rsqrt_.fill(0); }
        unsigned word_per_wr(LutType i_lut_wsel) const { return static_cast<unsigned>(lut_is_direct(i_lut_wsel) ? P_.INIT_DW / 8 : P_.INIT_DW / 16) & 0xFFu; }
        // 3-bit truncation of (writes of the whole table - 1)
        unsigned max_wr_cnt(LutType i_lut_wsel) const
        {
            const int full = i_lut_wsel == LUT_RSQRT ? 512 * 16 / P_.INIT_DW - 1 : (i_lut_wsel == LUT_RECIP ? 16 * 256 / P_.INIT_DW - 1 : 8 * 256 / P_.INIT_DW - 1);
            return static_cast<unsigned>(full) & 7u;
        }
        bool o_init_done(LutType i_lut_wsel) const { return wr_cnt_r_ == max_wr_cnt(i_lut_wsel); }
        unsigned lut_waddr() const { return lut_waddr_r_; }
        unsigned wr_cnt() const { return wr_cnt_r_; }
        // One clock edge. i_lut_wdata: INIT_DW/8 bytes, least significant first (read only when i_lut_wen).
        void tick(bool i_rstn, bool i_lut_wen, LutType i_lut_wsel, const uint8_t *i_lut_wdata)
        {
            if (i_lut_wen) store(i_lut_wsel, lut_waddr_r_, i_lut_wdata);   // the memories have no reset
            if (!i_rstn) { lut_waddr_r_ = 0; wr_cnt_r_ = 0; return; }
            const bool wrap = wr_cnt_r_ == max_wr_cnt(i_lut_wsel);
            if (i_lut_wen) { lut_waddr_r_ = (lut_waddr_r_ + word_per_wr(i_lut_wsel)) & 0x1FFu; wr_cnt_r_ = (wr_cnt_r_ + 1u) & 7u; }
            if (wrap) { lut_waddr_r_ = 0; wr_cnt_r_ = 0; }
        }
        // Direct fill of a table (not a path of the RTL): test benches and models that need a complete table.
        void load8(LutType t, const std::vector<uint8_t> &v) { for (size_t i = 0; i < v.size() && i < 256; i++) (t == LUT_ACT ? act_ : exp_)[i] = v[i]; }
        void load16(LutType t, const std::vector<uint16_t> &v)
        {
            for (size_t i = 0; i < v.size(); i++) { if (t == LUT_RECIP) { if (i < 256) recip_[i] = v[i]; } else if (i < 512) rsqrt_[i] = v[i]; }
        }
        uint8_t peek8(LutType t, unsigned a) const { return (t == LUT_ACT ? act_ : exp_)[a & 0xFFu]; }
        uint16_t peek16(LutType t, unsigned a) const { return t == LUT_RECIP ? recip_[a & 0xFFu] : rsqrt_[a & 0x1FFu]; }
        uint8_t rdata8(LutType i_lut_rsel, unsigned i_lut_raddr) const { return (i_lut_rsel == LUT_ACT ? act_ : exp_)[i_lut_raddr & 0xFFu]; }
        uint16_t rdata16(LutType i_lut_rsel, unsigned i_lut_raddr) const
        {
            return i_lut_rsel == LUT_RECIP ? recip_[i_lut_raddr & 0xFFu] : rsqrt_[i_lut_raddr & 0x1FFu];
        }
        uint16_t rdata16_nxt(LutType i_lut_rsel, unsigned i_lut_raddr) const
        {
            const unsigned a8 = i_lut_raddr & 0xFFu, a9 = i_lut_raddr & 0x1FFu;
            return i_lut_rsel == LUT_RECIP ? recip_[a8 < 255 ? a8 + 1 : a8] : rsqrt_[a9 < 511 ? a9 + 1 : a9];
        }

    private:
        LutParams P_;
        std::array<uint8_t, 256> act_, exp_;
        std::array<uint16_t, 256> recip_;
        std::array<uint16_t, 512> rsqrt_;
        unsigned lut_waddr_r_{0}, wr_cnt_r_{0};
        void store(LutType i_lut_wsel, unsigned waddr, const uint8_t *i_lut_wdata)
        {
            const unsigned lut_9b_waddr = waddr & 0x1FFu, lut_8b_waddr = waddr & 0xFFu;
            if (lut_is_direct(i_lut_wsel))
                for (int i = 0; i < P_.INIT_DW / 8; i++)
                {
                    const unsigned a = lut_8b_waddr + static_cast<unsigned>(i);
                    (i_lut_wsel == LUT_ACT ? act_ : exp_)[a & 0xFFu] = i_lut_wdata[i];
                }
            else
                for (int i = 0; i < P_.INIT_DW / 16; i++)
                {
                    const uint16_t w = static_cast<uint16_t>(i_lut_wdata[2 * i] | (i_lut_wdata[2 * i + 1] << 8));
                    if (i_lut_wsel == LUT_RECIP) { const unsigned a = lut_8b_waddr + static_cast<unsigned>(i); recip_[a & 0xFFu] = w; }
                    else { const unsigned a = lut_9b_waddr + static_cast<unsigned>(i); rsqrt_[a & 0x1FFu] = w; }
                }
        }
    };

    // ---- gvu_lut_lookup_lane.sv: the combinational logic between the pipeline registers --------------------------------
    // Every register also carries in_zero (the most significant bit of the RTL vector).
    struct LutP0 { uint32_t E, x; bool in_zero; };                               // pipe0: {in_zero, leading_bit_idx, i_32b_lookup_idx}
    struct LutP1 { uint32_t E_shift, lut_idx, pwl_frac; bool in_zero; };         // pipe1: {in_zero_r, E_shift, lut_idx, pwl_frac}
    struct LutP2 { uint32_t E_shift; uint16_t lut_16b; int32_t lerp_sub; uint32_t pwl_frac; bool in_zero; }; // pipe2
    struct LutP3 { uint32_t E_shift; uint16_t lut_16b; int32_t lerp_mul; bool in_zero; };                    // pipe3
    struct LutP4 { uint32_t E_shift; uint16_t lut_16b; uint16_t lerp_shift; bool in_zero; };                 // pipe4

    // Before pipe0: clz in Mode 1 gives the position E of the leading one (0 when no bit is set) and the all-zeros flag.
    inline LutP0 lut_stage0(uint32_t i_32b_lookup_idx)
    {
        const ClzOut c = clz(i_32b_lookup_idx, 32, true);
        return {c.idx, i_32b_lookup_idx, c.all_zeros};
    }

    // Between pipe0 and pipe1: index generation.
    //   fine_bits   = (x >> (E - 4))[3:0], E - 4 on 5 bits: for E < 4 the amount is 28..31 and the fine bits are 0
    //   E_sub       = E - i_base (log-scale) or E - i_W (PWL), signed 8 bits
    //   log index   = {E_sub[4:0], fine_bits}
    //   pwl_shift   = E_sub < 0 ? x << -E_sub : x >> E_sub   (the leading one moves to bit i_W)
    //   PWL index   = {E[0], pwl_shift[15:8]}, pwl_frac = pwl_shift[7:0]
    //   E_shift     = E (i_lut_sel == LUT_RECIP) or E >> 1 (otherwise)
    inline LutP1 lut_stage1(const LutP0 &q, LutType i_lut_sel, LutIdxMode i_lut_idx_mode, unsigned i_base, unsigned i_W)
    {
        const uint32_t E = q.E & 31u;
        const uint32_t fine_bits = (q.x >> ((E - 4u) & 31u)) & 15u;
        const int E_sub = static_cast<int>(E) - static_cast<int>((i_lut_idx_mode == LUT_LOG_SCALE ? i_base : i_W) & 31u);
        const uint32_t log_scale_idx = ((static_cast<uint32_t>(E_sub) & 31u) << 4) | fine_bits;
        const uint32_t pwl_shift = E_sub < 0 ? (q.x << (-E_sub)) : (q.x >> E_sub);
        const uint32_t pwl_idx = ((E & 1u) << 8) | ((pwl_shift >> 8) & 0xFFu);
        return {i_lut_sel == LUT_RECIP ? E : (E >> 1), i_lut_idx_mode == LUT_LOG_SCALE ? log_scale_idx : pwl_idx, pwl_shift & 0xFFu, q.in_zero};
    }
    // Between pipe1 and pipe2: memory read at pwl_addr (the registered index) and lerp_sub = lut[i+1] - lut[i] kept on
    // 8 signed bits (a difference outside [-128, 127] wraps).
    inline LutP2 lut_stage2(const LutP1 &q, const LutMem &mem, LutType i_lut_sel)
    {
        const uint16_t d = mem.rdata16(i_lut_sel, q.lut_idx), n = mem.rdata16_nxt(i_lut_sel, q.lut_idx);
        return {q.E_shift, d, static_cast<int32_t>(static_cast<int8_t>(static_cast<uint8_t>(n - d))), q.pwl_frac, q.in_zero};
    }
    // Between pipe2 and pipe3: lerp_mul = lerp_sub * {1'b0, pwl_frac}, signed 17 bits.
    inline LutP3 lut_stage3(const LutP2 &q) { return {q.E_shift, q.lut_16b, q.lerp_sub * static_cast<int32_t>(q.pwl_frac), q.in_zero}; }
    // Between pipe3 and pipe4: lerp_shift = lerp_mul >>> i_lerp_shift (arithmetic), low 16 bits kept.
    inline LutP4 lut_stage4(const LutP3 &q, unsigned i_lerp_shift) { return {q.E_shift, q.lut_16b, static_cast<uint16_t>(q.lerp_mul >> (i_lerp_shift & 31u)), q.in_zero}; }
    // After pipe4: lerp_result = lut[i] + lerp_shift on 16 bits.
    inline uint16_t lut_result(const LutP4 &q) { return static_cast<uint16_t>(q.lut_16b + q.lerp_shift); }

    // Value of one recip / rsqrt lookup with the control inputs held. input_zero: the input was 0 and the result is forced
    // to 0 (not an output of the RTL).
    struct LutValue { uint16_t data; uint32_t e_shift; bool input_zero; uint32_t lut_idx; };
    inline LutValue lut_value(const LutMem &mem, LutType i_lut_sel, LutIdxMode i_lut_idx_mode, unsigned i_base, unsigned i_W, unsigned i_lerp_shift, uint32_t x)
    {
        const LutP1 p1 = lut_stage1(lut_stage0(x), i_lut_sel, i_lut_idx_mode, i_base, i_W);
        if (p1.in_zero) return {0, p1.E_shift, true, p1.lut_idx};
        if (i_lut_idx_mode == LUT_LOG_SCALE) return {mem.rdata16(i_lut_sel, p1.lut_idx), p1.E_shift, false, p1.lut_idx};
        return {lut_result(lut_stage4(lut_stage3(lut_stage2(p1, mem, i_lut_sel)), i_lerp_shift)), p1.E_shift, false, p1.lut_idx};
    }

    // ---- gvu_lut_lookup_lane.sv: cycle model ---------------------------------------------------------------------------
    class LutLookupLane
    {
    public:
        struct In
        {
            bool i_pipe_flush{false}, i_lookup_valid{false};
            LutType i_lut_sel{LUT_RECIP};
            LutIdxMode i_lut_idx_mode{LUT_PWL};
            bool i_lut_compens{false};                                    // act / exp: the 8-bit index is offset by 128
            unsigned i_base{0}, i_W{16}, i_lerp_shift{8};
            uint32_t i_8b_lookup_idx{0}, i_32b_lookup_idx{0};
            bool i_lookup_ready{true};
        };
        struct Out
        {
            bool o_lookup_ready, o_lookup_valid;
            bool o_pipe_almost_done;                                     // log-scale: pipe0 valid; PWL: pipe3 valid
            uint8_t o_8b_lookup_data;
            uint16_t o_16b_lookup_data;
            uint32_t o_e_shift, o_lut_raddr;
        };
        explicit LutLookupLane(const LutParams &p = LutParams()) { p3_.bypass = !p.LERP_MUL_PIPE; p4_.bypass = !p.LERP_SHIFT_PIPE; }

        Out eval(const In &i, const LutMem &mem) const { Wires w; return comb(i, mem, w); }
        // State read for callers without an In of the cycle (has/rtl_ref_gvu_top.h): the valid bit held in pipe0
        // (log-scale) or pipe3 (PWL). Equals Out::o_pipe_almost_done with the default LERP_MUL_PIPE = 1.
        bool o_pipe_almost_done(LutIdxMode m = LUT_PWL) const { return m == LUT_LOG_SCALE ? p0_.valid : p3_.valid; }
        void tick(const In &i, const LutMem &mem, bool i_rstn = true)
        {
            Wires w;
            comb(i, mem, w);
            p0_.tick(i_rstn, i.i_pipe_flush, w.v0_in, w.d0_in, w.r1_out);
            p1_.tick(i_rstn, i.i_pipe_flush, w.v0_out, w.d1_in, w.r2_out);
            p2_.tick(i_rstn, i.i_pipe_flush, w.v1_out, w.d2_in, w.r3_out);
            p3_.tick(i_rstn, i.i_pipe_flush, w.v2_out, w.d3_in, w.r4_out);
            p4_.tick(i_rstn, i.i_pipe_flush, w.v3_out, w.d4_in, i.i_lookup_ready);
        }

    private:
        PipelineReg<LutP0> p0_;
        PipelineReg<LutP1> p1_;
        PipelineReg<LutP2> p2_;
        PipelineReg<LutP3> p3_;
        PipelineReg<LutP4> p4_;
        struct Wires
        {
            bool v0_in, v0_out, v1_out, v2_out, v3_out, v4_out, r0_out, r1_out, r2_out, r3_out, r4_out;
            LutP0 d0_in; LutP1 d1_in; LutP2 d2_in; LutP3 d3_in; LutP4 d4_in;
        };
        Out comb(const In &i, const LutMem &mem, Wires &w) const
        {
            const bool lut_direct = lut_is_direct(i.i_lut_sel);
            // data path, register outputs feeding the next stage
            w.d0_in = lut_stage0(i.i_32b_lookup_idx);
            w.v0_in = i.i_lookup_valid && !lut_direct;
            const LutP0 q0 = p0_.o_data(w.d0_in);
            w.d1_in = lut_stage1(q0, i.i_lut_sel, i.i_lut_idx_mode, i.i_base, i.i_W);
            const LutP1 q1 = p1_.o_data(w.d1_in);
            const uint32_t pwl_addr = q1.lut_idx & 0x1FFu;
            w.d2_in = lut_stage2(q1, mem, i.i_lut_sel);
            const LutP2 q2 = p2_.o_data(w.d2_in);
            w.d3_in = lut_stage3(q2);
            const LutP3 q3 = p3_.o_data(w.d3_in);
            w.d4_in = lut_stage4(q3, i.i_lerp_shift);
            const LutP4 q4 = p4_.o_data(w.d4_in);
            // valid chain, then ready chain from the output back to the input
            w.v0_out = p0_.o_valid(w.v0_in);
            w.v1_out = p1_.o_valid(w.v0_out);
            w.v2_out = p2_.o_valid(w.v1_out);
            w.v3_out = p3_.o_valid(w.v2_out);
            w.v4_out = p4_.o_valid(w.v3_out);
            w.r4_out = p4_.o_ready(w.v3_out, i.i_lookup_ready);
            w.r3_out = p3_.o_ready(w.v2_out, w.r4_out);
            w.r2_out = p2_.o_ready(w.v1_out, w.r3_out);
            w.r1_out = p1_.o_ready(w.v0_out, w.r2_out);
            w.r0_out = p0_.o_ready(w.v0_in, w.r1_out);
            Out o;
            // {1'b0, idx + 128}: the sum is not cut to 8 bits before it is placed on the 9-bit address; the memory uses [7:0]
            o.o_lut_raddr = lut_direct ? (((i.i_8b_lookup_idx & 0xFFu) + (i.i_lut_compens ? 128u : 0u)) & 0x1FFu) : pwl_addr;
            o.o_8b_lookup_data = mem.rdata8(i.i_lut_sel, o.o_lut_raddr);
            // in_zero_1r (pipe1) forces the log-scale result to 0, in_zero_4r (pipe4) the PWL result
            o.o_16b_lookup_data = i.i_lut_idx_mode == LUT_LOG_SCALE ? (q1.in_zero ? static_cast<uint16_t>(0) : mem.rdata16(i.i_lut_sel, o.o_lut_raddr))
                                                                     : (q4.in_zero ? static_cast<uint16_t>(0) : lut_result(q4));
            o.o_lookup_valid = lut_direct ? i.i_lookup_valid : (i.i_lut_idx_mode == LUT_LOG_SCALE ? w.v1_out : w.v4_out);
            o.o_lookup_ready = lut_direct ? i.i_lookup_ready : w.r0_out;
            o.o_e_shift = q4.E_shift & 31u;
            o.o_pipe_almost_done = i.i_lut_idx_mode == LUT_LOG_SCALE ? w.v0_out : w.v3_out;
            return o;
        }
    };

    // ---- gvu_lut_unit.sv -----------------------------------------------------------------------------------------------
    // NUM_LANE lookup lanes on one LutMem. The handshake outputs of the unit are those of lane 0
    // (o_lookup_valid = out_lookup_valid[0], o_lookup_ready = out_lookup_ready[0]); all lanes share the handshake inputs,
    // so they advance together whatever their inputs are.
    class LutUnit
    {
    public:
        struct In
        {
            bool i_pipe_flush{false}, i_lookup_valid{false};
            bool i_lut_wen{false};                                        // table load port
            LutType i_lut_wsel{LUT_ACT};
            std::array<uint8_t, 128> i_lut_wdata{};                        // INIT_DW / 8 bytes used
            LutType i_lut_rsel{LUT_RECIP};
            LutIdxMode i_lut_idx_mode{LUT_PWL};
            bool i_lut_compens{false};
            unsigned i_base{0}, i_W{16}, i_lerp_shift{8};
            V32 i_8b_lookup_idx{}, i_32b_lookup_idx{};
            bool i_lookup_ready{true};
        };
        struct Out
        {
            bool o_lookup_ready, o_lookup_valid, o_init_done, o_pipe_almost_done;
            std::array<uint8_t, GVU_LANES> o_8b_lookup_data;
            std::array<uint16_t, GVU_LANES> o_16b_lookup_data;
            V32 o_e_shift;
        };
        explicit LutUnit(const LutParams &p = LutParams()) : mem(p) { for (auto &l : lane_) l = LutLookupLane(p); }
        bool o_pipe_almost_done(LutIdxMode m = LUT_PWL) const { return lane_[0].o_pipe_almost_done(m); }   // lane 0, as the Out port
        LutMem mem;
        Out eval(const In &i) const
        {
            Out o{};
            o.o_init_done = mem.o_init_done(i.i_lut_wsel);
            for (int l = 0; l < GVU_LANES; l++)
            {
                const LutLookupLane::Out lo = lane_[static_cast<size_t>(l)].eval(lane_in(i, l), mem);
                if (l == 0) { o.o_lookup_valid = lo.o_lookup_valid; o.o_lookup_ready = lo.o_lookup_ready; o.o_pipe_almost_done = lo.o_pipe_almost_done; }
                o.o_8b_lookup_data[static_cast<size_t>(l)] = lo.o_8b_lookup_data;
                o.o_16b_lookup_data[static_cast<size_t>(l)] = lo.o_16b_lookup_data;
                o.o_e_shift[static_cast<size_t>(l)] = lo.o_e_shift;
            }
            return o;
        }
        void tick(const In &i, bool i_rstn = true)
        {
            for (int l = 0; l < GVU_LANES; l++) lane_[static_cast<size_t>(l)].tick(lane_in(i, l), mem, i_rstn);
            mem.tick(i_rstn, i.i_lut_wen, i.i_lut_wsel, i.i_lut_wdata.data());   // after the lanes: they read the tables before the edge
        }

    private:
        std::array<LutLookupLane, GVU_LANES> lane_;
        static LutLookupLane::In lane_in(const In &i, int l)
        {
            LutLookupLane::In li;
            li.i_pipe_flush = i.i_pipe_flush; li.i_lookup_valid = i.i_lookup_valid; li.i_lut_sel = i.i_lut_rsel;
            li.i_lut_idx_mode = i.i_lut_idx_mode; li.i_lut_compens = i.i_lut_compens; li.i_base = i.i_base; li.i_W = i.i_W; li.i_lerp_shift = i.i_lerp_shift;
            li.i_8b_lookup_idx = i.i_8b_lookup_idx[static_cast<size_t>(l)]; li.i_32b_lookup_idx = i.i_32b_lookup_idx[static_cast<size_t>(l)];
            li.i_lookup_ready = i.i_lookup_ready;
            return li;
        }
    };
} // namespace rtl_ref_gvu

#endif // RTL_REF_GVU_LUT_H
