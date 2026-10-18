// rtl_ref_gvu_requant.h -- cycle-level Requant pipeline of the vector-unit specification and the GEMM_FUSED epilogue built
// on it, composed from the units ported from the RTL (has/rtl_ref_gvu_fu.h, has/rtl_ref_gvu_lut.h):
//   GvuRequantPipe    Mul v32 int32 x int32 -> Round-Shift 64 (saturate, int16) -> Add zero point (16 bit, wraps) -> Clamp
//                     to int8. The same chain is the "Requant pipeline" box of the LayerNorm, Softmax and FUSED_ATTN
//                     drawings.
//   GvuGemmEpilogue   Requant pipeline -> v32int8 register -> LUT_act (direct table) -> skip_act mux -> PSUM write.
// Pure C++17, no SystemC; nothing here is used by the instruction path. The RTL has no module for these pipelines yet: the
// stage composition below follows the register cuts of the drawings, and the points listed under "Choices" are this
// model's. The functional model of the same arithmetic is has::requant and has::lut_direct (has/gvu_quant.h).
//
// Stream interface: one beat (32 lanes) per cycle, a valid bit, no back-pressure. In carries the beat and its parameters;
// Out carries the beat that leaves in the same cycle. eval() depends only on the state of the module (and, for the
// epilogue, on i_act_en); tick() is the clock edge.
//
// Depths (clock cycles from the cycle a beat is presented to the cycle it is on the output), from the cuts of the drawing
// and the measured latencies of the units:
//   Requant         multiplier 32x32 4 + v32int64 register 1, round-shift 1 + v32int16 register 1, Add zero point +
//                   v32int16 register 1, Clamp 0                                                              = 8
//   GEMM epilogue   Requant 8, v32int8 register 1, LUT_act 0                                                  = 9
//
// Choices of this model (each one is a question for the hardware side):
//   E1  Out_Scale and Out_Shift may change with every beat (one output channel per beat). The scale is used in the cycle
//       the beat is presented; the shift is presented with the beat and travels with it to the round-shift stage, five
//       cycles later.
//   E2  The zero point, the rounding mode and i_act_en are not pipelined: every stage uses the value present in the cycle
//       it works, so they must be held while beats are in flight.
//   E3  No back-pressure: the write to the PSUM SRAM always accepts. The multiplier is used in one mode, so it streams one
//       beat per cycle.
//   E4  The "Zero point skip" mux of the Requant drawing is not modelled.
//   E5  LUT_act is read through the lookup lanes of the LUT unit with the index offset by 128. The table is filled
//       directly (LutMem::load8): the load port of the RTL as written reaches only the first half of the table.
//       The skip_act mux sits after the table, so a beat takes the same number of cycles with and without activation.
#ifndef RTL_REF_GVU_REQUANT_H
#define RTL_REF_GVU_REQUANT_H

#include <array>
#include <cstdint>
#include <vector>
#include "has/rtl_ref_gvu_elemwise.h"
#include "has/rtl_ref_gvu_fu.h"
#include "has/rtl_ref_gvu_lut.h"

namespace rtl_ref_gvu
{
    // ---- Requant pipeline ----------------------------------------------------------------------------------------------
    class GvuRequantPipe
    {
    public:
        struct In
        {
            bool i_valid{false};
            V32 i_data{};                    // v32int32
            int32_t i_scale{1};              // Out_Scale of this beat (E1)
            unsigned i_shift{0};             // Out_Shift of this beat, 0 .. 63 (E1)
            int i_zp{0};                     // zero point, 16 bit (E2)
            RoundMode i_round{RND_NEAR_UP};  // (E2)
            uint32_t i_addr{0};              // carried with the beat
            uint32_t i_lane_mask{0xFFFFFFFFu};
        };
        struct Out
        {
            bool o_valid{false};
            V32 o_data{};                    // v32int8, one value per lane in the low 8 bits
            uint32_t o_addr{0}, o_lane_mask{0};
            bool o_busy{false};              // a beat is in flight
        };
        static constexpr int kLatMul = 4;                                       // 32x32 (measured on the RTL)
        static constexpr int kLatency = (kLatMul + 1) + 2 + 1;

