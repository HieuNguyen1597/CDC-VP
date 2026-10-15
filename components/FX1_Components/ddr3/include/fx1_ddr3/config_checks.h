// SPDX-License-Identifier: Apache-2.0
#pragma once
// Validation of every software-programmed configuration group (pure C++17). The same
// functions back the CSR commits, the live-policy writes, build validation (MAS Table
// 5-19 rule 6: reference images must pass their commit checks) and preinit images.
// Sources: MAS Table 5-20, MAS §6.2.12 "Programmable timing and the calendar",
// HAS §6.2.2/Table 8-11, HAS §8.1 Table 8-10/8-14/8-15, MAS §6.1.15.
#include "fx1_ddr3/profile.h"
#include "fx1_ddr3/speed_bin.h"

#include <array>
#include <cstdint>
#include <string>

namespace fx1::ddr3 {

struct CheckResult {
    bool ok = true;
    std::uint8_t blocker = 0;  // commit blocker code when !ok (0 when not a commit)
    std::string why;
    static CheckResult pass() { return {}; }
    static CheckResult fail(std::uint8_t code, std::string reason) { return {false, code, std::move(reason)}; }
};

// ---------------------------------------------------------------- DFI (MAS §6.2.12)
constexpr unsigned kDfiKWr = 2;        // k_wr: CSR build constant (MAS §5.6 / Table 6-142)
constexpr unsigned kDfiKRd = 4;        // k_rd
constexpr unsigned kDfiCalDepth = 32;  // CAL_DEPTH (MAS Table 5-7)

struct DfiTimingInputs {
    std::uint32_t cl, cwl, al, pwl, prl;  // active timing words
    std::uint32_t gate_offset;            // READ_GATE_OFFSET (build parameter, DEC-08)
    std::uint32_t wrdata_delay;           // tphy_wrdata (build parameter, DEC-08)
    unsigned ratio;
};
DfiTimingInputs dfi_inputs(const BuildConfig& cfg, const TimingImage& t) noexcept;
// tphy_wrlat = CWL + AL - PWL - k_wr and trddata_en = CL + AL + PRL + G - k_rd, computed
// without wrap-around. Returns false when either is negative.
bool dfi_formula(const DfiTimingInputs& in, std::int64_t& tphy_wrlat, std::int64_t& trddata_en) noexcept;
// Legal range: both >= R, < 256, and the calendar bound
// floor((2R - 1 + Dmax + 3) / R) + 1 <= CAL_DEPTH - 1, Dmax = tphy_wrlat + tphy_wrdata
// (writes) or trddata_en (reads).
CheckResult dfi_delays_legal(std::int64_t tphy_wrlat, std::int64_t trddata_en, const DfiTimingInputs& in);
// DFI_TIMING_COMMIT image check against the active timing (both formulas, ranges,
// PHY_MAX_READ_RETURN != 0, 1 <= margin <= 255, READ_TIMEOUT bound).
CheckResult check_dfi_image(const DfiImage& dfi, const DfiTimingInputs& in, std::uint32_t read_timeout);
// READ_TIMEOUT >= 5R + trddata_en + tphy_rdlat + 4 + margin, nonzero, < 2^31.
std::uint64_t read_timeout_floor(const DfiImage& dfi, unsigned ratio) noexcept;

// ---------------------------------------------------------------- topology (HAS §6.2.2)
struct Geometry {
    unsigned ranks = 0;        // active ranks (1..2)
    unsigned row_bits = 0;
    unsigned page_words = 0;   // 256..2048
    unsigned map_profile = 0;  // 0 linear, 1 interleaved (SOQ-31, CONFIRM)
    std::uint64_t physical_granules = 0;  // P
    std::uint64_t usable_granules = 0;    // U (ECC) or U0
    std::uint64_t ecc_base = 0;           // granule index of the hidden companion area
};
// Field decode of TOPO_SHADOW/TOPO_ACTIVE (HAS Table 8-11).
Geometry decode_topology(std::uint32_t topo, bool ecc) noexcept;
// TOPO_COMMIT validation (MAS Table 5-20 / HAS C-05 clamp). Blocker 0x11 on failure.
CheckResult check_topology(std::uint32_t topo, const BuildConfig& cfg, Geometry& out);

// ---------------------------------------------------------------- timing (MAS Table 5-20)
// TIMING_COMMIT validation of the 19-word bank. Blocker 0x04 on failure.
CheckResult check_timing(const TimingImage& t);

// ---------------------------------------------------------------- live policy (HAS Table 8-14)
struct PolicyImage {
    std::uint32_t write_drain_cfg = 0x00100408;
    std::uint32_t starve_limit = 0x400;
    std::uint32_t read_timeout = 0x400;
};
constexpr unsigned kReqDepth = 16;  // REQ_DEPTH (MAS Table 5-7)
// dfi may be null when no DFI image is active (the READ_TIMEOUT bound then is not checked).
CheckResult check_policy(const PolicyImage& p, const DfiImage* active_dfi, unsigned ratio);

// ---------------------------------------------------------------- mode registers (MAS Table 5-20)
// INIT_RW write validation of MR0..MR3 software fields (JEDEC JESD79-3F bit positions).
CheckResult check_mode_register(unsigned index, std::uint32_t value);

// ---------------------------------------------------------------- region map (MAS §6.1.15)
constexpr std::uint8_t kBlockerRegionNotReady = 0x0B;
constexpr std::uint8_t kBlockerRegionShape = 0x0C;
constexpr std::uint8_t kBlockerRegionCapacity = 0x0D;
constexpr std::uint8_t kBlockerTiming = 0x04;
constexpr std::uint8_t kBlockerTopology = 0x11;
using RegionImage = std::array<std::uint32_t, 32>;  // i = 2*(4p+r)+k, k=0 BASE, k=1 SIZE
// Enabled entries: BASE and SIZE multiples of 32, BASE+SIZE <= 2^32, no same-port overlap
// (0x0C); SIZE/32 <= USABLE_GRANULES (0x0D, GAP-FE-11). Different ports may overlap.
CheckResult check_region_image(const RegionImage& img, std::uint64_t usable_granules);

} // namespace fx1::ddr3
