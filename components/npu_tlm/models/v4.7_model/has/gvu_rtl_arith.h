// gvu_rtl_arith.h -- optional routing of the arithmetic of the functional vector-unit model through the units ported from
// the RTL (has/rtl_ref_gvu_fu.h, has/rtl_ref_gvu_lut.h). Off unless the environment variable HAS_ARITH_RTL is set to a
// non-zero value; with it off nothing computed by the model changes.
// When on, has::requant, has::dequant, has::add_sat32, has::LutIndirect and has::LutLogScale first compute their own
// result and then call the functions below. If the operands fit the containers of the units, the value handed back to the
// pipeline is the one of the ported units and the call is counted as "same" or "differs"; if they do not fit, the model
// value is kept and the call is counted as "not expressible". A table is printed when the program exits
// ("[arith-rtl] ..." lines), with the first difference of every function.
// Composition of the units per function (the order of the pipeline drawings; the RTL of the pipelines does not exist yet):
//   requant      Mul 32x32 -> Round-Shift 64 (saturate, int16) -> Add 16-bit (zero point, wraps) -> Clamp int8
//   dequant      Sub 16-bit (zero point) -> Mul 32x16 -> Round-Shift 64 (saturate, int32)
//   add          Add 32-bit (wraps)
//   LUT PWL      gvu_lut_lookup_lane, tables of 256 (recip) / 512 (rsqrt) entries, W 16, interpolation shift 8
//   LUT log      gvu_lut_lookup_lane in the log-scale mode with the Base of the model object
// Not routed (the model's own arithmetic is used): the exponential table and the comparator of SOFTMAX, the wide
// multiplications and round-shifts of LAYERNORM, the sums of FUSED_ATTN, average pooling, the activation table of the
// output stage.
#ifndef HAS_RTL_ARITH_H
#define HAS_RTL_ARITH_H

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include "has/rtl_ref_gvu_fu.h"
#include "has/rtl_ref_gvu_lut.h"

namespace has
{
    class RtlArith
    {
    public:
        enum Fn { F_REQUANT = 0, F_DEQUANT, F_ADD32, F_LUT_PWL_RECIP, F_LUT_PWL_RSQRT, F_LUT_LOG, F_N };

        static bool on() { return inst().on_; }

