// tb_rtl_ref_gvu_fu.cpp -- checks has/rtl_ref_gvu_fu.h (functional units ported from the RTL of the vector unit).
// Three kinds of check, each printed with its kind:
//   [arith]  the ported unit against an independent computation with native C++ integer types (random + boundary vectors)
//   [hw-tb]  the directed cases and the expected-value formulas of the RTL testbenches (tb_gvu_vector_clamp,
//            tb_gvu_vector_shift, tb_gvu_adder64_lane, tb_gvu_mul32x32), replayed on the ported unit
//   [cycle]  latency and ordering of the clocked models
// None of these is a comparison with an RTL simulation. With --dump <dir> the vectors and the outputs of the ported units
// are written as text (one beat per line, hex) so an RTL testbench can replay them: tools/has/gvu_oracle/.
// build: g++ -std=c++17 -O2 -Wall -I. tools/has/tb_rtl_ref_gvu_fu.cpp -o tb_rtl_ref_gvu_fu
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <deque>
#include <random>
#include <string>
#include "has/rtl_ref_gvu_fu.h"

using namespace rtl_ref_gvu;

static int g_fail = 0;
static uint64_t g_checks = 0;
static std::mt19937_64 g_rng(0x47565531u);
static FILE *g_dump = nullptr;
static std::string g_dump_dir;

static void open_dump(const char *name)
{
    if (g_dump) { fclose(g_dump); g_dump = nullptr; }
    if (!g_dump_dir.empty()) g_dump = fopen((g_dump_dir + "/" + name).c_str(), "w");
}
static void section(const char *kind, const char *name, uint64_t n, int fail_before)
{
    printf("%-7s %-58s %9" PRIu64 " vectors  %s\n", kind, name, n, g_fail == fail_before ? "ok" : "MISMATCH");
}
#define CHECK(cond, ...) do { g_checks++; if (!(cond)) { if (g_fail < 20) { printf("  mismatch: "); printf(__VA_ARGS__); printf("\n"); } g_fail++; } } while (0)

// Random value of `bits` bits with the boundary values over-represented.
static uint64_t rnd(int bits)
{
    const uint64_t m = bits >= 64 ? ~0ull : ((1ull << bits) - 1);
    const uint64_t top = 1ull << (bits - 1);
    switch (g_rng() % 12)
    {
    case 0: return 0;
    case 1: return 1;
    case 2: return m;                // -1
    case 3: return top;              // most negative
    case 4: return top - 1;          // most positive
    case 5: return (top + 1) & m;
    case 6: return (g_rng() % 256) & m;
    case 7: return (m - (g_rng() % 256)) & m;
    default: return g_rng() & m;
    }
}
static u128 rnd128(int bits)
{
    const u128 m = (static_cast<u128>(1) << bits) - 1;
    const u128 top = static_cast<u128>(1) << (bits - 1);
    switch (g_rng() % 10)
    {
    case 0: return 0;
    case 1: return m;
    case 2: return top;
    case 3: return top - 1;
    case 4: return g_rng() % 1024;
    case 5: return (m - (g_rng() % 1024)) & m;
    default: return ((static_cast<u128>(g_rng()) << 64) | g_rng()) & m;
    }
}
static i128 sx(u128 v, int w) { const u128 M = (static_cast<u128>(1) << w) - 1; v &= M; return ((v >> (w - 1)) & 1) ? static_cast<i128>(v | ~M) : static_cast<i128>(v); }
static int64_t sx64(uint64_t v, int w) { return w >= 64 ? static_cast<int64_t>(v) : static_cast<int64_t>(v << (64 - w)) >> (64 - w); }

static void test_clz()
{
    const int f0 = g_fail; uint64_t n = 0;
    open_dump("clz.txt");
    for (int it = 0; it < 200000; it++)
    {
        uint32_t x = static_cast<uint32_t>(rnd(32));
        if (it < 33) x = it == 32 ? 0 : (1u << it);
        else if (it < 66) x = (1u << (it - 33)) | static_cast<uint32_t>(g_rng() & ((1u << (it - 33)) - 1));
        const ClzOut m1 = clz(x, 32, true), m0 = clz(x, 32, false);
        CHECK(m1.all_zeros == (x == 0) && m0.all_zeros == (x == 0), "clz zero flag x=%08x", x);
        if (x)
        {
            CHECK(m1.idx == static_cast<uint32_t>(31 - __builtin_clz(x)), "clz msb x=%08x idx=%u", x, m1.idx);
            CHECK(m0.idx == static_cast<uint32_t>(__builtin_ctz(x)), "clz lsb x=%08x idx=%u", x, m0.idx);
        }
        else CHECK(m1.idx == 0 && m0.idx == 0, "clz idx of 0");
        if (g_dump) fprintf(g_dump, "%08x %x %x\n", x, m1.idx, m1.all_zeros ? 1 : 0);
        n++;
    }
    section("[arith]", "clz (DataWidth 32, Mode 1 and 0)", n, f0);
}

