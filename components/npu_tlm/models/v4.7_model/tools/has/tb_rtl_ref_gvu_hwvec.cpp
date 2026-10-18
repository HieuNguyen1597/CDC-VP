// tb_rtl_ref_gvu_hwvec.cpp -- runs the test vectors of the hardware team (LUT unit and Softmax, decimal text files) on the
// ported units of has/rtl_ref_gvu_fu.h and has/rtl_ref_gvu_lut.h. Pure C++17.
//   usage: tb_rtl_ref_gvu_hwvec <test dir>
//   <test dir>/lut_data/{exp,recip,rsqrt}_{lut,input,golden_output}.txt
//   <test dir>/softmax/{vec,tile}/{input,lut_exp,lut_recip,golden_output}.txt   (rows of 64 int8 values)
// LUT: exp is read directly; recip and rsqrt go through the lookup of the ported LUT unit in the interpolating mode with
// the settings of the pipeline controller of the RTL (index width 16, interpolation shift 8).
// Softmax: the data path of the three stages of the RTL top level, composed here from the ported units, one row at a time:
//   stage 1  m = max of the row (int8)
//   stage 2  d = m - x on 8 bits, e = LUT_exp[d], sigma = sum of e (int32)
//   stage 3  (r, E) = LUT_recip(sigma); p = r x e (16 x 16, e zero-extended); t = round-shift 72 of p by E (nearest, ties
//            away from zero, wrap to 32 bits); q = t x ATTN_Scale (32 x 32); u = round-shift 64 of q by ATTN_Shift
//            (nearest, ties away, wrap to 16 bits); a = clamp(u + Z_ATTN on 16 bits, -128, 127)
// with Z_ATTN = -128, ATTN_Scale = 2^30, ATTN_Shift = 37 as in the testbench of the RTL. Only values are checked here,
// not cycles. The same row is also computed with ties rounded up, to tell whether the vectors separate the two modes.
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>
#include "has/rtl_ref_gvu_fu.h"
#include "has/rtl_ref_gvu_lut.h"

namespace R = rtl_ref_gvu;

static bool read_ints(const std::string &p, std::vector<long> &v)
{
    std::ifstream f(p);
    if (!f) { std::printf("cannot read %s\n", p.c_str()); return false; }
    v.clear();
    for (long x; f >> x;) v.push_back(x);
    return true;
}

static int g_fail = 0;
static void verdict(const char *name, size_t n, size_t bad, const std::string &first)
{
    std::printf("%-58s %7zu values, different %zu %s%s\n", name, n, bad, bad ? "FAIL " : "PASS", first.c_str());
    if (bad || n == 0) g_fail++;
}

static const unsigned kW = 16, kLerp = 8, kBase = 7;   // gvu_pipe_controller: o_pwl_W, o_lerp_shift, o_logscale_base

static int8_t softmax_elem(const R::LutMem &mem, uint32_t r, uint32_t E, uint32_t e, R::RoundMode rm)
{
    const uint64_t p = R::vmul32x32_lane(R::MA_16X16, r & 0xFFFFu, e & 0xFFu) & 0xFFFFFFFFull;            // low product: r x e
    const R::u128 p72 = static_cast<R::u128>(static_cast<__int128>(static_cast<int32_t>(p)));             // sign-extended to 72 bits
    const uint32_t t = R::vroundshift72_lane(p72 & ((static_cast<R::u128>(1) << 72) - 1), E, rm, R::NARROW_WRAP, R::NW_32).out_data;
    const uint64_t scale40 = static_cast<uint64_t>(static_cast<int64_t>(1) << 30) & 0xFFFFFFFFFFull;      // ATTN_Scale sign-extended to 40 bits
    const uint64_t q = static_cast<uint64_t>(R::vmul48x40_lane(R::MB_32X32_16X8, t, scale40).out_prod);    // low 64 bits of the 32 x 32 product
    const uint32_t u = R::vroundshift64_lane(q, 37, rm, R::NARROW_WRAP, R::NW_16).out_data & 0xFFFFu;
    const uint32_t s = R::adder32_lane(R::ADD_16, 0, 0, false, u, static_cast<uint32_t>(-128) & 0xFFFFu) & 0xFFFFu;
    const int32_t c = R::sext32(R::clamp_lane(static_cast<uint32_t>(R::sext32(s, 16)), static_cast<uint32_t>(-128), 127u, 32, true), 32);
    (void)mem;
    return static_cast<int8_t>(c);
}

