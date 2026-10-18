// gvu_range_probe.h -- value-range survey of the vector-unit arithmetic. Observation only: it records, for the values that
// pass through the functional model (has/gvu_quant.h, gvu_softmax.h, gvu_layernorm.h, gvu_fused_attn.h, gvu_elemwise.h),
// the smallest and largest value, the signed width they need, and how many of them do not fit the container the RTL of
// the vector unit gives to that value (multiplier operand widths, 64 / 72-bit round-shift inputs, shift fields, int16 /
// int32 registers). Nothing computed by the model depends on it.
// Off unless the environment variable HAS_RANGE_PROBE is set to a non-zero value; then a table is printed when the
// program exits ("[range] ..." lines). The requant points are kept per pipeline (RangeProbe::Scope).
#ifndef HAS_RANGE_PROBE_H
#define HAS_RANGE_PROBE_H

#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace has
{
    class RangeProbe
    {
    public:
        typedef __int128 i128;
        // Pipelines that call has::requant()
        enum Ctx { C_GEMM = 0, C_ADD, C_ATTN_QK, C_SOFTMAX, C_ATTN_AV, C_POOL, C_N };
        // Points of has::requant(), one set per pipeline
        enum Req { R_X = 0, R_S, R_PROD, R_SHIFT, R_ROUNDED, R_SUM_ZP, R_N };
        // Other points
        enum Pt
        {
            DEQ_A = 0, DEQ_S, DEQ_PROD, DEQ_SHIFT, DEQ_V, ADD_SUM,
            SM_E, SM_SIGMA, SM_R, SM_P, SM_SH, SM_T,
            LN_S, LN_D, LN_DD, LN_V, LN_VPRE, LN_M0VAR, LN_VPROD, LN_TSVAR, LN_U, LN_TS7, LN_W, LN_R, LN_RD, LN_M0DIV, LN_ACC, LN_SHDIV,
            LN_GN, LN_M0MUL, LN_GPROD, LN_TSMUL, LN_YSUM,
            FA_QROW, FA_KCOL, FA_VCOL, FA_ACC_QK, FA_ACC_AV, AVG_ACC, AVG_S, AVG_PROD, AVG_SHIFT,
            P_N
        };
        struct Stat { uint64_t n{0}, over{0}; i128 lo{0}, hi{0}; };

        static bool on() { return inst().on_; }
        static int &ctx() { static int c = C_GEMM; return c; }
        struct Scope
        {
            int prev;
            explicit Scope(int c) : prev(ctx()) { ctx() = c; }
            ~Scope() { ctx() = prev; }
        };
        // limit > 0: signed container of `limit` bits; limit < 0: unsigned container of -limit bits (shift fields, table inputs)
        static void req(int id, i128 v) { if (on()) inst().add(inst().req_[ctx()][id], v, kReq[id].limit); }
        static void pt(int id, i128 v) { if (on()) inst().add(inst().pt_[id], v, kPt[id].limit); }

    private:
        struct Def { const char *name; int limit; const char *container; };
        static constexpr Def kReq[R_N] = {
            {"multiplicand", 32, "Mul v32 32x32 operand, 32 bits"},
            {"scale", 32, "Mul v32 32x32 operand, 32 bits"},
            {"product", 64, "round-shift input, 64 bits"},
            {"shift", -6, "round-shift amount, 6 bits (0..63)"},
            {"rounded value", 16, "narrowed to int16 (v32int16 register)"},
            {"narrowed + zero point", 16, "16-bit adder, wraps"},
        };
        static constexpr Def kPt[P_N] = {
            {"dequant: x - zero point", 16, "v32int16 register"},
            {"dequant: scale", 16, "Mul 32x16: 16-bit operand"},
            {"dequant: product", 64, "round-shift input, 64 bits"},
            {"dequant: shift", -6, "round-shift amount, 6 bits"},
            {"dequant: rounded value", 32, "v32int32 register"},
            {"add: sum of two dequantized values", 32, "32-bit adder, wraps"},
            {"softmax: e = LUT_exp entry", -8, "8-bit table"},
            {"softmax: sigma = sum of e", -32, "LUT index input, 32 bits unsigned"},
            {"softmax: r = LUT_recip result", -16, "16-bit table"},
            {"softmax: r * e", 24, "Mul 16x8 product, 24 bits"},
            {"softmax: shift E + Q - P", -6, "round-shift amount, 6 bits; a negative value is a left shift"},
            {"softmax: rounded r * e (requant multiplicand)", 32, "v32int32 register"},
            {"layernorm: S = sum of x", 32, "int32 accumulator"},
            {"layernorm: D = H * x - S", 32, "v32int32 register"},
            {"layernorm: D * D", 64, "Mul 32x32 product"},
            {"layernorm: V = sum of D * D", 64, "int64 accumulator"},
            {"layernorm: V >> Pre_Shift", 40, "Mul 40x32: 40-bit operand"},
            {"layernorm: M0_var", 32, "Mul 40x32: 32-bit operand"},
            {"layernorm: (V >> Pre_Shift) * M0_var", 72, "round-shift input, 72 bits"},
            {"layernorm: TS_var", -7, "round-shift amount, 7 bits; at 72 and above only the sign is left"},
            {"layernorm: (v - Z_var) * M0_7 + E_bias", 64, "round-shift input, 64 bits"},
            {"layernorm: TS_7", -6, "round-shift amount, 6 bits"},
            {"layernorm: w = LUT_rsqrt input", -32, "LUT index input, 32 bits unsigned"},
            {"layernorm: r = LUT_rsqrt result", -16, "16-bit table"},
            {"layernorm: r * D", 48, "48-bit operand of Mul 48x40"},
            {"layernorm: M0_div", 40, "40-bit operand of Mul 48x40 (32 bits in the 40x32 mode)"},
            {"layernorm: r * D * M0_div", 72, "round-shift input, 72 bits"},
            {"layernorm: TS_div + E", -7, "round-shift amount, 7 bits; at 72 and above only the sign is left"},
            {"layernorm: (n - Z_div) * gamma", 24, "Mul 16x8 product, 24 bits"},
            {"layernorm: M0_mul", 32, "32-bit multiplier operand"},
            {"layernorm: (n - Z_div) * gamma * M0_mul", 64, "round-shift input, 64 bits"},
            {"layernorm: TS_mul", -6, "round-shift amount, 6 bits"},
            {"layernorm: t - Z_mul + beta + Z_out", 32, "32-bit adder"},
            {"attention: row sum of Q", 16, "v32int16 accumulator"},
            {"attention: column sum of K", 16, "v32int16 accumulator"},
            {"attention: column sum of V", 16, "v32int16 accumulator"},
            {"attention: corrected Q.K sum (requant multiplicand)", 32, "partial-sum word, 32 bits"},
            {"attention: corrected A.V sum (requant multiplicand)", 32, "partial-sum word, 32 bits"},
            {"avg pool: window sum", 16, "int16 accumulator"},
            {"avg pool: Avg_Scale", 16, "16-bit multiplier operand"},
            {"avg pool: sum * Avg_Scale", 64, "round-shift input, 64 bits"},
            {"avg pool: Avg_Shift", -6, "round-shift amount, 6 bits"},
        };
        bool on_{false};
        Stat req_[C_N][R_N];
        Stat pt_[P_N];

        static RangeProbe &inst() { static RangeProbe p; return p; }
        RangeProbe()
        {
            const char *v = std::getenv("HAS_RANGE_PROBE");
            on_ = v && std::atoi(v) != 0;
            if (on_) std::atexit(&RangeProbe::report);
        }
        static bool fits(i128 v, int limit)
        {
            if (limit < 0) return v >= 0 && v < (static_cast<i128>(1) << (-limit));
            const i128 h = static_cast<i128>(1) << (limit - 1);
            return v >= -h && v < h;
        }
        void add(Stat &s, i128 v, int limit)
        {
            if (s.n == 0) { s.lo = s.hi = v; }
            else { if (v < s.lo) s.lo = v; if (v > s.hi) s.hi = v; }
            s.n++;
            if (!fits(v, limit)) s.over++;
        }
        // signed bits needed for [lo, hi]
        static int bits(i128 lo, i128 hi)
        {
            int b = 1;
            while (b < 127 && !(lo >= -(static_cast<i128>(1) << (b - 1)) && hi < (static_cast<i128>(1) << (b - 1)))) b++;
            return b;
        }
        static void dec(char *buf, size_t n, i128 v)
        {
            char t[48]; int k = 0;
            const bool neg = v < 0;
            unsigned __int128 u = neg ? -static_cast<unsigned __int128>(v) : static_cast<unsigned __int128>(v);
            do { t[k++] = static_cast<char>('0' + static_cast<int>(u % 10)); u /= 10; } while (u && k < 46);
            size_t o = 0;
            if (neg && o + 1 < n) buf[o++] = '-';
            while (k > 0 && o + 1 < n) buf[o++] = t[--k];
            buf[o] = 0;
        }
        static void line(const char *group, const Def &d, const Stat &s)
        {
            if (s.n == 0) return;
            char lo[52], hi[52];
            dec(lo, sizeof lo, s.lo); dec(hi, sizeof hi, s.hi);
            std::printf("[range] %-10s | %-46s | n %12llu | min %s | max %s | signed bits %d | outside %llu | %s\n", group, d.name,
                        static_cast<unsigned long long>(s.n), lo, hi, bits(s.lo, s.hi), static_cast<unsigned long long>(s.over), d.container);
        }
        static void report()
        {
            static const char *cn[C_N] = {"gemm_fused", "add", "attn_qk", "softmax", "attn_av", "avg_pool"};
            const RangeProbe &p = inst();
            std::printf("[range] value-range survey (HAS_RANGE_PROBE): group | value | count | min | max | signed bits needed | values outside the container | container\n");
            for (int c = 0; c < C_N; c++)
                for (int i = 0; i < R_N; i++)
                {
                    char g[40]; std::snprintf(g, sizeof g, "req/%s", cn[c]);
                    line(g, kReq[i], p.req_[c][i]);
                }
            for (int i = 0; i < P_N; i++) line("point", kPt[i], p.pt_[i]);
            std::fflush(stdout);
        }
    };
} // namespace has

#endif // HAS_RANGE_PROBE_H
