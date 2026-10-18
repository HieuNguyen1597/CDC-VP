// gvu_lut.h -- LUT unit of the HW drawing "LUT": LUT_direct (act / exp, in gvu_quant.h) and LUT_indirect (recip / rsqrt).
// Spec: has/HAS_IFACE_RCE.md §2. Interpretation of the drawing (the bit widths all agree with it; open points R1-R4 there):
//   E    = 31 - clz(x)                       exponent = MSB position (v32int5 "E_shift" output)
//   y    = x << (31 - E)                     mantissa left-aligned (bits below the MSB; zero-filled when E is small)
//   idx  = y[30:23]  (8 bits)                recip index;  rsqrt index = {E[0], y[30:23]} (9 bits)
//   frac = y[22:15]  (8 bits)                interpolation fraction
//   r    = T[idx] + (((T[idx+1] - T[idx]) * frac) >> 8)      delta fits int9, product int17, ">> 8" as drawn (floor)
//   recip(x) ~= r * 2^-(Q + E)               rsqrt(x) ~= r * 2^-(Q + floor(E/2))     with table format Q14 (default)
// Tables (loaded by the host, HAS §9.1 "LUT_recip, LUT_rsqrt"): recip T[i] = round(2^Q / (1 + i/256)), i = 0..256;
// rsqrt T[p*257 + i] = round(2^Q / sqrt((1 + i/256) * 2^p)), p = E[0] in {0,1}, i = 0..256.
//
// MAS V1.0 ("Vector LUT Unit", figure "LUT Pipeline", table "GVU's LUT memory map") names three index modes:
//   direct     act / exp, 256 x 1 B, 1 stage                              -> has::lut_direct (gvu_quant.h)
//   log-scale  index = {E - Base, 4 fine bits below the MSB}, the entry is the result, 3 stages -> has::LutLogScale
//   PWL        the input is shifted by E so 16 bits sit below the MSB: index = bits [15:8] (rsqrt: {E[0], index}),
//              fraction = bits [7:0], linear interpolation between lut[i] and lut[i+1], 6 stages -> has::LutIndirect
// So LutIndirect below (the class the reference runs use, called "log-scale" in the vectors of tools/fe/fe_ref_gvu.py) is
// the PWL mode of the MAS. Table depths in the MAS: recip 256 x 2 B, rsqrt 512 x 2 B; LutIndirect keeps 257 entries per
// segment by default and takes the MAS depths with mas_depth = true (the entry after the last one is then held).
// LutPwl further down is the earlier reading of the Base / W inputs (segment index with interpolation); it stays for its
// vectors and is not a mode of the MAS.
#ifndef HAS_LUT_H
#define HAS_LUT_H

#include <cmath>
#include <cstdint>
#include <vector>
#include "has/gvu_rtl_arith.h"

namespace has
{
    struct LutResult
    {
        int16_t r;     // mantissa, table format Q
        int e_shift;   // E (recip) or floor(E/2) (rsqrt)
        bool zero_in;  // x == 0 (undefined input; r saturated)
    };

    // Pipeline depth of the LUT unit per index mode (MAS V1.0, "Vector LUT Unit").
    constexpr int kLutStagesDirect = 1, kLutStagesLogScale = 3, kLutStagesPwl = 6;

    class LutIndirect
    {
    public:
        static constexpr int IDX_BITS = 8, FRAC_BITS = 8, N = (1 << IDX_BITS) + 1; // 257 entries per segment

        // mas_depth = true: recip 256 entries, rsqrt 512 entries (MAS table sizes). lut[i+1] of the last recip entry and of
        // the last entry of the odd-exponent rsqrt segment is outside the memory: the last entry is held (delta 0). The
        // even-exponent rsqrt segment continues into the odd one, whose first entry equals the entry it needs.
        // zero_out0 = true: an input of 0 returns r = 0 with the zero flag (RTL: the lookup lane drives 0 for a zero
        // input); false: the largest value (reference runs).
        // With HAS_ARITH_RTL (has/gvu_rtl_arith.h) and mas_depth = true the lookups go through the lookup lane ported from
        // the RTL, on a table memory loaded with the two images below.
        explicit LutIndirect(int q = 14, bool mas_depth = false, bool zero_out0 = false)
            : q_(q), mas_depth_(mas_depth), zero0_(zero_out0), recip_(N), rsqrt_(2 * N)
        {
            for (int i = 0; i < N; i++)
            {
                const double m = 1.0 + static_cast<double>(i) / (1 << IDX_BITS);
                recip_[i] = static_cast<int16_t>(std::lround(std::ldexp(1.0 / m, q_)));
                for (int p = 0; p < 2; p++)
                    rsqrt_[p * N + i] = static_cast<int16_t>(std::lround(std::ldexp(1.0 / std::sqrt(m * (1 << p)), q_)));
            }
            if (RtlArith::on() && mas_depth_)
            {
                const std::vector<int16_t> ir = mas_recip_image(), is = mas_rsqrt_image();
                rtl_mem_.load16(rtl_ref_gvu::LUT_RECIP, std::vector<uint16_t>(ir.begin(), ir.end()));
                rtl_mem_.load16(rtl_ref_gvu::LUT_RSQRT, std::vector<uint16_t>(is.begin(), is.end()));
            }
        }

