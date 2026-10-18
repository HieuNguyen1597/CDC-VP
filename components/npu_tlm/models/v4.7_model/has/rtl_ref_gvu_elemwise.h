// rtl_ref_gvu_elemwise.h -- cycle-level pipelines of the ELEM_WISE drawing of the vector-unit specification, composed
// from the units ported from the RTL (has/rtl_ref_gvu_fu.h) around the ported Scratchpad and load / store unit:
//   GvuMaxPoolPipe   max pool: comparator in accumulate mode over a window, result register, write
//   GvuResAddPipe    residual add: stage 1 (subtract zero point, dequant on two multipliers, 32-bit add) and stage 2
//                    (requant) through the Scratchpad
// Pure C++17, no SystemC; nothing here is used by the instruction path. The RTL has no controller for these pipelines
// yet: the control below (start, burst length, done) and the points listed under "Choices" are this model's.
// The functional model of the same operations is has/gvu_elemwise.h.
//
// Interface of both modules: In carries the configuration, a start pulse and the Scratchpad read data of the cycle;
// Out carries the requests for the load / store unit (GvuVlsu::In, complete), the write data and busy / done. The
// requests depend only on the state of the module, so a bench may evaluate the module once for the requests, the load /
// store unit and the Scratchpad for the read data, and the module again for the write data (ElemwiseBench does this).
// The configuration must be stable from the cycle before the start pulse to done.
//
// Depths (clock cycles), from the cuts of the drawing and the measured latencies of the units; a Scratchpad read takes
// two clocks, of which the second is the "read pipeline register" of the drawing:
//   max pool      read 2, comparator 0, v32int8 register 1                                       write at +3 of the last read
//   add stage 1   read 2, Sub + v32int16 register 1, multiplier + v32int64 register (A: 48x40 in the 40x32 mode 4 + 1,
//                 B: 32x32 in the 32x16 mode 2 + 1), round-shift 1 + v32int32 register 1, Add    write at +10 of its read
//   add stage 2   read 2, multiplier 32x32 4 + 1, round-shift 1 + v32int16 register 1, Add zero point + register 1,
//                 Clamp                                                                          write at +10 of its read
//
// Choices of this model (each one is a question for the hardware side):
//   C1  Window data of the max pool: the loader writes, for every output vector, the win input vectors of its window
//       as consecutive rows of vbank#0 (window-major). The load / store unit of the RTL has no stride mode.
//   C2  Branch B of add stage 1 is two cycles shorter than branch A (32x16 against 40x32); its result waits two cycles
//       in a register so that both operands of the adder belong to the same row.
//   C3  The v32int8 port of vbank#1 selects the part of the 1024-bit row with the address of the cycle the data is
//       used, two clocks after the access. The module takes the whole row and selects the part with its own copy of
//       the address of the access.
//   C4  Scratchpad byte masks are one bit per byte (ScratchpadParams::wmask_as_written = false in the bench).
//   C5  Write address: the address register of the load / store unit follows the start address while there is no
//       request. The max pool writes one row per window, so it presents the address of the next row as start address.
//   C6  The adder of stage 1 wraps on 32 bits (the functional model saturates); stage 2 narrows to int16 by saturation
//       and adds the zero point on 16 bits.
#ifndef RTL_REF_GVU_ELEMWISE_H
#define RTL_REF_GVU_ELEMWISE_H

#include <array>
#include <cstdint>
#include <vector>
#include "has/rtl_ref_gvu_fu.h"
#include "has/rtl_ref_gvu_scratchpad.h"
#include "has/rtl_ref_gvu_vlsu.h"

namespace rtl_ref_gvu
{
    // N registers in series carrying a valid bit; N = 0 is a wire.
    template <typename T> class ValidPipe
    {
    public:
        explicit ValidPipe(int n = 1) : v_(static_cast<size_t>(n), false), d_(static_cast<size_t>(n)) {}
        int depth() const { return static_cast<int>(v_.size()); }
        bool out_valid(bool in_valid) const { return v_.empty() ? in_valid : v_.back(); }
        const T &out(const T &in) const { return d_.empty() ? in : d_.back(); }
        void tick(bool in_valid, const T &in)
        {
            for (size_t i = v_.size(); i-- > 1;) { v_[i] = v_[i - 1]; d_[i] = d_[i - 1]; }
            if (!v_.empty()) { v_[0] = in_valid; d_[0] = in; }
        }
        bool empty() const { for (bool b : v_) if (b) return false; return true; }
        void clear() { for (size_t i = 0; i < v_.size(); i++) v_[i] = false; }

    private:
        std::vector<bool> v_;
        std::vector<T> d_;
    };

