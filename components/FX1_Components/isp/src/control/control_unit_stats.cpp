// SPDX-License-Identifier: Apache-2.0
// Statistics publication and readout of the control unit: AEC (HAS §6.21),
// AWB (§6.22), AF (§6.23), §9.5 frame association. Decisions: DEC-14 (soft
// reset), DEC-29/DEC-30 (single in-place AEC zone/histogram memory read with
// the partial sums of the frame in progress; FRAME_ID bound to the frame),
// ALG-AEC-01..06, ALG-AWB-01..04, ALG-AF-01..02, ALG-STAT-01..10.
// CSR/MAS 07/10 (milestone U2): AWB result banks with acquire/release and
// loss reporting (MAS §6.22.4-6.22.5), AF enable sampled at SOF (MAS §6.23.2),
// untouched AEC zones read green minimum 4095 (MAS §6.21.4.3).

#include "control/control_unit.h"
#include "fx1_isp/fx1_isp_csr.h"

namespace cdc::components::fx1_isp {

namespace {

std::uint32_t fld(std::uint32_t reg, std::uint32_t mask, std::uint32_t shift) { return (reg & mask) >> shift; }

}  // namespace

void control_unit::stats_reset(bool external) {
   // Soft reset clears published results and the zone/histogram memories
   // (they read the CSR reset value 0 again until the next enabled SOF) but
   // keeps configuration and the frame counter (DEC-14, ALG-STAT-07); i_rst_n
   // clears everything. The memories keep their size: only their contents
   // are made invisible.
   if (external) {
      aec_active_ = aec_config{};
      awb_sof_ = awb_config{};
      af_en_sof_ = false;
      frame_counter_ = 0;
      frame_id_ = 0;
   }
   aec_pub_ = aec_result{};
   af_pub_ = af_result{};
   aec_mem_live_ = false;
   // AWB: hard and COMMON soft reset clear ownership, ready order, lease,
   // written flags and loss reporting, and cancel the measurement in flight
   // (MAS p248). Payloads read zero while invalid.
   for (awb_bank &b : awb_banks_) {
      b = awb_bank{};
      b.stamp = settled;
   }
   awb_ready_.clear();
   awb_writing_ = awb_leased_ = -1;
   awb_overrun_ = false;
   awb_drops_ = 0;
   awb_loss_cycle_ = settled;
   awb_loss_count_ = 0;
   awb_zone_cycle_ = settled;
}

void control_unit::awb_loss() {
   // One lost enabled measurement: OVERRUN and a saturating DROP_COUNT. Losses
   // on one edge are remembered so a simultaneous CLEAR_REPORT keeps them
   // (MAS §6.22.4.3: clear first, then new events).
   if (awb_loss_cycle_ != current_cycle_) {
      awb_loss_cycle_ = current_cycle_;
      awb_loss_count_ = 0;
   }
   ++awb_loss_count_;
   awb_overrun_ = true;
   if (awb_drops_ != std::numeric_limits<std::uint32_t>::max()) {
      ++awb_drops_;
   }
}

void control_unit::awb_admit() {
   // At an enabled SOF reserve one FREE bank, bank 0 first; a bank freed on
   // this edge is not eligible yet. With an illegal grid or no FREE bank the
   // whole measurement is skipped and reported; the frame ID is consumed and
   // video continues (MAS §6.22.3.7, §6.22.5.5).
   awb_writing_ = -1;
   if (!awb_sof_.en) {
      return;  // disabled frames leave banks and reporting unchanged
   }
   const geometry g = active_geometry();
   const bool legal = g.width >= 1 && g.width <= 3840 && g.height >= 1 && g.height <= 2160 && awb_sof_.nx >= 1 &&
                      awb_sof_.nx <= std::min<std::uint32_t>(64, g.width) && awb_sof_.ny >= 1 &&
                      awb_sof_.ny <= std::min<std::uint32_t>(32, g.height);
   if (legal) {
      for (unsigned i = 0; i < awb_banks_.size(); ++i) {
         awb_bank &b = awb_banks_[i];
         if (b.st == awb_bank::state::free && (b.stamp == settled || b.stamp < current_cycle_)) {
            b.st = awb_bank::state::writing;
            b.stamp = current_cycle_;
            b.mem.restart(awb_sof_);  // clears the written flags of this bank only
            b.global = awb_result{};
            b.frame_id = frame_id_;
            b.context_id = awb_ctx_;
            awb_writing_ = static_cast<int>(i);
            break;
         }
      }
   }
   if (awb_writing_ < 0) {
      awb_loss();
      awb_sof_.en = false;  // the pipeline does not accumulate a skipped measurement
   }
}

bool control_unit::awb_lease_readable() const {
   // The lease is readable from the edge after ACQUIRE (user decision of
   // 2026-10-09: READ_VALID rises one core clock later; reads before return 0).
   return awb_leased_ >= 0 && (read_cycle_ == settled || read_cycle_ > awb_lease_cycle_);
}

const awb_bank *control_unit::awb_read_bank() const {
   return awb_lease_readable() ? &awb_banks_[static_cast<unsigned>(awb_leased_)] : nullptr;
}

void control_unit::awb_result_ctrl(const register_file::write_effect &fx) {
   // One-clock commands; reads return zero (MAS Table 138, §6.22.4.3).
   const std::uint32_t cmd = fx.w1s_set;
   if (cmd & FX1_ISP_AWB_RESULT_CTRL_CLEAR_REPORT_MASK) {
      awb_overrun_ = awb_loss_cycle_ == current_cycle_;
      awb_drops_ = awb_overrun_ ? awb_loss_count_ : 0u;
   }
   if (cmd & FX1_ISP_AWB_RESULT_CTRL_RELEASE_MASK) {
      // RELEASE wins over a simultaneous ACQUIRE; without a lease a no-op.
      if (awb_leased_ >= 0) {
         awb_bank &b = awb_banks_[static_cast<unsigned>(awb_leased_)];
         b.st = awb_bank::state::free;
         b.stamp = current_cycle_;
         awb_leased_ = -1;
      }
   } else if (cmd & FX1_ISP_AWB_RESULT_CTRL_ACQUIRE_MASK) {
      // The oldest READY bank; no-op with a lease or nothing READY. A bank
      // published on this edge is acquirable from the following one.
      if (awb_leased_ < 0 && !awb_ready_.empty() && awb_banks_[awb_ready_.front()].stamp < current_cycle_) {
         const unsigned i = awb_ready_.front();
         awb_ready_.pop_front();
         awb_banks_[i].st = awb_bank::state::leased;
         awb_banks_[i].stamp = current_cycle_;
         awb_leased_ = static_cast<int>(i);
         awb_lease_cycle_ = current_cycle_;
      }
   }
   regs_.hw_clear(FX1_ISP_AWB_RESULT_CTRL_OFFSET, FX1_ISP_AWB_RESULT_CTRL_ACQUIRE_MASK |
                                                      FX1_ISP_AWB_RESULT_CTRL_RELEASE_MASK |
                                                      FX1_ISP_AWB_RESULT_CTRL_CLEAR_REPORT_MASK);
}

void control_unit::stats_sof() {
   frame_id_ = ++frame_counter_;  // counts frames started, bound to this frame (ALG-STAT-01)
   const register_file &r = regs_;
   // AEC: the shadow set is transferred at the SOF after a commit (#296).
   if (r.peek(FX1_ISP_AEC_CTRL_OFFSET) & FX1_ISP_AEC_CTRL_COMMIT_MASK) {
      const std::uint32_t zc = r.peek(FX1_ISP_AEC_ZONE_CFG_OFFSET), zs = r.peek(FX1_ISP_AEC_ZONE_SIZE_OFFSET);
      const std::uint32_t cl = r.peek(FX1_ISP_AEC_SAMPLE_CLIP_OFFSET), th = r.peek(FX1_ISP_AEC_THRESH_OFFSET);
      aec_active_.en = r.peek(FX1_ISP_AEC_CTRL_OFFSET) & FX1_ISP_AEC_CTRL_EN_MASK;
      aec_active_.nx = fld(zc, FX1_ISP_AEC_ZONE_CFG_NUM_ZONE_X_MASK, FX1_ISP_AEC_ZONE_CFG_NUM_ZONE_X_SHIFT);
      aec_active_.ny = fld(zc, FX1_ISP_AEC_ZONE_CFG_NUM_ZONE_Y_MASK, FX1_ISP_AEC_ZONE_CFG_NUM_ZONE_Y_SHIFT);
      aec_active_.zw = fld(zs, FX1_ISP_AEC_ZONE_SIZE_ZONE_WIDTH_MASK, FX1_ISP_AEC_ZONE_SIZE_ZONE_WIDTH_SHIFT);
      aec_active_.zh = fld(zs, FX1_ISP_AEC_ZONE_SIZE_ZONE_HEIGHT_MASK, FX1_ISP_AEC_ZONE_SIZE_ZONE_HEIGHT_SHIFT);
      aec_active_.min_clip = fld(cl, FX1_ISP_AEC_SAMPLE_CLIP_SAMPLE_MIN_CLIP_MASK, FX1_ISP_AEC_SAMPLE_CLIP_SAMPLE_MIN_CLIP_SHIFT);
      aec_active_.max_clip = fld(cl, FX1_ISP_AEC_SAMPLE_CLIP_SAMPLE_MAX_CLIP_MASK, FX1_ISP_AEC_SAMPLE_CLIP_SAMPLE_MAX_CLIP_SHIFT);
      aec_active_.th_ue = fld(th, FX1_ISP_AEC_THRESH_TH_UE_MASK, FX1_ISP_AEC_THRESH_TH_UE_SHIFT);
      aec_active_.th_oe = fld(th, FX1_ISP_AEC_THRESH_TH_OE_MASK, FX1_ISP_AEC_THRESH_TH_OE_SHIFT);
      regs_.hw_clear(FX1_ISP_AEC_CTRL_OFFSET, FX1_ISP_AEC_CTRL_COMMIT_MASK);  // self-clearing
   }
   // AWB: configuration sampled at SOF (HAS §9.4.3, ALG-STAT-05).
   const std::uint32_t ac = r.peek(FX1_ISP_AWB_CTRL_OFFSET);
   awb_sof_.en = ac & FX1_ISP_AWB_CTRL_EN_MASK;
   awb_sof_.nx = fld(ac, FX1_ISP_AWB_CTRL_NUM_ZONE_X_MASK, FX1_ISP_AWB_CTRL_NUM_ZONE_X_SHIFT);
   awb_sof_.ny = fld(ac, FX1_ISP_AWB_CTRL_NUM_ZONE_Y_MASK, FX1_ISP_AWB_CTRL_NUM_ZONE_Y_SHIFT);
   awb_sof_.under = r.peek(FX1_ISP_AWB_UNDEREXPOSED_LIMIT_OFFSET) & FX1_ISP_AWB_UNDEREXPOSED_LIMIT_UNDEREXPOSED_LIMIT_MASK;
   awb_sof_.sat = r.peek(FX1_ISP_AWB_SATURATION_LIMIT_OFFSET) & FX1_ISP_AWB_SATURATION_LIMIT_SATURATION_LIMIT_MASK;
   af_en_sof_ = r.peek(FX1_ISP_AF_CTRL_OFFSET) & FX1_ISP_AF_CTRL_EN_MASK;
   // Context tags latched at SOF (HAS §9.5.1, ALG-STAT-04).
   aec_ctx_ = r.peek(FX1_ISP_AEC_CONTEXT_ID_OFFSET);
   awb_ctx_ = r.peek(FX1_ISP_AWB_CONTEXT_ID_OFFSET);
   af_ctx_ = r.peek(FX1_ISP_AF_CONTEXT_ID_OFFSET);
   awb_admit();
   // Enabled blocks start measuring: busy, and the in-place zone/histogram
   // memory restarts; the pipeline updates it row by row (DEC-30).
   if (aec_active_.en) {
      aec_mem_.restart(aec_active_);
      aec_mem_live_ = true;
      regs_.hw_set(FX1_ISP_AEC_STATUS_OFFSET, FX1_ISP_AEC_STATUS_BUSY_MASK, current_cycle_);
   }
   if (af_en_sof_) {
      regs_.hw_set(FX1_ISP_AF_STATUS_OFFSET, FX1_ISP_AF_STATUS_BUSY_MASK, current_cycle_);
   }
}

void control_unit::blocks_busy(bool busy) {
   // Frame granularity: every pixel block holds pixels from the accepted SOF
   // until the pipeline has emitted the frame's last row; a bypassed block
   // still carries the stream (DEC-38).
   static constexpr std::uint32_t regs[] = {
      FX1_ISP_DEMOSAIC_STATUS_OFFSET, FX1_ISP_CCM_STATUS_OFFSET, FX1_ISP_GAMMA_STATUS_OFFSET,
      FX1_ISP_GTM_STATUS_OFFSET,      FX1_ISP_NR_2D_STATUS_OFFSET, FX1_ISP_EE_STATUS_OFFSET,
      FX1_ISP_CNF_STATUS_OFFSET,      FX1_ISP_RESIZER_STATUS_OFFSET,
   };
   static_assert(FX1_ISP_DEMOSAIC_STATUS_BUSY_MASK == 1u && FX1_ISP_CCM_STATUS_BUSY_MASK == 1u &&
                     FX1_ISP_GAMMA_STATUS_BUSY_MASK == 1u && FX1_ISP_GTM_STATUS_BUSY_MASK == 1u &&
                     FX1_ISP_NR_2D_STATUS_BUSY_MASK == 1u && FX1_ISP_EE_STATUS_BUSY_MASK == 1u &&
                     FX1_ISP_CNF_STATUS_BUSY_MASK == 1u && FX1_ISP_RESIZER_STATUS_BUSY_MASK == 1u,
                 "busy is bit 0 of every block status register");
   for (const std::uint32_t r : regs) {
      if (busy) {
         regs_.hw_set(r, 1u, current_cycle_);
      } else {
         regs_.hw_clear(r, 1u);
      }
   }
}

void control_unit::stats_abort() {
   regs_.hw_clear(FX1_ISP_AEC_STATUS_OFFSET, FX1_ISP_AEC_STATUS_BUSY_MASK);
   regs_.hw_clear(FX1_ISP_AF_STATUS_OFFSET, FX1_ISP_AF_STATUS_BUSY_MASK);
   // The interrupted AWB measurement publishes nothing; its bank is freed.
   // READY and LEASED banks survive (MAS §6.22.5.4-6.22.5.5). The bank is
   // eligible at once: in silicon the next frame's SOF reaches AWB some clocks
   // after the abort, while this loosely-timed model can place both on one
   // edge (assumption U2-A4). The same-edge rule applies to RELEASE.
   if (awb_writing_ >= 0) {
      awb_banks_[static_cast<unsigned>(awb_writing_)].st = awb_bank::state::free;
      awb_banks_[static_cast<unsigned>(awb_writing_)].stamp = settled;
      awb_writing_ = -1;
   }
}

void control_unit::stats_publish(const aec_result *aec, const awb_result *awb, const af_result *af) {
   // Tap order AEC -> AWB -> AF; each publication sets its done bit and
   // raises stats_ready_irq, also when the done bit was still set from an
   // unread earlier frame (HAS Table 9-2, §6.21.6.5, ALG-STAT-11).
   const auto published = [this](std::uint32_t status, std::uint32_t done) {
      hw_set(status, done, current_cycle_);
      regs_.hw_set(FX1_ISP_COMMON_IRQ_STATUS_OFFSET, FX1_ISP_COMMON_IRQ_STATUS_STATS_READY_IRQ_MASK, current_cycle_);
   };
   if (aec) {
      aec_pub_ = *aec;  // the zone/histogram memory is already complete in place
      regs_.hw_write(FX1_ISP_AEC_FRAME_ID_OFFSET, 0xFFFFFFFFu, frame_id_);
      regs_.hw_write(FX1_ISP_AEC_RESULT_CONTEXT_ID_OFFSET, 0xFFFFFFFFu, aec_ctx_);
      regs_.hw_clear(FX1_ISP_AEC_STATUS_OFFSET, FX1_ISP_AEC_STATUS_BUSY_MASK);
      published(FX1_ISP_AEC_STATUS_OFFSET, FX1_ISP_AEC_STATUS_STAT_DONE_MASK);
   }
   if (awb && awb_writing_ >= 0) {
      // The complete bank becomes READY with its global snapshot; the frame
      // and context tags were bound at admission (MAS §6.22.4.2).
      awb_bank &b = awb_banks_[static_cast<unsigned>(awb_writing_)];
      b.global = *awb;
      b.st = awb_bank::state::ready;
      b.stamp = current_cycle_;
      awb_ready_.push_back(static_cast<unsigned>(awb_writing_));
      awb_writing_ = -1;
      published(FX1_ISP_AWB_STATUS_OFFSET, FX1_ISP_AWB_STATUS_STAT_DONE_MASK);
   }
   // AF: EN is sampled once per frame as it enters the pipeline; a frame that
   // started enabled is published even if EN was cleared during it (MAS
   // §6.23.2, ALG-AF-01/02 superseded). SCORE_VALID identifies published
   // scores; with frame sampling every publication is a complete frame
   // (assumption U2-A2, owner query U0-Q3).
   regs_.hw_clear(FX1_ISP_AF_STATUS_OFFSET, FX1_ISP_AF_STATUS_BUSY_MASK);
   if (af && af_en_sof_) {
      af_pub_ = *af;
      regs_.hw_write(FX1_ISP_AF_FRAME_ID_OFFSET, 0xFFFFFFFFu, frame_id_);
      regs_.hw_write(FX1_ISP_AF_RESULT_CONTEXT_ID_OFFSET, 0xFFFFFFFFu, af_ctx_);
      regs_.hw_write(FX1_ISP_AF_STATUS_OFFSET, FX1_ISP_AF_STATUS_SCORE_VALID_MASK,
                     FX1_ISP_AF_STATUS_SCORE_VALID_MASK);
      published(FX1_ISP_AF_STATUS_OFFSET, FX1_ISP_AF_STATUS_FRAME_DONE_MASK);
   }
}

void control_unit::stats_install_hooks() {
   // ---- AEC readout multiplexers (#319-#342) ----
   auto aec_chan = [this] {
      return fld(regs_.peek(FX1_ISP_AEC_CHANNEL_SEL_OFFSET), FX1_ISP_AEC_CHANNEL_SEL_CHANNEL_SEL_MASK,
                 FX1_ISP_AEC_CHANNEL_SEL_CHANNEL_SEL_SHIFT);
   };
   regs_.on_read(FX1_ISP_AEC_GLOBAL_SUM_LO_OFFSET, [this, aec_chan](std::uint32_t) {
      return static_cast<std::uint32_t>(aec_pub_.gsum[aec_chan()]);
   });
   regs_.on_read(FX1_ISP_AEC_GLOBAL_SUM_HI_OFFSET, [this, aec_chan](std::uint32_t) {
      return static_cast<std::uint32_t>((aec_pub_.gsum[aec_chan()] >> 32) & 1u);
   });
   regs_.on_read(FX1_ISP_AEC_GLOBAL_COUNT_OFFSET, [this, aec_chan](std::uint32_t) { return aec_pub_.gcnt[aec_chan()] & FX1_ISP_AEC_GLOBAL_COUNT_GLOBAL_COUNT_MASK; });
   // Zones read the memory as it is now (DEC-30): during an enabled frame a
   // zone already written holds its partial sums, one not yet written the
   // empty value (0, min 4095, max 0), as do addresses beyond the grid and an
   // illegal grid. Before the first enabled SOF after a reset every field
   // reads the CSR reset value 0 (ALG-AEC-03/04).
   auto aec_zone = [this]() -> long {
      const std::uint32_t z = regs_.peek(FX1_ISP_AEC_ZONE_ADDR_OFFSET) & FX1_ISP_AEC_ZONE_ADDR_ZONE_ADDR_MASK;
      if (!aec_mem_live_ || !aec_mem_.zones || z >= aec_mem_.nx * aec_mem_.ny) {
         return -1;
      }
      return static_cast<long>(z);
   };
   regs_.on_read(FX1_ISP_AEC_ZONE_SUM_OFFSET, [this, aec_zone, aec_chan](std::uint32_t) {
      const long z = aec_zone();
      return z < 0 ? 0u : aec_mem_.zsum[static_cast<std::size_t>(z)][aec_chan()] & FX1_ISP_AEC_ZONE_SUM_ZONE_SUM_MASK;
   });
   regs_.on_read(FX1_ISP_AEC_ZONE_COUNT_OFFSET, [this, aec_zone, aec_chan](std::uint32_t) {
      const long z = aec_zone();
      return z < 0 ? 0u : aec_mem_.zcnt[static_cast<std::size_t>(z)][aec_chan()] & FX1_ISP_AEC_ZONE_COUNT_ZONE_COUNT_MASK;
   });
   regs_.on_read(FX1_ISP_AEC_ZONE_GREEN_OE_UE_OFFSET, [this, aec_zone](std::uint32_t) {
      const long z = aec_zone();
      if (z < 0) {
         return 0u;
      }
      const std::size_t i = static_cast<std::size_t>(z);
      return ((aec_mem_.oe[i] & 0xFFFFu) << 16) | (aec_mem_.ue[i] & 0xFFFFu);
   });
   regs_.on_read(FX1_ISP_AEC_ZONE_GREEN_MIN_MAX_OFFSET, [this, aec_zone](std::uint32_t) {
      // Reset clears the written flags, so before the first enabled SOF every
      // zone is untouched: minimum 4095, maximum 0 (MAS §6.21.4.3, p237).
      const long z = aec_zone();
      if (z < 0) {
         return 4095u;  // empty: min 4095, max 0
      }
      return (aec_mem_.gmax[static_cast<std::size_t>(z)] << 12) | aec_mem_.gmin[static_cast<std::size_t>(z)];
   });
   regs_.on_read(FX1_ISP_AEC_HIST_DATA_OFFSET, [this](std::uint32_t) {
      const std::uint32_t b = regs_.peek(FX1_ISP_AEC_HIST_ADDR_OFFSET) & FX1_ISP_AEC_HIST_ADDR_HIST_ADDR_MASK;
      return aec_mem_live_ ? aec_mem_.hist[b] & FX1_ISP_AEC_HIST_DATA_HIST_COUNT_MASK : 0u;
   });

   // ---- AWB readout (#359-#399, MAS §6.22.4): only the leased bank, from
   // the edge after ACQUIRE; every result word reads 0 without a lease ----
   const auto global_word = [this](std::uint32_t off, std::uint64_t awb_result::*sum, bool high) {
      regs_.on_read(off, [this, sum, high](std::uint32_t) -> std::uint32_t {
         const awb_bank *b = awb_read_bank();
         if (!b) {
            return 0u;
         }
         const std::uint64_t v = b->global.*sum;
         return high ? static_cast<std::uint32_t>((v >> 32) & 0x7u) : static_cast<std::uint32_t>(v);
      });
   };
   global_word(FX1_ISP_AWB_GLOBAL_SUM_R_H_OFFSET, &awb_result::r, true);
   global_word(FX1_ISP_AWB_GLOBAL_SUM_R_L_OFFSET, &awb_result::r, false);
   global_word(FX1_ISP_AWB_GLOBAL_SUM_G_H_OFFSET, &awb_result::g, true);
   global_word(FX1_ISP_AWB_GLOBAL_SUM_G_L_OFFSET, &awb_result::g, false);
   global_word(FX1_ISP_AWB_GLOBAL_SUM_B_H_OFFSET, &awb_result::b, true);
   global_word(FX1_ISP_AWB_GLOBAL_SUM_B_L_OFFSET, &awb_result::b, false);
   regs_.on_read(FX1_ISP_AWB_GLOBAL_COUNT_OFFSET, [this](std::uint32_t) -> std::uint32_t {
      const awb_bank *b = awb_read_bank();
      return b ? b->global.n & FX1_ISP_AWB_GLOBAL_COUNT_GLOBAL_COUNT_MASK : 0u;
   });
   regs_.on_read(FX1_ISP_AWB_FRAME_ID_OFFSET, [this](std::uint32_t) -> std::uint32_t {
      const awb_bank *b = awb_read_bank();
      return b ? b->frame_id : 0u;
   });
   regs_.on_read(FX1_ISP_AWB_RESULT_CONTEXT_ID_OFFSET, [this](std::uint32_t) -> std::uint32_t {
      const awb_bank *b = awb_read_bank();
      return b ? b->context_id : 0u;
   });
   // Zones of the captured grid; addresses outside it and untouched zones read
   // 0. The read port is synchronous: a read on the edge of the AWB_ZONE_ADDR
   // write returns the word of the previous address (MAS §6.22.4.3).
   regs_.on_write(FX1_ISP_AWB_ZONE_ADDR_OFFSET, [this](const register_file::write_effect &fx) {
      awb_zone_prev_ = fx.old_value & FX1_ISP_AWB_ZONE_ADDR_ZONE_ADDR_MASK;
      awb_zone_cycle_ = current_cycle_;
   });
   auto awb_zone = [this]() -> std::pair<const awb_bank *, long> {
      const awb_bank *b = awb_read_bank();
      const bool stale = read_cycle_ != settled && awb_zone_cycle_ != settled && read_cycle_ <= awb_zone_cycle_;
      const std::uint32_t z = stale ? awb_zone_prev_
                                    : regs_.peek(FX1_ISP_AWB_ZONE_ADDR_OFFSET) & FX1_ISP_AWB_ZONE_ADDR_ZONE_ADDR_MASK;
      if (!b || !b->mem.zones || z >= b->mem.nx * b->mem.ny) {
         return {nullptr, -1};
      }
      return {b, static_cast<long>(z)};
   };
   const struct {
      std::uint32_t h, l;
      unsigned c;
   } awb_zone_regs[] = {{FX1_ISP_AWB_ZONE_SUM_R_H_OFFSET, FX1_ISP_AWB_ZONE_SUM_R_L_OFFSET, 0},
                        {FX1_ISP_AWB_ZONE_SUM_G_H_OFFSET, FX1_ISP_AWB_ZONE_SUM_G_L_OFFSET, 1},
                        {FX1_ISP_AWB_ZONE_SUM_B_H_OFFSET, FX1_ISP_AWB_ZONE_SUM_B_L_OFFSET, 2}};
   for (const auto &zr : awb_zone_regs) {
      const unsigned c = zr.c;
      regs_.on_read(zr.h, [awb_zone, c](std::uint32_t) {
         const auto [b, z] = awb_zone();
         return b ? static_cast<std::uint32_t>((b->mem.zsum[static_cast<std::size_t>(z)][c] >> 32) & 0x7u) : 0u;
      });
      regs_.on_read(zr.l, [awb_zone, c](std::uint32_t) {
         const auto [b, z] = awb_zone();
         return b ? static_cast<std::uint32_t>(b->mem.zsum[static_cast<std::size_t>(z)][c]) : 0u;
      });
   }
   regs_.on_read(FX1_ISP_AWB_ZONE_COUNT_OFFSET, [awb_zone](std::uint32_t) {
      const auto [b, z] = awb_zone();
      return b ? b->mem.zcnt[static_cast<std::size_t>(z)] & FX1_ISP_AWB_ZONE_COUNT_ZONE_COUNT_MASK : 0u;
   });
   // Ownership status and loss report (MAS Table 138).
   regs_.on_write(FX1_ISP_AWB_RESULT_CTRL_OFFSET,
                  [this](const register_file::write_effect &fx) { awb_result_ctrl(fx); });
   regs_.on_read(FX1_ISP_AWB_RESULT_STATUS_OFFSET, [this](std::uint32_t) -> std::uint32_t {
      const awb_bank *b = awb_read_bank();
      std::uint32_t v = static_cast<std::uint32_t>(awb_ready_.size()) << FX1_ISP_AWB_RESULT_STATUS_READY_COUNT_SHIFT;
      if (b) {
         v |= FX1_ISP_AWB_RESULT_STATUS_LEASED_MASK | FX1_ISP_AWB_RESULT_STATUS_READ_VALID_MASK;
         v |= static_cast<std::uint32_t>(awb_leased_) << FX1_ISP_AWB_RESULT_STATUS_READ_BANK_SHIFT;
         v |= (b->mem.nx << FX1_ISP_AWB_RESULT_STATUS_READ_NUM_ZONE_X_SHIFT) & FX1_ISP_AWB_RESULT_STATUS_READ_NUM_ZONE_X_MASK;
         v |= (b->mem.ny << FX1_ISP_AWB_RESULT_STATUS_READ_NUM_ZONE_Y_SHIFT) & FX1_ISP_AWB_RESULT_STATUS_READ_NUM_ZONE_Y_MASK;
      }
      v |= awb_writing_ >= 0 ? FX1_ISP_AWB_RESULT_STATUS_WRITE_BUSY_MASK : 0u;
      v |= awb_overrun_ ? FX1_ISP_AWB_RESULT_STATUS_OVERRUN_MASK : 0u;
      return v;
   });
   regs_.on_read(FX1_ISP_AWB_DROP_COUNT_OFFSET, [this](std::uint32_t) { return awb_drops_; });

   // ---- AF focus scores (#413-#416): latched registers ----
   regs_.on_read(FX1_ISP_AF_STAT_DATA_OFFSET, [this](std::uint32_t) {
      return af_pub_.fv[regs_.peek(FX1_ISP_AF_STAT_ADDR_OFFSET) & FX1_ISP_AF_STAT_ADDR_ZONE_ADDR_MASK];
   });
}

}  // namespace cdc::components::fx1_isp