static void test_adder32()
{
    const int f0 = g_fail; uint64_t n = 0;
    open_dump("adder32.txt");
    for (int it = 0; it < 400000; it++)
    {
        const AdderMode mode = static_cast<AdderMode>(it % 4);
        const unsigned sub8 = g_rng() & 15, sub16 = g_rng() & 3; const bool sub32 = g_rng() & 1;
        const uint32_t a = static_cast<uint32_t>(rnd(32)), b = static_cast<uint32_t>(rnd(32));
        const uint32_t r = adder32_lane(mode, sub8, sub16, sub32, a, b);
        uint32_t e = 0;
        if (mode == ADD_64) e = a + b;                           // no branch in the RTL: plain 32-bit add
        else if (mode == ADD_32) e = sub32 ? a - b : a + b;
        else if (mode == ADD_16)
            for (int k = 0; k < 2; k++)
            {
                const uint16_t x = static_cast<uint16_t>(a >> (16 * k)), y = static_cast<uint16_t>(b >> (16 * k));
                e |= static_cast<uint32_t>(static_cast<uint16_t>(((sub16 >> k) & 1) ? x - y : x + y)) << (16 * k);
            }
        else
            for (int k = 0; k < 4; k++)
            {
                const uint8_t x = static_cast<uint8_t>(a >> (8 * k)), y = static_cast<uint8_t>(b >> (8 * k));
                e |= static_cast<uint32_t>(static_cast<uint8_t>(((sub8 >> k) & 1) ? x - y : x + y)) << (8 * k);
            }
        CHECK(r == e, "adder32 mode=%d sub=%x/%x/%d a=%08x b=%08x got=%08x exp=%08x", mode, sub8, sub16, sub32, a, b, r, e);
        if (g_dump) fprintf(g_dump, "%x %x %x %x %08x %08x %08x\n", mode, sub8, sub16, sub32 ? 1 : 0, a, b, r);
        n++;
    }
    section("[arith]", "gvu_adder32_lane (ADD_32 / 16 / 8, add and sub, wrap)", n, f0);
    // The wrap that question Q-R1 is about: 16-bit element, 32767 + 100.
    const uint32_t w = adder32_lane(ADD_16, 0, 0, false, 32767u, 100u) & 0xFFFFu;
    CHECK(static_cast<int16_t>(w) == -32669, "16-bit wrap 32767 + 100 = %d", static_cast<int16_t>(w));
}

static void test_adder64()
{
    int f0 = g_fail; uint64_t n = 0;
    open_dump("adder64.txt");
    for (int it = 0; it < 400000; it++)
    {
        const AdderMode mode = static_cast<AdderMode>(it % 4);
        const unsigned sub32 = g_rng() & 3; const bool sub64 = g_rng() & 1;
        const uint64_t a = rnd(64), b = rnd(64);
        const uint64_t r = adder64_lane(mode, sub32, sub64, a, b);
        uint64_t e;
        // Combinational path: the sub inputs have no effect in the RTL (b is taken before b_0 is negated), so the unit adds.
        if (mode == ADD_64) e = a + b;
        else if (mode == ADD_32)
        {
            const uint32_t lo = static_cast<uint32_t>(a) + static_cast<uint32_t>(b);
            const uint32_t hi = static_cast<uint32_t>(a >> 32) + static_cast<uint32_t>(b >> 32);
            e = (static_cast<uint64_t>(hi) << 32) | lo;
        }
        else e = a + b;
        CHECK(r == e, "adder64 mode=%d a=%016" PRIx64 " b=%016" PRIx64 " got=%016" PRIx64 " exp=%016" PRIx64, mode, a, b, r, e);
        if (g_dump) fprintf(g_dump, "%x %x %x %016" PRIx64 " %016" PRIx64 " %016" PRIx64 "\n", mode, sub32, sub64 ? 1 : 0, a, b, r);
        n++;
    }
    section("[arith]", "gvu_adder64_lane combinational path (sub inputs ignored, wrap)", n, f0);

    // tb_gvu_adder64_lane: ADD_64, ADD_64_PIPE = 1, random valid / ready, results compared in order with a + b.
    // (That testbench also sets DOUBLE_PIPE_REG = 1, which is not ported; the single register is exercised here.)
    for (int sub = 0; sub < 2; sub++)
        for (int pipe = 0; pipe < 2; pipe++)
        {
            f0 = g_fail; n = 0;
            FuParams p; p.ADD_64_PIPE = pipe != 0;
            Adder64Lane dut(p);
            std::deque<std::pair<uint64_t, int>> exp;            // expected result, cycle of acceptance
            int lat_min = 1 << 30, lat_max = -1;
            uint64_t a = rnd(64), b = rnd(64); bool v = false;
            for (int cyc = 0; cyc < 60000; cyc++)
            {
                if (!v) { v = (g_rng() % 4) != 0; a = rnd(64); b = rnd(64); }
                const bool rdy = (g_rng() % 4) != 0;
                const Adder64Lane::Out o = dut.eval(ADD_64, sub != 0, v, a, b, rdy);
                const bool acc = v && o.o_64b_ready;
                if (acc) exp.push_back({(sub && pipe) ? a - b : a + b, cyc});   // the subtraction exists on the registered path only
                if (o.o_64b_valid && rdy)
                {
                    CHECK(!exp.empty(), "adder64 pipe: output without input");
                    if (!exp.empty())
                    {
                        CHECK(o.o_r == exp.front().first, "adder64 pipe=%d result %016" PRIx64 " exp %016" PRIx64, pipe, o.o_r, exp.front().first);
                        const int lat = cyc - exp.front().second;
                        lat_min = lat < lat_min ? lat : lat_min; lat_max = lat > lat_max ? lat : lat_max;
                        exp.pop_front(); n++;
                    }
                }
                dut.tick(true, false, ADD_64, sub != 0, v, a, b, rdy);
                if (acc) v = false;
            }
            CHECK(lat_min == lat_adder64(ADD_64, p), "adder64 pipe=%d minimum latency %d, constant %d", pipe, lat_min, lat_adder64(ADD_64, p));
            char nm[96]; snprintf(nm, sizeof nm, "Adder64Lane ADD_64_PIPE=%d %s: order, latency min %d max %d", pipe, sub ? "sub" : "add", lat_min, lat_max);
            section(pipe ? "[hw-tb]" : "[cycle]", nm, n, f0);
        }
}

