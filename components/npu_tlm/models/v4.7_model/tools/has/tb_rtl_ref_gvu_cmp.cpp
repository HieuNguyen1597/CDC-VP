// tb_rtl_ref_gvu_cmp.cpp -- difference report between the functional vector-unit model (has/gvu_*.h, namespace has) and
// the blocks ported from the RTL (has/rtl_ref_gvu_*.h, namespace rtl_ref_gvu) on the same vectors. It changes nothing and
// judges nothing: every row prints how many vectors differ and the first difference. Three knob sets of the model:
//   v47ref   has::Knobs()                 the configuration before the MAS
//   default  Knobs::spec_default()        the default of the instruction path
//   rtl      Knobs() + spec_rtl()         HAS_SPEC=rtl
// The RTL side of a pipeline is a chain of the ported units in the order of the pipeline drawing; that composition is this
// testbench's, the RTL of the pipeline controller and of the stream-switch routing does not exist yet.
// build: g++ -std=c++17 -O2 -Wall -I. tools/has/tb_rtl_ref_gvu_cmp.cpp -o tb_rtl_ref_gvu_cmp
#include <cinttypes>
#include <cstdio>
#include <random>
#include <string>
#include "has/gvu_layernorm.h"
#include "has/gvu_lut.h"
#include "has/gvu_quant.h"
#include "has/gvu_scratchpad.h"
#include "has/gvu_sp_map.h"
#include "has/gvu_vrf.h"
#include "has/rtl_ref_gvu_fu.h"
#include "has/rtl_ref_gvu_lut.h"
#include "has/rtl_ref_gvu_scratchpad.h"
#include "has/rtl_ref_gvu_vrf.h"

namespace R = rtl_ref_gvu;
static std::mt19937_64 g_rng(0x47565533u);

struct Row
{
    uint64_t n{0}, diff{0};
    char first[200]{0};
    template <typename... A> void miss(const char *fmt, A... a) { if (!diff) snprintf(first, sizeof first, fmt, a...); diff++; }
    void print(const char *block, const char *cfg, const char *vectors) const
    {
        printf("%-34s | %-8s | %-30s | %9" PRIu64 " | %9" PRIu64 " | %s\n", block, cfg, vectors, n, diff, diff ? first : "-");
    }
};
static void header(const char *title)
{
    printf("\n== %s\n%-34s | %-8s | %-30s | %9s | %9s | %s\n", title, "model function vs ported unit", "knobs", "vectors", "count", "differ", "first difference");
}

static R::RoundMode map_round(int m)
{
    switch (m)
    {
    case has::FLOOR: return R::RND_FLOOR;
    case has::HALF_UP: return R::RND_NEAR_UP;
    case has::HALF_AWAY: return R::RND_NEAR_AWAY;
    case has::HALF_EVEN: return R::RND_NEAR_EVEN;
    case has::CEIL: return R::RND_CEIL;
    case has::TRUNC: return R::RND_TRUNC;
    default: return R::RND_AWAY;
    }
}
static const char *round_name(int m) { static const char *n[7] = {"FLOOR", "HALF_UP", "HALF_AWAY", "HALF_EVEN", "CEIL", "TRUNC", "AWAY"}; return n[m]; }
static int64_t rnd64(int bits)
{
    const int k = static_cast<int>(g_rng() % 10);
    const int64_t v = static_cast<int64_t>(g_rng()) >> (64 - bits);
    if (k == 0) return 0;
    if (k == 1) return (static_cast<int64_t>(1) << (bits - 1)) - 1;
    if (k == 2) return -(static_cast<int64_t>(1) << (bits - 1));
    if (k == 3) return static_cast<int64_t>(g_rng() % 512) - 256;
    return v;
}

