// tb_rtl_ref_gvu_elemwise.cpp -- self-checking test of has/rtl_ref_gvu_elemwise.h: the cycle-level max pool and residual
// add pipelines, run with the ported load / store unit and Scratchpad, against
//   max pool      the maximum of every window computed directly on the tensor;
//   residual add  has::dequant, a wrapping 32-bit add and has::requant of the functional model (has/gvu_quant.h) with
//                 the wrapping zero-point adder (the arithmetic of the ported units).
// It also checks the cycle count of every run against rows * window + constant (max pool) and 2 * rows + constant
// (residual add). The data are random with the corner values mixed in; the shapes include the max pools of YOLOv8m
// (5 x 5, stride 1, padding 2 on 20 x 20).
// Edge cases, after the random runs: max pool with constant data (-128, 127), a tensor of one element, widths of 32 and 33
// outputs, bursts of exactly the capacity of the bank and one vector more; residual add with 1, 31, 32, 33 ... elements,
// lengths around the capacity of a burst, every alignment of tensor B inside the 1024-bit rows.
// build: g++ -std=c++17 -O2 -Wall -I. tools/has/tb_rtl_ref_gvu_elemwise.cpp -o tb_rtl_ref_gvu_elemwise
#include <cinttypes>
#include <cstdio>
#include <random>
#include <vector>
#include "has/gvu_quant.h"
#include "has/rtl_ref_gvu_elemwise.h"

using namespace rtl_ref_gvu;
static std::mt19937_64 g_rng(0x454C454Du);
static int g_fail = 0;
static int g_fill = 0;               // max pool data: 0 random, 1 all -128, 2 all 127
static int g_b_row = -1;             // residual add: first row of tensor B, -1 random
#define CHECK(c, ...) do { if (!(c)) { if (g_fail < 20) { std::printf("FAIL: " __VA_ARGS__); std::printf("\n"); } g_fail++; } } while (0)

static int8_t rnd8()
{
    const int k = static_cast<int>(g_rng() % 12);
    if (k == 0) return -128;
    if (k == 1) return 127;
    if (k == 2) return 0;
    return static_cast<int8_t>(g_rng());
}

// One max pool on a [C, H, W] tensor; returns the cycles spent in the pipeline.
static uint64_t run_maxpool(int C, int H, int W, int kk, int s, int p, uint64_t *elems, uint64_t *runs)
{
    const int Hp = H + 2 * p, Wp = W + 2 * p, Ho = (Hp - kk) / s + 1, Wo = (Wp - kk) / s + 1, vpr = (Wo + 31) / 32, win = kk * kk;
    std::vector<int8_t> in(static_cast<size_t>(C) * H * W);
    for (auto &v : in) v = g_fill == 1 ? static_cast<int8_t>(-128) : (g_fill == 2 ? static_cast<int8_t>(127) : rnd8());
    auto at = [&](int c, int y, int x) -> int8_t {                       // padded tensor, padding value -128
        y -= p; x -= p;
        return (y < 0 || y >= H || x < 0 || x >= W) ? static_cast<int8_t>(-128) : in[(static_cast<size_t>(c) * H + static_cast<size_t>(y)) * W + static_cast<size_t>(x)];
    };
    // output vectors in order: channel, output row, vector of the row
    struct OutVec { int c, y, x0; };
    std::vector<OutVec> ov;
    for (int c = 0; c < C; c++) for (int y = 0; y < Ho; y++) for (int v = 0; v < vpr; v++) ov.push_back({c, y, 32 * v});
    const size_t per_chunk = std::min<size_t>(800 / static_cast<size_t>(win), 128);     // rows of vbank#0, int8 rows of vbank#1
    uint64_t cyc = 0;
    for (size_t base = 0; base < ov.size(); base += per_chunk)
    {
        const size_t n = std::min(per_chunk, ov.size() - base);
        ElemwiseBench b;
        GvuMaxPoolPipe dut;
        // loader (C1): window-major rows
        for (size_t r = 0; r < n; r++)
            for (int ky = 0; ky < kk; ky++)
                for (int kx = 0; kx < kk; kx++)
                {
                    SpBytes row(32, static_cast<uint8_t>(0x80));
                    for (int l = 0; l < 32 && ov[base + r].x0 + l < Wo; l++)
                        row[static_cast<size_t>(l)] = static_cast<uint8_t>(at(ov[base + r].c, ov[base + r].y * s + ky, (ov[base + r].x0 + l) * s + kx));
                    b.poke_v0(static_cast<unsigned>(r * static_cast<size_t>(win) + static_cast<size_t>(ky * kk + kx)), row);
                }
        GvuMaxPoolPipe::In cfg;
        cfg.i_out_rows = static_cast<unsigned>(n); cfg.i_win = static_cast<unsigned>(win); cfg.i_src_row = 0; cfg.i_dst_row = 0;
        b.step(dut, cfg);                                                // configuration stable for one cycle
        cfg.i_start = true; b.step(dut, cfg); cfg.i_start = false;
        const uint64_t c0 = b.cycles;
        bool done = false;
        for (uint64_t guard = 0; guard < n * static_cast<uint64_t>(win) + 100 && !done; guard++) done = b.step(dut, cfg).o_done;
        const uint64_t used = b.cycles - c0;
        CHECK(done, "max pool k %d s %d: no done", kk, s);
        CHECK(used == n * static_cast<uint64_t>(win) + 4, "max pool k %d s %d rows %zu: %" PRIu64 " cycles, expected rows * win + 4 = %" PRIu64, kk, s, n, used, n * static_cast<uint64_t>(win) + 4);
        cyc += used;
        (*runs)++;
        for (size_t r = 0; r < n; r++)
        {
            const SpBytes row = b.peek_v1(static_cast<unsigned>(r / 4));
            for (int l = 0; l < 32 && ov[base + r].x0 + l < Wo; l++)
            {
                int8_t mx = -128;
                for (int ky = 0; ky < kk; ky++) for (int kx = 0; kx < kk; kx++) mx = std::max(mx, at(ov[base + r].c, ov[base + r].y * s + ky, (ov[base + r].x0 + l) * s + kx));
                const int8_t got = static_cast<int8_t>(row[32 * (r % 4) + static_cast<size_t>(l)]);
                CHECK(got == mx, "max pool k %d s %d c %d y %d x %d: got %d want %d", kk, s, ov[base + r].c, ov[base + r].y, ov[base + r].x0 + l, got, mx);
                (*elems)++;
            }
        }
    }
    return cyc;
}

