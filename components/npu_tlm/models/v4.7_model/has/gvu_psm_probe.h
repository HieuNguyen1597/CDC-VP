// gvu_psm_probe.h -- observation of how the core's partial-sum manager (PSM) uses the PSUM SRAM (SRAM-C) during a core
// pass. Observation only: nothing computed by the model and no cycle count depends on it.
// Off unless the environment variable HAS_PSM_PROBE is set to a non-zero value; then has::HasNpuTop samples, at every
// rising clock edge, the start and done signals of the core and the SRAM-C port of the PSM (write enable, read enable,
// address, write mask) and a table is printed when the program exits ("[psm-probe] ..." lines).
// The numbers answer what a clocked epilogue between the PSM and the SRAM (a write that lands kTail cycles later) has to
// cope with:
//   writes        how many per pass, how many masked lanes, whether a word is written more than once in a pass and
//                 whether the masks of two writes to one word overlap;
//   read / write  whether the PSM reads a word it has already written in the same pass, and how many cycles after the
//                 write (a distance of kTail or less would return the value from before the delayed write);
//   rhythm        cycles between consecutive writes, longest run of writes in consecutive cycles;
//   channels      on passes with the epilogue: how many output channels the masked lanes of one write belong to;
//   tail          cycles from the last write of a pass to the done signal of the core.
// Passes are kept in two groups: the epilogue is applied to the writes (last input-channel part of a GEMM_FUSED tile), or
// it is not (earlier input-channel parts, FUSED_ATTN products on the core).
#ifndef HAS_PSM_PROBE_H
#define HAS_PSM_PROBE_H

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace has
{
    class PsmProbe
    {
    public:
        static constexpr uint32_t kWords = 1536;   // SRAM-C words of one ping-pong half
        static constexpr int kTail = 9;            // depth of the cycle-level GEMM_FUSED epilogue (has/rtl_ref_gvu_requant.h)

        static bool on() { return inst().on_; }
        // One call per clock cycle. mask: bit l = lane l of the write is enabled. epilogue / npos / nch: state of the
        // write hook of HasNpuTop (epilogue applied, positions per channel of the tile, channels of the tile).
        static void tick(bool start, bool done, bool wren, bool rden, uint32_t addr, uint32_t mask, bool epilogue, uint32_t npos, uint32_t nch)
        {
            inst().step(start, done, wren, rden, addr, mask, epilogue, npos, nch);
        }

    private:
        static constexpr int kBins = 7;   // 1, 2, 3-4, 5-8, 9-16, 17-64, > 64
        struct Stat
        {
            uint64_t passes{0}, passes_no_write{0};
            uint64_t writes{0}, lanes{0}, lane_bins[4]{}, zero_mask{0}, words{0}, rewrites{0}, rewrites_overlap{0};
            uint64_t reads{0}, reads_after_write{0}, raw_bins[kBins]{}, raw_le_tail{0}, raw_min{~0ull};
            uint64_t gap_bins[kBins]{}, run_max{0};
            uint64_t span[2][3]{}, beyond_tile{0};
            uint64_t tail_bins[kBins]{}, tail_min{~0ull}, tail_max{0}, tail_zero{0};
        };
        bool on_{false};
        Stat st_[2];
        uint64_t cyc_{0}, outside_writes_{0}, outside_reads_{0}, open_at_exit_{0};
        bool start_q_{false}, done_q_{false}, in_pass_{false};
        int kind_{0};
        uint64_t last_w_{0}, run_{0};
        bool any_w_{false};
        uint32_t wcnt_[kWords];
        uint32_t wmask_[kWords];
        uint64_t wcyc_[kWords];

        static PsmProbe &inst() { static PsmProbe p; return p; }
        PsmProbe()
        {
            const char *v = std::getenv("HAS_PSM_PROBE");
            on_ = v && std::atoi(v) != 0;
            std::memset(wcnt_, 0, sizeof wcnt_);
            std::memset(wmask_, 0, sizeof wmask_);
            std::memset(wcyc_, 0, sizeof wcyc_);
            if (on_) std::atexit(&PsmProbe::report);
        }
        static int bin(uint64_t d) { return d <= 1 ? 0 : d == 2 ? 1 : d <= 4 ? 2 : d <= 8 ? 3 : d <= 16 ? 4 : d <= 64 ? 5 : 6; }
        static int popcount(uint32_t m) { int n = 0; for (; m; m &= m - 1) n++; return n; }

        void close_pass(bool with_done)
        {
            Stat &s = st_[kind_];
            if (!any_w_) s.passes_no_write++;
            else if (with_done)
            {
                const uint64_t d = cyc_ - last_w_;
                if (d == 0) s.tail_zero++;
                else s.tail_bins[bin(d)]++;
                if (d < s.tail_min) s.tail_min = d;
                if (d > s.tail_max) s.tail_max = d;
            }
            in_pass_ = false;
        }
        void step(bool start, bool done, bool wren, bool rden, uint32_t addr, uint32_t mask, bool epilogue, uint32_t npos, uint32_t nch)
        {
            if (!on_) return;
            cyc_++;
            if (start && !start_q_)
            {
                if (in_pass_) { open_at_exit_++; close_pass(false); }
                in_pass_ = true;
                kind_ = epilogue ? 0 : 1;
                st_[kind_].passes++;
                any_w_ = false;
                run_ = 0;
                std::memset(wcnt_, 0, sizeof wcnt_);
                std::memset(wmask_, 0, sizeof wmask_);
            }
            const uint32_t a = addr % kWords;
            if (wren)
            {
                if (!in_pass_) outside_writes_++;
                else
                {
                    Stat &s = st_[kind_];
                    const int n = popcount(mask);
                    s.writes++;
                    s.lanes += uint64_t(n);
                    if (n == 0) s.zero_mask++;
                    else s.lane_bins[n <= 8 ? 0 : n <= 16 ? 1 : n <= 31 ? 2 : 3]++;
                    if (wcnt_[a] == 0) s.words++;
                    else
                    {
                        s.rewrites++;
                        if (wmask_[a] & mask) s.rewrites_overlap++;
                    }
                    if (any_w_)
                    {
                        const uint64_t g = cyc_ - last_w_;
                        s.gap_bins[bin(g)]++;
                        run_ = g == 1 ? run_ + 1 : 1;
                    }
                    else run_ = 1;
                    if (run_ > s.run_max) s.run_max = run_;
                    if (kind_ == 0 && n > 0 && npos > 0)
                    {
                        int lo = 0, hi = 31;
                        while (!((mask >> lo) & 1u)) lo++;
                        while (!((mask >> hi) & 1u)) hi--;
                        const uint32_t e0 = a * 32u, x0 = (e0 + uint32_t(lo)) / npos, x1 = (e0 + uint32_t(hi)) / npos, sp = x1 - x0;
                        s.span[npos % 32u == 0 ? 0 : 1][sp >= 2 ? 2 : sp]++;
                        if (x1 >= nch) s.beyond_tile++;
                    }
                    wcnt_[a]++;
                    wmask_[a] |= mask;
                    wcyc_[a] = cyc_;
                    last_w_ = cyc_;
                    any_w_ = true;
                }
            }
            if (rden)
            {
                if (!in_pass_) outside_reads_++;
                else
                {
                    Stat &s = st_[kind_];
                    s.reads++;
                    if (wcnt_[a])
                    {
                        const uint64_t d = cyc_ - wcyc_[a];
                        s.reads_after_write++;
                        s.raw_bins[bin(d)]++;
                        if (d <= uint64_t(kTail)) s.raw_le_tail++;
                        if (d < s.raw_min) s.raw_min = d;
                    }
                }
            }
            if (in_pass_ && done && !done_q_) close_pass(true);
            start_q_ = start;
            done_q_ = done;
        }
        static void bins(const char *tag, const char *what, const uint64_t *b)
        {
            std::printf("[psm-probe] %s | %s | 1: %llu | 2: %llu | 3-4: %llu | 5-8: %llu | 9-16: %llu | 17-64: %llu | >64: %llu\n", tag, what,
                        (unsigned long long)b[0], (unsigned long long)b[1], (unsigned long long)b[2], (unsigned long long)b[3],
                        (unsigned long long)b[4], (unsigned long long)b[5], (unsigned long long)b[6]);
        }
        static void report()
        {
            static const char *tag[2] = {"epilogue passes", "other passes"};
            const PsmProbe &p = inst();
            std::printf("[psm-probe] PSM use of SRAM-C per core pass (HAS_PSM_PROBE), %llu cycles observed; writes / reads outside a pass %llu / %llu; "
                        "passes without done %llu\n", (unsigned long long)p.cyc_, (unsigned long long)p.outside_writes_,
                        (unsigned long long)p.outside_reads_, (unsigned long long)(p.open_at_exit_ + (p.in_pass_ ? 1 : 0)));
            for (int i = 0; i < 2; i++)
            {
                const Stat &s = p.st_[i];
                if (s.passes == 0) continue;
                std::printf("[psm-probe] %s | passes %llu (without a write %llu) | writes %llu | masked lanes %llu | words written %llu | "
                            "writes to a word already written in the pass %llu (mask overlaps the earlier one: %llu)\n", tag[i],
                            (unsigned long long)s.passes, (unsigned long long)s.passes_no_write, (unsigned long long)s.writes,
                            (unsigned long long)s.lanes, (unsigned long long)s.words, (unsigned long long)s.rewrites,
                            (unsigned long long)s.rewrites_overlap);
                std::printf("[psm-probe] %s | masked lanes per write | 0: %llu | 1-8: %llu | 9-16: %llu | 17-31: %llu | 32: %llu\n", tag[i],
                            (unsigned long long)s.zero_mask, (unsigned long long)s.lane_bins[0], (unsigned long long)s.lane_bins[1],
                            (unsigned long long)s.lane_bins[2], (unsigned long long)s.lane_bins[3]);
                std::printf("[psm-probe] %s | reads %llu | reads of a word already written in the pass %llu | smallest distance write -> read %s%llu "
                            "cycles | at a distance of %d cycles or less: %llu\n", tag[i], (unsigned long long)s.reads,
                            (unsigned long long)s.reads_after_write, s.reads_after_write ? "" : "(none) ",
                            (unsigned long long)(s.reads_after_write ? s.raw_min : 0), kTail, (unsigned long long)s.raw_le_tail);
                if (s.reads_after_write) bins(tag[i], "cycles from the write of a word to its next read", s.raw_bins);
                bins(tag[i], "cycles between consecutive writes", s.gap_bins);
                std::printf("[psm-probe] %s | longest run of writes in consecutive cycles %llu\n", tag[i], (unsigned long long)s.run_max);
                if (i == 0)
                    std::printf("[psm-probe] %s | output channels under the masked lanes of one write | positions per channel a multiple of 32: "
                                "one %llu, two %llu, three or more %llu | not a multiple: one %llu, two %llu, three or more %llu | "
                                "writes that reach beyond the channels of the tile %llu\n", tag[i],
                                (unsigned long long)s.span[0][0], (unsigned long long)s.span[0][1], (unsigned long long)s.span[0][2],
                                (unsigned long long)s.span[1][0], (unsigned long long)s.span[1][1], (unsigned long long)s.span[1][2],
                                (unsigned long long)s.beyond_tile);
                std::printf("[psm-probe] %s | cycles from the last write to done: smallest %llu, largest %llu, in the same cycle %llu\n", tag[i],
                            (unsigned long long)(s.tail_min == ~0ull ? 0 : s.tail_min), (unsigned long long)s.tail_max, (unsigned long long)s.tail_zero);
                bins(tag[i], "cycles from the last write to done", s.tail_bins);
            }
            std::fflush(stdout);
        }
    };
} // namespace has

#endif // HAS_PSM_PROBE_H
