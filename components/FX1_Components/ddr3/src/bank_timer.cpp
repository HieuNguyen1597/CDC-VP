// SPDX-License-Identifier: Apache-2.0
#include "fx1_ddr3/bank_timer.h"

#include <algorithm>

namespace fx1::ddr3 {

namespace {
constexpr std::uint64_t kBurstTck = 4;  // BL/2
}

BankTimingParams BankTimingParams::from(const TimingImage& t) noexcept {
    BankTimingParams p;
    p.cl = t[kCl];
    p.cwl = t[kCwl];
    p.al = t[kAdditiveLatency];
    p.trcd = t[kTRcd];
    p.trp = t[kTRp];
    p.tras = t[kTRas];
    p.trrd = t[kTRrd];
    p.tfaw = t[kTFaw];
    p.tccd = t[kTCcd];
    p.trtp = t[kTRtp];
    p.twr = t[kTWr];
    p.twtr = t[kTWtr];
    p.trtw = t[kTRtw];
    p.trtrs = t[kTRtrs];
    return p;
}

void BankTimer::reset() noexcept { *this = BankTimer{}; }

void BankTimer::close_all() noexcept {
    for (auto& rank : banks_)
        for (Bank& b : rank) b.open = false;
}

void BankTimer::set_refresh(unsigned rank, bool enabled, std::uint64_t origin, std::uint32_t trefi, std::uint32_t trfc,
                            std::uint64_t busy_until) noexcept {
    RefreshGrid& g = ref_[rank & 1u];
    if (origin != g.origin) g.last_k = 0;
    g.enabled = enabled && trefi != 0;
    g.origin = origin;
    g.trefi = trefi;
    g.trfc = trfc;
    g.busy_until = busy_until;
}

std::uint64_t BankTimer::refresh_adjust(unsigned rank, std::uint64_t t) noexcept {
    RefreshGrid& g = ref_[rank];
    const std::uint64_t in = t;
    bool refreshed = false;
    if (t < g.busy_until) {
        t = g.busy_until;  // debt repayment: back-to-back REF
        refreshed = true;
    }
    if (g.enabled && t >= g.origin + g.trefi) {
        const std::uint64_t k = (t - g.origin) / g.trefi;
        const std::uint64_t start = g.origin + k * g.trefi;
        if (k > g.last_k) {
            g.last_k = k;
            refreshed = true;
        }
        if (t < start + g.trfc) t = start + g.trfc;  // tRFC after REF
    }
    if (refreshed)
        for (Bank& b : banks_[rank]) b.open = false;  // PREA before REF
    if (t != in) ++refresh_delays_;
    return t;
}

std::uint64_t BankTimer::access(std::uint64_t now, const DramCoord& c, bool write, const BankTimingParams& p) noexcept {
    const unsigned rank = c.rank & 1u;
    // Commands issue in order: check the refresh state at the earliest time this access's
    // commands can follow the previous column command.
    const std::uint64_t probe = have_last_ ? std::max(now, last_col_) : now;
    const std::uint64_t adjusted = refresh_adjust(rank, probe);
    if (adjusted != probe) now = adjusted;
    Bank& b = banks_[rank][c.bank & 7u];
    std::uint64_t col = now;
    if (b.open && b.row == c.row) {
        ++hits_;
    } else {
        std::uint64_t act = now;
        if (b.open) {
            ++conflicts_;
            act = std::max(now, b.pre_ok) + p.trp;  // PRE, then ACT after tRP
        } else {
            ++misses_;
        }
        auto& win = acts_[rank];
        if (act_count_[rank] > 0) act = std::max(act, win[(act_count_[rank] - 1u) % 4u] + p.trrd);
        if (act_count_[rank] >= 4) act = std::max(act, win[act_count_[rank] % 4u] + p.tfaw);  // oldest of four
        win[act_count_[rank] % 4u] = act;
        ++act_count_[rank];
        b.open = true;
        b.row = c.row;
        b.act_at = act;
        b.pre_ok = act + p.tras;
        col = act + (p.trcd > p.al ? p.trcd - p.al : 0u);  // act_to_col_delay = max(0, tRCD - AL)
    }
    col = std::max(col, next_col_);
    if (have_last_) {
        if (rank != last_rank_) col = std::max(col, last_col_ + kBurstTck + p.trtrs);
        if (!write && last_write_) col = std::max(col, wr_data_end_ + p.twtr);
        if (write && !last_write_) col = std::max(col, last_col_ + p.trtw);
    }
    std::uint64_t end = 0;
    if (write) {
        end = col + p.al + p.cwl + kBurstTck;
        wr_data_end_ = end;
        b.pre_ok = std::max(b.pre_ok, end + p.twr);  // wr_precharge_delay
    } else {
        end = col + p.al + p.cl + kBurstTck;
        b.pre_ok = std::max(b.pre_ok, col + p.al + p.trtp);  // rd_precharge_delay
    }
    next_col_ = col + p.tccd;
    last_col_ = col;
    last_rank_ = rank;
    last_write_ = write;
    have_last_ = true;
    return end;
}

} // namespace fx1::ddr3