struct AddStats { uint64_t elems{0}, runs{0}, cycles{0}, sum_overflow{0}, differ_default{0}; };

// dist 0: scales and shifts of a quantized residual add; 1: full ranges (saturation and wrap paths)
static void run_resadd(size_t n_elems, int dist, AddStats *st)
{
    std::vector<int8_t> A(n_elems), B(n_elems);
    for (auto &v : A) v = rnd8();
    for (auto &v : B) v = rnd8();
    GvuResAddPipe::In cfg;
    if (dist == 0)
    {
        cfg.i_zp_a = static_cast<int>(g_rng() % 256) - 128; cfg.i_zp_b = static_cast<int>(g_rng() % 256) - 128; cfg.i_zp_out = static_cast<int>(g_rng() % 256) - 128;
        cfg.i_scale_a = static_cast<int32_t>((1u << 30) + g_rng() % (1u << 30)); cfg.i_scale_b = static_cast<int32_t>((1u << 30) + g_rng() % (1u << 30));
        cfg.i_scale_out = static_cast<int32_t>((1u << 30) + g_rng() % (1u << 30));
        cfg.i_shift_a = 18 + static_cast<unsigned>(g_rng() % 8); cfg.i_shift_b = 18 + static_cast<unsigned>(g_rng() % 8); cfg.i_shift_out = 40 + static_cast<unsigned>(g_rng() % 8);
    }
    else
    {
        cfg.i_zp_a = static_cast<int>(g_rng() % 256) - 128; cfg.i_zp_b = static_cast<int>(g_rng() % 256) - 128; cfg.i_zp_out = static_cast<int>(g_rng() % 65536) - 32768;
        cfg.i_scale_a = static_cast<int32_t>(g_rng()); cfg.i_scale_b = static_cast<int32_t>(g_rng()); cfg.i_scale_out = static_cast<int32_t>(g_rng());
        cfg.i_shift_a = static_cast<unsigned>(g_rng() % 40); cfg.i_shift_b = static_cast<unsigned>(g_rng() % 40); cfg.i_shift_out = static_cast<unsigned>(g_rng() % 64);
    }
    static const RoundMode rm[4] = {RND_NEAR_UP, RND_NEAR_AWAY, RND_NEAR_EVEN, RND_FLOOR};
    static const int hm[4] = {has::HALF_UP, has::HALF_AWAY, has::HALF_EVEN, has::FLOOR};
    const int ri = dist == 0 ? 0 : static_cast<int>(g_rng() % 4);
    cfg.i_round = rm[ri];
    has::Knobs kr, kd;                                                   // kr: wrapping zero-point adder, as the units; kd: default
    kr.round_mode = hm[ri]; kr.zp_add_wrap16 = 1;
    kd.round_mode = hm[ri];
    const size_t rows_total = (n_elems + 31) / 32, per_chunk = 124;      // vbank#1 holds 128 int8 rows; B starts at any of the first four
    for (size_t r0 = 0; r0 < rows_total; r0 += per_chunk)
    {
        const size_t rows = std::min(per_chunk, rows_total - r0);
        ElemwiseBench b;
        GvuResAddPipe dut;
        const unsigned b_row = g_b_row >= 0 ? static_cast<unsigned>(g_b_row) : static_cast<unsigned>(g_rng() % 4);   // any alignment inside the 1024-bit rows
        std::vector<SpBytes> v1(static_cast<size_t>((b_row + rows + 3) / 4), SpBytes(128, 0));
        for (size_t r = 0; r < rows; r++)
        {
            SpBytes ra(32, 0);
            for (size_t l = 0; l < 32; l++)
            {
                const size_t e = (r0 + r) * 32 + l;
                ra[l] = e < n_elems ? static_cast<uint8_t>(A[e]) : 0;
                v1[(b_row + r) / 4][32 * ((b_row + r) % 4) + l] = e < n_elems ? static_cast<uint8_t>(B[e]) : 0;
            }
            b.poke_v0(static_cast<unsigned>(r), ra);
        }
        for (size_t q = 0; q < v1.size(); q++) b.poke_v1(static_cast<unsigned>(q), v1[q]);
        cfg.i_rows = static_cast<unsigned>(rows); cfg.i_a_row = 0; cfg.i_b_row = b_row; cfg.i_sum_row = static_cast<unsigned>(g_rng() % 64);
        const size_t tail = n_elems - (r0 + rows - 1) * 32;
        cfg.i_last_lane_mask = (r0 + rows == rows_total && tail < 32) ? ((1u << tail) - 1u) : 0xFFFFFFFFu;
        b.step(dut, cfg);
        cfg.i_start = true; b.step(dut, cfg); cfg.i_start = false;
        const uint64_t c0 = b.cycles;
        bool done = false;
        for (uint64_t guard = 0; guard < 2 * rows + 200 && !done; guard++) done = b.step(dut, cfg).o_done;
        const uint64_t used = b.cycles - c0;
        CHECK(done, "residual add: no done");
        CHECK(used == 2 * rows + 22, "residual add rows %zu: %" PRIu64 " cycles, expected 2 * rows + 22 = %zu", rows, used, 2 * rows + 22);
        st->cycles += used; st->runs++;
        for (size_t r = 0; r < rows; r++)
        {
            const SpBytes srow = b.peek_v2(cfg.i_sum_row + static_cast<unsigned>(r)), orow = b.peek_v0(static_cast<unsigned>(r));
            for (size_t l = 0; l < 32; l++)
            {
                const size_t e = (r0 + r) * 32 + l;
                if (e >= n_elems) break;
                const int32_t da = has::dequant(A[e], cfg.i_zp_a, cfg.i_scale_a, static_cast<int>(cfg.i_shift_a), kr), db = has::dequant(B[e], cfg.i_zp_b, cfg.i_scale_b, static_cast<int>(cfg.i_shift_b), kr);
                const int32_t sum = static_cast<int32_t>(static_cast<uint32_t>(da) + static_cast<uint32_t>(db));
                if (static_cast<int64_t>(da) + db != sum) st->sum_overflow++;
                const int32_t got_sum = static_cast<int32_t>(static_cast<uint32_t>(srow[4 * l]) | (static_cast<uint32_t>(srow[4 * l + 1]) << 8) | (static_cast<uint32_t>(srow[4 * l + 2]) << 16) | (static_cast<uint32_t>(srow[4 * l + 3]) << 24));
                CHECK(got_sum == sum, "residual add element %zu: sum %d, expected %d (a %d b %d)", e, got_sum, sum, A[e], B[e]);
                const int8_t want = has::requant(sum, cfg.i_scale_out, static_cast<int>(cfg.i_shift_out), cfg.i_zp_out, kr), got = static_cast<int8_t>(orow[l]);
                CHECK(got == want, "residual add element %zu: out %d, expected %d (sum %d)", e, got, want, sum);
                if (has::requant(has::add_sat32(da, db), cfg.i_scale_out, static_cast<int>(cfg.i_shift_out), cfg.i_zp_out, kd) != got) st->differ_default++;
                st->elems++;
            }
        }
    }
}

