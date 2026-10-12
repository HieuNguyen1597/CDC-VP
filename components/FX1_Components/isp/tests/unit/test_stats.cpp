// SPDX-License-Identifier: Apache-2.0
// Directed tests of the statistics publication rules that the end-of-frame
// vectors cannot reach: AEC commit timing, zone/histogram view invalidation
// at SOF (DEC-29), SOF latching of configuration and context IDs, AF EN
// sampled at SOF, FRAME_ID binding, soft/external reset, readout wrap masks,
// AWB result banks (MAS §6.22.4-6.22.6, Table 140: LEASE, FULL, COMMAND,
// REPORT, GRID, RESET, PUBLISH) and BPC counters (MAS §6.9).
// Expected values are analytic (flat per-channel Bayer field) or
// differential (same frame with and without a mid-frame write).
// Rule identifiers: plan/alg/ALG_F (ALG-AEC-*, ALG-AWB-*, ALG-AF-*, ALG-STAT-*).

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "control/control_unit.h"
#include "fx1_check.h"
#include "fx1_isp/fx1_isp_csr.h"
#include "pipeline/isp_pipeline.h"

using namespace cdc::components::fx1_isp;

namespace {

constexpr std::uint32_t W = 64, H = 48;
constexpr std::uint16_t flat_values[4] = {100, 1000, 3300, 4000};  // R, Gr, Gb, B

// A frame-level stand-in for the engine: accepted SOF, snapshot, rows, EOF.
struct rig {
   control_unit cu;
   pipe::isp_pipeline p;
   pipe::pipeline_config cfg;
   std::uint64_t cycle = 1;
   std::vector<std::uint16_t> raw;