static void test_cmp()
{
    int f0 = g_fail; uint64_t n = 0;
    open_dump("cmp.txt");
    for (int it = 0; it < 400000; it++)
    {
        const ElemW ew = static_cast<ElemW>(it % 3); const bool sg = g_rng() & 1; const CmpOp op = static_cast<CmpOp>(g_rng() & 1);
        uint32_t a = static_cast<uint32_t>(rnd(32)), b = static_cast<uint32_t>(rnd(32));
        if (it % 7 == 0) b = a;
        const uint32_t r = lane_minmax(a, b, ew, sg, op);
        const int w = ew == EW_8 ? 8 : ew == EW_16 ? 16 : 32;
        uint32_t e = 0;
        for (int k = 0; k < 32 / w; k++)
        {
            const uint64_t m = (1ull << w) - 1, x = (a >> (k * w)) & m, y = (b >> (k * w)) & m;
            const int64_t xs = sg ? sx64(x, w) : static_cast<int64_t>(x), ys = sg ? sx64(y, w) : static_cast<int64_t>(y);
            const int64_t pick = op == CMP_MAX ? (xs > ys ? xs : ys) : (xs < ys ? xs : ys);
            e |= static_cast<uint32_t>(static_cast<uint64_t>(pick) & m) << (k * w);
        }
        CHECK(r == e, "cmp ew=%d sg=%d op=%d a=%08x b=%08x got=%08x exp=%08x", ew, sg, op, a, b, r, e);
        if (g_dump) fprintf(g_dump, "%x %x %x %08x %08x %08x\n", ew, sg ? 1 : 0, op, a, b, r);
        n++;
    }
    section("[arith]", "gvu_vcmp32 lane_minmax (EW_8 / 16 / 32, signed / unsigned)", n, f0);

    // Accumulate mode: running max / min over a window of beats, emitted on the last beat; broadcast clamp.
    f0 = g_fail; n = 0;
    for (int pipe = 0; pipe < 2; pipe++)
    {
        FuParams p; p.CMP_OUT_PIPE = pipe != 0;
        Vcmp32 dut(p);
        for (int win = 0; win < 3000; win++)
        {
            const int len = 1 + static_cast<int>(g_rng() % 9);
            const bool sg = g_rng() & 1; const CmpOp op = static_cast<CmpOp>(g_rng() & 1);
            int64_t ref[GVU_LANES];
            Vcmp32::Out last{};
            for (int bt = 0; bt < len; bt++)
            {
                Vcmp32::In in{};
                in.in_valid = true; in.in_cmp_mode = CMP_ACCUMULATE; in.in_op = op; in.in_elem_w = EW_32; in.in_signed = sg;
                in.in_first = bt == 0; in.in_last = bt == len - 1; in.out_ready = true;
                for (int l = 0; l < GVU_LANES; l++)
                {
                    in.in_src0[l] = static_cast<uint32_t>(rnd(32));
                    const int64_t v = sg ? static_cast<int64_t>(static_cast<int32_t>(in.in_src0[l])) : static_cast<int64_t>(in.in_src0[l]);
                    ref[l] = bt == 0 ? v : (op == CMP_MAX ? (v > ref[l] ? v : ref[l]) : (v < ref[l] ? v : ref[l]));
                }
                const Vcmp32::Out o = dut.eval(in);
                if (!pipe) { CHECK(o.out_valid == in.in_last, "cmp accumulate: out_valid on a beat that is not last"); last = o; }
                dut.tick(in);
            }
            if (pipe) { Vcmp32::In idle{}; idle.out_ready = true; last = dut.eval(idle); CHECK(last.out_valid, "cmp OUT_PIPE=1: result not valid one cycle after last"); dut.tick(idle); }
            for (int l = 0; l < GVU_LANES; l++) CHECK(last.out_res[l] == static_cast<uint32_t>(ref[l]), "cmp accumulate lane %d", l);
            n++;
        }
    }
    {
        Vcmp32 dut;
        Vcmp32::In in{}; in.in_valid = true; in.in_cmp_mode = CMP_ELEMENTWISE; in.out_ready = false;
        CHECK(!dut.eval(in).in_ready && !dut.eval(in).out_valid, "cmp OUT_PIPE=0: a beat is not accepted while out_ready = 0");
        in.in_cmp_mode = CMP_ACCUMULATE; in.in_last = false;
        CHECK(!dut.eval(in).in_ready, "cmp OUT_PIPE=0: an accumulate beat that does not emit also waits for out_ready");
        in.out_ready = true;
        CHECK(dut.eval(in).in_ready && !dut.eval(in).out_valid, "cmp OUT_PIPE=0: accumulate beat accepted without an output");
    }
    section("[cycle]", "Vcmp32 CMP_ACCUMULATE windows, OUT_PIPE 0 and 1", n, f0);
}

