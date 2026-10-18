// tb_gvu_mas.cpp -- unit check of the vector-unit items taken from the MAS: ceil rounding, log-scale LUT index, PWL LUT
// with the MAS table depths, softmax with the log-scale reciprocal, load/store unit ports and address generator,
// Scratchpad port map and capacity, stream register totals, pipeline controller state sequences.
// Reference vectors: tools/has/mas_ref.py (independent Python). No SystemC needed.
// Build and run (repository root):
//   python3 tools/has/mas_ref.py /tmp/mas_vec
//   g++ -std=c++17 -O2 -I. tools/has/tb_gvu_mas.cpp -o tools/has/tb_gvu_mas && tools/has/tb_gvu_mas /tmp/mas_vec
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>
#include "has/gvu_layernorm.h"
#include "has/gvu_lsu.h"
#include "has/gvu_lut.h"
#include "has/gvu_pipeline_ctrl.h"
#include "has/gvu_quant.h"
#include "has/gvu_softmax.h"
#include "has/gvu_sp_map.h"
#include "has/gvu_vrf.h"

using namespace has;

static int g_fail = 0;
#define CHECK(cond, ...) do { if (!(cond)) { if (g_fail < 40) std::printf(__VA_ARGS__); g_fail++; } } while (0)

static void verdict(const char *name, int before) { std::printf("[%s] %s\n", name, g_fail == before ? "PASS" : "FAIL"); }

static std::vector<int32_t> ints_after(const std::string &line)
{
    std::vector<int32_t> v;
    std::istringstream in(line.substr(1));
    long x;
    while (in >> x) v.push_back(int32_t(x));
    return v;
}

// ---------------------------------------------------------------- A. ceil rounding
static void test_ceil(const std::string &dir)
{
    const int f0 = g_fail;
    std::ifstream f(dir + "/round_ceil.txt");
    std::string line;
    long rows = 0;
    while (std::getline(f, line))
    {
        if (line.empty() || line[0] == '#') continue;
        long long p, out; int s;
        if (std::sscanf(line.c_str(), "%lld %d %lld", &p, &s, &out) != 3) { g_fail++; continue; }
        const long long got = static_cast<long long>(rshift(p, s, CEIL));
        const long long wide = static_cast<long long>(rshift_wide(p, s, CEIL));
        CHECK(got == out && wide == out, "[A] ceil(%lld >> %d) = %lld / %lld, want %lld\n", p, s, got, wide, out);
        rows++;
    }
    CHECK(rows > 100, "[A] no vectors read from %s\n", dir.c_str());
    // the four modes that existed keep their values (spot check of the enum order)
    CHECK(rshift(5, 1, FLOOR) == 2 && rshift(5, 1, HALF_UP) == 3 && rshift(-5, 1, HALF_AWAY) == -3 && rshift(5, 1, HALF_EVEN) == 2,
          "[A] an existing rounding mode changed\n");
    CHECK(rshift_wide(static_cast<i128>(1) << 70, 70, CEIL) == 1 && rshift_wide((static_cast<i128>(1) << 70) + 1, 70, CEIL) == 2,
          "[A] ceil with a shift above 63\n");
    std::printf("[A] ceil rounding: %ld vectors\n", rows);
    verdict("A", f0);
}

