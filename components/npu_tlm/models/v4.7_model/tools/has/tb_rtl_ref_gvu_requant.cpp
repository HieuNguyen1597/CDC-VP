// tb_rtl_ref_gvu_requant.cpp -- self-checking test of has/rtl_ref_gvu_requant.h: the cycle-level Requant pipeline and the
// GEMM_FUSED epilogue, against has::requant and has::lut_direct of the functional model (has/gvu_quant.h) with the
// wrapping zero-point adder (the arithmetic of the ported units).
// Every beat is checked for its value, for the cycle it leaves (8 cycles after it is presented for the Requant pipeline,
// 9 for the epilogue), for its order and for the address and lane mask carried with it; the number of output beats must
// equal the number of input beats. The streams have random gaps, bursts that follow each other without a gap, and a
// scale and shift that change with every beat.
//   directed   corner values of the data, the scale, the shift and the zero point, all seven rounding modes
//   quantized  scales and shifts of a quantized convolution
//   full       full parameter ranges (saturation and wrap paths)
// build: g++ -std=c++17 -O2 -Wall -I. tools/has/tb_rtl_ref_gvu_requant.cpp -o tb_rtl_ref_gvu_requant
#include <cinttypes>
#include <cstdio>
#include <random>
#include <vector>
#include "has/gvu_quant.h"
#include "has/rtl_ref_gvu_requant.h"

using namespace rtl_ref_gvu;
static std::mt19937_64 g_rng(0x52455155u);
static int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { if (g_fail < 20) { std::printf("FAIL: " __VA_ARGS__); std::printf("\n"); } g_fail++; } } while (0)

static const RoundMode kRm[7] = {RND_FLOOR, RND_NEAR_UP, RND_NEAR_AWAY, RND_NEAR_EVEN, RND_CEIL, RND_TRUNC, RND_AWAY};
static const int kHm[7] = {has::FLOOR, has::HALF_UP, has::HALF_AWAY, has::HALF_EVEN, has::CEIL, has::TRUNC, has::AWAY};

struct Beat
{
    V32 x{};
    int32_t scale{1};
    unsigned shift{0};
    uint32_t addr{0}, mask{0xFFFFFFFFu};
    unsigned gap{0};                 // idle cycles before the beat
};
struct Stats { uint64_t beats{0}, elems{0}, cycles{0}, differ_default{0}, sat16{0}, zpwrap{0}; };

static int32_t corner32()
{
    static const int32_t c[] = {INT32_MIN, INT32_MIN + 1, -65536, -32769, -32768, -129, -128, -1, 0, 1, 127, 128, 32767, 32768, 65535, INT32_MAX - 1, INT32_MAX};
    return c[g_rng() % (sizeof c / sizeof c[0])];
}

// overloads chosen by the fields the module has
template <typename I> static auto set_act(I &in, bool act_en, int) -> decltype(in.i_act_en, void()) { in.i_act_en = act_en; }
template <typename I> static void set_act(I &, bool, long) {}

