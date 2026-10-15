// SPDX-License-Identifier: Apache-2.0
// Core (no SystemC) regressions. Expected values are transcribed by hand from MAS
// Tables 5-10 and 5-15..5-18 and HAS Table 8-6; the implementation derives them from
// JEDEC formulas, so the two sides are independent oracles.
#include "ddr3_check.h"

#include "fx1_ddr3/axi_types.h"
#include "fx1_ddr3/config_checks.h"
#include "fx1_ddr3/profile.h"
#include "fx1_ddr3/speed_bin.h"
#include "fx1_ddr3/timebase.h"

#include <array>
#include <cstring>
#include <stdexcept>
#include <string>

using namespace fx1::ddr3;
using fx1_ddr3_test::expect;
using fx1_ddr3_test::expect_eq;

namespace {

constexpr std::array<SpeedBin, 4> kBins{SpeedBin::DDR3_800, SpeedBin::DDR3_1066, SpeedBin::DDR3_1333,
                                        SpeedBin::DDR3_1600};
using Col = std::array<std::uint32_t, 4>;  // 800, 1066, 1333, 1600

BuildConfig with(SpeedBin bin, Density d = Density::Gb4, Temperature t = Temperature::Normal) {
    BuildConfig c;
    c.speed_bin = bin;
    c.density = d;
    c.temperature = t;
    return c;
}

bool has_error(const BuildConfig& c, const std::string& field) {
    for (const auto& e : validate(c))
        if (e.field == field) return true;
    return false;
}

void test_profile() {
    for (const auto& c : {BuildConfig::native_r2(), BuildConfig::native_r1(), BuildConfig::legacy()})
        expect(validate(c).empty(), std::string("profile legal: ") + to_string(c.profile));

    // MAS Table 5-2 profile-fixed values.
    expect(BuildConfig::native_r2().dfi_ratio() == 2 && BuildConfig::native_r2().axi_data_bits() == 128 &&
               BuildConfig::native_r2().max_ranks() == 2,
           "NATIVE_R2: ratio 2, AXI128, 2 ranks");
    expect(BuildConfig::native_r1().dfi_ratio() == 2 && BuildConfig::native_r1().axi_data_bits() == 128 &&
               BuildConfig::native_r1().max_ranks() == 1,
           "NATIVE_R1: ratio 2, AXI128, 1 rank");
    expect(BuildConfig::legacy().dfi_ratio() == 1 && BuildConfig::legacy().axi_data_bits() == 64 &&
               BuildConfig::legacy().max_ranks() == 1 && BuildConfig::legacy().axi_beat_bytes() == 8,
           "LEGACY: ratio 1, AXI64, 1 rank");

    for (unsigned p = 1; p <= 4; ++p) {
        BuildConfig c;
        c.n_ports = p;
        expect(validate(c).empty(), "n_ports " + std::to_string(p) + " legal");
    }
    BuildConfig c;
    c.n_ports = 0; expect(has_error(c, "n_ports"), "n_ports 0 rejected");
    c = {}; c.n_ports = 5; expect(has_error(c, "n_ports"), "n_ports 5 rejected");
    c = {}; c.id_width = 9; expect(has_error(c, "id_width"), "ID width 9 rejected");
    c = {}; c.id_width = 0; expect(has_error(c, "id_width"), "ID width 0 rejected");
    c = {}; c.device_width = DeviceWidth::X4; c.density = Density::Gb8;
    expect(has_error(c, "density"), "8Gb x4 rejected (page needs 4096 words)");
    c = {}; c.map_profile_mask = 0; expect(has_error(c, "map_profile_mask"), "map mask 0 rejected");
    c = {}; c.map_profile_mask = 4; expect(has_error(c, "map_profile_mask"), "map mask 4 rejected");
    c = {}; c.row_bits_max = 11; expect(has_error(c, "row_bits_max"), "ROW_BITS_MAX 11 rejected");
    c = {}; c.row_bits_max = 15; expect(has_error(c, "row_bits_max"), "16-row device with ROW_BITS_MAX 15 rejected");
    c = {}; c.refresh_max_debt = 9; expect(has_error(c, "refresh_max_debt"), "refresh debt 9 rejected");
    c = {}; c.refresh_max_debt = 0; expect(has_error(c, "refresh_max_debt"), "refresh debt 0 rejected");
    c = {}; c.tags_per_port = 24; expect(has_error(c, "tags_per_port"), "N_TAG 24 rejected");
    c = {}; c.exclusive_port_mask = 0; expect(has_error(c, "exclusive_port_mask"), "EXCL_PORT_MASK 0 rejected");
    c = {}; c.exclusive_enable = false; c.exclusive_port_mask = 0;
    expect(validate(c).empty(), "EXCL_PORT_MASK ignored when exclusive monitor absent");
    c = {}; c.n_ports = 1; c.exclusive_port_mask = 2;
    expect(has_error(c, "exclusive_port_mask"), "exclusive mask on a disabled port rejected");
    c = {}; c.phy_read_latency = 0; expect(has_error(c, "phy_read_latency"), "PHY read latency 0 rejected");

    c = {}; c.n_ports = 9; c.id_width = 0;
    bool threw = false;
    try { require_valid(c); } catch (const std::invalid_argument& e) {
        threw = std::strstr(e.what(), "n_ports") && std::strstr(e.what(), "id_width");
    }
    expect(threw, "require_valid lists every error");

    // MAS Table 5-10 / 5-5 geometry.
    DeviceGeometry g{};
    expect(device_geometry(DeviceWidth::X8, Density::Gb4, g) && g.row_bits == 16 && g.column_bits == 10 &&
               g.page_words == 1024 && g.device_page_bytes == 1024,
           "4Gb x8: 16 rows, 10 cols, 1024 words, 1 KB page");
    expect(rank_bytes(g.row_bits, g.page_words) == (2ull << 30), "4Gb x8 rank = 2 GiB");
    expect(device_geometry(DeviceWidth::X4, Density::Gb2, g) && g.row_bits == 15 && g.column_bits == 11 &&
               g.page_words == 2048 && g.device_page_bytes == 1024 && rank_bytes(g.row_bits, g.page_words) == (2ull << 30),
           "2Gb x4: 15 rows, 11 cols, 2048 words, 1 KB page, 2 GiB rank");
    expect(device_geometry(DeviceWidth::X8, Density::Gb8, g) && g.device_page_bytes == 2048 &&
               g.max_ranks_within_4g == 1 && rank_bytes(g.row_bits, g.page_words) == (4ull << 30),
           "8Gb x8: 2 KB page, 4 GiB rank, 1 rank allowed");
    expect(device_geometry(DeviceWidth::X4, Density::Gb4, g) && g.max_ranks_within_4g == 1,
           "4Gb x4: 1 rank allowed");
    expect(!device_geometry(DeviceWidth::X4, Density::Gb8, g), "8Gb x4 unsupported");
}

void test_capability() {
    // HAS Table 8-6 field packing, computed by hand.
    expect_eq(cap_feature(BuildConfig::native_r2()), 0x00000DD5u, "CAP_FEATURE NATIVE_R2 ECC");
    expect_eq(cap_feature(BuildConfig::native_r1()), 0x00000DADu, "CAP_FEATURE NATIVE_R1 ECC");
    expect_eq(cap_feature(BuildConfig::legacy()), 0x000009ABu, "CAP_FEATURE LEGACY ECC");
    BuildConfig c = BuildConfig::native_r2();
    c.ecc_enable = false;
    c.irq_test_enable = false;
    c.map_profile_mask = 1;
    expect_eq(cap_feature(c), 0x000004D4u, "CAP_FEATURE NATIVE_R2, no ECC, no IRQ test, linear only");
    expect_eq(cap_width0(BuildConfig::native_r2()), 0x00208410u, "CAP_WIDTH0 ratio 2");
    expect_eq(cap_width0(BuildConfig::native_r1()), 0x00208410u, "CAP_WIDTH0 NATIVE_R1");
    expect_eq(cap_width0(BuildConfig::legacy()), 0x00204410u, "CAP_WIDTH0 ratio 1");
    expect_eq(caps::kCapDepth0, 0x04101008u, "CAP_DEPTH0");
    expect_eq(caps::kIpId, 0x44445233u, "IP_ID");
    expect_eq(caps::kAbiVersion, 0x00010000u, "ABI_VERSION");
}

void check_col(std::uint32_t (*get)(const BuildConfig&), const Col& want, const std::string& what,
               Density d = Density::Gb4, Temperature t = Temperature::Normal, DeviceWidth w = DeviceWidth::X8) {
    for (unsigned i = 0; i < 4; ++i) {
        BuildConfig c = with(kBins[i], d, t);
        c.device_width = w;
        expect_eq(get(c), want[i], what + " @" + std::to_string(static_cast<int>(kBins[i])));
    }
}

template <unsigned I>
std::uint32_t tw(const BuildConfig& c) { return timing_image(c)[I]; }

void test_speed_bin() {
    expect(tck_ps(SpeedBin::DDR3_800) == 2500 && tck_ps(SpeedBin::DDR3_1066) == 1875 &&
               tck_ps(SpeedBin::DDR3_1333) == 1500 && tck_ps(SpeedBin::DDR3_1600) == 1250,
           "tCK(min) per bin");

    // MAS Table 5-15, x8 4Gb (1 KB page), normal temperature.
    check_col(tw<kTRcd>, {6, 8, 10, 11}, "T_RCD");
    check_col(tw<kTRp>, {6, 8, 10, 11}, "T_RP");
    check_col(tw<kTRas>, {15, 20, 24, 28}, "T_RAS");
    check_col(tw<kTRc>, {21, 28, 34, 39}, "T_RC");
    check_col(tw<kTRrd>, {4, 4, 4, 5}, "T_RRD 1KB");
    check_col(tw<kTFaw>, {16, 20, 20, 24}, "T_FAW 1KB");
    check_col(tw<kTCcd>, {4, 4, 4, 4}, "T_CCD");
    check_col(tw<kTRtp>, {4, 4, 5, 6}, "T_RTP");
    check_col(tw<kTWr>, {6, 8, 10, 12}, "T_WR");
    check_col(tw<kTWtr>, {4, 4, 5, 6}, "T_WTR");
    check_col(tw<kTRtw>, {7, 8, 9, 9}, "T_RTW");
    check_col(tw<kTRtrs>, {2, 2, 2, 2}, "T_RTRS");
    check_col(tw<kTRfc>, {104, 139, 174, 208}, "T_RFC 4Gb");
    check_col(tw<kTRefi>, {3120, 4160, 5200, 6240}, "T_REFI normal");
    check_col(tw<kCl>, {6, 8, 10, 11}, "CL");
    check_col(tw<kCwl>, {5, 6, 7, 8}, "CWL");
    check_col(tw<kAdditiveLatency>, {0, 0, 0, 0}, "AL");
    check_col(tw<kPhyWriteLatency>, {2, 2, 2, 2}, "PHY_WRITE_LATENCY");
    check_col(tw<kPhyReadLatency>, {6, 6, 6, 6}, "PHY_READ_LATENCY");
    // 2 KB page (8Gb x8) and the other densities / temperature.
    check_col(tw<kTRrd>, {4, 6, 5, 6}, "T_RRD 2KB", Density::Gb8);
    check_col(tw<kTFaw>, {20, 27, 30, 32}, "T_FAW 2KB", Density::Gb8);
    check_col(tw<kTRfc>, {44, 59, 74, 88}, "T_RFC 1Gb", Density::Gb1);
    check_col(tw<kTRfc>, {64, 86, 107, 128}, "T_RFC 2Gb", Density::Gb2);
    check_col(tw<kTRfc>, {140, 187, 234, 280}, "T_RFC 8Gb", Density::Gb8);
    check_col(tw<kTRefi>, {1560, 2080, 2600, 3120}, "T_REFI extended", Density::Gb4, Temperature::Extended);

    // MAS Table 5-16.
    struct Row { std::uint32_t InitWaitImage::*m; Col want; const char* name; Density d; };
    const Row init_rows[] = {
        {&InitWaitImage::reset_assert, {80000, 106667, 133334, 160000}, "INIT_RESET_ASSERT_WAIT", Density::Gb4},
        {&InitWaitImage::reset_release, {200000, 266667, 333334, 400000}, "INIT_RESET_RELEASE_WAIT", Density::Gb4},
        {&InitWaitImage::cke_low, {5, 6, 7, 8}, "INIT_CKE_LOW_WAIT", Density::Gb4},
        {&InitWaitImage::cke_high, {48, 64, 80, 96}, "INIT_CKE_HIGH_WAIT 1Gb", Density::Gb1},
        {&InitWaitImage::cke_high, {108, 144, 180, 216}, "INIT_CKE_HIGH_WAIT 4Gb", Density::Gb4},
        {&InitWaitImage::cke_high, {68, 91, 114, 136}, "INIT_CKE_HIGH_WAIT 2Gb", Density::Gb2},
        {&InitWaitImage::cke_high, {144, 192, 240, 288}, "INIT_CKE_HIGH_WAIT 8Gb", Density::Gb8},
        {&InitWaitImage::dll, {512, 512, 512, 512}, "INIT_DLL_WAIT", Density::Gb4},
        {&InitWaitImage::zq, {512, 512, 512, 512}, "INIT_ZQ_WAIT", Density::Gb4},
        {&InitWaitImage::cmd_timeout, {1024, 1024, 1024, 1024}, "INIT_CMD_TIMEOUT", Density::Gb4},
        {&InitWaitImage::train_tras, {15, 20, 24, 28}, "TRAIN_TRAS_WAIT", Density::Gb4},
    };
    for (const Row& r : init_rows)
        for (unsigned i = 0; i < 4; ++i)
            expect_eq(init_wait_image(with(kBins[i], r.d)).*r.m, r.want[i],
                      std::string(r.name) + " @" + std::to_string(static_cast<int>(kBins[i])));

    // MAS Table 5-17 and ZQ interval (§5.6).
    struct XRow { std::uint32_t ExtTimingImage::*m; Col want; const char* name; Density d; };
    const XRow ext_rows[] = {
        {&ExtTimingImage::t_cke, {3, 3, 4, 4}, "T_CKE", Density::Gb4},
        {&ExtTimingImage::t_ckesr, {4, 4, 5, 5}, "T_CKESR", Density::Gb4},
        {&ExtTimingImage::t_xp, {3, 4, 4, 5}, "T_XP", Density::Gb4},
        {&ExtTimingImage::t_xpdll, {10, 13, 16, 20}, "T_XPDLL", Density::Gb4},
        {&ExtTimingImage::t_xs, {48, 64, 80, 96}, "T_XS 1Gb", Density::Gb1},
        {&ExtTimingImage::t_xs, {108, 144, 180, 216}, "T_XS 4Gb", Density::Gb4},
        {&ExtTimingImage::t_xs, {68, 91, 114, 136}, "T_XS 2Gb", Density::Gb2},
        {&ExtTimingImage::t_xs, {144, 192, 240, 288}, "T_XS 8Gb", Density::Gb8},
        {&ExtTimingImage::t_xsdll, {512, 512, 512, 512}, "T_XSDLL", Density::Gb4},
        {&ExtTimingImage::t_cksre, {5, 6, 7, 8}, "T_CKSRE", Density::Gb4},
        {&ExtTimingImage::t_cksrx, {5, 6, 7, 8}, "T_CKSRX", Density::Gb4},
        {&ExtTimingImage::t_mod, {12, 12, 12, 12}, "T_MOD", Density::Gb4},
        {&ExtTimingImage::t_mrd, {4, 4, 4, 4}, "T_MRD", Density::Gb4},
        {&ExtTimingImage::t_zqoper, {256, 256, 256, 256}, "T_ZQOPER", Density::Gb4},
        {&ExtTimingImage::t_zqcs, {64, 64, 64, 64}, "T_ZQCS", Density::Gb4},
        {&ExtTimingImage::zq_interval, {51200000, 68266666, 85333333, 102400000}, "ZQ_INTERVAL", Density::Gb4},
    };
    for (const XRow& r : ext_rows)
        for (unsigned i = 0; i < 4; ++i)
            expect_eq(ext_timing_image(with(kBins[i], r.d)).*r.m, r.want[i],
                      std::string(r.name) + " @" + std::to_string(static_cast<int>(kBins[i])));

    // MAS Table 5-18 (DDR3-1600 only) and the enable-delay formulas.
    const DfiImage d = dfi_image(with(SpeedBin::DDR3_1600));
    expect_eq(d.wr_enable_delay, 4u, "DFI_WR_ENABLE_DELAY @1600");
    expect_eq(d.rd_enable_delay, 13u, "DFI_RD_ENABLE_DELAY @1600");
    expect_eq(d.max_read_return, 256u, "PHY_MAX_READ_RETURN");
    expect_eq(d.assembly_margin, 1u, "DFI_ASSEMBLY_MARGIN");
    std::int64_t wr800 = 0, rd800 = 0;
    const BuildConfig b800 = with(SpeedBin::DDR3_800);
    expect(dfi_formula(dfi_inputs(b800, timing_image(b800)), wr800, rd800) && wr800 == 1 && rd800 == 8,
           "DFI formulas @800: 5+0-2-2 = 1 and 6+0+6+0-4 = 8 (legality is tested in dfi_bounds)");

    expect_eq(nck_ceil(10000, 1250), 8u, "nck_ceil exact");
    expect_eq(nck_ceil(10001, 1250), 9u, "nck_ceil rounds up");
    expect_eq(nck_ceil(1000, 1250, 4), 4u, "nck_ceil floor of min nCK");
}

void test_dfi_bounds() {
    // tphy_wrlat = CWL + AL - PWL - 2 and trddata_en = CL + AL + PRL + G - 4 must be
    // representable and DFI_TIMING_COMMIT-legal (MAS §6.2.12): >= R, < 256 and
    // floor((2R - 1 + Dmax + 3) / R) + 1 <= CAL_DEPTH - 1. Nothing may wrap or clamp.
    const Col cwl{5, 6, 7, 8};
    for (unsigned i = 0; i < 4; ++i) {
        const std::string at = " @" + std::to_string(static_cast<int>(kBins[i]));
        BuildConfig ok = with(kBins[i]);
        ok.phy_write_latency = cwl[i] - 4u;  // native R = 2: largest PWL gives tphy_wrlat = 2
        expect(validate(ok).empty(), "PWL = CWL - 4 accepted (tphy_wrlat = R)" + at);
        expect_eq(dfi_image(ok).wr_enable_delay, 2u, "PWL = CWL - 4 gives tphy_wrlat 2" + at);
        BuildConfig bad = ok;
        bad.phy_write_latency = cwl[i] - 3u;  // tphy_wrlat = 1 < R
        expect(has_error(bad, "phy_latency"), "tphy_wrlat below R rejected" + at);
        bad.phy_write_latency = cwl[i] - 1u;  // negative: no wrap
        expect(has_error(bad, "phy_latency"), "negative tphy_wrlat rejected" + at);
        bool threw = false;
        try { (void)dfi_image(bad); } catch (const std::invalid_argument&) { threw = true; }
        expect(threw, "dfi_image throws instead of wrapping" + at);
    }
    BuildConfig c = with(SpeedBin::DDR3_1600);
    c.phy_write_latency = 7;  // review R1 reproduction
    expect(has_error(c, "phy_latency"), "native R2 DDR3-1600 with PHY write latency 7 rejected");
    DfiImage untouched{11, 22, 33, 44};
    std::string why;
    expect(!derive_dfi_image(c, untouched, &why) && untouched.wr_enable_delay == 11 && !why.empty(),
           "failed derivation leaves the output unchanged and reports why");
    expect(has_error(with(SpeedBin::DDR3_800), "phy_latency"),
           "native DDR3-800 with the default PWL 2 gives tphy_wrlat 1 < R: rejected");
    BuildConfig leg800 = BuildConfig::legacy();
    leg800.speed_bin = SpeedBin::DDR3_800;
    expect(validate(leg800).empty() && dfi_image(leg800).wr_enable_delay == 1, "legacy (R = 1) DDR3-800 legal");

    // Calendar bound on the read side: R = 2 allows trddata_en <= 55, R = 1 allows <= 26.
    c = with(SpeedBin::DDR3_1600);
    c.phy_read_latency = 48;  // 11 + 0 + 48 + 0 - 4 = 55
    expect(validate(c).empty() && dfi_image(c).rd_enable_delay == 55, "trddata_en 55 accepted at R = 2");
    c.phy_read_latency = 49;
    expect(has_error(c, "phy_latency"), "trddata_en 56 exceeds the calendar at R = 2");
    BuildConfig leg = BuildConfig::legacy();
    leg.phy_read_latency = 19;  // 11 + 19 - 4 = 26
    expect(validate(leg).empty(), "trddata_en 26 accepted at R = 1");
    leg.phy_read_latency = 20;
    expect(has_error(leg, "phy_latency"), "trddata_en 27 exceeds the calendar at R = 1");
    // Write side (tphy_wrdata = 1): R = 2 allows tphy_wrlat <= 54, R = 1 allows <= 25.
    DfiTimingInputs in2{11, 8, 0, 2, 6, 0, 1, 2}, in1{11, 8, 0, 2, 6, 0, 1, 1};
    expect(dfi_delays_legal(54, 13, in2).ok && !dfi_delays_legal(55, 13, in2).ok, "write calendar bound at R = 2");
    expect(dfi_delays_legal(25, 13, in1).ok && !dfi_delays_legal(26, 13, in1).ok, "write calendar bound at R = 1");
    expect(dfi_delays_legal(4, 13, in2).ok, "MAS example: floor((3 + 13 + 3) / 2) + 1 = 10 <= 31");

    c = with(SpeedBin::DDR3_1600);
    c.phy_read_latency = 0xFFFFFFFFu;
    expect(has_error(c, "phy_latency"), "PHY read latency 2^32-1 rejected (no 32-bit wrap)");
    c = with(SpeedBin::DDR3_1600);
    c.read_gate_offset = 0xFFFFFFFFu;
    expect(has_error(c, "phy_latency"), "read-gate offset 2^32-1 rejected (no 32-bit wrap)");
    c = with(SpeedBin::DDR3_1600);
    c.t_rtrs = 0x80000000u;
    expect(has_error(c, "t_rtrs"), "T_RTRS of 2^31 rejected");
    for (const auto& p : {BuildConfig::native_r2(), BuildConfig::native_r1(), BuildConfig::legacy()})
        expect(dfi_image(p).wr_enable_delay == 4 && dfi_image(p).rd_enable_delay == 13,
               std::string("default build DFI image 4/13: ") + to_string(p.profile));
}

void test_config_checks() {
    const BuildConfig r2 = BuildConfig::native_r2();
    Geometry g{};
    // Topology (HAS Table 8-11 fields, MAS Table 5-20 rules, HAS C-05 clamp).
    expect(check_topology(0x501, r2, g).ok && g.ranks == 2 && g.row_bits == 16 && g.page_words == 1024 &&
               g.map_profile == 0 && g.physical_granules == (1ull << 27) && g.usable_granules == 119304640ull &&
               g.ecc_base == 119304640ull,
           "0x501: 2 ranks, 16 rows, 1024 words, linear, P = 2^27, U = ECC base = 119304640");
    expect(!check_topology(0x501, BuildConfig::native_r1(), g).ok, "2 active ranks on a one-rank build rejected");
    expect(check_topology(0x501, BuildConfig::native_r1(), g).blocker == 0x11, "topology blocker 0x11");
    expect(!check_topology(0x0B1 | (2u << 9), r2, g).ok, "11 row bits rejected");
    expect(!check_topology(0x111 | (2u << 9), r2, g).ok, "17 row bits rejected (ROW_BITS_MAX 16)");
    BuildConfig lin = r2;
    lin.map_profile_mask = 1;
    expect(!check_topology(0x501 | (1u << 12), lin, g).ok, "interleaved profile not built rejected");
    expect(check_topology(0x501 | (1u << 12), r2, g).ok && g.map_profile == 1, "interleaved profile accepted");
    expect(check_topology(0x701, r2, g).ok && g.physical_granules == (1ull << 28) && g.usable_granules == (1ull << 27),
           "P = 2^28 accepted, usable clamped to 2^27 (HAS C-05)");
    BuildConfig noecc = r2;
    noecc.ecc_enable = false;
    expect(check_topology(0x501, noecc, g).ok && g.usable_granules == (1ull << 27) && g.ecc_base == 0,
           "no ECC: U0 = P, ECC base 0");
    expect(check_topology(0x0C0, BuildConfig::native_r1(), g).ok && g.physical_granules == (1ull << 20),
           "smallest geometry: 1 rank, 12 rows, 256 words");

    // Timing (MAS Table 5-20 on the 19 HAS words).
    const TimingImage legal = timing_image(r2);
    expect(check_timing(legal).ok, "DDR3-1600 reference image legal");
    auto bad = [&legal](unsigned idx, std::uint32_t v) {
        TimingImage t = legal;
        t[idx] = v;
        return !check_timing(t).ok && check_timing(t).blocker == 0x04;
    };
    expect(bad(kCl, 4) && bad(kCl, 12), "CL outside 5..11 rejected (12..16 reserved)");
    expect(bad(kCwl, 4) && bad(kCwl, 9), "CWL outside 5..8 rejected");
    expect(bad(kTWr, 4) && bad(kTWr, 17), "T_WR outside 5..16 rejected");
    expect(bad(kAdditiveLatency, 8), "AL other than 0, CL-1, CL-2 rejected");
    TimingImage al = legal;
    al[kAdditiveLatency] = 10;
    expect(check_timing(al).ok, "AL = CL - 1 accepted");
    expect(bad(kTRtw, 8), "T_RTW < CL + 6 - CWL (= 9) rejected");
    expect(bad(kTCcd, 3) && bad(kTRrd, 3) && bad(kTRtp, 3) && bad(kTWtr, 3), "4 nCK floors");
    expect(bad(kTRcd, 0) && bad(kTRefi, 0), "zero where JEDEC has a minimum rejected");
    expect(bad(kTRefi, 0x80000000u), "values must be below 2^31");
    TimingImage zeros = legal;
    zeros[kTRtrs] = 0;
    zeros[kPhyWriteLatency] = 0;
    zeros[kPhyReadLatency] = 0;
    expect(check_timing(zeros).ok, "T_RTRS, PHY latencies may be 0 at the timing commit (GAP-BE-09)");

    // DFI image against the active timing (MAS §6.2.12).
    const DfiTimingInputs in = dfi_inputs(r2, legal);
    const DfiImage ref{4, 13, 256, 1};
    expect(check_dfi_image(ref, in, 1024).ok, "4/13/256/1 legal against the DDR3-1600 timing");
    expect(!check_dfi_image({5, 13, 256, 1}, in, 1024).ok, "write delay must equal the formula");
    expect(!check_dfi_image({4, 12, 256, 1}, in, 1024).ok, "read delay must equal the formula");
    expect(!check_dfi_image({4, 13, 256, 0}, in, 1024).ok && !check_dfi_image({4, 13, 256, 256}, in, 1024).ok,
           "assembly margin 1..255");
    expect(!check_dfi_image({4, 13, 0, 1}, in, 1024).ok, "PHY_MAX_READ_RETURN nonzero");
    expect(read_timeout_floor(ref, 2) == 284 && !check_dfi_image(ref, in, 283).ok && check_dfi_image(ref, in, 284).ok,
           "READ_TIMEOUT >= 5R + 13 + 256 + 4 + 1 = 284");
    DfiTimingInputs prl0 = in;
    prl0.prl = 0;
    prl0.gate_offset = 6;  // keeps the read formula at 13
    expect(!check_dfi_image(ref, prl0, 1024).ok, "PHY read latency 0 rejected at the DFI commit");

    // Region image (MAS §6.1.15, GAP-FE-11).
    RegionImage img{};
    img[0] = 0x80000000u;
    img[1] = 0x20000000u;
    expect(check_region_image(img, 119304640ull).ok, "port 0 region 0: 0x80000000 / 512 MiB legal");
    expect(check_region_image(img, 0).blocker == kBlockerRegionCapacity, "capacity 0 -> 0x0D");
    RegionImage m = img;
    m[1] = 0x20000010u;
    expect(check_region_image(m, 119304640ull).blocker == kBlockerRegionShape, "size not a multiple of 32 -> 0x0C");
    m = img;
    m[2] = 0xFFFFFFE0u;
    m[3] = 0x40u;
    expect(check_region_image(m, 119304640ull).blocker == kBlockerRegionShape, "BASE + SIZE > 2^32 -> 0x0C");
    m = img;
    m[2] = 0x9FFFFFE0u;
    m[3] = 0x40u;
    expect(check_region_image(m, 119304640ull).blocker == kBlockerRegionShape, "same-port overlap -> 0x0C");
    m = img;
    m[8] = 0x80000000u;  // port 1 region 0
    m[9] = 0x1000u;
    expect(check_region_image(m, 119304640ull).ok, "different ports may overlap");
    m = img;
    m[2] = 0xFFFFFFE0u;
    m[3] = 0x20u;
    expect(check_region_image(m, 119304640ull).ok, "region ending exactly at 2^32 legal");
}

void test_timebase() {
    TimeBase r2{1250, 2};
    expect_eq(r2.ctrl_period_ps(), 2500u, "ratio 2 controller period at DDR3-1600 = 2.5 ns (400 MHz)");
    expect_eq(r2.ctrl_cycles_for_tck(5), 3u, "5 tCK -> ceil(5/2) = 3 controller cycles");
    expect_eq(r2.ctrl_cycles_for_tck(4), 2u, "4 tCK -> 2 controller cycles");
    expect_eq(r2.tck_wait_ps(5), 7500u, "5 tCK wait rounds to 3 controller cycles");
    expect_eq(r2.ctrl_cycles_ps(1024), 2560000u, "1024 controller cycles = 2.56 us");
    TimeBase r1{2500, 1};
    expect_eq(r1.ctrl_cycles_for_tck(5), 5u, "ratio 1: tCK == controller cycle");
    expect_eq(r1.tck_wait_ps(6), 15000u, "ratio 1 DDR3-800: 6 tCK = 15 ns");
}

void test_burst_plan() {
    auto p = plan_generic_access(0x80000004, 4, 16, false);
    expect(p.size() == 1 && p[0].req.size == 2 && p[0].req.len == 0 && p[0].req.addr == 0x80000004 &&
               p[0].byte_count == 4 && p[0].req.burst == AxiBurst::Incr,
           "aligned word -> one narrow single-beat burst (AxSIZE 2)");
    p = plan_generic_access(0x80000008, 8, 16, true);
    expect(p.size() == 1 && p[0].req.size == 3 && p[0].req.write, "aligned doubleword -> AxSIZE 3");
    p = plan_generic_access(0x80000003, 4, 16, false);
    expect(p.size() == 1 && p[0].req.size == 4 && p[0].req.len == 0 && p[0].byte_count == 4,
           "unaligned word inside one beat -> one full-width beat");
    p = plan_generic_access(0x8000000E, 4, 16, false);
    expect(p.size() == 1 && p[0].req.len == 1, "unaligned word across a beat -> two beats");
    p = plan_generic_access(0x80000FF0, 0x20, 16, false);
    expect(p.size() == 2 && p[0].req.addr == 0x80000FF0 && p[0].byte_count == 16 && p[0].req.len == 0 &&
               p[1].req.addr == 0x80001000 && p[1].byte_offset == 16 && p[1].byte_count == 16,
           "4 KiB boundary splits the access");
    p = plan_generic_access(0x80000000, 4096, 16, true);
    expect(p.size() == 1 && p[0].req.len == 255 && p[0].byte_count == 4096, "4 KiB at AXI128 = one 256-beat burst");
    p = plan_generic_access(0x80000000, 4096, 8, true);
    expect(p.size() == 2 && p[0].req.len == 255 && p[1].req.addr == 0x80000800 && p[1].byte_offset == 2048,
           "4 KiB at AXI64 = two 256-beat bursts");
    p = plan_generic_access(0x80000010, 100, 8, false);
    std::uint64_t covered = 0;
    for (const auto& b : p) covered += b.byte_count;
    expect(covered == 100 && p.size() == 1 && p[0].req.len == 12, "100 bytes at AXI64 from aligned start: 13 beats");
    expect(plan_generic_access(0x80000000, 0, 16, false).empty(), "zero length -> no burst");
    expect(plan_generic_access(0xFFFFFFFF, 2, 16, false).empty(), "32-bit overflow -> no burst");
    p = plan_generic_access(0xFFFFFFF0, 16, 16, false);
    expect(p.size() == 1 && p[0].req.addr == 0xFFFFFFF0, "last 16 bytes of the address space");
    AxiRequest r;
    r.len = 15;
    r.size = 3;
    expect(r.span_bytes() == 128 && r.beats() == 16, "HAS §6.1.3 example: 16-beat 8-byte INCR spans 128 bytes");
}

} // namespace

int main(int argc, char** argv) {
    const std::string which = argc > 1 ? argv[1] : "all";
    const bool all = which == "all";
    bool ran = false;
    if (all || which == "profile") { test_profile(); ran = true; }
    if (all || which == "capability") { test_capability(); ran = true; }
    if (all || which == "speed_bin") { test_speed_bin(); ran = true; }
    if (all || which == "dfi_bounds") { test_dfi_bounds(); ran = true; }
    if (all || which == "config_checks") { test_config_checks(); ran = true; }
    if (all || which == "timebase") { test_timebase(); ran = true; }
    if (all || which == "burst_plan") { test_burst_plan(); ran = true; }
    if (!ran) {
        std::cerr << "unknown case " << which << '\n';
        return 2;
    }
    return fx1_ddr3_test::summary("fx1_ddr3 core " + which);
}