// ---------------------------------------------------------------- B. log-scale LUT index
static void test_logscale(const std::string &dir)
{
    const int f0 = g_fail;
    for (int rs = 0; rs < 2; rs++)
    {
        const int base = rs ? 0 : 7, qt = rs ? 14 : 21;
        std::ifstream f(dir + (rs ? "/lut_ls_rsqrt.txt" : "/lut_ls_recip.txt"));
        std::string line;
        std::vector<int32_t> T;
        long rows = 0, bad = 0;
        const std::vector<int32_t> mine = LutLogScale::make_table(rs != 0, base, qt);
        LutLogScale *L = nullptr;
        while (std::getline(f, line))
        {
            if (line.empty() || line[0] == '#') continue;
            if (line[0] == 'T') { T = ints_after(line); L = new LutLogScale(T, base, 7, rs != 0); continue; }
            unsigned long long x; long idx, r;
            if (!L || std::sscanf(line.c_str(), "%llu %ld %ld", &x, &idx, &r) != 3) { bad++; continue; }
            const LutResult got = L->lookup(uint32_t(x));
            if (L->index(uint32_t(x)) != idx || got.r != r || got.e_shift != 7) bad++;
            rows++;
        }
        CHECK(T == mine, "[B] %s: make_table differs from the reference table\n", rs ? "rsqrt" : "recip");
        CHECK(int(T.size()) == (rs ? 512 : 256), "[B] %s table has %zu entries\n", rs ? "rsqrt" : "recip", T.size());
        CHECK(rows > 1000 && bad == 0, "[B] %s: %ld rows, %ld mismatches\n", rs ? "rsqrt" : "recip", rows, bad);
        // accuracy of the direct entry (no interpolation) on the covered range, entries not saturated and >= 64
        double worst = 0, sum = 0; long n = 0; uint32_t wx = 0;
        if (L)
            for (uint32_t x = 1u << (rs ? 4 : 7); x < (1u << (rs ? 12 : 15)); x += (x < 4096 ? 1 : 1 + x / 997))
            {
                if (!L->in_range(x)) continue;
                const double want = rs ? 1.0 / std::sqrt(double(x)) : 1.0 / double(x);
                const double e = std::fabs(std::ldexp(double(L->lookup(x).r), -qt) - want) / want;
                sum += e; n++;
                if (e > worst) { worst = e; wx = x; }
            }
        std::printf("[B] log-scale %s: %ld vectors; relative error on %s: max %.3e (x = %u), mean %.3e\n", rs ? "rsqrt" : "recip", rows,
                    rs ? "[2^4, 2^12)" : "[2^7, 2^15)", worst, wx, n ? sum / n : 0.0);
        CHECK(worst < 0.05, "[B] %s error %.3e above 5 %%\n", rs ? "rsqrt" : "recip", worst);
        if (L)
        {
            CHECK(!L->in_range(0) && (rs || !L->in_range(100)) && L->in_range(1u << 20), "[B] in_range\n");
            delete L;
        }
    }
    verdict("B", f0);
}

// ---------------------------------------------------------------- C. PWL mode with the MAS table depths
static void test_pwl_depth(const std::string &dir)
{
    const int f0 = g_fail;
    const LutIndirect ext(14), mas(14, true);
    CHECK(mas.mas_recip_image().size() == 256 && mas.mas_rsqrt_image().size() == 512, "[C] image sizes\n");
    CHECK(2 * (mas.mas_recip_image().size() + mas.mas_rsqrt_image().size()) + 256 + 256 == 2048, "[C] LUT memory is not 2 KB\n");
    for (int rs = 0; rs < 2; rs++)
    {
        std::ifstream f(dir + (rs ? "/lut_pwl_mas_rsqrt.txt" : "/lut_pwl_mas_recip.txt"));
        std::string line;
        long rows = 0, bad = 0, differ = 0;
        double worst_ext = 0, worst_mas = 0;
        while (std::getline(f, line))
        {
            if (line.empty() || line[0] == '#') continue;
            unsigned long long x; long r, e;
            if (std::sscanf(line.c_str(), "%llu %ld %ld", &x, &r, &e) != 3) { bad++; continue; }
            const LutResult a = rs ? mas.rsqrt(uint32_t(x)) : mas.recip(uint32_t(x));
            const LutResult b = rs ? ext.rsqrt(uint32_t(x)) : ext.recip(uint32_t(x));
            if (a.r != r || a.e_shift != e) bad++;
            if (a.r != b.r) differ++;
            const double want = rs ? 1.0 / std::sqrt(double(x)) : 1.0 / double(x);
            worst_mas = std::max(worst_mas, std::fabs(mas.value(a) - want) / want);
            worst_ext = std::max(worst_ext, std::fabs(ext.value(b) - want) / want);
            rows++;
        }
        CHECK(rows > 1000 && bad == 0, "[C] %s: %ld rows, %ld mismatches\n", rs ? "rsqrt" : "recip", rows, bad);
        std::printf("[C] PWL %s, %d entries: %ld vectors, %ld differ from the 257-entry table; max relative error %.3e (257 entries: %.3e)\n",
                    rs ? "rsqrt" : "recip", rs ? 512 : 256, rows, differ, worst_mas, worst_ext);
    }
    verdict("C", f0);
}