static void test_clamp()
{
    int f0 = g_fail; uint64_t n = 0;
    open_dump("clamp.txt");
    for (int it = 0; it < 400000; it++)
    {
        const DataFormat f = static_cast<DataFormat>(it % 3); const bool sg = g_rng() & 1; const int w = format_bits(f);
        const uint32_t m = mask32(w);
        uint32_t a = static_cast<uint32_t>(rnd(w)) & m, lo = static_cast<uint32_t>(rnd(w)) & m, hi = static_cast<uint32_t>(rnd(w)) & m;
        if (it % 5 == 0) a = lo;
        if (it % 5 == 1) a = hi;
        const uint32_t r = clamp_lane(a, lo, hi, w, sg);
        const int64_t as = sg ? sx64(a, w) : a, ls = sg ? sx64(lo, w) : lo, hs = sg ? sx64(hi, w) : hi;
        const int64_t e = as < ls ? ls : (as > hs ? hs : as);
        CHECK(r == (static_cast<uint32_t>(e) & m), "clamp w=%d sg=%d a=%x lo=%x hi=%x got=%x", w, sg, a, lo, hi, r);
        if (g_dump) fprintf(g_dump, "%x %x %08x %08x %08x %08x\n", f, sg ? 1 : 0, a, lo, hi, r);
        n++;
    }
    section("[arith]", "gvu_vector_clamp lane (I8 / I16 / I32, signed / unsigned)", n, f0);

    // gvu_vector_clamp32: signed 32 bits, the hi test first; half of the vectors have lo > hi.
    f0 = g_fail; n = 0;
    open_dump("clamp32.txt");
    for (int it = 0; it < 400000; it++)
    {
        const int w = (it % 3 == 0) ? 8 : ((it % 3 == 1) ? 16 : 32);
        int32_t a = static_cast<int32_t>(sx64(static_cast<uint32_t>(rnd(w)) & mask32(w), w));
        int32_t lo = static_cast<int32_t>(sx64(static_cast<uint32_t>(rnd(w)) & mask32(w), w)), hi = static_cast<int32_t>(sx64(static_cast<uint32_t>(rnd(w)) & mask32(w), w));
        if ((it & 1) && lo > hi) { const int32_t t = lo; lo = hi; hi = t; }
        if (it % 5 == 0) a = lo;
        if (it % 5 == 1) a = hi;
        const int32_t r = clamp32_lane(a, lo, hi);
        const int32_t e = a > hi ? hi : (a < lo ? lo : a);
        CHECK(r == e, "clamp32 a=%d lo=%d hi=%d got=%d", a, lo, hi, r);
        // with lo <= hi the lane clamp of the earlier module gives the same value on the sign-extended lane
        if (lo <= hi)
            CHECK(static_cast<int32_t>(clamp_lane(static_cast<uint32_t>(a), static_cast<uint32_t>(lo), static_cast<uint32_t>(hi), 32, true)) == r,
                  "clamp_lane differs from clamp32 a=%d lo=%d hi=%d", a, lo, hi);
        if (g_dump) fprintf(g_dump, "%08x %08x %08x %08x\n", static_cast<uint32_t>(a), static_cast<uint32_t>(lo), static_cast<uint32_t>(hi), static_cast<uint32_t>(r));
        n++;
    }
    section("[arith]", "gvu_vector_clamp32 lane (signed 32 bits, hi test first)", n, f0);

    // tb_gvu_vector_clamp: TC1 I8 signed, TC2 I16 unsigned, TC3 I32 signed; lo = -10 / 10, hi = 10 / 100; the lane value
    // cycles through lo-1, lo, 0, hi, hi+1. Expected value as written in that testbench.
    f0 = g_fail; n = 0;
    const struct { DataFormat f; bool sg; } tc[3] = {{GVU_FMT_I8, true}, {GVU_FMT_I16, false}, {GVU_FMT_I32, true}};
    for (const auto &t : tc)
    {
        const int w = format_bits(t.f); const uint32_t m = mask32(w);
        V32 a{}, lo{}, hi{}, y{}, exp{};
        for (int i = 0; i < GVU_LANES; i++)
        {
            const uint32_t l = t.sg ? static_cast<uint32_t>(-10) : 10u, h = t.sg ? 10u : 100u;
            const uint32_t v = (i % 5 == 0) ? l - 1 : (i % 5 == 1) ? l : (i % 5 == 2) ? 0 : (i % 5 == 3) ? h : h + 1;
            const int32_t sv = sext32(v & m, w), sl = sext32(l & m, w), sh = sext32(h & m, w);
            a[i] = v & m; lo[i] = l & m; hi[i] = h & m;
            exp[i] = (t.sg ? (sv < sl) : ((v & m) < (l & m))) ? (l & m) : (t.sg ? (sv > sh) : ((v & m) > (h & m))) ? (h & m) : (v & m);
        }
        const bool err = vector_clamp(t.f, t.sg, a, lo, hi, y);
        CHECK(!err, "clamp testcase: rsp_error");
        for (int i = 0; i < GVU_LANES; i++) CHECK(y[i] == exp[i], "clamp testcase fmt=%d lane %d got=%x exp=%x", t.f, i, y[i], exp[i]);
        n++;
    }
    { V32 z{}, y{}; y.fill(5); CHECK(vector_clamp(GVU_FMT_I64, true, z, z, z, y) && y[0] == 0, "clamp I64 must raise rsp_error and drive 0"); }
    section("[hw-tb]", "tb_gvu_vector_clamp TC1-TC3 + I64 error", n, f0);
}