// Runs one stream with the zero point, the rounding mode and (epilogue) the activation enable held. table = nullptr: the
// Requant pipeline alone.
template <typename Dut> static void run_stream(Dut &dut, const std::vector<Beat> &beats, int zp, int ri, const int8_t *table, bool act_en, int latency, Stats *st)
{
    has::Knobs kr, kd;                                                   // kr: wrapping zero-point adder, as the units; kd: default
    kr.round_mode = kHm[ri]; kr.zp_add_wrap16 = 1;
    kd.round_mode = kHm[ri];
    std::vector<uint64_t> sent(beats.size(), 0);
    size_t in_idx = 0, out_idx = 0;
    unsigned wait = beats.empty() ? 0 : beats[0].gap;
    uint64_t cyc = 0;
    const uint64_t limit = [&] { uint64_t n = static_cast<uint64_t>(latency) + 8; for (const Beat &b : beats) n += b.gap + 1; return n; }();
    for (; cyc < limit; cyc++)
    {
        typename Dut::In in;
        in.i_zp = zp; in.i_round = kRm[ri];
        if (in_idx < beats.size() && wait == 0)
        {
            const Beat &b = beats[in_idx];
            in.i_valid = true; in.i_data = b.x; in.i_scale = b.scale; in.i_shift = b.shift; in.i_addr = b.addr; in.i_lane_mask = b.mask;
        }
        set_act(in, act_en, 0);
        const typename Dut::Out o = dut.eval(in);
        if (o.o_valid)
        {
            CHECK(out_idx < beats.size(), "output beat %zu without an input beat", out_idx);
            if (out_idx < beats.size())
            {
                const Beat &b = beats[out_idx];
                CHECK(cyc == sent[out_idx] + static_cast<uint64_t>(latency), "beat %zu leaves at cycle %" PRIu64 ", presented at %" PRIu64 ", expected + %d", out_idx, cyc, sent[out_idx], latency);
                CHECK(o.o_addr == b.addr && o.o_lane_mask == b.mask, "beat %zu: address / mask %u / %08x, expected %u / %08x", out_idx, o.o_addr, o.o_lane_mask, b.addr, b.mask);
                has::QuantCounters qc;
                for (size_t l = 0; l < GVU_LANES; l++)
                {
                    const int32_t x = static_cast<int32_t>(b.x[l]);
                    int8_t want = has::requant(x, b.scale, static_cast<int>(b.shift), zp, kr, &qc);
                    int8_t dflt = has::requant(x, b.scale, static_cast<int>(b.shift), zp, kd);
                    if (table && act_en) { want = has::lut_direct(table, want); dflt = has::lut_direct(table, dflt); }
                    const int8_t got = static_cast<int8_t>(o.o_data[l]);
                    CHECK(got == want, "beat %zu lane %zu: x %d scale %d shift %u zp %d round %d: got %d want %d", out_idx, l, x, b.scale, b.shift, zp, kHm[ri], got, want);
                    if (got != dflt) st->differ_default++;
                    st->elems++;
                }
                st->sat16 += qc.sat16; st->zpwrap += qc.zpwrap;
            }
            out_idx++;
        }
        dut.tick(in);
        if (in.i_valid) { sent[in_idx] = cyc; in_idx++; wait = in_idx < beats.size() ? beats[in_idx].gap : 0; }
        else if (wait > 0) wait--;
    }
    CHECK(in_idx == beats.size() && out_idx == beats.size(), "stream of %zu beats: %zu presented, %zu left", beats.size(), in_idx, out_idx);
    typename Dut::In idle;
    CHECK(!dut.eval(idle).o_busy, "busy after the last beat left");
    st->beats += beats.size(); st->cycles += cyc;
}

static unsigned rnd_gap(int kind) { return kind == 0 ? 0u : (g_rng() % 4 == 0 ? static_cast<unsigned>(g_rng() % 12) : 0u); }

static std::vector<Beat> directed_beats()
{
    static const int32_t scales[] = {1, 2, -1, 1 << 30, INT32_MAX, INT32_MIN, 0, 0x5A5A5A5A};
    static const unsigned shifts[] = {0, 1, 15, 16, 31, 32, 47, 62, 63};
    std::vector<Beat> v;
    uint32_t addr = 0;
    for (int32_t sc : scales)
        for (unsigned sh : shifts)
        {
            Beat b;
            for (size_t l = 0; l < GVU_LANES; l++) b.x[l] = static_cast<uint32_t>(l < 20 ? corner32() : static_cast<int32_t>(g_rng()));
            b.scale = sc; b.shift = sh; b.addr = addr++; b.mask = static_cast<uint32_t>(g_rng()); b.gap = rnd_gap(1);
            v.push_back(b);
        }
    return v;
}
// dist 0: a quantized convolution; 1: full ranges
static std::vector<Beat> random_beats(size_t n, int dist, int gap_kind)
{
    std::vector<Beat> v(n);
    for (size_t k = 0; k < n; k++)
    {
        Beat &b = v[k];
        if (dist == 0)
        {
            b.scale = static_cast<int32_t>((1u << 30) + g_rng() % (1u << 30)); b.shift = 45 + static_cast<unsigned>(g_rng() % 6);
            for (size_t l = 0; l < GVU_LANES; l++) b.x[l] = static_cast<uint32_t>(static_cast<int32_t>(g_rng() % (1u << 22)) - (1 << 21));
        }
        else
        {
            b.scale = static_cast<int32_t>(g_rng()); b.shift = static_cast<unsigned>(g_rng() % 64);
            for (size_t l = 0; l < GVU_LANES; l++) b.x[l] = static_cast<uint32_t>(g_rng() % 6 == 0 ? corner32() : static_cast<int32_t>(g_rng()));
        }
        b.addr = static_cast<uint32_t>(g_rng()); b.mask = static_cast<uint32_t>(g_rng()); b.gap = rnd_gap(gap_kind);
    }
    return v;
}