        // round_mode: has::RoundMode. sat16: the int16 narrowing of the requant saturates (has::SAT16).
        static bool requant(int64_t x, int64_t S, int s, int zp, int round_mode, bool sat16, int model, int *out)
        {
            namespace R = rtl_ref_gvu;
            Stat &st = inst().st_[F_REQUANT];
            st.calls++;
            if (!fit(x, 32) || !fit(S, 32) || s < 0 || s > 63 || !sat16 || !fit(zp, 16) || round_mode < 0 || round_mode > 6) { st.not_expr++; return false; }
            const uint64_t p = R::vmul32x32_lane(R::MA_32X32, static_cast<uint32_t>(x), static_cast<uint32_t>(S));
            const R::RsOut rs = R::vroundshift64_lane(p, static_cast<unsigned>(s), map_round(round_mode), R::NARROW_SATURATE, R::NW_16);
            const uint32_t sum = R::adder32_lane(R::ADD_16, 0, 0, false, rs.out_data & 0xFFFFu, static_cast<uint32_t>(zp) & 0xFFFFu) & 0xFFFFu;
            const int h = R::sext32(R::clamp_lane(sum, 0xFF80u, 0x007Fu, 16, true), 16);
            if (h != model) st.miss("x=%lld S=%lld s=%d zp=%d model=%d units=%d", static_cast<long long>(x), static_cast<long long>(S), s, zp, model, h);
            *out = h;
            return true;
        }
        // sub_before: the zero point is subtracted before the multiplication (has::SUB_BEFORE).
        static bool dequant(int x8, int zp, int64_t S, int s, int round_mode, bool sub_before, int32_t model, int32_t *out)
        {
            namespace R = rtl_ref_gvu;
            Stat &st = inst().st_[F_DEQUANT];
            st.calls++;
            if (!sub_before || !fit(x8, 16) || !fit(zp, 16) || !fit(S, 32) || s < 0 || s > 63 || round_mode < 0 || round_mode > 6) { st.not_expr++; return false; }
            const uint32_t d = R::adder32_lane(R::ADD_16, 0, 1, false, static_cast<uint32_t>(x8) & 0xFFFFu, static_cast<uint32_t>(zp) & 0xFFFFu) & 0xFFFFu;
            const uint64_t p = R::vmul32x32_lane(R::MA_32X16, static_cast<uint32_t>(S), d);
            const int32_t h = static_cast<int32_t>(R::vroundshift64_lane(p, static_cast<unsigned>(s), map_round(round_mode), R::NARROW_SATURATE, R::NW_32).out_data);
            if (h != model) st.miss("x8=%d zp=%d S=%lld s=%d model=%d units=%d", x8, zp, static_cast<long long>(S), s, model, h);
            *out = h;
            return true;
        }
        static int32_t add32(int32_t a, int32_t b, int32_t model)
        {
            namespace R = rtl_ref_gvu;
            Stat &st = inst().st_[F_ADD32];
            st.calls++;
            const int32_t h = static_cast<int32_t>(R::adder32_lane(R::ADD_32, 0, 0, false, static_cast<uint32_t>(a), static_cast<uint32_t>(b)));
            if (h != model) st.miss("a=%d b=%d model=%d units=%d", a, b, model, h);
            return h;
        }
        // PWL mode: r, e and zero flag of the lookup lane. model_e is not compared for x = 0.
        static void lut_pwl(const rtl_ref_gvu::LutMem &mem, bool rsqrt, uint32_t x, int model_r, int model_e, int16_t *r, int *e, bool *zero)
        {
            namespace R = rtl_ref_gvu;
            Stat &st = inst().st_[rsqrt ? F_LUT_PWL_RSQRT : F_LUT_PWL_RECIP];
            st.calls++;
            const R::LutValue h = R::lut_value(mem, rsqrt ? R::LUT_RSQRT : R::LUT_RECIP, R::LUT_PWL, 0, 16, 8, x);
            if (static_cast<uint16_t>(model_r) != h.data || (x != 0 && static_cast<uint32_t>(model_e) != h.e_shift))
                st.miss("x=%u model r=%d E=%d units r=%d E=%u", x, model_r, model_e, static_cast<int16_t>(h.data), h.e_shift);
            *r = static_cast<int16_t>(h.data); *e = static_cast<int>(h.e_shift); *zero = h.input_zero;
        }
        // Log-scale mode: the table entry (the exponent shift comes from the instruction, not from the lane).
        static int16_t lut_log(const rtl_ref_gvu::LutMem &mem, bool rsqrt, unsigned base, uint32_t x, int model_r)
        {
            namespace R = rtl_ref_gvu;
            Stat &st = inst().st_[F_LUT_LOG];
            st.calls++;
            const R::LutValue h = R::lut_value(mem, rsqrt ? R::LUT_RSQRT : R::LUT_RECIP, R::LUT_LOG_SCALE, base, 16, 8, x);
            if (static_cast<uint16_t>(model_r) != h.data) st.miss("x=%u base=%u model r=%d units r=%d", x, base, model_r, static_cast<int16_t>(h.data));
            return static_cast<int16_t>(h.data);
        }
        // A lookup the units cannot express (a table that is not 256 / 512 entries deep).
        static void lut_not_expressible(bool rsqrt) { Stat &st = inst().st_[rsqrt ? F_LUT_PWL_RSQRT : F_LUT_PWL_RECIP]; st.calls++; st.not_expr++; }

    private:
        struct Stat
        {
            uint64_t calls{0}, not_expr{0}, differ{0};
            char first[160]{0};
            template <typename... A> void miss(const char *fmt, A... a) { if (!differ) std::snprintf(first, sizeof first, fmt, a...); differ++; }
        };
        bool on_{false};
        Stat st_[F_N];

        static RtlArith &inst() { static RtlArith p; return p; }
        RtlArith()
        {
            const char *v = std::getenv("HAS_ARITH_RTL");
            on_ = v && std::atoi(v) != 0;
            if (on_) std::atexit(&RtlArith::report);
        }
        static bool fit(int64_t v, int bits) { const int64_t h = static_cast<int64_t>(1) << (bits - 1); return v >= -h && v < h; }
        static rtl_ref_gvu::RoundMode map_round(int m)
        {
            namespace R = rtl_ref_gvu;
            static const R::RoundMode t[7] = {R::RND_FLOOR, R::RND_NEAR_UP, R::RND_NEAR_AWAY, R::RND_NEAR_EVEN, R::RND_CEIL, R::RND_TRUNC, R::RND_AWAY};
            return t[m];
        }
        static void report()
        {
            static const char *fn[F_N] = {"requant", "dequant", "add 32-bit", "LUT PWL recip", "LUT PWL rsqrt", "LUT log-scale"};
            const RtlArith &p = inst();
            std::printf("[arith-rtl] arithmetic routed through the ported units (HAS_ARITH_RTL): function | calls | through the units | "
                        "differ from the model arithmetic | not expressible | first difference\n");
            for (int i = 0; i < F_N; i++)
            {
                const Stat &s = p.st_[i];
                if (s.calls == 0) continue;
                std::printf("[arith-rtl] %-14s | calls %12llu | units %12llu | differ %10llu | not expressible %10llu | %s\n", fn[i],
                            static_cast<unsigned long long>(s.calls), static_cast<unsigned long long>(s.calls - s.not_expr),
                            static_cast<unsigned long long>(s.differ), static_cast<unsigned long long>(s.not_expr), s.differ ? s.first : "-");
            }
            std::fflush(stdout);
        }
    };
} // namespace has

#endif // HAS_RTL_ARITH_H