static void test_shift()
{
    int f0 = g_fail; uint64_t n = 0;
    open_dump("shift.txt");
    for (int it = 0; it < 400000; it++)
    {
        const DataFormat f = static_cast<DataFormat>(it % 3); const int w = format_bits(f); const uint32_t m = mask32(w);
        const ShiftOp op = static_cast<ShiftOp>(g_rng() % 4);    // 3 = default branch
        const unsigned am = (it % 11 == 0) ? static_cast<unsigned>(g_rng() % 64) : static_cast<unsigned>(g_rng() % (w + 2));
        const uint32_t a = static_cast<uint32_t>(rnd(w)) & m;
        const uint32_t r = shift_lane(a, w, op, am);
        uint64_t e;
        if (op == GVU_SHIFT_LEFT) e = am >= 64 ? 0 : (static_cast<uint64_t>(a) << am);
        else if (op == GVU_SHIFT_RIGHT_LOGICAL) e = am >= 64 ? 0 : (static_cast<uint64_t>(a) >> am);
        else e = static_cast<uint64_t>(sx64(a, w) >> (am > 63 ? 63 : am));
        CHECK(r == (static_cast<uint32_t>(e) & m), "shift w=%d op=%d am=%u a=%x got=%x exp=%x", w, op, am, a, r, static_cast<uint32_t>(e) & m);
        if (g_dump) fprintf(g_dump, "%x %x %02x %08x %08x\n", f, op, am, a, r);
        n++;
    }
    section("[arith]", "gvu_vector_shift lane (I8 / I16 / I32, amount 0..63)", n, f0);

    // tb_gvu_vector_shift: TC1 I8 left 1, TC2 I16 logical right 5, TC3 I32 arithmetic right 31; lanes 0, 1, 0x80000000,
    // 0xffffffff, then 0x12340000 + i.
    f0 = g_fail; n = 0;
    const struct { DataFormat f; ShiftOp op; unsigned sh; } tc[3] = {{GVU_FMT_I8, GVU_SHIFT_LEFT, 1}, {GVU_FMT_I16, GVU_SHIFT_RIGHT_LOGICAL, 5}, {GVU_FMT_I32, GVU_SHIFT_RIGHT_ARITH, 31}};
    for (const auto &t : tc)
    {
        const int w = format_bits(t.f); const uint32_t m = mask32(w);
        V32 a{}, y{};
        uint32_t exp[GVU_LANES];
        for (int i = 0; i < GVU_LANES; i++)
        {
            const uint32_t v = (i == 0) ? 0 : (i == 1) ? 1 : (i == 2) ? 0x80000000u : (i == 3) ? 0xffffffffu : (0x12340000u + i);
            a[i] = v & m;
            exp[i] = t.op == GVU_SHIFT_LEFT ? ((v & m) << t.sh) & m : t.op == GVU_SHIFT_RIGHT_LOGICAL ? (v & m) >> t.sh : static_cast<uint32_t>(sext32(v & m, w) >> t.sh) & m;
        }
        CHECK(!vector_shift(t.f, t.op, t.sh, a, y), "shift testcase: rsp_error");
        for (int i = 0; i < GVU_LANES; i++) CHECK(y[i] == exp[i], "shift testcase fmt=%d lane %d got=%x exp=%x", t.f, i, y[i], exp[i]);
        n++;
    }
    section("[hw-tb]", "tb_gvu_vector_shift TC1-TC3", n, f0);
}