// has::rshift / has::rshift_wide against gvu_vroundshift64 / 72 (wrap narrowing to 32 bits so the low 32 bits compare).
static void cmp_roundshift()
{
    header("Round-shift");
    for (int mode = 0; mode < 7; mode++)
    {
        Row r64, r72, r72h;
        for (int it = 0; it < 300000; it++)
        {
            const int s = static_cast<int>(g_rng() % 64);
            int64_t p = rnd64(64);
            if (it % 3 == 0 && s > 0) p = (rnd64(20) << s) + (static_cast<int64_t>(1) << (s - 1)) + static_cast<int64_t>(g_rng() % 3) - 1;   // ties
            const has::i128 m = has::rshift(p, s, mode);
            const R::RsOut o = R::vroundshift64_lane(static_cast<uint64_t>(p), static_cast<unsigned>(s), map_round(mode), R::NARROW_WRAP, R::NW_32);
            r64.n++;
            if (static_cast<uint32_t>(static_cast<int64_t>(m)) != o.out_data || (m > INT32_MAX || m < INT32_MIN) != o.out_ovf)
                r64.miss("p=%" PRId64 " s=%d model=%" PRId64 " rtl=%08x ovf=%d", p, s, static_cast<int64_t>(m), o.out_data, o.out_ovf);
        }
        for (int it = 0; it < 300000; it++)
        {
            const bool high = it & 1;
            const int s = high ? 73 + static_cast<int>(g_rng() % 54) : static_cast<int>(g_rng() % 73);
            const has::i128 p = (static_cast<has::i128>(static_cast<int8_t>(rnd64(8))) << 64) | static_cast<uint64_t>(g_rng()); // 72-bit value
            const has::i128 m = has::rshift_wide(p, s, mode);
            const R::RsOut o = R::vroundshift72_lane(static_cast<R::u128>(p), static_cast<unsigned>(s), map_round(mode), R::NARROW_WRAP, R::NW_32);
            Row &r = high ? r72h : r72;
            r.n++;
            if (static_cast<uint32_t>(static_cast<int64_t>(m)) != o.out_data)
                r.miss("p=%02x%016" PRIx64 " s=%d model=%" PRId64 " rtl=%d", static_cast<unsigned>(static_cast<uint64_t>(p >> 64) & 0xFF), static_cast<uint64_t>(p), s, static_cast<int64_t>(m), static_cast<int32_t>(o.out_data));
        }
        char b[64];
        snprintf(b, sizeof b, "rshift %s / vroundshift64", round_name(mode)); r64.print(b, "any", "int64, shift 0..63, ties");
        snprintf(b, sizeof b, "rshift_wide %s / vroundshift72", round_name(mode)); r72.print(b, "any", "int72, shift 0..72");
        r72h.print(b, "any", "int72, shift 73..126");
    }
}

// Requant: Mul 32x32 -> RoundShift int64 (saturate, int16) -> Add zero point (16-bit element) -> Clamp int8.
static int rtl_requant(int32_t x, int32_t S, int s, int zp, int round_mode, bool *ovf)
{
    const uint64_t p = R::vmul32x32_lane(R::MA_32X32, static_cast<uint32_t>(x), static_cast<uint32_t>(S));
    const R::RsOut rs = R::vroundshift64_lane(p, static_cast<unsigned>(s), map_round(round_mode), R::NARROW_SATURATE, R::NW_16);
    const uint32_t sum = R::adder32_lane(R::ADD_16, 0, 0, false, rs.out_data & 0xFFFFu, static_cast<uint32_t>(zp) & 0xFFFFu) & 0xFFFFu;
    if (ovf) *ovf = rs.out_ovf;
    return R::sext32(R::clamp_lane(sum, 0xFF80u, 0x007Fu, 16, true), 16);
}
// Dequant: Sub zero point (16-bit element) -> Mul 32x16 -> RoundShift int64 (saturate, int32).
static int32_t rtl_dequant(int x8, int zp, int32_t S, int s, int round_mode)
{
    const uint32_t d = R::adder32_lane(R::ADD_16, 0, 1, false, static_cast<uint32_t>(x8) & 0xFFFFu, static_cast<uint32_t>(zp) & 0xFFFFu) & 0xFFFFu;
    const uint64_t p = R::vmul32x32_lane(R::MA_32X16, static_cast<uint32_t>(S), d);
    return static_cast<int32_t>(R::vroundshift64_lane(p, static_cast<unsigned>(s), map_round(round_mode), R::NARROW_SATURATE, R::NW_32).out_data);
}

