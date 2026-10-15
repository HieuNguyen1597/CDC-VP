// SPDX-License-Identifier: Apache-2.0
#pragma once
// JEDEC speed-bin derived values (MAS §5.5-5.6). These are NOT CSR reset values in
// the VP (SPEC-03: HAS timing shadows reset to 0). They are used for:
//  - the MAS-only timing words that have no CSR offset (DEC-08), as build parameters;
//  - the legal image written by tests, preinit mode and firmware examples.
#include "fx1_ddr3/profile.h"

#include <array>
#include <cstdint>
#include <string>

namespace fx1::ddr3 {

// Index of the 19-word HAS timing bank (HAS Table 8-12).
enum TimingIndex : unsigned {
    kTRcd = 0, kTRp, kTRas, kTRc, kTRrd, kTFaw, kTCcd, kTRtp, kTWr, kTWtr, kTRtw,
    kTRtrs, kTRfc, kTRefi, kCl, kCwl, kAdditiveLatency, kPhyWriteLatency, kPhyReadLatency,
    kTimingWords
};
using TimingImage = std::array<std::uint32_t, kTimingWords>;

// Timing words that exist only in MAS (Table 5-17, ZQ_INTERVAL); no CSR offset.
struct ExtTimingImage {
    std::uint32_t t_cke, t_ckesr, t_xp, t_xpdll, t_xs, t_xsdll;
    std::uint32_t t_cksre, t_cksrx, t_mod, t_mrd, t_zqoper, t_zqcs;
    std::uint32_t zq_interval;   // 0 disables periodic ZQCS
};

// Initialization / training waits in tCK (MAS Table 5-16), CSR order 0x3060..0x307C.
struct InitWaitImage {
    std::uint32_t reset_assert;   // INIT_RESET_ASSERT_WAIT (200 us)
    std::uint32_t reset_release;  // INIT_RESET_RELEASE_WAIT (500 us)
    std::uint32_t cke_low;        // INIT_CKE_LOW_WAIT
    std::uint32_t cke_high;       // INIT_CKE_HIGH_WAIT (tXPR)
    std::uint32_t dll;            // INIT_DLL_WAIT (tDLLK)
    std::uint32_t zq;             // INIT_ZQ_WAIT (tZQinit)
    std::uint32_t cmd_timeout;    // INIT_CMD_TIMEOUT, controller cycles, not tCK
    std::uint32_t train_tras;     // TRAIN_TRAS_WAIT
};

// The 4 HAS DFI shadow words (0x5010..0x501C).
struct DfiImage {
    std::uint32_t wr_enable_delay;   // CWL + AL - PWL - 2
    std::uint32_t rd_enable_delay;   // CL + AL + PRL + G - 4
    std::uint32_t max_read_return;   // PHY_MAX_READ_RETURN
    std::uint32_t assembly_margin;   // DFI_ASSEMBLY_MARGIN
};

// tCK(min) of each bin in picoseconds (MAS Table 5-13).
std::uint32_t tck_ps(SpeedBin bin) noexcept;
// ceil(t / tCK) with t in picoseconds; never below min_nck.
std::uint32_t nck_ceil(std::uint64_t t_ps, std::uint32_t tck, std::uint32_t min_nck = 0) noexcept;

// Reference image for the build (slowest sub-bin of the rate, MAS §5.5).
TimingImage timing_image(const BuildConfig& cfg) noexcept;
ExtTimingImage ext_timing_image(const BuildConfig& cfg) noexcept;
InitWaitImage init_wait_image(const BuildConfig& cfg) noexcept;

// Derives the DFI image without wrap-around and applies the DFI_TIMING_COMMIT legal range
// (>= R, < 256, calendar bound; see config_checks.h). Returns false (and the reason)
// otherwise; `out` is then unchanged.
bool derive_dfi_image(const BuildConfig& cfg, DfiImage& out, std::string* why = nullptr);
// Same, but throws std::invalid_argument for an unrepresentable configuration.
// validate() rejects such configurations, so this never throws for a legal build.
DfiImage dfi_image(const BuildConfig& cfg);

} // namespace fx1::ddr3