   explicit rig(bool flat_luma = false) : raw(std::size_t{W} * H) {
      for (std::uint32_t y = 0; y < H; ++y) {
         for (std::uint32_t x = 0; x < W; ++x) {
            raw[std::size_t{y} * W + x] = flat_luma ? 2000 : flat_values[((y & 1u) << 1) | (x & 1u)];
         }
      }
      wr(FX1_ISP_COMMON_FRAME_WIDTH_OFFSET, W);
      wr(FX1_ISP_COMMON_FRAME_HEIGHT_OFFSET, H);
   }
   void wr(std::uint32_t off, std::uint32_t v) { cu.csr_write(off, v, 0xFFFFFFFFu, cycle++); }
   std::uint32_t rd(std::uint32_t off) const { return cu.csr_read(off); }
   std::uint32_t rd_sel(std::uint32_t sel_off, std::uint32_t sel, std::uint32_t off) {
      wr(sel_off, sel);
      return rd(off);
   }
   void sof() {
      cu.accepted_sof(cycle++);
      cfg = pipe::snapshot_config(cu);
      p.begin(cfg, W, H, [](std::vector<std::uint8_t> &&) {}, [](std::vector<std::uint8_t> &&) {});
   }
   void rows(std::uint32_t from, std::uint32_t to) {
      for (std::uint32_t y = from; y < to; ++y) {
         p.push(&raw[std::size_t{y} * W]);
         ++cycle;
      }
   }
   void eof() {
      p.finish();
      cu.advance_to(cycle++);  // the publication edge
      pipe::end_of_frame(cu, cfg, p.stats());
   }
   void frame(const std::function<void()> &mid = nullptr) {
      sof();
      rows(0, H / 2);
      if (mid) {
         mid();
      }
      rows(H / 2, H);
      eof();
   }
   void aec(std::uint32_t nx, std::uint32_t ny, std::uint32_t zw, std::uint32_t zh, std::uint32_t ctrl) {
      wr(FX1_ISP_AEC_ZONE_CFG_OFFSET, nx | (ny << 8));
      wr(FX1_ISP_AEC_ZONE_SIZE_OFFSET, zw | (zh << 16));
      wr(FX1_ISP_AEC_CTRL_OFFSET, ctrl);
   }
   std::uint32_t aec_zone(std::uint32_t z, std::uint32_t sel, std::uint32_t off) {
      wr(FX1_ISP_AEC_ZONE_ADDR_OFFSET, z);
      return rd_sel(FX1_ISP_AEC_CHANNEL_SEL_OFFSET, sel, off);
   }
   std::uint32_t aec_global_sum(std::uint32_t sel) {
      return rd_sel(FX1_ISP_AEC_CHANNEL_SEL_OFFSET, sel, FX1_ISP_AEC_GLOBAL_SUM_LO_OFFSET);
   }
   std::uint32_t hist(std::uint32_t b) { return rd_sel(FX1_ISP_AEC_HIST_ADDR_OFFSET, b, FX1_ISP_AEC_HIST_DATA_OFFSET); }
   std::uint32_t af(std::uint32_t z) { return rd_sel(FX1_ISP_AF_STAT_ADDR_OFFSET, z, FX1_ISP_AF_STAT_DATA_OFFSET); }
   // AWB ownership (MAS Table 138).
   void awb_cmd(std::uint32_t bits) { wr(FX1_ISP_AWB_RESULT_CTRL_OFFSET, bits); }
   void acquire() { awb_cmd(FX1_ISP_AWB_RESULT_CTRL_ACQUIRE_MASK); }
   void release() { awb_cmd(FX1_ISP_AWB_RESULT_CTRL_RELEASE_MASK); }
   std::uint32_t awb_status() const { return rd(FX1_ISP_AWB_RESULT_STATUS_OFFSET); }
   std::uint32_t ready_count() const { return awb_status() & FX1_ISP_AWB_RESULT_STATUS_READY_COUNT_MASK; }
   bool leased() const { return (awb_status() & FX1_ISP_AWB_RESULT_STATUS_READ_VALID_MASK) != 0; }
   std::uint32_t drops() const { return rd(FX1_ISP_AWB_DROP_COUNT_OFFSET); }
   std::uint32_t awb_zone_count(std::uint32_t z) { return rd_sel(FX1_ISP_AWB_ZONE_ADDR_OFFSET, z, FX1_ISP_AWB_ZONE_COUNT_OFFSET); }
   // Acquire, read FRAME_ID, release: the identity of the oldest READY result.
   std::uint32_t take_awb_frame_id() {
      acquire();
      const std::uint32_t id = rd(FX1_ISP_AWB_FRAME_ID_OFFSET);
      release();
      return id;
   }
};

constexpr std::uint32_t awb_1x1 = 1 | (1 << 1) | (1 << 8);  // enabled, 1 x 1 grid
constexpr std::uint32_t overrun = FX1_ISP_AWB_RESULT_STATUS_OVERRUN_MASK;
constexpr std::uint32_t write_busy = FX1_ISP_AWB_RESULT_STATUS_WRITE_BUSY_MASK;

constexpr std::uint32_t aec_commit = FX1_ISP_AEC_CTRL_EN_MASK | FX1_ISP_AEC_CTRL_COMMIT_MASK;
constexpr std::uint32_t aec_done = FX1_ISP_AEC_STATUS_STAT_DONE_MASK;
constexpr std::uint32_t af_done = FX1_ISP_AF_STATUS_FRAME_DONE_MASK;
constexpr std::uint32_t af_valid = FX1_ISP_AF_STATUS_SCORE_VALID_MASK;

// Flat field, 16x12 zones of 4x4: every zone holds 4 samples per channel.
void check_flat_aec_frame(rig &r, std::uint32_t zones_x_total) {
   FX1_CHECK_EQ(r.aec_global_sum(0), 100u * W * H / 4);
   FX1_CHECK_EQ(r.aec_zone(0, 0, FX1_ISP_AEC_ZONE_SUM_OFFSET), 100u * 4 * (zones_x_total == 1 ? W * H / 16 : 1));
}

// ALG-AEC-06 / #296: shadow set, commit transferred at the next SOF.
void test_aec_commit() {
   rig r;
   r.aec(16, 12, 4, 4, FX1_ISP_AEC_CTRL_EN_MASK);  // en without commit: no effect
   r.frame();
   FX1_CHECK_EQ(r.rd(FX1_ISP_AEC_STATUS_OFFSET), 0);
   FX1_CHECK_EQ(r.rd(FX1_ISP_AEC_FRAME_ID_OFFSET), 0);

   r.wr(FX1_ISP_AEC_CTRL_OFFSET, aec_commit);
   FX1_CHECK_EQ(r.rd(FX1_ISP_AEC_CTRL_OFFSET), aec_commit);  // pending reads 1
   r.wr(FX1_ISP_AEC_CTRL_OFFSET, FX1_ISP_AEC_CTRL_EN_MASK);  // writing 0 to a W1S bit: still pending
   FX1_CHECK_EQ(r.rd(FX1_ISP_AEC_CTRL_OFFSET), aec_commit);
   r.frame();
   FX1_CHECK_EQ(r.rd(FX1_ISP_AEC_CTRL_OFFSET), FX1_ISP_AEC_CTRL_EN_MASK);  // self-cleared at SOF
   FX1_CHECK_EQ(r.rd(FX1_ISP_AEC_STATUS_OFFSET), aec_done);
   FX1_CHECK_EQ(r.rd(FX1_ISP_AEC_FRAME_ID_OFFSET), 2);  // frames started, not frames measured
   check_flat_aec_frame(r, 16);

   r.wr(FX1_ISP_AEC_ZONE_CFG_OFFSET, 1 | (1 << 8));  // new grid, no commit
   r.frame();
   FX1_CHECK_EQ(r.rd(FX1_ISP_AEC_FRAME_ID_OFFSET), 3);
   check_flat_aec_frame(r, 16);

   r.frame([&] { r.wr(FX1_ISP_AEC_CTRL_OFFSET, aec_commit); });  // commit mid-frame: next SOF
   check_flat_aec_frame(r, 16);
   FX1_CHECK_EQ(r.rd(FX1_ISP_AEC_CTRL_OFFSET), aec_commit);
   r.frame();
   check_flat_aec_frame(r, 1);  // 1x1 grid: the single zone absorbs the frame (ALG-AEC-01)
   FX1_CHECK_EQ(r.rd(FX1_ISP_AEC_CTRL_OFFSET), FX1_ISP_AEC_CTRL_EN_MASK);

   // A commit of en = 0 lets the frame in progress complete and publish.
   r.frame([&] { r.wr(FX1_ISP_AEC_CTRL_OFFSET, FX1_ISP_AEC_CTRL_COMMIT_MASK); });
   FX1_CHECK_EQ(r.rd(FX1_ISP_AEC_FRAME_ID_OFFSET), 6);
   r.wr(FX1_ISP_AEC_STATUS_OFFSET, aec_done);  // W1C
   r.frame();
   FX1_CHECK_EQ(r.rd(FX1_ISP_AEC_FRAME_ID_OFFSET), 6);  // disabled: frozen (ALG-STAT-06)
   FX1_CHECK_EQ(r.rd(FX1_ISP_AEC_STATUS_OFFSET), 0);
   FX1_CHECK_EQ(r.rd(FX1_ISP_AEC_CTRL_OFFSET), 0);
   check_flat_aec_frame(r, 1);                          // results retained
   FX1_CHECK_EQ(r.rd(FX1_ISP_AEC_STATUS_OFFSET) & FX1_ISP_AEC_STATUS_ERROR_MASK, 0);  // never set
}

// DEC-29/DEC-30, ALG-STAT-02/03: zone and histogram memory is one memory
// restarted at the SOF of an enabled frame and updated row by row: during
// the frame, written zones read their partial sums and unwritten zones the
// empty value; the global results keep the previous publication until EOF.
void test_single_memory_partial_reads() {
   rig r;
   // One zone per pair of rows: zone k holds rows 2k (R, Gr) and 2k+1 (Gb, B).
   r.aec(1, 24, W, 2, aec_commit);
   r.wr(FX1_ISP_AWB_CTRL_OFFSET, 1 | (1 << 1) | (24 << 8));  // AWB zone k = rows 2k, 2k+1 as well
   r.frame();
   // AWB-LEASE: the result of frame 1 is leased and read while frame 2 is
   // measured into the other bank; it never changes before RELEASE.
   r.acquire();
   const std::uint32_t awb_count = r.rd(FX1_ISP_AWB_GLOBAL_COUNT_OFFSET);
   r.wr(FX1_ISP_AWB_ZONE_ADDR_OFFSET, 0);
   const std::uint32_t awb_zone0 = r.rd(FX1_ISP_AWB_ZONE_COUNT_OFFSET);
   FX1_CHECK(awb_zone0 > 0);

   r.sof();  // the next enabled frame starts: nothing written yet
   FX1_CHECK_EQ(r.rd(FX1_ISP_AEC_STATUS_OFFSET), FX1_ISP_AEC_STATUS_BUSY_MASK | aec_done);
   FX1_CHECK_EQ(r.aec_zone(0, 0, FX1_ISP_AEC_ZONE_SUM_OFFSET), 0);
   FX1_CHECK_EQ(r.aec_zone(0, 0, FX1_ISP_AEC_ZONE_GREEN_OE_UE_OFFSET), 0);
   FX1_CHECK_EQ(r.aec_zone(0, 0, FX1_ISP_AEC_ZONE_GREEN_MIN_MAX_OFFSET), 4095);  // empty: min 4095, max 0
   FX1_CHECK_EQ(r.hist(1000 >> 6), 0);
   FX1_CHECK_EQ(r.rd(FX1_ISP_AWB_ZONE_COUNT_OFFSET), awb_zone0);  // the lease, not the bank being written
   FX1_CHECK_EQ(r.awb_status() & (write_busy | FX1_ISP_AWB_RESULT_STATUS_READ_BANK_MASK), write_busy);

   std::uint32_t last_rows = 0;
   bool probed = false;
   for (std::uint32_t n = 1; n <= H; ++n) {
      r.rows(n - 1, n);
      // Rows that reached the AEC tap so far, from the zone counts.
      std::uint32_t even = 0, odd = 0;
      for (std::uint32_t z = 0; z < 24; ++z) {
         const std::uint32_t cr = r.aec_zone(z, 0, FX1_ISP_AEC_ZONE_COUNT_OFFSET);
         const std::uint32_t cb = r.aec_zone(z, 3, FX1_ISP_AEC_ZONE_COUNT_OFFSET);
         FX1_CHECK(cr == 0 || cr == W / 2);
         FX1_CHECK(cb == 0 || cb == W / 2);
         FX1_CHECK(cb == 0 || cr == W / 2);  // raster order
         if (cr == 0) {
            FX1_CHECK_EQ(r.aec_zone(z, 0, FX1_ISP_AEC_ZONE_GREEN_MIN_MAX_OFFSET), 4095);  // not written yet
         }
         even += cr != 0;
         odd += cb != 0;
      }
      const std::uint32_t rows = even + odd;
      FX1_CHECK(rows >= last_rows && rows <= n);
      FX1_CHECK(rows + 4 >= n);  // line-buffer latency of the blocks before the tap
      last_rows = rows;
      FX1_CHECK_EQ(r.hist(1000 >> 6), even * (W / 2));  // Gr rows so far
      FX1_CHECK_EQ(r.hist(3300 >> 6), odd * (W / 2));   // Gb rows so far
      if (rows >= 5 && !probed) {  // a completed zone reads its final value mid-frame
         FX1_CHECK_EQ(r.aec_zone(0, 0, FX1_ISP_AEC_ZONE_SUM_OFFSET), 100u * (W / 2));
         FX1_CHECK_EQ(r.aec_zone(0, 0, FX1_ISP_AEC_ZONE_GREEN_MIN_MAX_OFFSET), (3300u << 12) | 1000u);
         FX1_CHECK_EQ(r.aec_zone(2, 1, FX1_ISP_AEC_ZONE_SUM_OFFSET), 1000u * (W / 2));
         probed = true;
      }
      FX1_CHECK_EQ(r.awb_zone_count(0), awb_zone0);  // leased bank: stable
      // Global results and IDs keep the previous publication until EOF.
      FX1_CHECK_EQ(r.aec_global_sum(0), 100u * W * H / 4);
      FX1_CHECK_EQ(r.rd(FX1_ISP_AWB_GLOBAL_COUNT_OFFSET), awb_count);
      FX1_CHECK_EQ(r.rd(FX1_ISP_AEC_FRAME_ID_OFFSET), 1);
      FX1_CHECK_EQ(r.rd(FX1_ISP_AEC_STATUS_OFFSET) & FX1_ISP_AEC_STATUS_BUSY_MASK, FX1_ISP_AEC_STATUS_BUSY_MASK);
   }
   FX1_CHECK(probed);
   r.eof();
   FX1_CHECK_EQ(r.awb_zone_count(0), awb_zone0);
   FX1_CHECK_EQ(r.rd(FX1_ISP_AWB_FRAME_ID_OFFSET), 1);
   FX1_CHECK_EQ(r.ready_count(), 1);  // frame 2 waits in the other bank
   r.release();
   FX1_CHECK_EQ(r.rd(FX1_ISP_AEC_STATUS_OFFSET), aec_done);
   FX1_CHECK_EQ(r.rd(FX1_ISP_AEC_FRAME_ID_OFFSET), 2);
   FX1_CHECK_EQ(r.aec_zone(23, 3, FX1_ISP_AEC_ZONE_COUNT_OFFSET), W / 2);
   FX1_CHECK_EQ(r.hist(1000 >> 6), W * H / 4);
   r.acquire();
   FX1_CHECK_EQ(r.rd(FX1_ISP_AWB_FRAME_ID_OFFSET), 2);
   FX1_CHECK_EQ(r.awb_zone_count(0), awb_zone0);
   r.release();

   // A disabled frame leaves the memory untouched (ALG-STAT-06).
   r.wr(FX1_ISP_AEC_CTRL_OFFSET, FX1_ISP_AEC_CTRL_COMMIT_MASK);
   r.frame();
   r.sof();
   r.rows(0, H / 2);
   FX1_CHECK_EQ(r.aec_zone(23, 3, FX1_ISP_AEC_ZONE_COUNT_OFFSET), W / 2);
   r.rows(H / 2, H);
   r.eof();
}

// ALG-STAT-03 / owner query B20: unwritten zones read empty, never data of an
// older frame.
void test_aliasing() {
   rig r;
   r.aec(16, 12, 4, 4, aec_commit);
   r.frame();
   // A15 aliasing probe: zones 8..15 of each row are inside the grid but not
   // visited with 8-pixel zones; they must read empty, not frame-1 data.
   r.aec(16, 12, 8, 4, aec_commit);
   r.frame();
   r.frame();
   FX1_CHECK_EQ(r.aec_zone(7, 0, FX1_ISP_AEC_ZONE_COUNT_OFFSET), 8);
   FX1_CHECK_EQ(r.aec_zone(8, 0, FX1_ISP_AEC_ZONE_COUNT_OFFSET), 0);
   FX1_CHECK_EQ(r.aec_zone(8, 0, FX1_ISP_AEC_ZONE_GREEN_MIN_MAX_OFFSET), 4095);
   FX1_CHECK_EQ(r.aec_zone(16 * 12, 0, FX1_ISP_AEC_ZONE_GREEN_MIN_MAX_OFFSET), 4095);  // beyond grid
   FX1_CHECK_EQ(r.aec_zone(1023, 0, FX1_ISP_AEC_ZONE_SUM_OFFSET), 0);
}

// ALG-AEC-04a, ALG-STAT-07, DEC-14.
void test_resets() {
   rig r;
   // Reset clears the written flags: every zone is untouched, min 4095, max 0
   // (MAS §6.21.4.3, p237; the CSR reset value 0 is superseded).
   FX1_CHECK_EQ(r.aec_zone(0, 0, FX1_ISP_AEC_ZONE_GREEN_MIN_MAX_OFFSET), 4095);
   FX1_CHECK_EQ(r.aec_zone(1023, 0, FX1_ISP_AEC_ZONE_GREEN_MIN_MAX_OFFSET), 4095);
   FX1_CHECK_EQ(r.aec_zone(0, 0, FX1_ISP_AEC_ZONE_SUM_OFFSET), 0);
   r.aec(16, 12, 4, 4, aec_commit);
   r.wr(FX1_ISP_AWB_CTRL_OFFSET, 1 | (4 << 1) | (3 << 8));
   r.wr(FX1_ISP_AF_CTRL_OFFSET, 1);
   r.wr(FX1_ISP_AEC_CONTEXT_ID_OFFSET, 0xA1);
   r.frame();
   FX1_CHECK_EQ(r.aec_zone(1023, 0, FX1_ISP_AEC_ZONE_GREEN_MIN_MAX_OFFSET), 4095);
   FX1_CHECK_EQ(r.rd(FX1_ISP_AEC_RESULT_CONTEXT_ID_OFFSET), 0xA1);
   FX1_CHECK(r.rd(FX1_ISP_COMMON_IRQ_STATUS_OFFSET) & FX1_ISP_COMMON_IRQ_STATUS_STATS_READY_IRQ_MASK);

   r.wr(FX1_ISP_COMMON_CTRL_OFFSET, FX1_ISP_COMMON_CTRL_SOFT_RST_MASK);
   r.cycle += 64;
   FX1_CHECK_EQ(r.rd(FX1_ISP_AEC_STATUS_OFFSET), 0);
   FX1_CHECK_EQ(r.rd(FX1_ISP_AWB_STATUS_OFFSET), 0);
   FX1_CHECK_EQ(r.rd(FX1_ISP_AF_STATUS_OFFSET), 0);
   FX1_CHECK_EQ(r.rd(FX1_ISP_COMMON_IRQ_STATUS_OFFSET), 0);
   FX1_CHECK_EQ(r.aec_global_sum(0), 0);
   FX1_CHECK_EQ(r.aec_zone(0, 0, FX1_ISP_AEC_ZONE_SUM_OFFSET), 0);
   // Untouched again (assumption U2-A1, owner query U0-Q5).
   FX1_CHECK_EQ(r.aec_zone(0, 0, FX1_ISP_AEC_ZONE_GREEN_MIN_MAX_OFFSET), 4095);
   FX1_CHECK_EQ(r.hist(1000 >> 6), 0);
   FX1_CHECK_EQ(r.af(0), 0);
   FX1_CHECK_EQ(r.rd(FX1_ISP_AEC_RESULT_CONTEXT_ID_OFFSET), 0);
   FX1_CHECK_EQ(r.rd(FX1_ISP_AEC_FRAME_ID_OFFSET), 1);  // preserved (DEC-14)
   FX1_CHECK_EQ(r.rd(FX1_ISP_AF_FRAME_ID_OFFSET), 1);
   // AWB: the COMMON soft reset clears ownership and the ready result (MAS
   // p248); nothing to acquire, every result word reads 0.
   FX1_CHECK_EQ(r.awb_status(), 0);
   r.acquire();
   FX1_CHECK(!r.leased());
   FX1_CHECK_EQ(r.rd(FX1_ISP_AWB_GLOBAL_COUNT_OFFSET), 0);
   FX1_CHECK_EQ(r.rd(FX1_ISP_AWB_FRAME_ID_OFFSET), 0);
   FX1_CHECK_EQ(r.rd(FX1_ISP_AEC_CTRL_OFFSET), FX1_ISP_AEC_CTRL_EN_MASK);
   r.frame();  // active set and counter survive: publishes without a new commit
   FX1_CHECK_EQ(r.rd(FX1_ISP_AEC_FRAME_ID_OFFSET), 2);
   FX1_CHECK_EQ(r.take_awb_frame_id(), 2);
   FX1_CHECK_EQ(r.rd(FX1_ISP_AEC_STATUS_OFFSET), aec_done);
   check_flat_aec_frame(r, 16);

   // A pending commit survives a soft reset and is consumed at the next accepted
   // SOF (owner answer Q4; DEC-32 superseded): the 1 x 1 grid applies.
   r.aec(1, 1, 4, 4, aec_commit);
   r.wr(FX1_ISP_COMMON_CTRL_OFFSET, FX1_ISP_COMMON_CTRL_SOFT_RST_MASK);
   r.cycle += 64;
   FX1_CHECK_EQ(r.rd(FX1_ISP_AEC_CTRL_OFFSET), aec_commit);
   r.frame();
   FX1_CHECK_EQ(r.rd(FX1_ISP_AEC_CTRL_OFFSET), FX1_ISP_AEC_CTRL_EN_MASK);  // consumed
   check_flat_aec_frame(r, 1);

   r.cu.external_reset();
   FX1_CHECK_EQ(r.rd(FX1_ISP_AEC_FRAME_ID_OFFSET), 0);
   FX1_CHECK_EQ(r.aec_zone(0, 0, FX1_ISP_AEC_ZONE_GREEN_MIN_MAX_OFFSET), 4095);
   FX1_CHECK_EQ(r.awb_status(), 0);
   r.wr(FX1_ISP_COMMON_FRAME_WIDTH_OFFSET, W);
   r.wr(FX1_ISP_COMMON_FRAME_HEIGHT_OFFSET, H);
   r.wr(FX1_ISP_AEC_CTRL_OFFSET, FX1_ISP_AEC_CTRL_EN_MASK);  // active set was reset: en needs a commit
   r.wr(FX1_ISP_AF_CTRL_OFFSET, 1);
   r.frame();
   FX1_CHECK_EQ(r.rd(FX1_ISP_AEC_STATUS_OFFSET), 0);
   FX1_CHECK_EQ(r.rd(FX1_ISP_AF_FRAME_ID_OFFSET), 1);  // counter restarted
}

// ALG-STAT-04 / §9.5.1: CONTEXT_ID latched at SOF, republished at EOF.
void test_context_latch() {
   rig r;
   r.aec(16, 12, 4, 4, aec_commit);
   r.wr(FX1_ISP_AWB_CTRL_OFFSET, awb_1x1);
   r.wr(FX1_ISP_AF_CTRL_OFFSET, 1);
   r.wr(FX1_ISP_AEC_CONTEXT_ID_OFFSET, 0x11);
   r.wr(FX1_ISP_AWB_CONTEXT_ID_OFFSET, 0x22);
   r.wr(FX1_ISP_AF_CONTEXT_ID_OFFSET, 0x33);
   r.frame([&] {
      r.wr(FX1_ISP_AEC_CONTEXT_ID_OFFSET, 0x44);
      r.wr(FX1_ISP_AWB_CONTEXT_ID_OFFSET, 0x55);
      r.wr(FX1_ISP_AF_CONTEXT_ID_OFFSET, 0x66);
   });
   FX1_CHECK_EQ(r.rd(FX1_ISP_AEC_RESULT_CONTEXT_ID_OFFSET), 0x11);
   FX1_CHECK_EQ(r.rd(FX1_ISP_AWB_RESULT_CONTEXT_ID_OFFSET), 0);  // no lease
   FX1_CHECK_EQ(r.rd(FX1_ISP_AF_RESULT_CONTEXT_ID_OFFSET), 0x33);
   r.frame();
   FX1_CHECK_EQ(r.rd(FX1_ISP_AEC_RESULT_CONTEXT_ID_OFFSET), 0x44);
   FX1_CHECK_EQ(r.rd(FX1_ISP_AF_RESULT_CONTEXT_ID_OFFSET), 0x66);
   // AWB banks in completion order, each with its own tags.
   r.acquire();
   FX1_CHECK_EQ(r.rd(FX1_ISP_AWB_RESULT_CONTEXT_ID_OFFSET), 0x22);
   FX1_CHECK_EQ(r.rd(FX1_ISP_AWB_FRAME_ID_OFFSET), 1);
   r.release();
   r.acquire();
   FX1_CHECK_EQ(r.rd(FX1_ISP_AWB_RESULT_CONTEXT_ID_OFFSET), 0x55);
   FX1_CHECK_EQ(r.rd(FX1_ISP_AWB_FRAME_ID_OFFSET), 2);
   r.release();
}

// AF EN is sampled once per frame as it enters the pipeline (MAS §6.23.2,
// ALG-AF-01/02 superseded): a frame started enabled is measured and published
// even if EN is cleared during it; a frame started disabled is ignored.
void test_af_enable() {
   rig flat(true);
   flat.wr(FX1_ISP_AF_CTRL_OFFSET, 1);
   flat.frame();
   for (std::uint32_t z = 0; z < 16; ++z) {
      FX1_CHECK_EQ(flat.af(z), 0);  // F01: flat luma, replicate borders
   }

   rig r;
   r.wr(FX1_ISP_AF_CTRL_OFFSET, 1);
   r.frame();
   FX1_CHECK_EQ(r.rd(FX1_ISP_AF_STATUS_OFFSET), af_done | af_valid);
   FX1_CHECK_EQ(r.rd(FX1_ISP_AF_FRAME_ID_OFFSET), 1);
   std::uint32_t full[16];
   for (std::uint32_t z = 0; z < 16; ++z) {
      full[z] = r.af(z);
   }
   FX1_CHECK(full[0] > 0);  // Gr != Gb leaves a luma texture after demosaic
   r.wr(FX1_ISP_AF_STATUS_OFFSET, af_done);  // W1C
   FX1_CHECK_EQ(r.rd(FX1_ISP_AF_STATUS_OFFSET), af_valid);

   r.frame([&] { r.wr(FX1_ISP_AF_CTRL_OFFSET, 0); });  // cleared mid-frame: still measured
   FX1_CHECK_EQ(r.rd(FX1_ISP_AF_STATUS_OFFSET), af_done | af_valid);
   FX1_CHECK_EQ(r.rd(FX1_ISP_AF_FRAME_ID_OFFSET), 2);
   for (std::uint32_t z = 0; z < 16; ++z) {
      FX1_CHECK_EQ(r.af(z), full[z]);
   }
   r.wr(FX1_ISP_AF_STATUS_OFFSET, af_done);

   r.frame([&] { r.wr(FX1_ISP_AF_CTRL_OFFSET, 1); });  // set mid-frame: ignored completely
   FX1_CHECK_EQ(r.rd(FX1_ISP_AF_STATUS_OFFSET), af_valid);
   FX1_CHECK_EQ(r.rd(FX1_ISP_AF_FRAME_ID_OFFSET), 2);
   FX1_CHECK_EQ(r.af(15), full[15]);
   r.frame();
   FX1_CHECK_EQ(r.rd(FX1_ISP_AF_STATUS_OFFSET), af_done | af_valid);
   FX1_CHECK_EQ(r.rd(FX1_ISP_AF_FRAME_ID_OFFSET), 4);
   FX1_CHECK_EQ(r.af(0), full[0]);
   FX1_CHECK_EQ(r.rd(FX1_ISP_AF_STATUS_OFFSET) & FX1_ISP_AF_STATUS_BUSY_MASK, 0);
}

// ALG-STAT-05: the AWB set is sampled at SOF.
void test_awb_sampled_at_sof() {
   auto setup = [](rig &r) {
      r.wr(FX1_ISP_AWB_UNDEREXPOSED_LIMIT_OFFSET, 50);
      r.wr(FX1_ISP_AWB_SATURATION_LIMIT_OFFSET, 4050);
      r.wr(FX1_ISP_AWB_CTRL_OFFSET, 1 | (4 << 1) | (3 << 8));
   };
   rig a, b;
   setup(a);
   setup(b);
   a.frame();
   b.frame([&] {
      b.wr(FX1_ISP_AWB_CTRL_OFFSET, 0);
      b.wr(FX1_ISP_AWB_UNDEREXPOSED_LIMIT_OFFSET, 3000);  // would reject every pixel if live
      b.wr(FX1_ISP_AWB_SATURATION_LIMIT_OFFSET, 4095);
   });
   FX1_CHECK_EQ(b.rd(FX1_ISP_AWB_STATUS_OFFSET), FX1_ISP_AWB_STATUS_STAT_DONE_MASK);
   a.acquire();
   b.acquire();
   for (std::uint32_t off = FX1_ISP_AWB_GLOBAL_SUM_R_H_OFFSET; off <= FX1_ISP_AWB_GLOBAL_COUNT_OFFSET; off += 4) {
      FX1_CHECK_EQ(b.rd(off), a.rd(off));
   }
   for (std::uint32_t z = 0; z < 12; ++z) {
      a.wr(FX1_ISP_AWB_ZONE_ADDR_OFFSET, z);
      b.wr(FX1_ISP_AWB_ZONE_ADDR_OFFSET, z);
      FX1_CHECK_EQ(b.rd(FX1_ISP_AWB_ZONE_COUNT_OFFSET), a.rd(FX1_ISP_AWB_ZONE_COUNT_OFFSET));
      FX1_CHECK_EQ(b.rd(FX1_ISP_AWB_ZONE_SUM_G_L_OFFSET), a.rd(FX1_ISP_AWB_ZONE_SUM_G_L_OFFSET));
   }
   FX1_CHECK(a.rd(FX1_ISP_AWB_GLOBAL_COUNT_OFFSET) > 0);
   b.frame();  // disabled at this SOF: no measurement, no loss, the lease untouched
   FX1_CHECK_EQ(b.rd(FX1_ISP_AWB_FRAME_ID_OFFSET), 1);
   FX1_CHECK_EQ(b.rd(FX1_ISP_AWB_GLOBAL_COUNT_OFFSET), a.rd(FX1_ISP_AWB_GLOBAL_COUNT_OFFSET));
   FX1_CHECK_EQ(b.ready_count(), 0);
   FX1_CHECK_EQ(b.drops(), 0);
   FX1_CHECK_EQ(b.awb_status() & overrun, 0);
}

// ALG-STAT-01/09/11: one ID per started frame; an aborted frame consumes an
// ID and publishes nothing; stats_ready_irq per publication.
void test_frame_id_abort_irq() {
   rig r;
   r.aec(16, 12, 4, 4, aec_commit);
   r.wr(FX1_ISP_AWB_CTRL_OFFSET, awb_1x1);
   r.wr(FX1_ISP_AF_CTRL_OFFSET, 1);
   r.wr(FX1_ISP_COMMON_IRQ_EN_OFFSET, FX1_ISP_COMMON_IRQ_EN_STATS_READY_EN_MASK);
   FX1_CHECK(!r.cu.irq_level());
   r.frame();
   for (std::uint32_t off : {FX1_ISP_AEC_FRAME_ID_OFFSET, FX1_ISP_AF_FRAME_ID_OFFSET}) {
      FX1_CHECK_EQ(r.rd(off), 1);
   }
   FX1_CHECK_EQ(r.take_awb_frame_id(), 1);
   FX1_CHECK(r.cu.irq_level());
   r.wr(FX1_ISP_COMMON_IRQ_STATUS_OFFSET, FX1_ISP_COMMON_IRQ_STATUS_STATS_READY_IRQ_MASK);
   FX1_CHECK(!r.cu.irq_level());

   r.sof();  // aborted: the engine calls stats_abort() instead of end_of_frame
   r.rows(0, H / 2);
   r.cu.stats_abort();
   FX1_CHECK_EQ(r.rd(FX1_ISP_AEC_STATUS_OFFSET), aec_done);  // busy cleared, old done kept
   FX1_CHECK_EQ(r.rd(FX1_ISP_AF_STATUS_OFFSET) & FX1_ISP_AF_STATUS_BUSY_MASK, 0);
   FX1_CHECK_EQ(r.rd(FX1_ISP_AEC_FRAME_ID_OFFSET), 1);
   // Single memory: the aborted frame's rows are what the zones now hold.
   FX1_CHECK_EQ(r.aec_zone(0, 0, FX1_ISP_AEC_ZONE_SUM_OFFSET), 400);
   FX1_CHECK_EQ(r.aec_zone(16 * 11, 0, FX1_ISP_AEC_ZONE_GREEN_MIN_MAX_OFFSET), 4095);  // bottom row not reached
   FX1_CHECK(!r.cu.irq_level());
   FX1_CHECK_EQ(r.awb_status(), 0);  // the aborted AWB bank was freed, nothing published
   FX1_CHECK(r.cu.awb_bank_state(0) == awb_bank::state::free && r.cu.awb_bank_state(1) == awb_bank::state::free);
   r.frame();
   for (std::uint32_t off : {FX1_ISP_AEC_FRAME_ID_OFFSET, FX1_ISP_AF_FRAME_ID_OFFSET}) {
      FX1_CHECK_EQ(r.rd(off), 3);  // skips the aborted frame's ID
   }
   FX1_CHECK_EQ(r.take_awb_frame_id(), 3);
   FX1_CHECK(r.cu.irq_level());  // done bits were never cleared: the event still interrupts
}

// Accumulators wrap; the published registers carry the CSR field widths
// (HAS Table 6-71, p176; A12 4K all-4095 global sum).
void test_readout_widths() {
   control_unit cu;
   cu.csr_write(FX1_ISP_AF_CTRL_OFFSET, 1, 0xFFFFFFFFu, 1);
   cu.csr_write(FX1_ISP_AEC_ZONE_CFG_OFFSET, 1 | (1 << 8), 0xFFFFFFFFu, 1);
   cu.csr_write(FX1_ISP_AEC_CTRL_OFFSET, aec_commit, 0xFFFFFFFFu, 1);
   cu.csr_write(FX1_ISP_AWB_CTRL_OFFSET, 1 | (1 << 1) | (1 << 8), 0xFFFFFFFFu, 1);
   cu.accepted_sof(2);
   aec_result a;
   a.gsum[0] = 4095ull * (3840 * 2160 / 4);  // one channel of a 4K all-4095 frame: 8 491 392 000
   a.gcnt[0] = (1u << 21) + 7;
   aec_memory &am = cu.aec_zone_memory();  // 1x1 grid restarted at the SOF above
   FX1_CHECK_EQ(am.zsum.size(), 1);
   am.zsum[0][0] = (1u << 28) + 9;
   am.zcnt[0][0] = 65536 + 3;
   am.oe[0] = 65536 + 0x20;     // bits above 16 must not reach the neighbouring field
   am.ue[0] = (3u << 16) + 2;
   am.gmin[0] = 17;
   am.gmax[0] = 4000;
   am.hist[0] = (1u << 22) + 11;
   awb_result b;
   b.r = (1ull << 35) + (5ull << 32) + 77;
   b.n = (1u << 23) + 4;
   awb_memory &bm = cu.awb_zone_memory();
   FX1_CHECK_EQ(bm.zsum.size(), 1);
   bm.zsum[0][0] = (1ull << 35) + (6ull << 32) + 78;
   bm.zcnt[0] = (1u << 23) + 5;
   af_result f;
   f.fv[3] = 0xFFFFFFFFu;
   cu.stats_publish(&a, &b, &f);
   cu.csr_write(FX1_ISP_AWB_RESULT_CTRL_OFFSET, FX1_ISP_AWB_RESULT_CTRL_ACQUIRE_MASK, 0xFFFFFFFFu, 3);
   FX1_CHECK_EQ(cu.csr_read(FX1_ISP_AEC_GLOBAL_SUM_HI_OFFSET), 1);
   FX1_CHECK_EQ(cu.csr_read(FX1_ISP_AEC_GLOBAL_SUM_LO_OFFSET), 4196424704u);
   FX1_CHECK_EQ(cu.csr_read(FX1_ISP_AEC_GLOBAL_COUNT_OFFSET), 7);
   FX1_CHECK_EQ(cu.csr_read(FX1_ISP_AEC_ZONE_SUM_OFFSET), 9);
   FX1_CHECK_EQ(cu.csr_read(FX1_ISP_AEC_ZONE_COUNT_OFFSET), 3);
   FX1_CHECK_EQ(cu.csr_read(FX1_ISP_AEC_ZONE_GREEN_OE_UE_OFFSET), (0x20u << 16) | 2u);
   FX1_CHECK_EQ(cu.csr_read(FX1_ISP_AEC_ZONE_GREEN_MIN_MAX_OFFSET), (4000u << 12) | 17u);
   FX1_CHECK_EQ(cu.csr_read(FX1_ISP_AEC_HIST_DATA_OFFSET), 11);
   FX1_CHECK_EQ(cu.csr_read(FX1_ISP_AWB_GLOBAL_SUM_R_H_OFFSET), 5);
   FX1_CHECK_EQ(cu.csr_read(FX1_ISP_AWB_GLOBAL_SUM_R_L_OFFSET), 77);
   FX1_CHECK_EQ(cu.csr_read(FX1_ISP_AWB_GLOBAL_COUNT_OFFSET), 4);
   FX1_CHECK_EQ(cu.csr_read(FX1_ISP_AWB_ZONE_SUM_R_H_OFFSET), 6);
   FX1_CHECK_EQ(cu.csr_read(FX1_ISP_AWB_ZONE_SUM_R_L_OFFSET), 78);
   FX1_CHECK_EQ(cu.csr_read(FX1_ISP_AWB_ZONE_COUNT_OFFSET), 5);
   cu.csr_write(FX1_ISP_AF_STAT_ADDR_OFFSET, 3, 0xFFFFFFFFu, 4);
   FX1_CHECK_EQ(cu.csr_read(FX1_ISP_AF_STAT_DATA_OFFSET), 0xFFFFFFFFu);
   // Writes to published results are ignored (RO).
   cu.csr_write(FX1_ISP_AWB_GLOBAL_COUNT_OFFSET, 0x123, 0xFFFFFFFFu, 5);
   FX1_CHECK_EQ(cu.csr_read(FX1_ISP_AWB_GLOBAL_COUNT_OFFSET), 4);
}

// DEC-38 block busy bits; DEC-39 / M5-A1 warnings for requested features that
// are not modelled.
void test_block_busy_and_warnings() {
   control_unit cu;
   const std::uint32_t regs[] = {FX1_ISP_DEMOSAIC_STATUS_OFFSET, FX1_ISP_CCM_STATUS_OFFSET, FX1_ISP_GAMMA_STATUS_OFFSET,
                                 FX1_ISP_GTM_STATUS_OFFSET,      FX1_ISP_NR_2D_STATUS_OFFSET, FX1_ISP_EE_STATUS_OFFSET,
                                 FX1_ISP_CNF_STATUS_OFFSET,      FX1_ISP_RESIZER_STATUS_OFFSET};
   cu.blocks_busy(true);
   for (std::uint32_t r : regs) {
      FX1_CHECK_EQ(cu.csr_read(r) & 1u, 1);
      cu.csr_write(r, 0xFFFFFFFFu, 0xFFFFFFFFu, 2);  // RO
      FX1_CHECK_EQ(cu.csr_read(r) & 1u, 1);
   }
   cu.csr_write(FX1_ISP_COMMON_CTRL_OFFSET, FX1_ISP_COMMON_CTRL_SOFT_RST_MASK, 0xFFFFFFFFu, 3);
   for (std::uint32_t r : regs) {
      FX1_CHECK_EQ(cu.csr_read(r) & 1u, 0);  // soft reset: no frame in flight
   }
   cu.blocks_busy(true);
   cu.blocks_busy(false);
   FX1_CHECK_EQ(cu.csr_read(FX1_ISP_CNF_STATUS_OFFSET), 0);

   auto warnings = [](std::uint32_t off, std::uint32_t v) {
      control_unit c;
      c.csr_write(FX1_ISP_COMMON_FRAME_WIDTH_OFFSET, W, 0xFFFFFFFFu, 1);
      c.csr_write(FX1_ISP_COMMON_FRAME_HEIGHT_OFFSET, H, 0xFFFFFFFFu, 2);
      if (off) {
         c.csr_write(off, v, 0xFFFFFFFFu, 3);
      }
      c.accepted_sof(4);
      const pipe::pipeline_config cfg = pipe::snapshot_config(c);
      pipe::isp_pipeline p;
      std::vector<std::string> w;
      p.begin(cfg, W, H, [](std::vector<std::uint8_t> &&) {}, [](std::vector<std::uint8_t> &&) {},
              [&](const std::string &m) { w.push_back(m); });
      return w;
   };
   FX1_CHECK_EQ(warnings(0, 0).size(), 0);
   const auto ofmt = warnings(FX1_ISP_OFMT_CTRL_OFFSET, FX1_ISP_OFMT_CTRL_STRIDE_EN_MASK);
   FX1_CHECK(ofmt.size() == 1 && ofmt[0].find("DEC-39") != std::string::npos);
   const auto wdr = warnings(FX1_ISP_D_WDR_CTRL_OFFSET, FX1_ISP_D_WDR_CTRL_EN_MASK);
   FX1_CHECK(wdr.size() == 1 && wdr[0].find("M5-A1") != std::string::npos);
   const auto tnr = warnings(FX1_ISP_TNR_3D_CTRL_OFFSET, FX1_ISP_TNR_3D_CTRL_EN_MASK);
   FX1_CHECK(tnr.size() == 1 && tnr[0].find("M5-A1") != std::string::npos);
}

// AWB-FULL and queue order (MAS §6.22.4.2, §6.22.5.5): with one bank leased
// and the other READY, enabled frames are skipped whole, each counted once;
// video keeps running; admission resumes after RELEASE; READY banks are
// acquired oldest first.
void test_awb_full_and_order() {
   rig r;
   r.wr(FX1_ISP_AWB_CTRL_OFFSET, awb_1x1);
   r.wr(FX1_ISP_AF_CTRL_OFFSET, 1);
   r.frame();  // frame 1 -> bank 0
   r.acquire();
   FX1_CHECK(r.leased());
   FX1_CHECK_EQ(r.awb_status() & FX1_ISP_AWB_RESULT_STATUS_READ_BANK_MASK, 0);
   const std::uint32_t count1 = r.rd(FX1_ISP_AWB_GLOBAL_COUNT_OFFSET);
   FX1_CHECK(count1 > 0);
   r.frame();  // frame 2 -> bank 1
   FX1_CHECK_EQ(r.ready_count(), 1);
   FX1_CHECK_EQ(r.awb_status() & overrun, 0);
   r.frame();  // frames 3 and 4: no FREE bank
   r.frame();
   FX1_CHECK_EQ(r.drops(), 2);
   FX1_CHECK_EQ(r.awb_status() & overrun, overrun);
   FX1_CHECK_EQ(r.ready_count(), 1);
   FX1_CHECK_EQ(r.rd(FX1_ISP_AF_FRAME_ID_OFFSET), 4);  // the other statistics and video continued
   FX1_CHECK_EQ(r.rd(FX1_ISP_AWB_FRAME_ID_OFFSET), 1);  // the lease is never written
   FX1_CHECK_EQ(r.rd(FX1_ISP_AWB_GLOBAL_COUNT_OFFSET), count1);
   r.release();
   FX1_CHECK(!r.leased());
   r.frame();  // frame 5 -> bank 0 again
   FX1_CHECK_EQ(r.ready_count(), 2);
   FX1_CHECK_EQ(r.take_awb_frame_id(), 2);  // completion order
   FX1_CHECK_EQ(r.take_awb_frame_id(), 5);
   FX1_CHECK_EQ(r.ready_count(), 0);
   FX1_CHECK_EQ(r.drops(), 2);  // reporting is independent of ownership
}

// AWB-COMMAND (MAS §6.22.4.3): no-ops, RELEASE priority, byte strobes,
// command reads, one clock after ACQUIRE, following-edge eligibility.
void test_awb_commands() {
   rig r;
   const std::uint32_t acq = FX1_ISP_AWB_RESULT_CTRL_ACQUIRE_MASK, rel = FX1_ISP_AWB_RESULT_CTRL_RELEASE_MASK;
   const std::uint32_t valid = FX1_ISP_AWB_RESULT_STATUS_READ_VALID_MASK | FX1_ISP_AWB_RESULT_STATUS_LEASED_MASK;
   r.wr(FX1_ISP_AWB_CTRL_OFFSET, awb_1x1);
   r.acquire();  // nothing READY
   r.release();  // no lease
   FX1_CHECK_EQ(r.awb_status(), 0);
   r.frame();
   r.cu.csr_write(FX1_ISP_AWB_RESULT_CTRL_OFFSET, acq, 0xFFFFFF00u, r.cycle++);  // byte 0 not strobed
   FX1_CHECK(!r.leased());
   r.awb_cmd(acq | rel);  // RELEASE wins; no implicit acquisition
   FX1_CHECK(!r.leased());
   FX1_CHECK_EQ(r.ready_count(), 1);
   // ACQUIRE: owned and readable from the next edge (user decision 2026-10-09).
   const std::uint64_t c = r.cycle;
   r.cu.csr_write(FX1_ISP_AWB_RESULT_CTRL_OFFSET, acq, 0xFFFFFFFFu, c);
   FX1_CHECK_EQ(r.cu.csr_read(FX1_ISP_AWB_RESULT_CTRL_OFFSET, c), 0);  // commands read 0
   FX1_CHECK_EQ(r.cu.csr_read(FX1_ISP_AWB_RESULT_STATUS_OFFSET, c) & valid, 0);
   FX1_CHECK_EQ(r.cu.csr_read(FX1_ISP_AWB_GLOBAL_COUNT_OFFSET, c), 0);
   FX1_CHECK_EQ(r.cu.csr_read(FX1_ISP_AWB_FRAME_ID_OFFSET, c), 0);
   FX1_CHECK_EQ(r.cu.csr_read(FX1_ISP_AWB_RESULT_STATUS_OFFSET, c + 1) & valid, valid);
   FX1_CHECK(r.cu.csr_read(FX1_ISP_AWB_GLOBAL_COUNT_OFFSET, c + 1) > 0);
   FX1_CHECK_EQ(r.cu.csr_read(FX1_ISP_AWB_FRAME_ID_OFFSET, c + 1), 1);
   r.cycle = c + 1;
   // The zone read port is synchronous: on the edge of the AWB_ZONE_ADDR
   // write the data is still the previous address's (MAS §6.22.4.3).
   const std::uint32_t zone0 = r.cu.csr_read(FX1_ISP_AWB_ZONE_COUNT_OFFSET, r.cycle);
   FX1_CHECK(zone0 > 0);
   const std::uint64_t za = r.cycle++;
   r.cu.csr_write(FX1_ISP_AWB_ZONE_ADDR_OFFSET, 1, 0xFFFFFFFFu, za);  // beyond the 1 x 1 grid: reads 0
   FX1_CHECK_EQ(r.cu.csr_read(FX1_ISP_AWB_ZONE_COUNT_OFFSET, za), zone0);
   FX1_CHECK_EQ(r.cu.csr_read(FX1_ISP_AWB_ZONE_COUNT_OFFSET, za + 1), 0);
   r.cu.csr_write(FX1_ISP_AWB_ZONE_ADDR_OFFSET, 0, 0xFFFFFFFFu, r.cycle);
   FX1_CHECK_EQ(r.cu.csr_read(FX1_ISP_AWB_ZONE_COUNT_OFFSET, r.cycle), 0);
   ++r.cycle;
   FX1_CHECK_EQ(r.cu.csr_read(FX1_ISP_AWB_ZONE_COUNT_OFFSET, r.cycle), zone0);
   // Grid counts of the leased bank, not of the live configuration.
   r.wr(FX1_ISP_AWB_CTRL_OFFSET, 1 | (8 << 1) | (6 << 8));
   FX1_CHECK_EQ((r.awb_status() & FX1_ISP_AWB_RESULT_STATUS_READ_NUM_ZONE_X_MASK) >>
                    FX1_ISP_AWB_RESULT_STATUS_READ_NUM_ZONE_X_SHIFT, 1);
   FX1_CHECK_EQ(r.awb_zone_count(1), 0);  // outside the captured 1 x 1 grid
   // A second ACQUIRE while leased is a no-op.
   r.frame();  // frame 2 -> bank 1
   r.acquire();
   FX1_CHECK_EQ(r.rd(FX1_ISP_AWB_FRAME_ID_OFFSET), 1);
   FX1_CHECK_EQ(r.ready_count(), 1);
   r.release();
   r.release();  // no lease: no-op
   FX1_CHECK_EQ(r.ready_count(), 1);
   FX1_CHECK_EQ(r.take_awb_frame_id(), 2);
   // A bank published on an ACQUIRE edge is acquirable from the next edge.
   r.sof();
   r.rows(0, H);
   r.p.finish();
   const std::uint64_t e = r.cycle;
   r.cu.advance_to(e);
   pipe::end_of_frame(r.cu, r.cfg, r.p.stats());
   r.cu.csr_write(FX1_ISP_AWB_RESULT_CTRL_OFFSET, acq, 0xFFFFFFFFu, e);
   FX1_CHECK(!r.leased());
   FX1_CHECK_EQ(r.ready_count(), 1);
   r.cycle = e + 1;
   r.acquire();
   FX1_CHECK(r.leased());
   // A bank released on an SOF edge is eligible from the next edge: with the
   // other bank READY, that SOF is skipped.
   r.frame();  // bank 1 READY (bank 0 leased)
   const std::uint64_t s0 = r.cycle;
   r.cu.csr_write(FX1_ISP_AWB_RESULT_CTRL_OFFSET, rel, 0xFFFFFFFFu, s0);
   const std::uint32_t drops = r.drops();
   r.cu.accepted_sof(s0);
   FX1_CHECK_EQ(r.drops(), drops + 1);
   FX1_CHECK_EQ(r.awb_status() & write_busy, 0);
   r.cycle = s0 + 1;
   r.cu.stats_abort();
   r.frame();  // bank 0 is eligible again
   FX1_CHECK_EQ(r.drops(), drops + 1);
   FX1_CHECK_EQ(r.ready_count(), 2);
}

// AWB-REPORT (MAS §6.22.4.3): CLEAR_REPORT clears OVERRUN and DROP_COUNT only;
// a loss on the same edge survives it; STAT_DONE clear releases nothing.
void test_awb_report() {
   rig r;
   const std::uint32_t clear = FX1_ISP_AWB_RESULT_CTRL_CLEAR_REPORT_MASK;
   r.wr(FX1_ISP_AWB_CTRL_OFFSET, awb_1x1);
   r.frame();
   r.acquire();
   r.frame();
   r.frame();  // skipped
   FX1_CHECK_EQ(r.drops(), 1);
   r.wr(FX1_ISP_AWB_STATUS_OFFSET, FX1_ISP_AWB_STATUS_STAT_DONE_MASK);  // W1C: no bank released
   FX1_CHECK(r.leased());
   FX1_CHECK_EQ(r.ready_count(), 1);
   r.awb_cmd(clear);
   FX1_CHECK_EQ(r.drops(), 0);
   FX1_CHECK_EQ(r.awb_status() & overrun, 0);
   FX1_CHECK(r.leased());  // reporting cleared, data kept
   FX1_CHECK_EQ(r.ready_count(), 1);
   FX1_CHECK_EQ(r.rd(FX1_ISP_AWB_FRAME_ID_OFFSET), 1);
   // Clear first, then a loss on the same edge: OVERRUN set, count 1.
   const std::uint64_t c = r.cycle;
   r.cu.accepted_sof(c);  // skipped: no FREE bank
   r.cu.csr_write(FX1_ISP_AWB_RESULT_CTRL_OFFSET, clear, 0xFFFFFFFFu, c);
   FX1_CHECK_EQ(r.drops(), 1);
   FX1_CHECK_EQ(r.awb_status() & overrun, overrun);
   r.cycle = c + 1;
   r.cu.stats_abort();
   r.awb_cmd(clear);
   FX1_CHECK_EQ(r.drops(), 0);
}

// AWB-GRID (MAS §6.22.3.7): an illegal grid is not accumulated or published;
// it is reported; disabled frames do not count.
void test_awb_grid() {
   const struct {
      std::uint32_t nx, ny;
      bool legal;
   } cases[] = {{0, 1, false}, {1, 0, false}, {64, 32, true}, {65, 1, false}, {1, 33, false}, {W, H, false}};
   for (const auto &k : cases) {
      rig r;
      const std::string ctx = std::to_string(k.nx) + " x " + std::to_string(k.ny);
      r.wr(FX1_ISP_AWB_CTRL_OFFSET, 1 | ((k.nx & 0x7F) << 1) | ((k.ny & 0x3F) << 8));
      r.frame();
      FX1_CHECK_EQ_CTX(r.ready_count(), k.legal ? 1u : 0u, ctx);
      FX1_CHECK_EQ_CTX(r.drops(), k.legal ? 0u : 1u, ctx);
      FX1_CHECK_EQ_CTX(r.rd(FX1_ISP_AWB_STATUS_OFFSET), k.legal ? 1u : 0u, ctx + " STAT_DONE");
      r.wr(FX1_ISP_AWB_CTRL_OFFSET, 0);
      r.frame();
      FX1_CHECK_EQ_CTX(r.drops(), k.legal ? 0u : 1u, ctx + " disabled frame");
   }
   // 64 x 32 is legal on a 64 x 48 image, not on an image narrower than 64.
   rig n;
   n.wr(FX1_ISP_COMMON_FRAME_WIDTH_OFFSET, 32);
   n.wr(FX1_ISP_AWB_CTRL_OFFSET, 1 | (64 << 1) | (32 << 8));
   n.cu.accepted_sof(n.cycle++);
   FX1_CHECK_EQ(n.drops(), 1);
}

// AWB-RESET (MAS p248): the DMA-only soft reset aborts the measurement in
// flight and keeps READY/LEASED banks and reporting; the COMMON soft reset and
// i_rst_n clear ownership and reporting.
void test_awb_reset_scopes() {
   const auto setup = [](rig &r) {
      r.wr(FX1_ISP_AWB_CTRL_OFFSET, awb_1x1);
      r.frame();  // frame 1 -> bank 0, leased
      r.acquire();
      r.frame();  // frame 2 -> bank 1, READY
      r.frame();  // frame 3 skipped
      r.sof();    // frame 4 skipped as well; nothing written
      r.release();
      r.cu.stats_abort();
      r.frame();  // frame 5 -> bank 0 READY
      r.acquire();  // lease frame 2
      r.sof();    // frame 6 -> nothing free (bank 0 READY, bank 1 leased)
      r.rows(0, H / 2);
   };
   {
      rig r;
      setup(r);
      FX1_CHECK_EQ(r.drops(), 3);
      r.wr(FX1_ISP_DMA_CTRL_OFFSET, FX1_ISP_DMA_CTRL_SOFT_RESET_MASK);
      r.cycle += 64;
      FX1_CHECK(r.leased());
      FX1_CHECK_EQ(r.rd(FX1_ISP_AWB_FRAME_ID_OFFSET), 2);
      FX1_CHECK_EQ(r.ready_count(), 1);
      FX1_CHECK_EQ(r.drops(), 3);
      FX1_CHECK_EQ(r.awb_status() & overrun, overrun);
      r.release();
      FX1_CHECK_EQ(r.take_awb_frame_id(), 5);
   }
   {
      // The DMA reset aborts a measurement being written: its bank is freed.
      rig r;
      r.wr(FX1_ISP_AWB_CTRL_OFFSET, awb_1x1);
      r.sof();
      r.rows(0, H / 2);
      FX1_CHECK_EQ(r.awb_status() & write_busy, write_busy);
      r.wr(FX1_ISP_DMA_CTRL_OFFSET, FX1_ISP_DMA_CTRL_SOFT_RESET_MASK);
      r.cycle += 64;
      FX1_CHECK_EQ(r.awb_status(), 0);
      FX1_CHECK(r.cu.awb_bank_state(0) == awb_bank::state::free);
   }
   {
      rig r;
      setup(r);
      r.wr(FX1_ISP_COMMON_CTRL_OFFSET, FX1_ISP_COMMON_CTRL_SOFT_RST_MASK);
      r.cycle += 64;
      FX1_CHECK_EQ(r.awb_status(), 0);
      FX1_CHECK_EQ(r.drops(), 0);
      FX1_CHECK_EQ(r.rd(FX1_ISP_AWB_FRAME_ID_OFFSET), 0);
      r.frame();
      FX1_CHECK_EQ(r.take_awb_frame_id(), 7);  // the frame counter is kept
   }
   {
      rig r;
      setup(r);
      r.cu.external_reset();
      FX1_CHECK_EQ(r.awb_status(), 0);
      FX1_CHECK_EQ(r.drops(), 0);
   }
}

// BPC counters (MAS §6.9): cleared at every accepted SOF, counted in an
// enabled frame; DYNAMIC_DET_EN has no effect (DEC-28 superseded).
void test_bpc_counters() {
   rig r;  // the flat channels leave the 5 x 5 same-colour neighbourhood flat
   r.raw[std::size_t{20} * W + 20] = 4095;  // one hot R pixel
   r.wr(FX1_ISP_BPC_THRESH_OFFSET, 100);    // floor 100, k 0
   r.wr(FX1_ISP_BPC_CTRL_OFFSET, 1);
   r.frame();
   const std::uint32_t cand = r.rd(FX1_ISP_BPC_NUM_CANDIDATES_OFFSET);
   FX1_CHECK(cand >= 1);
   FX1_CHECK_EQ(r.rd(FX1_ISP_BPC_NUM_DEFECTIVE_OFFSET), cand);  // k = 0: thresholds coincide
   r.wr(FX1_ISP_BPC_MODE_OFFSET, FX1_ISP_BPC_MODE_DYNAMIC_DET_EN_MASK);
   r.frame();
   FX1_CHECK_EQ(r.rd(FX1_ISP_BPC_NUM_CANDIDATES_OFFSET), cand);  // no effect
   r.sof();
   FX1_CHECK_EQ(r.rd(FX1_ISP_BPC_NUM_CANDIDATES_OFFSET), 0);  // cleared at SOF
   r.rows(0, H);
   r.eof();
   FX1_CHECK_EQ(r.rd(FX1_ISP_BPC_NUM_CANDIDATES_OFFSET), cand);
   r.wr(FX1_ISP_BPC_CTRL_OFFSET, 0);
   r.frame();  // disabled: cleared at SOF, not counted (assumption, owner query U0-Q4)
   FX1_CHECK_EQ(r.rd(FX1_ISP_BPC_NUM_CANDIDATES_OFFSET), 0);
   FX1_CHECK_EQ(r.rd(FX1_ISP_BPC_NUM_DEFECTIVE_OFFSET), 0);
}

}  // namespace

int main() {
   test_block_busy_and_warnings();
   test_aec_commit();
   test_single_memory_partial_reads();
   test_aliasing();
   test_resets();
   test_context_latch();
   test_af_enable();
   test_awb_sampled_at_sof();
   test_frame_id_abort_irq();
   test_readout_widths();
   test_awb_full_and_order();
   test_awb_commands();
   test_awb_report();
   test_awb_grid();
   test_awb_reset_scopes();
   test_bpc_counters();
   return fx1_test::summary("fx1_isp_test_stats");
}