// Independent round-shift: exact value x / 2^S compared through twice the remainder, no shared formula with the unit.
static i128 ref_round(i128 x, unsigned S, int mode)
{
    if (S == 0) return x;
    if (S >= 120)                                               // only reachable for the 72-bit unit: |x| < 2^71 << 2^S
    {
        if (x == 0) return 0;
        if (mode == RND_FLOOR) return x < 0 ? -1 : 0;
        if (mode == RND_CEIL) return x > 0 ? 1 : 0;
        if (mode == RND_AWAY) return x < 0 ? -1 : 1;
        return 0;                                               // toward zero and every nearest mode: |x / 2^S| < 1/2
    }
    const i128 one = static_cast<i128>(1) << S;
    i128 q = x / one, r = x % one;                              // C++: toward zero
    if (r < 0) { q -= 1; r += one; }                            // floor, 0 <= r < 2^S
    const bool neg = x < 0;
    const int c = (2 * r > one) - (2 * r < one);                // remainder against one half: +1, 0, -1
    switch (mode)
    {
    case RND_FLOOR: return q;
    case RND_CEIL: return r ? q + 1 : q;
    case RND_TRUNC: return (neg && r) ? q + 1 : q;
    case RND_AWAY: return (!neg && r) ? q + 1 : q;
    case RND_NEAR_UP: return c >= 0 ? q + 1 : q;
    case RND_NEAR_AWAY: return (c > 0 || (c == 0 && !neg)) ? q + 1 : q;
    default: return (c > 0 || (c == 0 && (q & 1))) ? q + 1 : q;
    }
}

static void test_roundshift(int IN_W)
{
    const int f0 = g_fail; uint64_t n = 0, novf = 0;
    open_dump(IN_W == 64 ? "rs64.txt" : "rs72.txt");
    // IN_W = 72: the shift input has 7 bits. Up to 72 the unit is compared with the exact value here; 73..127 is
    // characterised separately in test_roundshift72_high().
    const unsigned smax = IN_W == 64 ? 63 : 72;
    for (int it = 0; it < 1500000; it++)
    {
        const RoundMode rm = static_cast<RoundMode>(it % 7);
        const NarrowMode nm = static_cast<NarrowMode>(g_rng() & 1); const NarrowW nw = static_cast<NarrowW>(g_rng() % 3);
        unsigned S = static_cast<unsigned>(g_rng() % (smax + 1));
        if (it % 5 == 0) S = static_cast<unsigned>(g_rng() % 4);
        u128 d = rnd128(IN_W);
        if (it % 3 == 0 && S > 0 && S < static_cast<unsigned>(IN_W) - 1)
        {   // ties and near-ties around a value that fits the target width
            const i128 q = static_cast<i128>(static_cast<int64_t>(g_rng())) >> (64 - (8 + g_rng() % 26));
            const i128 off = static_cast<i128>(static_cast<int>(g_rng() % 3)) - 1;
            const i128 v = q * (static_cast<i128>(1) << S) + (static_cast<i128>(1) << (S - 1)) + off;
            d = static_cast<u128>(v) & ((static_cast<u128>(1) << IN_W) - 1);
        }
        const RsOut o = vroundshift_lane(d, IN_W, S, rm, nm, nw);
        const i128 y = ref_round(sx(d, IN_W), S, rm);
        const int nb = narrow_bits(nw);
        const i128 hi = (static_cast<i128>(1) << (nb - 1)) - 1, lo = -(static_cast<i128>(1) << (nb - 1));
        const bool ovf = y > hi || y < lo;
        i128 e = y;
        if (ovf) e = nm == NARROW_SATURATE ? (y > hi ? hi : lo) : sx(static_cast<u128>(y), nb);
        CHECK(o.out_ovf == ovf && o.out_data == static_cast<uint32_t>(static_cast<int32_t>(e)),
              "roundshift%d S=%u mode=%d narrow=%d/%d got=%08x ovf=%d exp=%08x ovf=%d", IN_W, S, rm, nm, nw, o.out_data, o.out_ovf, static_cast<uint32_t>(static_cast<int32_t>(e)), ovf);
        if (g_dump && it < 300000)
            fprintf(g_dump, "%02x %x %x %x %02x%016" PRIx64 " %08x %x\n", S, rm, nm, nw, static_cast<unsigned>(static_cast<uint64_t>(d >> 64) & 0xFF), static_cast<uint64_t>(d), o.out_data, o.out_ovf ? 1 : 0);
        novf += ovf; n++;
    }
    char nm[96]; snprintf(nm, sizeof nm, "gvu_vroundshift%d (7 modes, shift 0..%u, %" PRIu64 " ovf)", IN_W, smax, novf);
    section("[arith]", nm, n, f0);
}