static void cmp_quant()
{
    header("Requant / dequant / add (pipeline composed from the ported units)");
    has::Knobs kv, kd, kr;
    kd.spec_default();
    kr.spec_rtl();
    const struct { const char *name; const has::Knobs *k; } cfg[3] = {{"v47ref", &kv}, {"default", &kd}, {"rtl", &kr}};
    for (int dist = 0; dist < 3; dist++)
        for (const auto &c : cfg)
        {
            Row r; uint64_t sat_then_wrap = 0;
            for (int it = 0; it < 500000; it++)
            {
                int32_t x, S; int s, zp = static_cast<int>(g_rng() % 256) - 128;
                if (dist == 0) { x = static_cast<int32_t>(rnd64(21)); S = static_cast<int32_t>((1u << 30) + (g_rng() % (1u << 30))); s = 31 + static_cast<int>(g_rng() % 15); }
                else if (dist == 1) { x = static_cast<int32_t>(rnd64(32)); S = static_cast<int32_t>(rnd64(32)); s = static_cast<int>(g_rng() % 64); }
                else { x = static_cast<int32_t>(rnd64(26)); S = static_cast<int32_t>((1u << 30) + (g_rng() % (1u << 30))); s = 24 + static_cast<int>(g_rng() % 8); } // results at the int16 limits
                const int m = has::requant(x, S, s, zp, *c.k);
                bool ovf = false;
                const int h = rtl_requant(x, S, s, zp, c.k->round_mode, &ovf);
                r.n++;
                if (m != h) { r.miss("x=%d S=%d s=%d zp=%d model=%d rtl=%d", x, S, s, zp, m, h); if (ovf) sat_then_wrap++; }
            }
            const char *v = dist == 0 ? "|x|<2^20, Q31 scale, s 31..45" : dist == 1 ? "full int32 range" : "result at the int16 limit";
            r.print("requant / mul + rs + add16 + clamp", c.name, v);
            if (r.diff) printf("%-34s   of which after int16 saturation: %" PRIu64 "\n", "", sat_then_wrap);
        }
    for (const auto &c : cfg)
    {
        Row r;
        for (int it = 0; it < 500000; it++)
        {
            const int x8 = static_cast<int>(g_rng() % 256) - 128, zp = static_cast<int>(g_rng() % 256) - 128;
            const int32_t S = (it & 1) ? static_cast<int32_t>(rnd64(32)) : static_cast<int32_t>((1u << 30) + (g_rng() % (1u << 30)));
            const int s = static_cast<int>(g_rng() % 64);
            const int32_t m = has::dequant(x8, zp, S, s, *c.k), h = rtl_dequant(x8, zp, S, s, c.k->round_mode);
            r.n++;
            if (m != h) r.miss("x8=%d zp=%d S=%d s=%d model=%d rtl=%d", x8, zp, S, s, m, h);
        }
        r.print("dequant / sub16 + mul32x16 + rs", c.name, "int8, zp int8, int32 scale");
    }
    for (int dist = 0; dist < 2; dist++)
    {
        Row r;
        for (int it = 0; it < 500000; it++)
        {
            const int32_t a = static_cast<int32_t>(rnd64(dist ? 32 : 25)), b = static_cast<int32_t>(rnd64(dist ? 32 : 25));
            const int32_t m = has::add_sat32(a, b), h = static_cast<int32_t>(R::adder32_lane(R::ADD_32, 0, 0, false, static_cast<uint32_t>(a), static_cast<uint32_t>(b)));
            r.n++;
            if (m != h) r.miss("a=%d b=%d model=%d rtl=%d", a, b, m, h);
        }
        r.print("add_sat32 / adder32 ADD_32", "any", dist ? "full int32 range" : "|a|, |b| < 2^24");
    }
}