// ---------------------------------------------------------------- D. softmax with the log-scale reciprocal
static void test_softmax(const std::string &dir)
{
    const int f0 = g_fail;
    std::ifstream f(dir + "/softmax_ls.txt");
    std::string line;
    std::vector<int32_t> E, T;
    const Knobs k;
    const LutIndirect pwl(14);
    long rows = 0, bad = 0;
    double cos_ls_min = 1, cos_pwl_min = 1, lsb_ls = 0, lsb_pwl = 0;
    while (std::getline(f, line))
    {
        if (line.empty() || line[0] == '#') continue;
        if (line[0] == 'E') { E = ints_after(line); continue; }
        if (line[0] == 'T') { T = ints_after(line); continue; }
        std::istringstream in(line);
        int L; std::string bar;
        in >> L >> bar;
        std::vector<int> x(static_cast<size_t>(L)), a(static_cast<size_t>(L));
        for (auto &v : x) in >> v;
        long m, sigma, r;
        in >> bar >> m >> sigma >> r >> bar;
        for (auto &v : a) in >> v;
        const LutLogScale ls(T, 7, 7);
        SoftmaxCfg c;
        c.exp = E.data();
        c.ls_recip = &ls;
        const SoftmaxRow o = softmax_row(x, c, k);
        long diff = (o.m != m) + (o.sigma != sigma) + (o.r != r) + (o.E != 7);
        for (int j = 0; j < L; j++) diff += o.a[size_t(j)] != a[size_t(j)];
        if (diff) bad++;
        // accuracy against the exact softmax of the same exp table: p_j = e_j / sigma, a_j = 256 p_j - 128
        SoftmaxCfg c2;
        c2.exp = E.data();
        c2.log_recip = &pwl;
        const SoftmaxRow o2 = softmax_row(x, c2, k);
        double dot1 = 0, dot2 = 0, nn = 0, n1 = 0, n2 = 0;
        for (int j = 0; j < L; j++)
        {
            const double p = double(E[size_t(o.m - x[size_t(j)])]) / double(o.sigma);
            const double q1 = (o.a[size_t(j)] + 128) / 256.0, q2 = (o2.a[size_t(j)] + 128) / 256.0;
            dot1 += p * q1; dot2 += p * q2; nn += p * p; n1 += q1 * q1; n2 += q2 * q2;
            lsb_ls = std::max(lsb_ls, std::fabs(256.0 * p - 128.0 - o.a[size_t(j)]));
            lsb_pwl = std::max(lsb_pwl, std::fabs(256.0 * p - 128.0 - o2.a[size_t(j)]));
        }
        if (n1 > 0) cos_ls_min = std::min(cos_ls_min, dot1 / std::sqrt(nn * n1));
        if (n2 > 0) cos_pwl_min = std::min(cos_pwl_min, dot2 / std::sqrt(nn * n2));
        rows++;
    }
    CHECK(rows >= 100 && bad == 0, "[D] softmax log-scale: %ld rows, %ld mismatching\n", rows, bad);
    std::printf("[D] softmax, log-scale reciprocal: %ld rows bit-exact with the reference\n", rows);
    std::printf("[D] against exact e/sigma: log-scale min cosine %.5f, max error %.2f LSB; PWL min cosine %.5f, max error %.2f LSB\n",
                cos_ls_min, lsb_ls, cos_pwl_min, lsb_pwl);
    CHECK(softmax_cycles(197) == 200 + 202 + 215, "[D] softmax_cycles default changed\n");
    CHECK(softmax_cycles(197, kLutStagesPwl) == softmax_cycles(197) + 2 && softmax_cycles(197, kLutStagesLogScale) + 1 == softmax_cycles(197),
          "[D] softmax_cycles with the MAS LUT depths\n");
    verdict("D", f0);
}