static void run_softmax(const std::string &dir, const char *name)
{
    std::vector<long> x, gold, le, lr;
    if (!read_ints(dir + "/input.txt", x) || !read_ints(dir + "/golden_output.txt", gold) || !read_ints(dir + "/lut_exp.txt", le) ||
        !read_ints(dir + "/lut_recip.txt", lr) || le.size() != 256 || lr.size() != 256 || x.size() != gold.size() || x.size() % 64)
    { verdict(name, 0, 1, " (files missing or of an unexpected size)"); return; }
    R::LutMem mem;
    mem.load8(R::LUT_EXP, std::vector<uint8_t>(le.begin(), le.end()));
    mem.load16(R::LUT_RECIP, std::vector<uint16_t>(lr.begin(), lr.end()));
    size_t bad = 0, bad_up = 0, zero_sigma = 0;
    char first[200] = "";
    for (size_t r0 = 0; r0 < x.size(); r0 += 64)
    {
        int m = -128;
        for (size_t j = 0; j < 64; j++) m = static_cast<int8_t>(x[r0 + j]) > m ? static_cast<int8_t>(x[r0 + j]) : m;
        uint32_t e[64], sigma = 0;
        for (size_t j = 0; j < 64; j++)
        {
            const uint32_t d = R::adder32_lane(R::ADD_8, 1, 0, false, static_cast<uint32_t>(m) & 0xFFu, static_cast<uint32_t>(x[r0 + j]) & 0xFFu) & 0xFFu;
            e[j] = mem.rdata8(R::LUT_EXP, d);
            sigma += e[j];
        }
        const R::LutValue lv = R::lut_value(mem, R::LUT_RECIP, R::LUT_PWL, kBase, kW, kLerp, sigma);
        if (lv.input_zero) zero_sigma++;
        for (size_t j = 0; j < 64; j++)
        {
            const int8_t a = softmax_elem(mem, lv.data, lv.e_shift, e[j], R::RND_NEAR_AWAY), g = static_cast<int8_t>(gold[r0 + j]);
            if (softmax_elem(mem, lv.data, lv.e_shift, e[j], R::RND_NEAR_UP) != g) bad_up++;
            if (a != g)
            {
                if (!bad) std::snprintf(first, sizeof first, " first: row %zu position %zu x %ld max %d sigma %u r %u E %u ported %d golden %d",
                                        r0 / 64, j, x[r0 + j], m, sigma, unsigned(lv.data), lv.e_shift, int(a), int(g));
                bad++;
            }
        }
    }
    verdict(name, x.size(), bad, first);
    std::printf("    the same rows with ties rounded up instead of away from zero: different %zu; rows with a zero sum %zu\n", bad_up, zero_sigma);
}

static void run_lut(const std::string &dir)
{
    std::vector<long> lut, in, gold;
    R::LutMem mem;
    if (read_ints(dir + "/exp_lut.txt", lut) && read_ints(dir + "/exp_input.txt", in) && read_ints(dir + "/exp_golden_output.txt", gold) && in.size() == gold.size())
    {
        mem.load8(R::LUT_EXP, std::vector<uint8_t>(lut.begin(), lut.end()));
        size_t bad = 0;
        for (size_t i = 0; i < in.size(); i++) bad += mem.rdata8(R::LUT_EXP, static_cast<unsigned>(in[i])) != static_cast<uint8_t>(gold[i]);
        verdict("LUT exp, direct read", in.size(), bad, "");
    }
    else verdict("LUT exp, direct read", 0, 1, " (files)");
    const struct { const char *name; R::LutType t; const char *label; } tabs[2] = {{"recip", R::LUT_RECIP, "LUT recip, interpolating lookup"},
                                                                                   {"rsqrt", R::LUT_RSQRT, "LUT rsqrt, interpolating lookup"}};
    for (const auto &tb : tabs)
    {
        const std::string b = dir + "/" + tb.name;
        if (!read_ints(b + "_lut.txt", lut) || !read_ints(b + "_input.txt", in) || !read_ints(b + "_golden_output.txt", gold) || in.size() != gold.size())
        { verdict(tb.label, 0, 1, " (files)"); continue; }
        mem.load16(tb.t, std::vector<uint16_t>(lut.begin(), lut.end()));
        size_t bad = 0;
        char first[160] = "";
        for (size_t i = 0; i < in.size(); i++)
        {
            const R::LutValue v = R::lut_value(mem, tb.t, R::LUT_PWL, kBase, kW, kLerp, static_cast<uint32_t>(in[i]));
            if (v.data != static_cast<uint16_t>(gold[i]))
            {
                if (!bad) std::snprintf(first, sizeof first, " first: input %ld ported %u golden %ld", in[i], unsigned(v.data), gold[i]);
                bad++;
            }
        }
        verdict(tb.label, in.size(), bad, first);
    }
}

int main(int argc, char **argv)
{
    if (argc < 2) { std::printf("usage: %s <test dir of the hardware drop>\n", argv[0]); return 2; }
    const std::string d = argv[1];
    run_lut(d + "/lut_data");
    run_softmax(d + "/softmax/vec", "Softmax, one row of 64 (ported data path)");
    run_softmax(d + "/softmax/tile", "Softmax, 64 rows of 64 (ported data path)");
    std::printf("RESULT: %s\n", g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