static void cmp_lut()
{
    header("LUT (recip / rsqrt)");
    const struct { const char *name; bool mas_depth, zero0; } cfg[3] = {{"v47ref", false, false}, {"default", true, true}, {"rtl", true, true}};
    for (const auto &c : cfg)
    {
        const has::LutIndirect lut(14, c.mas_depth, c.zero0);
        R::LutMem mem;
        const std::vector<int16_t> ir = lut.mas_recip_image(), is = lut.mas_rsqrt_image();
        mem.load16(R::LUT_RECIP, std::vector<uint16_t>(ir.begin(), ir.end()));
        mem.load16(R::LUT_RSQRT, std::vector<uint16_t>(is.begin(), is.end()));
        for (int rs = 0; rs < 2; rs++)
        {
            Row r, rz; uint64_t idx255 = 0;
            for (uint64_t it = 0; it < 1500000; it++)
            {
                uint32_t x = it < (1u << 20) ? static_cast<uint32_t>(it) : static_cast<uint32_t>(g_rng());
                if (it >= (1u << 20) && it % 4 == 0) x >>= (g_rng() % 32);
                const has::LutResult m = rs ? lut.rsqrt(x) : lut.recip(x);
                const R::LutValue h = R::lut_value(mem, rs ? R::LUT_RSQRT : R::LUT_RECIP, R::LUT_PWL, 0, 16, 8, x);
                Row &row = x == 0 ? rz : r;
                row.n++;
                if (static_cast<uint16_t>(m.r) != h.data || (x && static_cast<uint32_t>(m.e_shift) != h.e_shift) || m.zero_in != h.input_zero)
                {
                    row.miss("x=%u model r=%d E=%d rtl r=%d E=%u", x, m.r, m.e_shift, static_cast<int16_t>(h.data), h.e_shift);
                    if ((h.lut_idx & 0xFF) == 255) idx255++;
                }
            }
            r.print(rs ? "LutIndirect::rsqrt / LUT PWL" : "LutIndirect::recip / LUT PWL", c.name, "x 1..2^20 + random uint32");
            if (r.diff) printf("%-34s   of which with table index 255: %" PRIu64 "\n", "", idx255);
            rz.print(rs ? "LutIndirect::rsqrt / LUT PWL" : "LutIndirect::recip / LUT PWL", c.name, "x = 0");
        }
    }
    for (int rs = 0; rs < 2; rs++)
    {
        Row r;
        for (int base = 0; base < 16; base += 3)
        {
            const std::vector<int32_t> t = has::LutLogScale::make_table(rs != 0, base, 14 + base);
            const has::LutLogScale lut(t, base, 0, rs != 0);
            R::LutMem mem;
            mem.load16(rs ? R::LUT_RSQRT : R::LUT_RECIP, std::vector<uint16_t>(t.begin(), t.end()));
            for (int it = 0; it < 100000; it++)
            {
                uint32_t x = static_cast<uint32_t>(g_rng()) >> (g_rng() % 32);
                if (it < 4096) x = static_cast<uint32_t>(it);
                const has::LutResult m = lut.lookup(x);
                const R::LutValue h = R::lut_value(mem, rs ? R::LUT_RSQRT : R::LUT_RECIP, R::LUT_LOG_SCALE, static_cast<unsigned>(base), 16, 8, x);
                r.n++;
                if (static_cast<uint16_t>(m.r) != h.data || m.zero_in != h.input_zero) r.miss("x=%u base=%d model r=%d rtl r=%d", x, base, m.r, static_cast<int16_t>(h.data));
            }
        }
        r.print(rs ? "LutLogScale (rsqrt) / LUT log-scale" : "LutLogScale (recip) / LUT log-scale", "any", "base 0..15, x incl. 0");
    }
}

static void cmp_latency()
{
    printf("\n== Latency (clock cycles). Model: knobs of has::Knobs. Ported: constants from the RTL parameter defaults.\n");
    has::Knobs k0, kg, kr;
    kg.gvu_latencies();
    kr.rtl_latencies();
    const int mul32 = R::mula_latency(R::MA_32X32), mul16 = R::mula_latency(R::MA_32X16), rs = R::lat_vroundshift();
    const int add = R::kLatAdder32, clamp = R::kLatClamp, cmp = R::lat_vcmp32(), lutd = R::lat_lut(R::LUT_ACT, R::LUT_PWL);
    // "fu" = sum of the latencies inside the units; "fu+reg" = the same with one stream register after every unit whose
    // own latency is 0, plus the read register and (ADD, MAX) the write register the model counts. The second form is an
    // ESTIMATE of this testbench: where the stream registers sit is decided by the stream-switch routing, which has no RTL.
    const int obp_fu = mul32 + rs + add + clamp + lutd, obp_reg = 1 + mul32 + rs + (add + 1) + (clamp + 1) + (lutd + 1);
    const int add1_fu = add + mul16 + rs + add, add1_reg = 1 + (add + 1) + mul16 + rs + (add + 1) + 1;
    const int add2_fu = mul32 + rs + add + clamp, add2_reg = 1 + mul32 + rs + (add + 1) + (clamp + 1) + 1;
    const int max_fu = cmp, max_reg = 1 + (cmp + 1) + 1;
    printf("%-30s | %7s | %13s | %13s | %9s | %12s\n", "pipeline", "Knobs()", "gvu_latencies", "rtl_latencies", "ported fu", "ported fu+reg");
    printf("%-30s | %7d | %13d | %13d | %9d | %12d\n", "OBP (requant + LUT direct)", k0.lat_obp, kg.lat_obp, kr.lat_obp, obp_fu, obp_reg);
    printf("%-30s | %7d | %13d | %13d | %9d | %12d\n", "ELEM_WISE ADD stage 1", k0.lat_add1, kg.lat_add1, kr.lat_add1, add1_fu, add1_reg);
    printf("%-30s | %7d | %13d | %13d | %9d | %12d\n", "ELEM_WISE ADD stage 2", k0.lat_add2, kg.lat_add2, kr.lat_add2, add2_fu, add2_reg);
    printf("%-30s | %7d | %13d | %13d | %9d | %12d\n", "MAX_POOL", k0.lat_max, kg.lat_max, kr.lat_max, max_fu, max_reg);
    printf("%-30s | model %d (knob lut_stages; MAS %d) | ported %d (LERP_MUL_PIPE 1, LERP_SHIFT_PIPE 0)\n", "LUT PWL", k0.lut_stages, has::kLutStagesPwl, R::lat_lut(R::LUT_RECIP, R::LUT_PWL));
    printf("%-30s | model / MAS %d | ported %d\n", "LUT log-scale", has::kLutStagesLogScale, R::lat_lut(R::LUT_RECIP, R::LUT_LOG_SCALE));
    printf("%-30s | model / MAS %d | ported %d (combinational read of a flip-flop table)\n", "LUT direct", has::kLutStagesDirect, lutd);
    printf("%-30s | unit latencies: mul 32x32 %d, 32x16 %d, 32x8 %d, 16x16 %d; mul 48x40 %d / %d / %d / %d; round-shift %d; adder / clamp / shift / cmp 0\n", "ported units",
           mul32, mul16, R::mula_latency(R::MA_32X8), R::mula_latency(R::MA_16X16), R::mulb_latency(R::MB_40X32), R::mulb_latency(R::MB_32X32_16X8), R::mulb_latency(R::MB_32X8), R::mulb_latency(R::MB_3X16X8), rs);
}