// ---------------------------------------------------------------- E. load/store unit: ports and address generator
static void test_lsu()
{
    const int f0 = g_fail;
    using B = LsuBank; using M = LsuMode;
    const int want[4][5] = {{-1, -1, 0, 1, -1}, {-1, 0, -1, -1, -1}, {0, 2, -1, -1, 1}, {0, 1, -1, -1, -1}};
    for (int b = 0; b < 4; b++)
        for (int m = 0; m < 5; m++)
            CHECK(lsu_port(B(b), M(m)) == want[b][m], "[E] port of %s %s = %d, want %d\n", lsu_bank_name(B(b)), lsu_mode_name(M(m)),
                  lsu_port(B(b), M(m)), want[b][m]);
    GvuLsu lsu;
    lsu.store(B::VBANK1, M::V32I16, 64);     // LayerNorm stage 3: var + epsilon, 2 vectors of 32 x int16
    lsu.load(B::VBANK1, M::V32I16, 33);
    lsu.load(B::VBANK0, M::V32I32, 32);      // no such port
    lsu.store_axi(4096);
    const auto &c = lsu.at(B::VBANK1, M::V32I16);
    CHECK(c.wr == 2 && c.wr_bytes == 128 && c.rd == 2 && c.rd_bytes == 66 && lsu.illegal() == 1 && lsu.axi_wr == 1 && lsu.axi_wr_bytes == 4096,
          "[E] v32int16 / illegal / AXI counting\n");
    CHECK(lsu.report().find("vbank#1 v32int16 rd 2 (66 B) wr 2 (128 B)") != std::string::npos, "[E] report line\n");

    // address generator: tensor [2][3][4][5] at 100
    const uint32_t shp[4] = {2, 3, 4, 5};
    const GvuAgu t = GvuAgu::tensor(100, shp);
    const std::vector<int64_t> lin = t.sequence();
    bool ok = lin.size() == 120;
    for (size_t i = 0; ok && i < lin.size(); i++) ok = lin[i] == int64_t(100 + i);
    CHECK(ok, "[E] tensor / reshape order is not linear\n");
    // reshape [2][3][4][5] -> [1][6][2][10]: same addresses in the same order
    const uint32_t shp2[4] = {1, 6, 2, 10};
    CHECK(GvuAgu::tensor(100, shp2).sequence() == lin, "[E] reshape changes the address order\n");
    // transpose (0, 2, 1, 3): out[a][c][b][d] = in[a][b][c][d]
    const int perm[4] = {0, 2, 1, 3};
    const GvuAgu p = t.permuted(perm);
    ok = p.size() == 120;
    for (uint32_t a = 0; a < 2 && ok; a++)
        for (uint32_t cc = 0; cc < 4 && ok; cc++)
            for (uint32_t b = 0; b < 3 && ok; b++)
                for (uint32_t d = 0; d < 5 && ok; d++) ok = p.addr(a, cc, b, d) == int64_t(100 + ((a * 3 + b) * 4 + cc) * 5 + d);
    CHECK(ok, "[E] permute addresses\n");
    // slice axis 2, offset 1, end 4, step 2 -> elements 1 and 3
    const GvuAgu s = t.sliced(2, 1, 4, 2);
    CHECK(s.count[2] == 2 && s.addr(0, 0, 0, 0) == 105 && s.addr(0, 0, 1, 0) == 115 && s.addr(1, 2, 1, 4) == 100 + ((1 * 3 + 2) * 4 + 3) * 5 + 4,
          "[E] slice addresses\n");
    // split axis 1 into sizes 1 + 2, concat of the same parts: disjoint and complete
    std::set<int64_t> seen;
    size_t total = 0;
    uint32_t off = 0;
    for (uint32_t part : {1u, 2u})
    {
        const GvuAgu g = GvuAgu::concat_part(100, shp, 1, off, part);
        const GvuAgu g2 = t.sliced(1, off, off + part, 1);
        CHECK(g.sequence() == g2.sequence(), "[E] concat part and split part differ\n");
        for (int64_t a : g.sequence()) { seen.insert(a); total++; }
        off += part;
    }
    CHECK(total == 120 && seen.size() == 120 && *seen.begin() == 100 && *seen.rbegin() == 219, "[E] split / concat coverage\n");
    std::printf("[E] load/store unit: port table 20 entries, v32int16 mode, address generator (reshape, permute, slice, split, concat)\n");
    verdict("E", f0);
}

