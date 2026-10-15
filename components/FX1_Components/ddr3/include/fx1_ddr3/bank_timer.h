// SPDX-License-Identifier: Apache-2.0
#pragma once
// Approximate per-bank DRAM timing (DEC-02; HAS §6.2.6-6.2.7, MAS Table 6-98/6-99).
// Pure C++17, all values in tCK.
//
// Open-page policy, BL8. Each granule access is one column command; the model tracks the
// open row and the earliest legal PRE per (rank, bank), ACT spacing (tRRD, tFAW window of
// four), column spacing (tCCD), rank switch (BL/2 + tRTRS), and the WR->RD (CWL + AL +
// BL/2 + tWTR) and RD->WR (tRTW) turnarounds. Refresh: on a rank whose refresh runs, a
// REF window [origin + k tREFI, + tRFC), k >= 1, closes the rank's rows (PREA, REF) and
// delays commands to its end; debt repayment keeps the rank busy. Requests are served in
// arrival order; there is no reordering scheduler.
#include "fx1_ddr3/address_map.h"
#include "fx1_ddr3/speed_bin.h"

#include <array>
#include <cstdint>

namespace fx1::ddr3 {

struct BankTimingParams {
    std::uint32_t cl = 11, cwl = 8, al = 0;
    std::uint32_t trcd = 11, trp = 11, tras = 28, trrd = 6, tfaw = 32, tccd = 4;
    std::uint32_t trtp = 6, twr = 12, twtr = 6, trtw = 7, trtrs = 2;
    static BankTimingParams from(const TimingImage& t) noexcept;
};

class BankTimer {
public:
    void reset() noexcept;
    void close_all() noexcept;  // precharge power-down / self-refresh entry (BE-6.2.6-03)
    // Refresh grid of one rank (tCK). A new origin restarts the window count; `busy_until`
    // covers back-to-back REFs that repay debt.
    void set_refresh(unsigned rank, bool enabled, std::uint64_t origin, std::uint32_t trefi, std::uint32_t trfc,
                     std::uint64_t busy_until) noexcept;
    // Issues one BL8 column access at or after `now`; returns the tCK at which its data
    // transfer ends (read data returned, or write data on the bus).
    std::uint64_t access(std::uint64_t now, const DramCoord& c, bool write, const BankTimingParams& p) noexcept;

    std::uint64_t row_hits() const noexcept { return hits_; }
    std::uint64_t row_misses() const noexcept { return misses_; }     // bank closed: ACT
    std::uint64_t row_conflicts() const noexcept { return conflicts_; } // other row: PRE + ACT
    std::uint64_t refresh_delays() const noexcept { return refresh_delays_; } // accesses pushed by REF

private:
    struct RefreshGrid {
        bool enabled = false;
        std::uint64_t origin = 0;
        std::uint32_t trefi = 0, trfc = 0;
        std::uint64_t busy_until = 0;
        std::uint64_t last_k = 0;  // last refresh window seen by an access
    };
    std::uint64_t refresh_adjust(unsigned rank, std::uint64_t t) noexcept;
    std::array<RefreshGrid, 2> ref_{};
    std::uint64_t refresh_delays_ = 0;
    struct Bank {
        bool open = false;
        std::uint32_t row = 0;
        std::uint64_t act_at = 0;
        std::uint64_t pre_ok = 0;  // earliest PRE (tRAS, read-to-precharge, write recovery)
    };
    std::array<std::array<Bank, 8>, 2> banks_{};
    std::array<std::array<std::uint64_t, 4>, 2> acts_{};  // last four ACTs per rank (tFAW)
    std::array<unsigned, 2> act_count_{};
    std::uint64_t next_col_ = 0;
    std::uint64_t last_col_ = 0;
    std::uint64_t wr_data_end_ = 0;
    unsigned last_rank_ = 0;
    bool last_write_ = false;
    bool have_last_ = false;
    std::uint64_t hits_ = 0, misses_ = 0, conflicts_ = 0;
};

} // namespace fx1::ddr3