// gvu_vroundshift72 with in_shift 73..127: IN_W'(1) << S is 0, so mask is all ones and half is 0 (half is 2^71 only at
// S = 72). The floor is the sign, rem is the whole input, rem_gt_h = (input != 0), rem_eq_h = (input == 0). The directed
// modes still give the exact result; the three nearest modes do not: a non-negative input rounds up to 1 (NEAR_EVEN: a
// positive input). The port must reproduce exactly that; the number of beats that differ from the exact value is printed.
static void test_roundshift72_high()
{
    const int f0 = g_fail; uint64_t n = 0, dev[7] = {0, 0, 0, 0, 0, 0, 0};
    for (int it = 0; it < 400000; it++)
    {
        const RoundMode rm = static_cast<RoundMode>(it % 7);
        const unsigned S = 73 + static_cast<unsigned>(g_rng() % 55);
        const u128 d = rnd128(72);
        const i128 x = sx(d, 72);
        const RsOut o = vroundshift_lane(d, 72, S, rm, NARROW_WRAP, NW_32);
        const bool neg = x < 0, nz = x != 0;
        bool inc = false;
        if (rm == RND_CEIL) inc = nz;
        else if (rm == RND_TRUNC) inc = neg && nz;
        else if (rm == RND_AWAY) inc = !neg && nz;
        else if (rm == RND_NEAR_UP) inc = true;
        else if (rm == RND_NEAR_AWAY) inc = nz || !neg;
        else if (rm == RND_NEAR_EVEN) inc = nz;
        const int32_t lit = (neg ? -1 : 0) + (inc ? 1 : 0);
        CHECK(o.out_data == static_cast<uint32_t>(lit) && !o.out_ovf, "roundshift72 S=%u mode=%d got=%08x literal=%d", S, rm, o.out_data, lit);
        if (static_cast<i128>(lit) != ref_round(x, S, rm)) dev[rm]++;
        if (g_dump && it < 100000) fprintf(g_dump, "%02x %x %x %x %02x%016" PRIx64 " %08x %x\n", S, rm, 0, 0, static_cast<unsigned>(static_cast<uint64_t>(d >> 64) & 0xFF), static_cast<uint64_t>(d), o.out_data, o.out_ovf ? 1 : 0);
        n++;
    }
    section("[arith]", "gvu_vroundshift72, shift 73..127: as the RTL expressions evaluate", n, f0);
    printf("        beats that differ from the exact rounded value: floor %" PRIu64 ", ceil %" PRIu64 ", trunc %" PRIu64 ", away %" PRIu64 ", near-up %" PRIu64 ", near-away %" PRIu64 ", near-even %" PRIu64 "\n",
           dev[0], dev[1], dev[2], dev[3], dev[4], dev[5], dev[6]);
    CHECK(dev[0] + dev[1] + dev[2] + dev[3] == 0, "directed modes must stay exact above 72");
}