static void cmp_vrf()
{
    printf("\n== Registers: the 16 pipeline stages of has::pipeline_reg_needs() against the two RTL organisations\n");
    const R::VrfParams vp;
    int fit_model = 0, fit_vrf = 0, n = 0;
    std::string no_vrf;
    for (const has::RegNeed &q : has::pipeline_reg_needs())
    {
        n++;
        if (has::vrf_fits(q, has::VrfLayout::Spec)) fit_model++;           // = stream_vrf.sv: 3 / 6 / 6 / 4 ports by reconfiguration
        const bool ok = q.i64 <= vp.NUM_INT64_PORT && q.i32 <= vp.NUM_INT32_PORT && q.i16 <= vp.NUM_INT16_PORT && q.i8 <= vp.NUM_INT8_PORT &&
                        q.bytes() <= static_cast<uint32_t>(vp.bytes());
        if (ok) fit_vrf++; else { if (!no_vrf.empty()) no_vrf += "; "; no_vrf += q.pipeline; }
    }
    printf("stream_vrf.sv (3 x int64 + 4 x int32, %d B; ports %d / %d / %d / %d by reconfiguration) = layout Spec of the model: %d of %d stages fit\n",
           R::StreamVrf::kBytes, R::StreamVrf::kVint64Ports, R::StreamVrf::kVint32Ports, R::StreamVrf::kVint16Ports, R::StreamVrf::kVint8Ports, fit_model, n);
    printf("vrf.sv (%d B aliased, ports int64 %d / int32 %d / int16 %d / int8 %d): %d of %d stages fit; not: %s\n", vp.bytes(), vp.NUM_INT64_PORT, vp.NUM_INT32_PORT,
           vp.NUM_INT16_PORT, vp.NUM_INT8_PORT, fit_vrf, n, no_vrf.c_str());
}