// ---------------------------------------------------------------- F. Scratchpad port map and capacity
static void test_sp_map()
{
    const int f0 = g_fail;
    int stages = 0;
    for (const SpStage &st : pipeline_sp_access())
    {
        std::string why;
        const int n = sp_port_conflicts(st, &why);
        CHECK(n == 0, "[F] %s: %d port problem(s): %s\n", st.pipeline, n, why.c_str());
        stages++;
    }
    CHECK(stages == 16, "[F] %d stages, want 16\n", stages);
    // negative controls: the check must see a second same-cycle access on a single-port bank and a missing port
    SpStage bad1{"neg1", {{"x", LsuBank::VBANK0, LsuMode::V32I8, false, 0}, {"y", LsuBank::VBANK0, LsuMode::V32I8, true, 0}}};
    SpStage bad2{"neg2", {{"x", LsuBank::VBANK2, LsuMode::V32I16, false, 0}}};
    SpStage bad3{"neg3", {{"a", LsuBank::SBANK, LsuMode::S32, false, 0}, {"b", LsuBank::SBANK, LsuMode::S32, false, 0}}};
    CHECK(sp_port_conflicts(bad1) == 1 && sp_port_conflicts(bad2) == 1 && sp_port_conflicts(bad3) == 1, "[F] negative controls not detected\n");

    Knobs def, mas;
    mas.scratch_mas128k();
    CHECK(def.sp_banked == 0 && def.sp_v0 == 4096 && def.sp_v1 == 4096 && def.sp_v2 == 16384 && def.sp_s == 0 && def.scratch_bytes == 24576,
          "[F] default Scratchpad knobs changed\n");
    CHECK(def.str().find("sbank") == std::string::npos, "[F] default knob string changed\n");
    const uint64_t half = uint64_t(mas.sp_v0) + mas.sp_v1 + mas.sp_v2 + mas.sp_s;
    CHECK(mas.sp_banked == 1 && half == 60928 && half <= Knobs::kMasScratchHalfBytes, "[F] MAS preset = %llu B\n", (unsigned long long)half);
    const SpBytes t256 = mas_capacity_table(256), t512 = mas_capacity_table(512);
    std::printf("[F] 16 stages of the port table: no conflict. MAS capacity table: typical %llu B, maximum %llu B, half of 128 KB = %u B\n",
                (unsigned long long)t256.total(), (unsigned long long)t512.total(), Knobs::kMasScratchHalfBytes);
    CHECK(t256.total() == 61108 && t256.total() <= Knobs::kMasScratchHalfBytes, "[F] typical total\n");
    CHECK(t512.total() == 92596 && t512.total() > Knobs::kMasScratchHalfBytes, "[F] maximum total\n");

    struct Case { const char *name; SpPipeline p; uint64_t n; int want_rows; };
    const Case cases[] = {
        {"Softmax L 197", SpPipeline::Softmax, 197, 32},
        {"Softmax L 768", SpPipeline::Softmax, 768, 32},
        {"LayerNorm H 256", SpPipeline::LayerNorm, 256, 32},
        {"LayerNorm H 512", SpPipeline::LayerNorm, 512, 16},
        {"LayerNorm H 768", SpPipeline::LayerNorm, 768, 0},
        {"Residual 32 per row", SpPipeline::ResidualAdd, 32, 32},
    };
    for (const Case &c : cases)
    {
        const int rows = sp_max_rows(c.p, c.n, mas);
        const SpBytes b = sp_need(c.p, 32, c.n);
        std::printf("[F]   %-20s 32 rows need v0/v1/v2/s = %llu/%llu/%llu/%llu B; rows that fit the MAS preset: %d\n", c.name,
                    (unsigned long long)b.v0, (unsigned long long)b.v1, (unsigned long long)b.v2, (unsigned long long)b.s, rows);
        CHECK(rows == c.want_rows, "[F] %s: %d rows fit, want %d\n", c.name, rows, c.want_rows);
    }
    // LayerNorm H 768 (ViT-B): the sbank of the preset (2 560 B) is below gamma + beta (3 840 B); with a larger sbank the D
    // values in vbank#2 limit the lanes to 10 rows.
    Knobs big = mas;
    big.sp_s = 4096;
    const int r768 = sp_max_rows(SpPipeline::LayerNorm, 768, big);
    std::printf("[F]   LayerNorm H 768 with sbank 4096 B: %d rows per group (vbank#2 holds D for %u rows)\n", r768, mas.sp_v2 / (4 * 768));
    CHECK(r768 == 10, "[F] LayerNorm H 768 rows = %d, want 10\n", r768);
    CHECK(sp_max_rows(SpPipeline::LayerNorm, 256, mas) == 32 && !sp_fits(sp_need(SpPipeline::LayerNorm, 32, 256, true), mas),
          "[F] LayerNorm result kept in vbank#1\n");
    // balanced split of the same half: 14 LayerNorm rows of 768, ADD chunk 7 904, softmax of ViT-B (L 197) still 32 rows
    Knobs bal;
    bal.scratch_mas128k_bal();
    const uint64_t bal_half = uint64_t(bal.sp_v0) + bal.sp_v1 + bal.sp_v2 + bal.sp_s;
    const int bal_ln = sp_max_rows(SpPipeline::LayerNorm, 768, bal), bal_sm = sp_max_rows(SpPipeline::Softmax, 197, bal);
    const uint32_t bal_chunk = std::min(std::min(bal.sp_v0, bal.sp_v1), bal.sp_v2 / 4) / 32 * 32;
    std::printf("[F]   balanced preset: %llu B per half; LayerNorm H 768 %d rows, Softmax L 197 %d rows, ADD chunk %u elements\n",
                (unsigned long long)bal_half, bal_ln, bal_sm, bal_chunk);
    CHECK(bal_half == Knobs::kMasScratchHalfBytes && bal_ln == 14 && bal_sm == 32 && bal_chunk == 7904, "[F] balanced preset\n");
    verdict("F", f0);
}