static void test_mul()
{
    int f0 = g_fail; uint64_t n = 0;
    open_dump("mula.txt");
    for (int it = 0; it < 1000000; it++)
    {
        const MulaMode m = static_cast<MulaMode>(it % 4);
        const uint32_t a = static_cast<uint32_t>(rnd(32)), b = static_cast<uint32_t>(rnd(32));
        const uint64_t r = vmul32x32_lane(m, a, b);
        uint64_t e;
        const int64_t as = static_cast<int32_t>(a);
        if (m == MA_32X32) e = static_cast<uint64_t>(as * static_cast<int64_t>(static_cast<int32_t>(b)));   // expected value of tb_gvu_mul32x32
        else if (m == MA_32X16) e = static_cast<uint64_t>(as * static_cast<int16_t>(b));
        else if (m == MA_32X8) e = static_cast<uint64_t>(as * static_cast<int8_t>(b));
        else
        {
            const uint32_t lo = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(a)) * static_cast<int32_t>(static_cast<int16_t>(b)));
            const uint32_t hi = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(a >> 16)) * static_cast<int32_t>(static_cast<int16_t>(b >> 16)));
            e = (static_cast<uint64_t>(hi) << 32) | lo;
        }
        CHECK(r == e, "mul32x32 mode=%d a=%08x b=%08x got=%016" PRIx64 " exp=%016" PRIx64, m, a, b, r, e);
        if (g_dump && it < 300000) fprintf(g_dump, "%x %08x %08x %016" PRIx64 "\n", m, a, b, r);
        n++;
    }
    section("[hw-tb]", "gvu_vmul32x32 (32x32 / 32x16 / 32x8 / 2 x 16x16) = signed product", n, f0);

    f0 = g_fail; n = 0;
    open_dump("mulb.txt");
    const u128 M72 = (static_cast<u128>(1) << 72) - 1;
    for (int it = 0; it < 1000000; it++)
    {
        const MulbMode m = static_cast<MulbMode>(it % 4);
        const uint64_t a = rnd(48), b = rnd(40);
        const MulbOut o = vmul48x40_lane(m, a, b);
        u128 e = 0; uint32_t ea = 0;
        if (m == MB_40X32) e = static_cast<u128>(static_cast<i128>(sx64(a, 40)) * sx64(b, 32)) & M72;
        else if (m == MB_32X32_16X8)
        {
            e = static_cast<u128>(static_cast<i128>(sx64(a, 32)) * sx64(b, 32)) & M72;
            ea = static_cast<uint32_t>(static_cast<int32_t>(sx64(a >> 32, 16) * sx64(b >> 32, 8)));
        }
        else if (m == MB_32X8) e = static_cast<u128>(static_cast<i128>(sx64(a, 32)) * sx64(b, 8)) & M72;
        else
            for (int k = 0; k < 3; k++)
                e |= static_cast<u128>(static_cast<uint64_t>(sx64(a >> (16 * k), 16) * sx64(b >> (8 * k), 8)) & 0xFFFFFFull) << (24 * k);
        CHECK(o.out_prod == e && o.out_aux == ea, "mul48x40 mode=%d a=%012" PRIx64 " b=%010" PRIx64 " prod hi=%02x lo=%016" PRIx64 " aux=%08x exp aux=%08x", m, a, b,
              static_cast<unsigned>(static_cast<uint64_t>(o.out_prod >> 64)), static_cast<uint64_t>(o.out_prod), o.out_aux, ea);
        if (g_dump && it < 300000)
            fprintf(g_dump, "%x %012" PRIx64 " %010" PRIx64 " %02x%016" PRIx64 " %08x\n", m, a, b, static_cast<unsigned>(static_cast<uint64_t>(o.out_prod >> 64) & 0xFF), static_cast<uint64_t>(o.out_prod), o.out_aux);
        n++;
    }
    section("[arith]", "gvu_vmul48x40 (40x32 / 32x32 + 16x8 / 32x8 / 3 x 16x8)", n, f0);
}

static void test_pipeline_reg()
{
    const int f0 = g_fail; uint64_t n = 0;
    PipelineReg<int> r;
    std::deque<int> q;
    int next = 0; bool v = false; int d = 0;
    for (int cyc = 0; cyc < 20000; cyc++)
    {
        if (!v && (g_rng() & 1)) { v = true; d = next++; }
        const bool rdy = (g_rng() % 3) != 0;
        if (r.o_valid(v) && rdy) { CHECK(!q.empty() && r.o_data(d) == q.front(), "pipeline_reg order"); if (!q.empty()) q.pop_front(); n++; }
        const bool acc = v && r.o_ready(v, rdy);
        if (acc) q.push_back(d);
        r.tick(true, false, v, d, rdy);
        if (acc) v = false;
    }
    section("[cycle]", "pipeline_reg (single register): no loss, no duplication", n, f0);
}

int main(int argc, char **argv)
{
    for (int i = 1; i + 1 < argc; i++) if (!strcmp(argv[i], "--dump")) g_dump_dir = argv[i + 1];
    printf("tb_rtl_ref_gvu_fu: units of has/rtl_ref_gvu_fu.h. No RTL simulation is involved in these checks.\n");
    test_clz();
    test_adder32();
    test_adder64();
    test_cmp();
    test_clamp();
    test_shift();
    test_roundshift(64);
    test_roundshift(72);
    test_roundshift72_high();
    test_mul();
    test_pipeline_reg();
    if (g_dump) fclose(g_dump);
    printf("latency constants (RTL parameter defaults): adder32 %d, adder64 %d (ADD_64_PIPE=1: %d), cmp %d (OUT_PIPE=1: 1), clamp %d, shift %d,\n"
           "  round-shift %d (S2_PIPE=1: 2), mul32x32 %d/%d/%d/%d (32x32, 32x16, 32x8, 16x16), mul48x40 %d/%d/%d/%d\n",
           kLatAdder32, lat_adder64(ADD_64), 1, lat_vcmp32(), kLatClamp, kLatShift, lat_vroundshift(),
           mula_latency(MA_32X32), mula_latency(MA_32X16), mula_latency(MA_32X8), mula_latency(MA_16X16),
           mulb_latency(MB_40X32), mulb_latency(MB_32X32_16X8), mulb_latency(MB_32X8), mulb_latency(MB_3X16X8));
    printf("checks %" PRIu64 ", mismatches %d\n", g_checks, g_fail);
    printf(g_fail ? "RESULT: FAIL\n" : "RESULT: PASS\n");
    return g_fail ? 1 : 0;
}