static void cmp_scratchpad()
{
    printf("\n== Scratchpad: has/gvu_sp_map.h (MAS port table) and the capacities of the knob sets against gvu_scratchpad.sv\n");
    // Logical ports of gvu_scratchpad.sv: sbank port 0 int32 and port 1 int8 (dual-port memory), vbank0 v32int8, vbank1
    // v32int32 / v32int16 / v32int8 and vbank2 v32int32 / v32int8 (single-port memories: one port per cycle, chosen by
    // i_vbank1_port_sel / i_vbank2_port_sel).
    auto rtl_has_port = [](has::LsuBank b, has::LsuMode m) {
        switch (b)
        {
        case has::LsuBank::SBANK: return m == has::LsuMode::S32 || m == has::LsuMode::S8;
        case has::LsuBank::VBANK0: return m == has::LsuMode::V32I8;
        case has::LsuBank::VBANK1: return m == has::LsuMode::V32I32 || m == has::LsuMode::V32I16 || m == has::LsuMode::V32I8;
        default: return m == has::LsuMode::V32I32 || m == has::LsuMode::V32I8;
        }
    };
    int stages = 0, bad_stages = 0, acc = 0, bad_acc = 0, v2_conflict = 0;
    for (const has::SpStage &st : has::pipeline_sp_access())
    {
        stages++;
        bool bad = false;
        for (size_t i = 0; i < st.acc.size(); i++)
        {
            acc++;
            if (!rtl_has_port(st.acc[i].bank, st.acc[i].mode))
            {
                bad_acc++; bad = true;
                printf("  %-22s %-16s %s %s: no such port in the wrapper\n", st.pipeline, st.acc[i].what, has::lsu_bank_name(st.acc[i].bank), has::lsu_mode_name(st.acc[i].mode));
            }
            for (size_t j = i + 1; j < st.acc.size(); j++)
                if (st.acc[i].bank != has::LsuBank::SBANK && st.acc[i].bank == st.acc[j].bank && st.acc[i].group == st.acc[j].group) { v2_conflict++; bad = true; }
        }
        if (bad) bad_stages++;
    }
    printf("accesses of the MAS port table: %d in %d stages; without a port in the RTL: %d (in %d stages); two accesses of one single-port bank in one cycle: %d\n", acc, stages, bad_acc, bad_stages, v2_conflict);
    const R::ScratchpadParams sp;
    has::Knobs kv, kd, km, kr;
    kd.spec_default();
    km.spec_mas10();
    kr.spec_default(); kr.scratch_rtl();
    const struct { const char *name; const has::Knobs *k; } cfg[4] = {{"v47ref", &kv}, {"default", &kd}, {"mas10", &km}, {"rtl", &kr}};
    printf("capacity per buffer, bytes (bank depths of gvu_pkg.sv: vbank0 %u, vbank1 %u, vbank2 %u, sbank %u = %u):\n", sp.vbank_bytes(0), sp.vbank_bytes(1), sp.vbank_bytes(2),
           sp.sbank_bytes(), sp.vbank_bytes(0) + sp.vbank_bytes(1) + sp.vbank_bytes(2) + sp.sbank_bytes());
    for (const auto &c : cfg)
        printf("  %-8s vbank0 %6u (%s)  vbank1 %6u (%s)  vbank2 %6u (%s)  sbank %6u (%s)\n", c.name,
               c.k->sp_v0, c.k->sp_v0 <= sp.vbank_bytes(0) ? "fits" : "exceeds", c.k->sp_v1, c.k->sp_v1 <= sp.vbank_bytes(1) ? "fits" : "exceeds",
               c.k->sp_v2, c.k->sp_v2 <= sp.vbank_bytes(2) ? "fits" : "exceeds", c.k->sp_s, c.k->sp_s <= sp.sbank_bytes() ? "fits" : "exceeds");
    printf("  (\"rtl\" = the default configuration with HAS_SP_PRESET=rtl)\n");
    printf("rows of 768 elements in parallel with the RTL depths: LayerNorm %d (scalar bank needs %u B for gamma + beta of 768 channels as int32 words, the bank has %u B), Softmax of 197 elements %d\n",
           has::sp_max_rows(has::SpPipeline::LayerNorm, 768, [&] { has::Knobs k2 = kr; k2.sp_s = 0; return k2; }()), 8u * 768u, sp.sbank_bytes(), has::sp_max_rows(has::SpPipeline::Softmax, 197, kr));
    printf("other differences (read from the code, not measured): the model memory (has::HasScratchpad) is one flat byte array that throws on an\n"
           "  out-of-range access; the RTL addresses words (32 bits in sbank, 256 bits in vbank0, 1024 bits in vbank1 / vbank2) with narrow ports on parts of\n"
           "  a word, read data two clocks after the address, no range or error outputs, and no DMA access yet (the DMA side of every bank is not driven).\n"
           "  As written, the byte mask of the memories writes bits [i, i + 8) for mask bit i (see has/rtl_ref_gvu_scratchpad.h). The int8 port of sbank\n"
           "  addresses a byte of an int32 word (model knob sp_s_word32: v47ref %d, default %d).\n", kv.sp_s_word32, kd.sp_s_word32);
}

int main()
{
    printf("tb_rtl_ref_gvu_cmp: functional model (has::) against the units ported from the RTL (rtl_ref_gvu::). Report only.\n");
    cmp_roundshift();
    cmp_quant();
    cmp_lut();
    cmp_latency();
    cmp_vrf();
    cmp_scratchpad();
    printf("\nRESULT: REPORT DONE\n");
    return 0;
}
