// SPDX-License-Identifier: Apache-2.0
#include "fx1_ddr3/config_checks.h"

#include "fx1_ddr3/provisional_encodings.h"

#include <algorithm>

namespace fx1::ddr3 {

namespace {
constexpr std::uint64_t kTimeLimit = 0x80000000ull;  // values < 2^(BE_TIME_W-1), BE_TIME_W = 32
std::string s(std::uint64_t v) { return std::to_string(v); }
} // namespace

// ---------------------------------------------------------------- DFI
DfiTimingInputs dfi_inputs(const BuildConfig& cfg, const TimingImage& t) noexcept {
    return {t[kCl], t[kCwl], t[kAdditiveLatency], t[kPhyWriteLatency], t[kPhyReadLatency],
            cfg.read_gate_offset, cfg.dfi_wrdata_delay, cfg.dfi_ratio()};
}

bool dfi_formula(const DfiTimingInputs& in, std::int64_t& wr, std::int64_t& rd) noexcept {
    wr = std::int64_t{in.cwl} + in.al - in.pwl - kDfiKWr;
    rd = std::int64_t{in.cl} + in.al + in.prl + in.gate_offset - kDfiKRd;
    return wr >= 0 && rd >= 0;
}

CheckResult dfi_delays_legal(std::int64_t wr, std::int64_t rd, const DfiTimingInputs& in) {
    const std::int64_t r = in.ratio;
    if (wr < r || rd < r)
        return CheckResult::fail(FX1_DDR3_BLK_FIELD_ILLEGAL,
                                 "tphy_wrlat (" + std::to_string(wr) + ") and trddata_en (" + std::to_string(rd) +
                                     ") must be >= R (" + std::to_string(r) + ")");
    if (wr > 255 || rd > 255 || in.wrdata_delay > 255)
        return CheckResult::fail(FX1_DDR3_BLK_FIELD_ILLEGAL, "DFI delays must be below 256");
    auto calendar_ok = [r](std::int64_t dmax) {
        return (2 * r - 1 + dmax + 3) / r + 1 <= static_cast<std::int64_t>(kDfiCalDepth) - 1;
    };
    if (!calendar_ok(wr + in.wrdata_delay) || !calendar_ok(rd))
        return CheckResult::fail(FX1_DDR3_BLK_FIELD_ILLEGAL,
                                 "DFI delays exceed the calendar range (CAL_DEPTH " + s(kDfiCalDepth) + ")");
    return CheckResult::pass();
}

std::uint64_t read_timeout_floor(const DfiImage& dfi, unsigned ratio) noexcept {
    return 5ull * ratio + dfi.rd_enable_delay + dfi.max_read_return + 4ull + dfi.assembly_margin;
}

CheckResult check_dfi_image(const DfiImage& dfi, const DfiTimingInputs& in, std::uint32_t read_timeout) {
    std::int64_t wr = 0, rd = 0;
    if (!dfi_formula(in, wr, rd))
        return CheckResult::fail(FX1_DDR3_BLK_FIELD_ILLEGAL, "active timing gives a negative DFI delay");
    if (dfi.wr_enable_delay != wr)
        return CheckResult::fail(FX1_DDR3_BLK_FIELD_ILLEGAL,
                                 "DFI_WR_ENABLE_DELAY must equal CWL + AL - PWL - 2 = " + std::to_string(wr));
    if (dfi.rd_enable_delay != rd)
        return CheckResult::fail(FX1_DDR3_BLK_FIELD_ILLEGAL,
                                 "DFI_RD_ENABLE_DELAY must equal CL + AL + PRL + G - 4 = " + std::to_string(rd));
    CheckResult legal = dfi_delays_legal(wr, rd, in);
    if (!legal.ok) return legal;
    if (in.prl == 0) return CheckResult::fail(FX1_DDR3_BLK_FIELD_ILLEGAL, "PHY read latency must not be 0");
    if (dfi.max_read_return == 0 || dfi.max_read_return >= kTimeLimit)
        return CheckResult::fail(FX1_DDR3_BLK_FIELD_ILLEGAL, "PHY_MAX_READ_RETURN must be nonzero and < 2^31");
    if (dfi.assembly_margin < 1 || dfi.assembly_margin > 255)
        return CheckResult::fail(FX1_DDR3_BLK_FIELD_ILLEGAL, "DFI_ASSEMBLY_MARGIN must be 1..255");
    if (read_timeout < read_timeout_floor(dfi, in.ratio))
        return CheckResult::fail(FX1_DDR3_BLK_FIELD_ILLEGAL,
                                 "READ_TIMEOUT below 5R + trddata_en + tphy_rdlat + 4 + margin = " +
                                     s(read_timeout_floor(dfi, in.ratio)));
    return CheckResult::pass();
}

// ---------------------------------------------------------------- topology
Geometry decode_topology(std::uint32_t topo, bool ecc) noexcept {
    Geometry g;
    g.ranks = (topo & 1u) + 1u;
    g.row_bits = (topo >> 4) & 0x1Fu;
    g.page_words = 256u << ((topo >> 9) & 3u);
    g.map_profile = (topo >> 12) & 1u;
    // P = active ranks x 8 banks x 2^rows x (page words / 8) granules (HAS §6.2.2).
    g.physical_granules = std::uint64_t{g.ranks} << g.row_bits;
    g.physical_granules *= g.page_words;
    const std::uint64_t cap = std::uint64_t{1} << 27;
    if (ecc) {
        g.usable_granules = std::min(8 * (g.physical_granules / 9), cap);  // SPEC-07
        g.ecc_base = g.usable_granules;
    } else {
        g.usable_granules = std::min(g.physical_granules, cap);
        g.ecc_base = 0;
    }
    return g;
}

CheckResult check_topology(std::uint32_t topo, const BuildConfig& cfg, Geometry& out) {
    const Geometry g = decode_topology(topo, cfg.ecc_enable);
    auto fail = [](const std::string& why) { return CheckResult::fail(kBlockerTopology, why); };
    if (g.ranks > cfg.max_ranks())
        return fail("active ranks " + s(g.ranks) + " exceed the build maximum " + s(cfg.max_ranks()));
    if (g.row_bits < 12 || g.row_bits > cfg.row_bits_max)
        return fail("row bits must be 12.." + s(cfg.row_bits_max));
    if (!((cfg.map_profile_mask >> g.map_profile) & 1u))
        return fail("mapping profile is not built (MAP_PROFILE_MASK)");
    // HAS C-05: physical counts up to 2^28 granules are legal; usable is clamped to 2^27.
    if (g.physical_granules > (std::uint64_t{1} << 28)) return fail("physical capacity above 2^28 granules");
    if (cfg.ecc_enable && g.physical_granules / 9 == 0) return fail("no usable granule left with ECC");
    out = g;
    return CheckResult::pass();
}

// ---------------------------------------------------------------- timing
CheckResult check_timing(const TimingImage& t) {
    auto fail = [](const std::string& why) { return CheckResult::fail(kBlockerTiming, why); };
    static const char* const names[kTimingWords] = {
        "T_RCD", "T_RP", "T_RAS", "T_RC", "T_RRD", "T_FAW", "T_CCD", "T_RTP", "T_WR", "T_WTR", "T_RTW",
        "T_RTRS", "T_RFC", "T_REFI", "CL", "CWL", "ADDITIVE_LATENCY", "PHY_WRITE_LATENCY", "PHY_READ_LATENCY"};
    for (unsigned i = 0; i < kTimingWords; ++i) {
        if (t[i] >= kTimeLimit) return fail(std::string(names[i]) + " must be below 2^31");
        // Zero allowed only where JEDEC has no minimum (GAP-BE-09): T_RTRS, AL, PHY latencies
        // (PHY_READ_LATENCY != 0 is checked at the DFI commit, MAS Table 5-20).
        const bool zero_ok = i == kTRtrs || i == kAdditiveLatency || i == kPhyWriteLatency || i == kPhyReadLatency;
        if (!zero_ok && t[i] == 0) return fail(std::string(names[i]) + " must be nonzero");
    }
    const std::uint32_t cl = t[kCl], cwl = t[kCwl], al = t[kAdditiveLatency];
    if (cl < 5 || cl > 11) return fail("CL must be 5..11 (12..16 reserved)");
    if (cwl < 5 || cwl > 8) return fail("CWL must be 5..8");
    if (t[kTWr] < 5 || t[kTWr] > 16) return fail("T_WR must be 5..16");
    if (!(al == 0 || al == cl - 1 || al == cl - 2)) return fail("ADDITIVE_LATENCY must be 0, CL-1 or CL-2");
    if (std::int64_t{t[kTRtw]} < std::int64_t{cl} + 6 - cwl) return fail("T_RTW must be at least CL + 6 - CWL");
    for (unsigned i : {unsigned{kTCcd}, unsigned{kTRrd}, unsigned{kTRtp}, unsigned{kTWtr}})
        if (t[i] < 4) return fail(std::string(names[i]) + " must be at least 4");
    return CheckResult::pass();
}

// ---------------------------------------------------------------- policy
CheckResult check_policy(const PolicyImage& p, const DfiImage* dfi, unsigned ratio) {
    const unsigned enter = p.write_drain_cfg & 0xFFu;
    const unsigned exit_level = (p.write_drain_cfg >> 8) & 0xFFu;
    const unsigned max_writes = p.write_drain_cfg >> 16;
    if (enter < 1 || enter > kReqDepth) return CheckResult::fail(0, "WRITE_DRAIN_CFG enter level must be 1..16");
    if (exit_level >= enter) return CheckResult::fail(0, "WRITE_DRAIN_CFG exit level must be below the enter level");
    if (max_writes < 1) return CheckResult::fail(0, "WRITE_DRAIN_CFG maximum writes must be at least 1");
    if (p.starve_limit == 0 || p.starve_limit >= kTimeLimit)
        return CheckResult::fail(0, "STARVE_LIMIT must be 1..2^31-1");
    if (p.read_timeout == 0 || p.read_timeout >= kTimeLimit)
        return CheckResult::fail(0, "READ_TIMEOUT must be 1..2^31-1");
    if (dfi && p.read_timeout < read_timeout_floor(*dfi, ratio))
        return CheckResult::fail(0, "READ_TIMEOUT below the bound of the active DFI image");
    return CheckResult::pass();
}

// ---------------------------------------------------------------- mode registers
CheckResult check_mode_register(unsigned index, std::uint32_t v) {
    auto bit = [v](unsigned b) { return (v >> b) & 1u; };
    if (index == 1) {
        const unsigned rtt_nom = bit(9) << 2 | bit(6) << 1 | bit(2);  // {A9, A6, A2}
        if (rtt_nom > 3) return CheckResult::fail(0, "MR1 Rtt_Nom must be off, RZQ/4, RZQ/2 or RZQ/6");
        const unsigned dic = bit(5) << 1 | bit(1);  // {A5, A1}
        if (dic > 1) return CheckResult::fail(0, "MR1 output drive must be RZQ/6 or RZQ/7");
    } else if (index == 2) {
        if (((v >> 9) & 3u) == 3u) return CheckResult::fail(0, "MR2 Rtt_WR must not be 11");
        if (bit(6) && bit(7)) return CheckResult::fail(0, "MR2 SRT and ASR must not both be 1");
    }
    return CheckResult::pass();
}

// ---------------------------------------------------------------- region map
CheckResult check_region_image(const RegionImage& img, std::uint64_t usable_granules) {
    for (unsigned p = 0; p < 4; ++p) {
        for (unsigned r = 0; r < 4; ++r) {
            const std::uint64_t base = img[2 * (4 * p + r)], size = img[2 * (4 * p + r) + 1];
            if (size == 0) continue;
            if (base % 32 || size % 32)
                return CheckResult::fail(kBlockerRegionShape, "region BASE and SIZE must be multiples of 32");
            if (base + size > 0x100000000ull)
                return CheckResult::fail(kBlockerRegionShape, "region BASE + SIZE exceeds 2^32");
            for (unsigned q = 0; q < r; ++q) {
                const std::uint64_t b2 = img[2 * (4 * p + q)], s2 = img[2 * (4 * p + q) + 1];
                if (s2 && base < b2 + s2 && b2 < base + size)
                    return CheckResult::fail(kBlockerRegionShape, "regions of one port overlap");
            }
        }
    }
    for (unsigned i = 0; i < 16; ++i) {
        const std::uint64_t size = img[2 * i + 1];
        if (size && size / 32 > usable_granules)
            return CheckResult::fail(kBlockerRegionCapacity, "region size exceeds USABLE_GRANULES");
    }
    return CheckResult::pass();
}

} // namespace fx1::ddr3