int main()
{
    static const int zps[] = {-128, -1, 0, 1, 127, -32768, 32767, 12345, -20000};
    std::printf("[cycle] depths: Requant pipeline %d, GEMM epilogue %d\n", GvuRequantPipe::kLatency, GvuGemmEpilogue::kLatency);

    // ---- Requant pipeline -------------------------------------------------------------------------------------------
    int f0 = g_fail;
    Stats sd;
    {
        GvuRequantPipe dut;                                              // one module for all the streams: it must drain between them
        for (int ri = 0; ri < 7; ri++) for (int zp : zps) run_stream(dut, directed_beats(), zp, ri, nullptr, false, GvuRequantPipe::kLatency, &sd);
    }
    std::printf("[cycle] Requant, directed: %" PRIu64 " beats, %" PRIu64 " elements, %" PRIu64 " cycles, int16 saturations %" PRIu64 ", zero-point wraps %" PRIu64 ", elements that differ from the model with default knobs %" PRIu64 ", %s\n",
                sd.beats, sd.elems, sd.cycles, sd.sat16, sd.zpwrap, sd.differ_default, g_fail == f0 ? "ok" : "FAIL");
    f0 = g_fail;
    Stats sq;
    {
        GvuRequantPipe dut;
        for (int it = 0; it < 60; it++) run_stream(dut, random_beats(1 + g_rng() % 700, 0, it % 3 == 0 ? 0 : 1), static_cast<int>(g_rng() % 256) - 128, 1, nullptr, false, GvuRequantPipe::kLatency, &sq);
    }
    std::printf("[cycle] Requant, scales of a quantized convolution: %" PRIu64 " beats, %" PRIu64 " elements, %" PRIu64 " cycles, int16 saturations %" PRIu64 ", zero-point wraps %" PRIu64 ", elements that differ from the model with default knobs %" PRIu64 ", %s\n",
                sq.beats, sq.elems, sq.cycles, sq.sat16, sq.zpwrap, sq.differ_default, g_fail == f0 ? "ok" : "FAIL");
    f0 = g_fail;
    Stats sf;
    {
        GvuRequantPipe dut;
        for (int it = 0; it < 120; it++) run_stream(dut, random_beats(1 + g_rng() % 300, 1, it % 3 == 0 ? 0 : 1), static_cast<int>(g_rng() % 65536) - 32768, static_cast<int>(g_rng() % 7), nullptr, false, GvuRequantPipe::kLatency, &sf);
    }
    std::printf("[cycle] Requant, full parameter ranges: %" PRIu64 " beats, %" PRIu64 " elements, %" PRIu64 " cycles, int16 saturations %" PRIu64 ", zero-point wraps %" PRIu64 ", elements that differ from the model with default knobs %" PRIu64 ", %s\n",
                sf.beats, sf.elems, sf.cycles, sf.sat16, sf.zpwrap, sf.differ_default, g_fail == f0 ? "ok" : "FAIL");

    // ---- GEMM epilogue ----------------------------------------------------------------------------------------------
    f0 = g_fail;
    Stats se, sn;
    {
        GvuGemmEpilogue dut;
        for (int it = 0; it < 80; it++)
        {
            int8_t table[256];
            for (int8_t &t : table) t = static_cast<int8_t>(g_rng());
            if (it % 8 == 0) for (int n = 0; n < 256; n++) table[n] = static_cast<int8_t>(n - 128);   // identity
            dut.load_act(table);
            const int dist = it % 2;
            const int zp = dist == 0 ? static_cast<int>(g_rng() % 256) - 128 : static_cast<int>(g_rng() % 65536) - 32768;
            const int ri = dist == 0 ? 1 : static_cast<int>(g_rng() % 7);
            const bool act = it % 4 != 3;
            run_stream(dut, random_beats(1 + g_rng() % 500, dist, it % 3 == 0 ? 0 : 1), zp, ri, table, act, GvuGemmEpilogue::kLatency, act ? &se : &sn);
        }
    }
    std::printf("[cycle] GEMM epilogue, activation table: %" PRIu64 " beats, %" PRIu64 " elements, %" PRIu64 " cycles, elements that differ from the model with default knobs %" PRIu64 "; skip_act: %" PRIu64 " beats, %" PRIu64 " elements, %s\n",
                se.beats, se.elems, se.cycles, se.differ_default, sn.beats, sn.elems, g_fail == f0 ? "ok" : "FAIL");

    // ---- reset with beats in flight -----------------------------------------------------------------------------------
    f0 = g_fail;
    {
        GvuGemmEpilogue dut;
        GvuGemmEpilogue::In in;
        in.i_valid = true;
        for (int c = 0; c < 5; c++) dut.tick(in);
        in.i_valid = false;
        dut.tick(in, false);
        int seen = 0;
        for (int c = 0; c < 20; c++) { if (dut.eval(in).o_valid) seen++; dut.tick(in); }
        CHECK(seen == 0 && !dut.eval(in).o_busy, "reset: %d beats left the pipeline", seen);
    }
    std::printf("[cycle] reset with beats in flight: %s\n", g_fail == f0 ? "ok" : "FAIL");
    std::printf("RESULT: %s (failures %d)\n", g_fail == 0 ? "PASS" : "FAIL", g_fail);
    return g_fail == 0 ? 0 : 1;
}