        Out eval(const In &) const
        {
            static const Beat none{};
            Out o;
            o.o_valid = zp_.out_valid(false);
            const Beat &q = zp_.out(none);
            // Clamp v32int32 to int8 on the registered 16-bit sum
            for (size_t l = 0; l < GVU_LANES; l++) o.o_data[l] = clamp_lane(q.v[l], 0xFF80u, 0x007Fu, 16, true) & 0xFFu;
            o.o_addr = q.addr; o.o_lane_mask = q.mask;
            o.o_busy = !(mul_.empty() && rs_.empty() && zp_.empty());
            return o;
        }
        void tick(const In &i, bool i_rstn = true)
        {
            if (!i_rstn) { mul_.clear(); rs_.clear(); zp_.clear(); return; }
            static const Prod none_p{};
            static const Beat none{};
            // multiplier (value at the input, latency as a delay) and its v32int64 register
            Prod po{};
            if (i.i_valid)
            {
                for (size_t l = 0; l < GVU_LANES; l++) po.p[l] = vmul32x32_lane(MA_32X32, i.i_data[l], static_cast<uint32_t>(i.i_scale));
                po.shift = i.i_shift; po.addr = i.i_addr; po.mask = i.i_lane_mask;
            }
            // round-shift, saturating to int16, and the v32int16 register
            Beat rs{};
            const bool mul_v = mul_.out_valid(false);
            if (mul_v)
            {
                const Prod &q = mul_.out(none_p);
                for (size_t l = 0; l < GVU_LANES; l++) rs.v[l] = vroundshift64_lane(q.p[l], q.shift, i.i_round, NARROW_SATURATE, NW_16).out_data & 0xFFFFu;
                rs.addr = q.addr; rs.mask = q.mask;
            }
            // Add zero point on 16 bits (wraps) and the v32int16 register
            Beat zs{};
            const bool rs_v = rs_.out_valid(false);
            if (rs_v)
            {
                const Beat &q = rs_.out(none);
                for (size_t l = 0; l < GVU_LANES; l++) zs.v[l] = adder32_lane(ADD_16, 0, 0, false, q.v[l], static_cast<uint32_t>(i.i_zp) & 0xFFFFu) & 0xFFFFu;
                zs.addr = q.addr; zs.mask = q.mask;
            }
            zp_.tick(rs_v, zs);
            rs_.tick(mul_v, rs);
            mul_.tick(i.i_valid, po);
        }

    private:
        struct Prod { V64 p{}; unsigned shift{0}; uint32_t addr{0}, mask{0}; };
        struct Beat { V32 v{}; uint32_t addr{0}, mask{0}; };
        ValidPipe<Prod> mul_{kLatMul + 1};                                      // multiplier + v32int64 register
        ValidPipe<Beat> rs_{2};                                                 // round-shift + v32int16 register
        ValidPipe<Beat> zp_{1};                                                 // Add zero point + v32int16 register
    };

    // ---- GEMM_FUSED epilogue -------------------------------------------------------------------------------------------
    class GvuGemmEpilogue
    {
    public:
        struct In : GvuRequantPipe::In
        {
            bool i_act_en{true};             // 0: skip_act, the requantized value is written (E2)
        };
        typedef GvuRequantPipe::Out Out;
        static constexpr int kLatency = GvuRequantPipe::kLatency + 1;

        explicit GvuGemmEpilogue(const LutParams &p = LutParams()) : lut_(p) {}
        // The activation table, 256 entries indexed by the int8 value + 128.
        void load_act(const int8_t *table)
        {
            std::vector<uint8_t> t(256);
            for (size_t n = 0; n < 256; n++) t[n] = static_cast<uint8_t>(table[n]);
            lut_.mem.load8(LUT_ACT, t);
        }

        Out eval(const In &i) const
        {
            static const Out none{};
            Out o = r0_.out(none);
            o.o_valid = r0_.out_valid(false);
            o.o_busy = o.o_valid || rq_.eval(i).o_busy;
            if (o.o_valid && i.i_act_en)
            {
                LutUnit::In li;
                li.i_lookup_valid = true; li.i_lut_rsel = LUT_ACT; li.i_lut_compens = true;
                li.i_8b_lookup_idx = o.o_data;
                const LutUnit::Out lo = lut_.eval(li);
                for (size_t l = 0; l < GVU_LANES; l++) o.o_data[l] = lo.o_8b_lookup_data[l];
            }
            return o;
        }
        void tick(const In &i, bool i_rstn = true)
        {
            if (!i_rstn) { rq_.tick(i, false); r0_.clear(); return; }
            const Out q = rq_.eval(i);
            r0_.tick(q.o_valid, q);                                             // v32int8 register after the Requant pipeline
            rq_.tick(i);
        }

    private:
        GvuRequantPipe rq_;
        ValidPipe<Out> r0_{1};
        LutUnit lut_;
    };
} // namespace rtl_ref_gvu

#endif // RTL_REF_GVU_REQUANT_H