        int q() const { return q_; }
        bool mas_depth() const { return mas_depth_; }
        const std::vector<int16_t> &recip_table() const { return recip_; }
        const std::vector<int16_t> &rsqrt_table() const { return rsqrt_; }
        // The memory images of the MAS: LUT_recip 256 x 16 bit, LUT_rsqrt 512 x 16 bit ({E[0], index} addressing).
        std::vector<int16_t> mas_recip_image() const { return std::vector<int16_t>(recip_.begin(), recip_.begin() + (N - 1)); }
        std::vector<int16_t> mas_rsqrt_image() const
        {
            std::vector<int16_t> t(rsqrt_.begin(), rsqrt_.begin() + (N - 1));
            t.insert(t.end(), rsqrt_.begin() + N, rsqrt_.begin() + N + (N - 1));
            return t;
        }

        LutResult recip(uint32_t x) const { return lookup(x, false); }
        LutResult rsqrt(uint32_t x) const { return lookup(x, true); }

        // Real value represented by a result (for accuracy evaluation only)
        double value(const LutResult &res) const { return std::ldexp(static_cast<double>(res.r), -(q_ + res.e_shift)); }

    private:
        int q_;
        bool mas_depth_, zero0_;
        std::vector<int16_t> recip_, rsqrt_;
        rtl_ref_gvu::LutMem rtl_mem_;                              // loaded only with HAS_ARITH_RTL and mas_depth

        static int msb(uint32_t x) { return 31 - __builtin_clz(x); }

        LutResult lookup(uint32_t x, bool is_rsqrt) const
        {
            const LutResult m = lookup_model(x, is_rsqrt);
            if (!RtlArith::on()) return m;
            if (!mas_depth_) { RtlArith::lut_not_expressible(is_rsqrt); return m; }
            LutResult h = m;
            RtlArith::lut_pwl(rtl_mem_, is_rsqrt, x, m.r, m.e_shift, &h.r, &h.e_shift, &h.zero_in);
            return h;
        }
        LutResult lookup_model(uint32_t x, bool is_rsqrt) const
        {
            if (x == 0)
                return {static_cast<int16_t>(zero0_ ? 0 : INT16_MAX), 0, true};
            const int E = msb(x);
            const uint32_t y = x << (31 - E);
            const int idx = static_cast<int>((y >> (31 - IDX_BITS)) & ((1u << IDX_BITS) - 1));
            const int frac = static_cast<int>((y >> (31 - IDX_BITS - FRAC_BITS)) & ((1u << FRAC_BITS) - 1));
            const int16_t *T = is_rsqrt ? &rsqrt_[(E & 1) * N] : recip_.data();
            const bool hold = mas_depth_ && idx == N - 2 && (!is_rsqrt || (E & 1));
            const int delta = hold ? 0 : T[idx + 1] - T[idx];       // int9 per the drawing
            const int r = T[idx] + ((delta * frac) >> FRAC_BITS);   // int17 product, arithmetic >> 8
            return {static_cast<int16_t>(r), is_rsqrt ? (E >> 1) : E, false};
        }
    };
    // Log-scale index mode of the MAS (figure "LUT Pipeline", upper index path): E = msb(x); the "Sub v32int8" forms
    // E - Base, truncated to 4 bits (recip, 8-bit index) or 5 bits (rsqrt, 9-bit index); "Fine bit extract v32int4" takes the
    // 4 bits below the MSB; index = {E - Base, fine}; the table entry is the result (no interpolation, 3 stages). The
    // exponent is inside the index, so the entries carry it and the shift that follows uses the E_shift of the instruction.
    // Truncation is as drawn: E - Base outside [0, 2^seg_bits) wraps; in_range() tells whether an input is covered.
    // As in the RTL (gvu_lut_lookup_lane): the fine bits are x >> (E - 4), so an input below 16 (E < 4) has no fine bits
    // (0); the interpolation difference lut[i+1] - lut[i] of the PWL mode is a signed 8-bit value there, and an input of 0
    // returns 0 with the zero flag.
    // make_table() is the host-side generator used by the unit check: T[seg*16 + f] = round(2^q_total / g(x_mid)) saturated
    // to int16, x_mid = 2^(Base + seg) * (1 + (f + 0.5) / 16), g = x (recip) or sqrt(x) (rsqrt).
    class LutLogScale
    {
    public:
        static constexpr int FINE_BITS = 4;
        LutLogScale(const std::vector<int32_t> &table, int base, int e_shift, bool is_rsqrt = false)
            : T_(table), base_(base), e_shift_(e_shift), rsqrt_(is_rsqrt)
        {
            if (RtlArith::on()) rtl_mem_.load16(rsqrt_ ? rtl_ref_gvu::LUT_RSQRT : rtl_ref_gvu::LUT_RECIP, std::vector<uint16_t>(T_.begin(), T_.end()));
        }