// ---------------------------------------------------------------- G. stream register file totals
static void test_vrf()
{
    const int f0 = g_fail;
    const RegNeed fit[] = {{"3 x i64", 3, 0, 0, 0}, {"6 x i32", 0, 6, 0, 0}, {"6 x i16", 0, 0, 6, 0}, {"4 x i8", 0, 0, 0, 4}};
    const RegNeed nofit[] = {{"4 x i64", 4, 0, 0, 0}, {"7 x i32", 0, 7, 0, 0}, {"7 x i16", 0, 0, 7, 0}, {"5 x i8", 0, 0, 0, 5}};
    for (const RegNeed &n : fit) CHECK(vrf_fits(n, VrfLayout::Spec), "[G] %s does not fit\n", n.pipeline);
    for (const RegNeed &n : nofit) CHECK(!vrf_fits(n, VrfLayout::Spec), "[G] %s fits\n", n.pipeline);
    CHECK((3 * 8 + 4 * 4) * 32 == 1280, "[G] register file bytes\n");
    int stages = 0;
    for (const RegNeed &n : pipeline_reg_needs()) { CHECK(vrf_fits(n, VrfLayout::Spec), "[G] %s does not fit\n", n.pipeline); stages++; }
    std::printf("[G] stream registers: logical totals 3 x v32int64, 6 x v32int32, 6 x v32int16, 4 x v32int8 (1 280 B); %d stages fit\n", stages);
    verdict("G", f0);
}

// ---------------------------------------------------------------- H. pipeline controller
static std::string join(const std::vector<std::string> &v)
{
    std::string s;
    for (const auto &x : v) s += (s.empty() ? "" : " ") + x;
    return s;
}

static void test_ctrl()
{
    const int f0 = g_fail;
    const GvuPipelineCtrl pc;
    const PcTrace s = pc.softmax(40, 8, false);   // 2 groups of rows, 8 elements per row
    const std::string want_s =
        "IDLE SETUP READ_DATA STAGE_1 x8 WRITE_DATA READ_DATA STAGE_1 x8 WRITE_DATA "
        "SETUP READ_DATA STAGE_2 x8 WRITE_DATA READ_DATA STAGE_2 x8 WRITE_DATA "
        "SETUP READ_DATA STAGE_3 x8 READ_DATA STAGE_3 x8 IDLE";
    CHECK(join(s.log) == want_s, "[H] softmax sequence:\n  got  %s\n  want %s\n", join(s.log).c_str(), want_s.c_str());
    CHECK(s.at(PcState::SETUP) == 3 && s.at(PcState::READ_DATA) == 6 && s.at(PcState::WRITE_DATA) == 4 && s.at(PcState::STAGE) == 48 &&
          s.at(PcState::WAIT_SA) == 0 && s.at(PcState::IDLE) == 2, "[H] softmax visit counts\n");
    const PcTrace sf = pc.softmax(32, 197, true);
    CHECK(sf.at(PcState::WAIT_SA) == 1 && sf.at(PcState::READ_DATA) == 2 && sf.stage_visits[1] == 197 && sf.stage_visits[3] == 197,
          "[H] softmax inside FUSED_ATTN\n");
    // one group: stage cycles = softmax_cycles(L); the controller adds 3 SETUP (one with a multiplier flush) + 3 reads + 2 writes
    const PcTrace s1 = pc.softmax(32, 197, false);
    CHECK(s1.cycles == softmax_cycles(197) + (1 + 1 + 6) + 3 + 2, "[H] softmax cycles %llu\n", (unsigned long long)s1.cycles);

    const PcTrace l = pc.layernorm(33, 4);
    const std::string want_l =
        "IDLE SETUP READ_DATA STAGE_1 x4 WRITE_DATA READ_DATA STAGE_1 x4 WRITE_DATA "
        "SETUP READ_DATA READ_DATA STAGE_2 x4 STAGE_2 x1 READ_DATA READ_DATA STAGE_2 x4 STAGE_2 x1 "
        "SETUP READ_DATA STAGE_3 x2 "
        "SETUP READ_DATA READ_DATA STAGE_4 x4 READ_DATA STAGE_4 x4 "
        "SETUP READ_DATA READ_DATA STAGE_5 x4 STAGE_5 x4 END";
    CHECK(join(l.log) == want_l, "[H] layernorm sequence:\n  got  %s\n  want %s\n", join(l.log).c_str(), want_l.c_str());
    CHECK(l.at(PcState::SETUP) == 5 && l.at(PcState::END) == 1 && l.stage_visits[2] == 10 && l.stage_visits[3] == 2, "[H] layernorm visit counts\n");

    const PcTrace r = pc.residual(100);
    CHECK(join(r.log) == "IDLE SETUP READ_DATA STAGE_1 x4 SETUP READ_DATA STAGE_2 x4 IDLE", "[H] residual sequence: %s\n", join(r.log).c_str());
    std::printf("[H] pipeline controller: softmax, FUSED_ATTN softmax, layernorm and residual sequences; softmax 32 x 197: %llu cycles "
                "(stages %llu + control %llu)\n", (unsigned long long)s1.cycles, (unsigned long long)softmax_cycles(197),
                (unsigned long long)(s1.cycles - softmax_cycles(197)));
    verdict("H", f0);
}