    inline V32 lanes_s8(const SpBytes &row) { V32 v{}; for (int l = 0; l < GVU_LANES; l++) v[static_cast<size_t>(l)] = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(row[static_cast<size_t>(l)]))); return v; }
    inline V32 lanes_s32(const SpBytes &row)
    {
        V32 v{};
        for (int l = 0; l < GVU_LANES; l++)
        {
            const size_t o = static_cast<size_t>(4 * l);
            v[static_cast<size_t>(l)] = static_cast<uint32_t>(row[o]) | (static_cast<uint32_t>(row[o + 1]) << 8) | (static_cast<uint32_t>(row[o + 2]) << 16) | (static_cast<uint32_t>(row[o + 3]) << 24);
        }
        return v;
    }
    inline SpBytes row_i8(const V32 &v) { SpBytes r(GVU_LANES); for (int l = 0; l < GVU_LANES; l++) r[static_cast<size_t>(l)] = static_cast<uint8_t>(v[static_cast<size_t>(l)]); return r; }
    inline SpBytes row_i32(const V32 &v)
    {
        SpBytes r(4 * GVU_LANES);
        for (int l = 0; l < GVU_LANES; l++)
            for (int b = 0; b < 4; b++) r[static_cast<size_t>(4 * l + b)] = static_cast<uint8_t>(v[static_cast<size_t>(l)] >> (8 * b));
        return r;
    }

    // ---- max pool ------------------------------------------------------------------------------------------------------
    class GvuMaxPoolPipe
    {
    public:
        struct In
        {
            bool i_start{false};
            unsigned i_out_rows{0};          // output vectors (rows of 32 lanes)
            unsigned i_win{1};               // input vectors per output vector (k * k)
            unsigned i_src_row{0};           // first row of vbank#0
            unsigned i_dst_row{0};           // first row of vbank#1 seen through its v32int8 port (0 .. 4 * depth - 1)
            uint32_t i_last_lane_mask{0xFFFFFFFFu};   // lanes written in the last output row
            SpBytes i_vbank0_rdata;          // Scratchpad read data of this cycle
        };
        struct Out
        {
            GvuVlsu::In vlsu;
            SpBytes o_vbank1_wdata_2;        // v32int8 row for port 2 of vbank#1
            bool o_busy{false}, o_done{false};
        };
        explicit GvuMaxPoolPipe(const FuParams &p = FuParams()) : cmp_(p) {}

        Out eval(const In &i) const
        {
            Out o;
            o.vlsu.i_vbank0_rd = st_ == RUN && rd_left_ > 0;
            o.vlsu.i_vbank0_start_addr = i.i_src_row;
            o.vlsu.i_vbank0_lane_wmask = 0xFFFFFFFFu;
            o.vlsu.i_vbank1_port_sel = 2;
            o.vlsu.i_vbank1_wr = wr_.out_valid(false);
            o.vlsu.i_vbank1_start_addr = i.i_dst_row + wr_idx_;                 // C5
            o.vlsu.i_vbank1_lane_wmask = (wr_idx_ + 1 == i.i_out_rows) ? i.i_last_lane_mask : 0xFFFFFFFFu;
            o.o_vbank1_wdata_2 = row_i8(wr_.out(V32{}));
            o.o_busy = st_ != IDLE;
            o.o_done = st_ == DONE;
            return o;
        }
        void tick(const In &i, bool i_rstn = true)
        {
            if (!i_rstn) { st_ = IDLE; rd_left_ = 0; wr_idx_ = 0; win_cnt_ = 0; rdv_.clear(); wr_.clear(); cmp_.reset(); return; }
            const bool rd = st_ == RUN && rd_left_ > 0;
            const bool data_valid = rdv_.out_valid(false);
            // comparator, accumulate mode over the window (first / last from the position in the window)
            Vcmp32::In c{};
            c.in_valid = data_valid; c.in_cmp_mode = CMP_ACCUMULATE; c.in_op = CMP_MAX; c.in_elem_w = EW_8; c.in_signed = true; c.in_src1_bcast = false;
            c.in_first = win_cnt_ == 0; c.in_last = win_cnt_ + 1 == i.i_win; c.out_ready = true;
            if (data_valid) c.in_src0 = lanes_s8(i.i_vbank0_rdata);
            const Vcmp32::Out co = cmp_.eval(c);
            const bool wrote = wr_.out_valid(false);
            cmp_.tick(c);
            wr_.tick(co.out_valid, co.out_res);                                 // v32int8 register after the comparator
            rdv_.tick(rd, 0);
            if (data_valid) win_cnt_ = (win_cnt_ + 1 == i.i_win) ? 0 : win_cnt_ + 1;
            if (wrote) wr_idx_++;
            if (rd) rd_left_--;
            if (st_ == IDLE) { if (i.i_start) { st_ = RUN; rd_left_ = static_cast<uint64_t>(i.i_out_rows) * i.i_win; wr_idx_ = 0; win_cnt_ = 0; } }
            else if (st_ == RUN) { if (wrote && wr_idx_ == i.i_out_rows) st_ = DONE; }
            else st_ = IDLE;                                                    // DONE lasts one cycle
        }

    private:
        enum St { IDLE, RUN, DONE };
        St st_{IDLE};
        uint64_t rd_left_{0};
        unsigned wr_idx_{0}, win_cnt_{0};
        ValidPipe<int> rdv_{2};                                                 // Scratchpad read latency
        ValidPipe<V32> wr_{1};
        Vcmp32 cmp_;
    };

    // ---- residual add --------------------------------------------------------------------------------------------------
    class GvuResAddPipe
    {
    public:
        struct In
        {
            bool i_start{false};
            unsigned i_rows{0};              // vectors of 32 elements
            unsigned i_a_row{0};             // tensor A, first row of vbank#0; the result is written back to these rows
            unsigned i_b_row{0};             // tensor B, first row of vbank#1 through its v32int8 port
            unsigned i_sum_row{0};           // sum, first row of vbank#2 (v32int32)
            int i_zp_a{0}, i_zp_b{0}, i_zp_out{0};
            int32_t i_scale_a{1}, i_scale_b{1}, i_scale_out{1};
            unsigned i_shift_a{0}, i_shift_b{0}, i_shift_out{0};
            RoundMode i_round{RND_NEAR_UP};
            uint32_t i_last_lane_mask{0xFFFFFFFFu};
            SpBytes i_vbank0_rdata, i_vbank1_rdata_0, i_vbank2_rdata_0;         // vbank#1: the whole 1024-bit row (C3)
        };
        struct Out
        {
            GvuVlsu::In vlsu;
            SpBytes o_vbank2_wdata_0;        // sum, v32int32
            SpBytes o_vbank0_wdata;          // result, v32int8
            bool o_busy{false}, o_done{false};
            int o_stage{0};                  // 0 idle, 1, 2
        };
        static constexpr int kLatMulA = 4, kLatMulB = 2, kLatMulOut = 4;        // 40x32, 32x16, 32x32 (measured on the RTL)

        Out eval(const In &i) const
        {
            Out o;
            const bool s1 = st_ == ST1, s2 = st_ == ST2;
            o.vlsu.i_vbank0_rd = s1 && rd_left_ > 0;
            o.vlsu.i_vbank0_wr = s2 && res_.out_valid(false);
            o.vlsu.i_vbank0_start_addr = i.i_a_row;
            o.vlsu.i_vbank0_lane_wmask = (s2 && wr_idx_ + 1 == i.i_rows) ? i.i_last_lane_mask : 0xFFFFFFFFu;
            o.vlsu.i_vbank1_port_sel = 2;
            o.vlsu.i_vbank1_rd = s1 && rd_left_ > 0;
            o.vlsu.i_vbank1_start_addr = i.i_b_row;
            o.vlsu.i_vbank1_lane_wmask = 0xFFFFFFFFu;
            o.vlsu.i_vbank2_port_sel = false;
            o.vlsu.i_vbank2_rd = s2 && rd_left_ > 0;
            o.vlsu.i_vbank2_wr = s1 && sum_valid();
            o.vlsu.i_vbank2_start_addr = i.i_sum_row;
            o.vlsu.i_vbank2_lane_wmask = (s1 && wr_idx_ + 1 == i.i_rows) ? i.i_last_lane_mask : 0xFFFFFFFFu;
            o.o_vbank2_wdata_0 = row_i32(sum());
            // stage 2 tail: Clamp v32int32 to int8 on the registered 16-bit sum
            V32 y{};
            const V32 &q = res_.out(V32{});
            for (size_t l = 0; l < GVU_LANES; l++) y[l] = clamp_lane(q[l], 0xFF80u, 0x007Fu, 16, true);
            o.o_vbank0_wdata = row_i8(y);
            o.o_busy = st_ != IDLE;
            o.o_done = st_ == DONE;
            o.o_stage = s1 ? 1 : (s2 ? 2 : 0);
            return o;
        }
        void tick(const In &i, bool i_rstn = true)
        {
            if (!i_rstn) { st_ = IDLE; clear(); return; }
            const bool s1 = st_ == ST1, s2 = st_ == ST2;
            const bool rd = (s1 || s2) && rd_left_ > 0;
            const bool dv = rdv_.out_valid(false);                              // read data of this cycle is valid
            const bool wrote = s1 ? sum_valid() : (s2 && res_.out_valid(false));

            // ---- stage 1 --------------------------------------------------------------------------------------------
            // Sub v32int16 #1-0 / #1-1, registers v32int16 #0 / #1
            V32 da{}, db{};
            if (s1 && dv)
            {
                const V32 a = lanes_s8(i.i_vbank0_rdata);
                const unsigned part = b_part_.out(0);                           // C3: part of the row at the access
                SpBytes brow(GVU_LANES);
                for (int l = 0; l < GVU_LANES; l++) brow[static_cast<size_t>(l)] = i.i_vbank1_rdata_0[static_cast<size_t>(GVU_LANES * static_cast<int>(part) + l)];
                const V32 b = lanes_s8(brow);
                for (size_t l = 0; l < GVU_LANES; l++)
                {
                    da[l] = adder32_lane(ADD_16, 0, 1, false, a[l] & 0xFFFFu, static_cast<uint32_t>(i.i_zp_a) & 0xFFFFu) & 0xFFFFu;
                    db[l] = adder32_lane(ADD_16, 0, 1, false, b[l] & 0xFFFFu, static_cast<uint32_t>(i.i_zp_b) & 0xFFFFu) & 0xFFFFu;
                }
            }
            // multipliers (value at the input, latency as a delay) and their v32int64 registers
            std::array<u128, GVU_LANES> pa{};
            V64 pb{};
            if (sub_.out_valid(false))
            {
                const V32 &qa = sub_.out(Pair{}).a, &qb = sub_.out(Pair{}).b;
                for (size_t l = 0; l < GVU_LANES; l++)
                {
                    const uint64_t a40 = static_cast<uint64_t>(static_cast<int64_t>(sext32(qa[l], 16))) & 0xFFFFFFFFFFull;
                    pa[l] = vmul48x40_lane(MB_40X32, a40, static_cast<uint32_t>(i.i_scale_a)).out_prod;
                    pb[l] = vmul32x32_lane(MA_32X16, static_cast<uint32_t>(i.i_scale_b), qb[l]);
                }
            }
            // round-shift (72 bit for A, 64 bit for B), saturating to int32, and the v32int32 registers
            V32 ra{}, rb{};
            if (mula_.out_valid(false)) for (size_t l = 0; l < GVU_LANES; l++) ra[l] = vroundshift72_lane(mula_.out(Prod72{})[l], i.i_shift_a, i.i_round, NARROW_SATURATE, NW_32).out_data;
            if (mulb_.out_valid(false)) for (size_t l = 0; l < GVU_LANES; l++) rb[l] = vroundshift64_lane(mulb_.out(V64{})[l], i.i_shift_b, i.i_round, NARROW_SATURATE, NW_32).out_data;

            // ---- stage 2 --------------------------------------------------------------------------------------------
            V64 po{};
            if (s2 && dv)
            {
                const V32 x = lanes_s32(i.i_vbank2_rdata_0);
                for (size_t l = 0; l < GVU_LANES; l++) po[l] = vmul32x32_lane(MA_32X32, x[l], static_cast<uint32_t>(i.i_scale_out));
            }
            V32 rs{}, zs{};
            if (mulo_.out_valid(false)) for (size_t l = 0; l < GVU_LANES; l++) rs[l] = vroundshift64_lane(mulo_.out(V64{})[l], i.i_shift_out, i.i_round, NARROW_SATURATE, NW_16).out_data & 0xFFFFu;
            if (rso_.out_valid(false)) for (size_t l = 0; l < GVU_LANES; l++) zs[l] = adder32_lane(ADD_16, 0, 0, false, rso_.out(V32{})[l], static_cast<uint32_t>(i.i_zp_out) & 0xFFFFu) & 0xFFFFu;

            // ---- clock edge -----------------------------------------------------------------------------------------
            const bool mula_v = mula_.out_valid(false), mulb_v = mulb_.out_valid(false), sub_v = sub_.out_valid(false);
            const bool mulo_v = mulo_.out_valid(false), rso_v = rso_.out_valid(false);
            res_.tick(rso_v, zs);
            rso_.tick(mulo_v, rs);
            mulo_.tick(s2 && dv, po);
            rb_.tick(mulb_v, rb);
            ra_.tick(mula_v, ra);
            mulb_.tick(sub_v, pb);
            mula_.tick(sub_v, pa);
            sub_.tick(s1 && dv, Pair{da, db});
            b_part_.tick(rd && s1, (i.i_b_row + rd_idx_) & 3u);
            rdv_.tick(rd, 0);
            if (rd) { rd_left_--; rd_idx_++; }
            if (wrote) wr_idx_++;
            switch (st_)
            {
            case IDLE: if (i.i_start) { clear(); st_ = ST1; rd_left_ = i.i_rows; } break;
            case ST1:  if (wrote && wr_idx_ == i.i_rows) { st_ = GAP; } break;
            case GAP:  st_ = ST2; rd_left_ = i.i_rows; rd_idx_ = 0; wr_idx_ = 0; break;   // one cycle without a request: the address registers reload
            case ST2:  if (wrote && wr_idx_ == i.i_rows) st_ = DONE; break;
            default:   st_ = IDLE; break;
            }
        }

    private:
        enum St { IDLE, ST1, GAP, ST2, DONE };
        struct Pair { V32 a{}, b{}; };
        typedef std::array<u128, GVU_LANES> Prod72;
        St st_{IDLE};
        unsigned rd_left_{0}, rd_idx_{0}, wr_idx_{0};
        ValidPipe<int> rdv_{2};                                                 // Scratchpad read latency
        ValidPipe<unsigned> b_part_{2};
        ValidPipe<Pair> sub_{1};                                                // v32int16 registers #0 / #1
        ValidPipe<Prod72> mula_{kLatMulA + 1};                                  // multiplier #0 + v32int64 register #0
        ValidPipe<V64> mulb_{kLatMulB + 1};                                     // multiplier #1 + v32int64 register #1
        ValidPipe<V32> ra_{2};                                                  // round-shift + v32int32 register (branch A)
        ValidPipe<V32> rb_{2 + (kLatMulA - kLatMulB)};                          // the same for branch B, plus the wait of C2
        ValidPipe<V64> mulo_{kLatMulOut + 1};                                   // stage 2: multiplier + v32int64 register
        ValidPipe<V32> rso_{2};                                                 // round-shift + v32int16 register
        ValidPipe<V32> res_{1};                                                 // Add zero point + v32int16 register

        bool sum_valid() const { return ra_.out_valid(false) && rb_.out_valid(false); }
        V32 sum() const
        {
            V32 s{};
            const V32 &a = ra_.out(V32{}), &b = rb_.out(V32{});
            for (size_t l = 0; l < GVU_LANES; l++) s[l] = adder32_lane(ADD_32, 0, 0, false, a[l], b[l]);   // C6: wraps
            return s;
        }
        void clear()
        {
            rd_left_ = 0; rd_idx_ = 0; wr_idx_ = 0;
            rdv_.clear(); b_part_.clear(); sub_.clear(); mula_.clear(); mulb_.clear(); ra_.clear(); rb_.clear(); mulo_.clear(); rso_.clear(); res_.clear();
        }
    };

    // ---- bench: a module with the ported load / store unit and Scratchpad ----------------------------------------------
    // One step() is one clock cycle. The Scratchpad uses one mask bit per byte (C4).
    class ElemwiseBench
    {
    public:
        ElemwiseBench() : sp(params()), vlsu(params()) {}
        GvuScratchpad sp;
        GvuVlsu vlsu;
        uint64_t cycles{0};
        static ScratchpadParams params() { ScratchpadParams p; p.wmask_as_written = false; return p; }

        // Direct access to the buffer of the accelerator side (what a DMA transfer would have written / will read).
        void poke_v0(unsigned row, const SpBytes &d) { sp.sram(0, 0).poke(row, d); }
        SpBytes peek_v0(unsigned row) { return sp.sram(0, 0).word(row); }
        SpBytes peek_v1(unsigned row) { return sp.sram(1, 0).word(row); }
        SpBytes peek_v2(unsigned row) { return sp.sram(2, 0).word(row); }
        void poke_v1(unsigned row, const SpBytes &d) { sp.sram(1, 0).poke(row, d); }

        template <typename M> typename M::Out step(M &m, typename M::In in)
        {
            const typename M::Out req = m.eval(in);                             // requests: state only
            const GvuVlsu::Out v = vlsu.eval(req.vlsu);
            GvuScratchpad::In s = sp_in(v);
            const GvuScratchpad::Out rd = sp.out(s);
            set_rdata(in, rd, 0);
            const typename M::Out o = m.eval(in);                               // write data of this cycle
            set_wdata(s, o, 0);
            m.tick(in);
            vlsu.tick(o.vlsu);
            sp.tick(s);
            cycles++;
            return o;
        }

    private:
        static GvuScratchpad::In sp_in(const GvuVlsu::Out &v)
        {
            GvuScratchpad::In s;
            s.i_vbank0_en = v.o_vbank0_en; s.i_vbank0_wen = v.o_vbank0_wen; s.i_vbank0_addr = v.o_vbank0_addr;
            s.i_vbank0_wmask = SpBytes{static_cast<uint8_t>(v.o_vbank0_wmask), static_cast<uint8_t>(v.o_vbank0_wmask >> 8), static_cast<uint8_t>(v.o_vbank0_wmask >> 16), static_cast<uint8_t>(v.o_vbank0_wmask >> 24)};
            s.i_vbank1_port_sel = v.o_vbank1_port_sel;
            s.i_vbank1_en_0 = v.o_vbank1_en_0; s.i_vbank1_wen_0 = v.o_vbank1_wen_0; s.i_vbank1_addr_0 = v.o_vbank1_addr_0; s.i_vbank1_wmask_0 = v.o_vbank1_wmask_0;
            s.i_vbank1_en_2 = v.o_vbank1_en_2; s.i_vbank1_wen_2 = v.o_vbank1_wen_2; s.i_vbank1_addr_2 = v.o_vbank1_addr_2;
            s.i_vbank1_wmask_2 = SpBytes{static_cast<uint8_t>(v.o_vbank1_wmask_2), static_cast<uint8_t>(v.o_vbank1_wmask_2 >> 8), static_cast<uint8_t>(v.o_vbank1_wmask_2 >> 16), static_cast<uint8_t>(v.o_vbank1_wmask_2 >> 24)};
            s.i_vbank2_port_sel = v.o_vbank2_port_sel;
            s.i_vbank2_en_0 = v.o_vbank2_en_0; s.i_vbank2_wen_0 = v.o_vbank2_wen_0; s.i_vbank2_addr_0 = v.o_vbank2_addr_0; s.i_vbank2_wmask_0 = v.o_vbank2_wmask_0;
            s.i_vbank0_wdata.assign(GVU_LANES, 0); s.i_vbank1_wdata_0.assign(4 * GVU_LANES, 0); s.i_vbank1_wdata_2.assign(GVU_LANES, 0); s.i_vbank2_wdata_0.assign(4 * GVU_LANES, 0);
            return s;
        }
        // overloads chosen by the fields the module has
        template <typename I> static auto set_rdata(I &in, const GvuScratchpad::Out &rd, int) -> decltype(in.i_vbank2_rdata_0, void())
        {
            in.i_vbank0_rdata = rd.o_vbank0_rdata; in.i_vbank1_rdata_0 = rd.o_vbank1_rdata_0; in.i_vbank2_rdata_0 = rd.o_vbank2_rdata_0;
        }
        template <typename I> static void set_rdata(I &in, const GvuScratchpad::Out &rd, long) { in.i_vbank0_rdata = rd.o_vbank0_rdata; }
        template <typename O> static auto set_wdata(GvuScratchpad::In &s, const O &o, int) -> decltype(o.o_vbank2_wdata_0, void())
        {
            s.i_vbank2_wdata_0 = o.o_vbank2_wdata_0; s.i_vbank0_wdata = o.o_vbank0_wdata;
        }
        template <typename O> static void set_wdata(GvuScratchpad::In &s, const O &o, long) { s.i_vbank1_wdata_2 = o.o_vbank1_wdata_2; }
    };
} // namespace rtl_ref_gvu

#endif // RTL_REF_GVU_ELEMWISE_H