        int seg_bits() const { return rsqrt_ ? 5 : 4; }
        int entries() const { return 1 << (seg_bits() + FINE_BITS); }
        bool in_range(uint32_t x) const
        {
            if (x == 0) return false;
            const int seg = (31 - __builtin_clz(x)) - base_;
            return seg >= 0 && seg < (1 << seg_bits());
        }
        int index(uint32_t x) const
        {
            if (x == 0) return 0;
            const int E = 31 - __builtin_clz(x);
            const int seg = (E - base_) & ((1 << seg_bits()) - 1);
            const int fine = E >= FINE_BITS ? static_cast<int>((x >> (E - FINE_BITS)) & ((1u << FINE_BITS) - 1)) : 0;
            return (seg << FINE_BITS) | fine;
        }
        LutResult lookup(uint32_t x) const
        {
            LutResult m = {static_cast<int16_t>(x == 0 ? 0 : T_[size_t(index(x))]), e_shift_, x == 0};
            if (RtlArith::on()) m.r = RtlArith::lut_log(rtl_mem_, rsqrt_, static_cast<unsigned>(base_) & 31u, x, m.r);
            return m;
        }
        LutResult recip(uint32_t x) const { return lookup(x); }

        static std::vector<int32_t> make_table(bool is_rsqrt, int base, int q_total)
        {
            const int n = 1 << ((is_rsqrt ? 5 : 4) + FINE_BITS);
            std::vector<int32_t> t(static_cast<size_t>(n));
            for (int i = 0; i < n; i++)
            {
                const double xm = std::ldexp(1.0 + ((i & 15) + 0.5) / 16.0, base + (i >> FINE_BITS));
                const double v = std::ldexp(1.0 / (is_rsqrt ? std::sqrt(xm) : xm), q_total);
                t[size_t(i)] = static_cast<int32_t>(v >= 32767.0 ? 32767 : std::lround(v));
            }
            return t;
        }

    private:
        std::vector<int32_t> T_;
        int base_, e_shift_;
        bool rsqrt_;
        rtl_ref_gvu::LutMem rtl_mem_;                              // loaded only with HAS_ARITH_RTL
    };

    // Segment index with interpolation: the reading of the Base / W inputs made before the MAS (vector-unit note, figure 12).
    // Not a mode of the MAS (see the header); kept because its golden vectors exist. Base, W and E_shift come from the
    // instruction; the host loads a 513-entry table (n_idx + 1). Golden: tools/fe/fe_ref_gvu.py pwl_index / lut_pwl.
    //   form SEG (default, as drawn): E = msb(x), y = x << (31-E); seg = E - Base; idx = seg << W | y[30:31-W];
    //                                  frac = y[30-W:23-W];  E < Base -> (0, 0)
    //   form UNIFORM (alternative):   off = x - Base; idx = off >> W; frac = 8 bits below; off < 0 -> (0, 0)
    //   idx >= n_idx -> (n_idx-1, 255);  r = T[idx] + ((T[idx+1] - T[idx]) * frac >> 8)  (same int9 interpolation, floor)
    class LutPwl
    {
    public:
        enum Form { SEG = 0, UNIFORM = 1 };
        LutPwl(const std::vector<int32_t> &table, int base, int w, int e_shift, Form form = SEG)
            : T_(table), base_(base), w_(w), e_shift_(e_shift), form_(form) {}

        LutResult recip(uint32_t x) const
        {
            int idx, frac;
            index(x, idx, frac);
            const int delta = T_[size_t(idx) + 1] - T_[size_t(idx)];
            return {static_cast<int16_t>(T_[size_t(idx)] + ((delta * frac) >> 8)), e_shift_, x == 0};
        }
        void index(uint32_t x, int &idx, int &frac) const
        {
            const int n_idx = static_cast<int>(T_.size()) - 1;
            idx = 0; frac = 0;
            if (form_ == SEG)
            {
                if (x == 0) return;
                const int E = 31 - __builtin_clz(x);
                const uint32_t y = x << (31 - E);
                const int seg = E - base_;
                if (seg < 0) return;
                idx = (seg << w_) | (w_ ? static_cast<int>((y >> (31 - w_)) & ((1u << w_) - 1)) : 0);
                frac = static_cast<int>((y >> (23 - w_)) & 0xFF);
            }
            else
            {
                const int64_t off = int64_t(x) - base_;
                if (off < 0) return;
                idx = static_cast<int>(off >> w_);
                frac = static_cast<int>((w_ >= 8 ? (off >> (w_ - 8)) : (off << (8 - w_))) & 0xFF);
            }
            if (idx >= n_idx) { idx = n_idx - 1; frac = 255; }
        }

    private:
        std::vector<int32_t> T_;
        int base_, w_, e_shift_;
        Form form_;
    };
} // namespace has

#endif // HAS_LUT_H