// ---------------------------------------------------------------- I. behaviour read from the RTL of the vector unit
static void test_rtl(const std::string &dir)
{
    const int f0 = g_fail;
    long rows = 0;
    for (int m = 0; m < 2; m++)
    {
        std::ifstream f(dir + (m ? "/round_away.txt" : "/round_trunc.txt"));
        std::string line;
        while (std::getline(f, line))
        {
            if (line.empty() || line[0] == '#') continue;
            long long p, out; int s;
            if (std::sscanf(line.c_str(), "%lld %d %lld", &p, &s, &out) != 3) { g_fail++; continue; }
            const int mode = m ? AWAY : TRUNC;
            CHECK(static_cast<long long>(rshift(p, s, mode)) == out && static_cast<long long>(rshift_wide(p, s, mode)) == out,
                  "[I] round mode %d of %lld >> %d, want %lld\n", mode, p, s, out);
            rows++;
        }
    }
    CHECK(rows > 8000, "[I] rounding vectors missing\n");
    // zero-point adder: 16-bit, wrapping. x * S >> s saturates to 32767; + zp 100 wraps to -32669 -> clamp -128 (reference: 127)
    Knobs ref, rtl;
    rtl.spec_rtl();
    QuantCounters c;
    CHECK(requant(1 << 20, 1 << 10, 5, 100, ref) == 127 && requant(1 << 20, 1 << 10, 5, 100, rtl, &c) == -128 && c.zpwrap == 1,
          "[I] wrapping zero-point adder (positive)\n");
    CHECK(requant(-(1 << 20), 1 << 10, 5, -100, ref) == -128 && requant(-(1 << 20), 1 << 10, 5, -100, rtl, &c) == 127 && c.zpwrap == 2,
          "[I] wrapping zero-point adder (negative)\n");
    long same = 0;
    for (int x = -40000; x <= 40000; x += 7)
        for (int zp : {-128, -3, 0, 77, 127})
            same += requant(x, 1 << 8, 8, zp, ref) == requant(x, 1 << 8, 8, zp, rtl);   // |r| <= 40000: wraps only near the int16 limits
    CHECK(same > 0, "[I] requant sweep\n");
    // zero input of the LUT, entry differences of the PWL tables as signed 8-bit values
    const LutIndirect lr(14, true, true), lref(14);
    CHECK(lr.recip(0).r == 0 && lr.recip(0).zero_in && lr.rsqrt(0).r == 0 && lref.recip(0).r == INT16_MAX, "[I] LUT zero input\n");
    CHECK(lr.recip(1000).r == LutIndirect(14, true).recip(1000).r, "[I] zero option changed a non-zero lookup\n");
    int dmax = 0;
    const std::vector<int16_t> ri = lr.mas_recip_image(), si = lr.mas_rsqrt_image();
    for (size_t i = 0; i + 1 < ri.size(); i++) dmax = std::max(dmax, std::abs(ri[i + 1] - ri[i]));
    for (size_t i = 0; i + 1 < si.size(); i++) dmax = std::max(dmax, std::abs(si[i + 1] - si[i]));
    CHECK(dmax <= 127, "[I] a table difference (%d) does not fit a signed 8-bit value\n", dmax);
    // log-scale index below 16: no fine bits; zero input returns 0
    const std::vector<int32_t> T = LutLogScale::make_table(true, 0, 14);
    const LutLogScale ls(T, 0, 0, true);
    CHECK(ls.index(9) == (3 << 4) && ls.index(15) == (3 << 4) && ls.index(16) == (4 << 4) && ls.index(24) == ((4 << 4) | 8) && ls.lookup(0).r == 0,
          "[I] log-scale index of small inputs\n");
    // scalar bank words, knob group, default knobs untouched
    CHECK(sp_need(SpPipeline::LayerNorm, 32, 768, false, true).s == 6144 && sp_need(SpPipeline::LayerNorm, 32, 768).s == 3840, "[I] sbank words\n");
    CHECK(rtl.lut_mas_depth == 1 && rtl.lut_stages == 4 && rtl.lut_zero0 == 1 && rtl.zp_add_wrap16 == 1 && rtl.sp_s_word32 == 1 && rtl.lat_obp == 6 &&
          ref.zp_add_wrap16 == 0 && ref.lut_zero0 == 0 && ref.sp_s_word32 == 0 && ref.lat_obp == 6 && ref.str().find("zp_add") == std::string::npos,
          "[I] knob group\n");
    // default configuration of the instruction path
    Knobs dflt;
    dflt.spec_default();
    const uint64_t dhalf = uint64_t(dflt.sp_v0) + dflt.sp_v1 + dflt.sp_v2 + dflt.sp_s;
    const uint32_t dchunk = std::min(std::min(dflt.sp_v0, dflt.sp_v1), dflt.sp_v2 / 4) / 32 * 32;
    CHECK(dhalf == Knobs::kMasScratchHalfBytes && sp_max_rows(SpPipeline::LayerNorm, 768, dflt) == 14 &&
          sp_max_rows(SpPipeline::Softmax, 197, dflt) == 32 && sp_max_rows(SpPipeline::LayerNorm, 256, dflt) == 32 && dchunk == 5600 &&
          dflt.sp_limit_rows == 1 && dflt.lut_mas_depth == 1 && dflt.lut_stages == 4 && dflt.lut_zero0 == 1 && dflt.sp_s_word32 == 1 &&
          dflt.zp_add_wrap16 == 0 && dflt.round_mode == HALF_UP, "[I] default configuration (spec_default)\n");
    std::printf("[I] default configuration: %llu B per half (10784 / 5600 / 43008 + sbank 6144), LayerNorm H 768 %d rows, ADD chunk %u\n",
                (unsigned long long)dhalf, sp_max_rows(SpPipeline::LayerNorm, 768, dflt), dchunk);
    Knobs balw;
    balw.scratch_mas128k_bal();
    balw.sp_s_word32 = 1;
    std::printf("[I] RTL items: %ld rounding vectors (toward zero, away from zero), wrapping zero-point adder, LUT zero input, largest table "
                "difference %d (signed 8-bit), log-scale small inputs; LayerNorm H 768 with word-sized scalars needs sbank %d B "
                "(balanced preset then holds %d rows)\n", rows, dmax, 6144, sp_max_rows(SpPipeline::LayerNorm, 768, balw));
    verdict("I", f0);
}

int main(int argc, char **argv)
{
    if (argc < 2) { std::fprintf(stderr, "usage: %s <vector dir written by tools/has/mas_ref.py>\n", argv[0]); return 2; }
    const std::string dir = argv[1];
    test_ceil(dir);
    test_logscale(dir);
    test_pwl_depth(dir);
    test_softmax(dir);
    test_lsu();
    test_sp_map();
    test_vrf();
    test_ctrl();
    test_rtl(dir);
    std::printf("RESULT: %s (%d failed checks)\n", g_fail ? "FAIL" : "PASS", g_fail);
    return g_fail ? 1 : 0;
}
