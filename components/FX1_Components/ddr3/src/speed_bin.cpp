// SPDX-License-Identifier: Apache-2.0
// JEDEC JESD79-3F minimums as listed in MAS Tables 5-12..5-14, converted with
// nCK = ceil(t / tCK) (tREFI and the ZQ interval round down, MAS §5.1/§5.6).
#include "fx1_ddr3/speed_bin.h"

#include "fx1_ddr3/config_checks.h"

#include <algorithm>
#include <stdexcept>

namespace fx1::ddr3 {

namespace {
constexpr std::uint64_t ns(std::uint64_t v) { return v * 1000u; }  // ns -> ps

struct BinData {
    std::uint32_t cl, cwl;
    std::uint64_t trcd_ps, tras_ps, trc_ps;    // reference sub-bin (MAS Table 5-13)
    std::uint64_t trrd_1k_ps, trrd_2k_ps, tfaw_1k_ps, tfaw_2k_ps;
    std::uint64_t tcke_ps, txp_ps;
};

BinData bin_data(SpeedBin b) {
    switch (b) {
    case SpeedBin::DDR3_800:   // DDR3-800E 6-6-6
        return {6, 5, 15000, 37500, 52500, ns(10), ns(10), ns(40), ns(50), 7500, 7500};
    case SpeedBin::DDR3_1066:  // DDR3-1066G 8-8-8
        return {8, 6, 15000, 37500, 52500, 7500, ns(10), 37500, ns(50), 5625, 7500};
    case SpeedBin::DDR3_1333:  // DDR3-1333J 10-10-10
        return {10, 7, 15000, 36000, 51000, ns(6), 7500, ns(30), ns(45), 5625, ns(6)};
    case SpeedBin::DDR3_1600:  // DDR3-1600K 11-11-11
    default:
        return {11, 8, 13750, 35000, 48750, ns(6), 7500, ns(30), ns(40), ns(5), ns(6)};
    }
}

std::uint64_t trfc_ps(Density d) {
    switch (d) {
    case Density::Gb1: return ns(110);
    case Density::Gb2: return ns(160);
    case Density::Gb4: return ns(260);
    case Density::Gb8: default: return ns(350);
    }
}

bool two_kb_page(const BuildConfig& c) {
    DeviceGeometry g{};
    return device_geometry(c.device_width, c.density, g) && g.device_page_bytes >= 2048;
}

std::uint32_t floor_nck(std::uint64_t t_ps, std::uint32_t tck) {
    return static_cast<std::uint32_t>(t_ps / tck);
}
} // namespace

std::uint32_t tck_ps(SpeedBin bin) noexcept {
    switch (bin) {
    case SpeedBin::DDR3_800: return 2500;
    case SpeedBin::DDR3_1066: return 1875;
    case SpeedBin::DDR3_1333: return 1500;
    case SpeedBin::DDR3_1600: default: return 1250;
    }
}

std::uint32_t nck_ceil(std::uint64_t t_ps, std::uint32_t tck, std::uint32_t min_nck) noexcept {
    const auto n = static_cast<std::uint32_t>((t_ps + tck - 1) / tck);
    return std::max(n, min_nck);
}

TimingImage timing_image(const BuildConfig& c) noexcept {
    const BinData b = bin_data(c.speed_bin);
    const std::uint32_t tck = tck_ps(c.speed_bin);
    const bool page2k = two_kb_page(c);
    TimingImage t{};
    t[kTRcd] = nck_ceil(b.trcd_ps, tck);
    t[kTRp] = nck_ceil(b.trcd_ps, tck);  // tRP = tRCD for the reference sub-bins
    t[kTRas] = nck_ceil(b.tras_ps, tck);
    t[kTRc] = nck_ceil(b.trc_ps, tck);
    t[kTRrd] = nck_ceil(page2k ? b.trrd_2k_ps : b.trrd_1k_ps, tck, 4);
    t[kTFaw] = nck_ceil(page2k ? b.tfaw_2k_ps : b.tfaw_1k_ps, tck);
    t[kTCcd] = 4;
    t[kTRtp] = nck_ceil(7500, tck, 4);
    t[kTWr] = nck_ceil(ns(15), tck);
    t[kTWtr] = nck_ceil(7500, tck, 4);
    t[kTRtw] = b.cl + 4u + 2u - b.cwl;  // RL + tCCD + 2 nCK - WL, BL8, AL = 0
    t[kTRtrs] = c.t_rtrs;
    t[kTRfc] = nck_ceil(trfc_ps(c.density), tck);
    t[kTRefi] = floor_nck(c.temperature == Temperature::Extended ? ns(3900) : ns(7800), tck);
    t[kCl] = b.cl;
    t[kCwl] = b.cwl;
    t[kAdditiveLatency] = 0;
    t[kPhyWriteLatency] = c.phy_write_latency;
    t[kPhyReadLatency] = c.phy_read_latency;
    return t;
}

ExtTimingImage ext_timing_image(const BuildConfig& c) noexcept {
    const BinData b = bin_data(c.speed_bin);
    const std::uint32_t tck = tck_ps(c.speed_bin);
    ExtTimingImage x{};
    x.t_cke = nck_ceil(b.tcke_ps, tck, 3);
    x.t_ckesr = x.t_cke + 1u;
    x.t_xp = nck_ceil(b.txp_ps, tck, 3);
    x.t_xpdll = nck_ceil(ns(24), tck, 10);
    x.t_xs = nck_ceil(trfc_ps(c.density) + ns(10), tck, 5);
    x.t_xsdll = 512;
    x.t_cksre = nck_ceil(ns(10), tck, 5);
    x.t_cksrx = x.t_cksre;
    x.t_mod = nck_ceil(ns(15), tck, 12);
    x.t_mrd = 4;
    x.t_zqoper = nck_ceil(ns(320), tck, 256);
    x.t_zqcs = nck_ceil(ns(80), tck, 64);
    x.zq_interval = floor_nck(ns(128) * 1000000u, tck);  // 128 ms
    return x;
}

InitWaitImage init_wait_image(const BuildConfig& c) noexcept {
    const BinData b = bin_data(c.speed_bin);
    const std::uint32_t tck = tck_ps(c.speed_bin);
    InitWaitImage w{};
    w.reset_assert = nck_ceil(ns(200) * 1000u, tck);   // 200 us
    w.reset_release = nck_ceil(ns(500) * 1000u, tck);  // 500 us
    w.cke_low = nck_ceil(ns(10), tck, 5);
    w.cke_high = nck_ceil(trfc_ps(c.density) + ns(10), tck, 5);  // tXPR
    w.dll = 512;
    w.zq = nck_ceil(ns(640), tck, 512);
    w.cmd_timeout = 1024;
    w.train_tras = nck_ceil(b.tras_ps, tck);
    return w;
}

bool derive_dfi_image(const BuildConfig& c, DfiImage& out, std::string* why) {
    // Same formulas and legal range as DFI_TIMING_COMMIT (MAS §6.2.12), so the reference
    // image of a legal build always passes its commit check (MAS Table 5-19 rule 6).
    const DfiTimingInputs in = dfi_inputs(c, timing_image(c));
    std::int64_t wr = 0, rd = 0;
    if (!dfi_formula(in, wr, rd)) {
        if (why) *why = "DFI delay CWL + AL - PWL - 2 or CL + AL + PRL + G - 4 is negative";
        return false;
    }
    const CheckResult legal = dfi_delays_legal(wr, rd, in);
    if (!legal.ok) {
        if (why) *why = legal.why;
        return false;
    }
    out.wr_enable_delay = static_cast<std::uint32_t>(wr);
    out.rd_enable_delay = static_cast<std::uint32_t>(rd);
    out.max_read_return = 256;
    out.assembly_margin = 1;
    return true;
}

DfiImage dfi_image(const BuildConfig& c) {
    DfiImage d{};
    std::string why;
    if (!derive_dfi_image(c, d, &why)) throw std::invalid_argument("dfi_image: " + why);
    return d;
}

} // namespace fx1::ddr3