int main()
{
    int f0 = g_fail;
    uint64_t elems = 0, runs = 0, cyc = 0;
    const struct { int C, H, W, k, s, p; } mp[] = {
        {6, 20, 20, 5, 1, 2},     // the max pool of YOLOv8m (SPPF): 5 x 5, stride 1, padding 2
        {3, 40, 40, 2, 2, 0}, {2, 33, 47, 3, 2, 1}, {4, 7, 70, 3, 1, 1}, {2, 16, 16, 1, 1, 0}, {1, 9, 100, 2, 1, 0},
    };
    for (const auto &m : mp) cyc += run_maxpool(m.C, m.H, m.W, m.k, m.s, m.p, &elems, &runs);
    std::printf("[cycle] max pool: %" PRIu64 " output elements in %" PRIu64 " runs, %" PRIu64 " cycles (rows * window + 4 per run), %s\n", elems, runs, cyc, g_fail == f0 ? "ok" : "FAIL");

    f0 = g_fail;
    AddStats s0, s1;
    for (int it = 0; it < 40; it++) run_resadd(64 + static_cast<size_t>(g_rng() % 9000), 0, &s0);
    std::printf("[cycle] residual add, scales of a quantized add: %" PRIu64 " elements in %" PRIu64 " runs, %" PRIu64 " cycles (2 * rows + 22 per run), 32-bit sum overflows %" PRIu64 ", elements that differ from the model with default knobs %" PRIu64 ", %s\n",
                s0.elems, s0.runs, s0.cycles, s0.sum_overflow, s0.differ_default, g_fail == f0 ? "ok" : "FAIL");
    f0 = g_fail;
    for (int it = 0; it < 60; it++) run_resadd(64 + static_cast<size_t>(g_rng() % 5000), 1, &s1);
    std::printf("[cycle] residual add, full parameter ranges: %" PRIu64 " elements in %" PRIu64 " runs, %" PRIu64 " cycles, 32-bit sum overflows %" PRIu64 ", elements that differ from the model with default knobs %" PRIu64 ", %s\n",
                s1.elems, s1.runs, s1.cycles, s1.sum_overflow, s1.differ_default, g_fail == f0 ? "ok" : "FAIL");

    // ---- edge cases ---------------------------------------------------------------------------------------------------
    f0 = g_fail;
    elems = 0; runs = 0; cyc = 0;
    const struct { int C, H, W, k, s, p; } me[] = {
        {1, 1, 1, 1, 1, 0}, {1, 1, 1, 3, 1, 1}, {1, 2, 2, 2, 2, 0}, {1, 3, 3, 5, 1, 2},
        {1, 4, 32, 1, 1, 0}, {1, 4, 33, 1, 1, 0}, {2, 5, 32, 3, 1, 1}, {2, 5, 33, 3, 1, 1}, {1, 6, 64, 2, 2, 0}, {1, 6, 66, 2, 2, 0}, {1, 6, 65, 2, 2, 1},
        {1, 32, 32, 5, 1, 2}, {1, 33, 20, 5, 1, 2},     // 5 x 5: 32 output vectors fill vbank#0 (800 rows), 33 need a second burst
        {1, 88, 9, 3, 1, 1}, {1, 89, 9, 3, 1, 1},       // 3 x 3: 88 and 89 output vectors
        {1, 128, 5, 1, 1, 0}, {1, 129, 5, 1, 1, 0},     // 1 x 1: 128 and 129 output vectors (the int8 rows of vbank#1)
        {1, 256, 8, 2, 2, 0}, {1, 258, 8, 2, 2, 0},     // 2 x 2: 128 and 129 output vectors
        {3, 11, 13, 5, 2, 2}, {2, 12, 40, 3, 2, 0}, {2, 9, 9, 2, 1, 1},
    };
    for (g_fill = 0; g_fill < 3; g_fill++) for (const auto &m : me) cyc += run_maxpool(m.C, m.H, m.W, m.k, m.s, m.p, &elems, &runs);
    g_fill = 0;
    std::printf("[cycle] max pool, edge cases (random, all -128, all 127): %" PRIu64 " output elements in %" PRIu64 " runs, %" PRIu64 " cycles, %s\n", elems, runs, cyc, g_fail == f0 ? "ok" : "FAIL");

    f0 = g_fail;
    AddStats e0, e1;
    static const size_t sizes[] = {1, 2, 31, 32, 33, 63, 64, 65, 127, 128, 129, 3967, 3968, 3969, 4000, 4096, 4097, 7936, 7937};   // a burst holds 124 rows = 3968 elements
    for (g_b_row = 0; g_b_row < 4; g_b_row++)
        for (size_t n : sizes) { run_resadd(n, 0, &e0); run_resadd(n, 1, &e1); }
    g_b_row = -1;
    std::printf("[cycle] residual add, edge lengths and alignments: quantized scales %" PRIu64 " elements in %" PRIu64 " runs, elements that differ from the model with default knobs %" PRIu64 "; full ranges %" PRIu64 " elements in %" PRIu64 " runs, 32-bit sum overflows %" PRIu64 ", %s\n",
                e0.elems, e0.runs, e0.differ_default, e1.elems, e1.runs, e1.sum_overflow, g_fail == f0 ? "ok" : "FAIL");
    std::printf("RESULT: %s (failures %d)\n", g_fail == 0 ? "PASS" : "FAIL", g_fail);
    return g_fail == 0 ? 0 : 1;
}
