// SPDX-License-Identifier: Apache-2.0
// Register-level behaviour of the FX1 ISP that does not depend on the DMA
// engines or the pixel pipeline (milestone M1): reset, soft reset,
// interrupt/error/statistics aggregation, LUT ports and derived geometry.
// Commands whose behaviour belongs to later milestones are delivered to
// pluggable block hooks. No SystemC dependency; time is a core-clock cycle.
#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <map>
#include <vector>

#include "control/ee_types.h"
#include "control/stats_types.h"
#include "pipeline/gtm_curve.h"
#include "registers/register_file.h"

namespace cdc::components::fx1_isp {

struct geometry {
   std::uint32_t width;
   std::uint32_t height;
};

// Resizer configuration (MAS §6.20.4.3): the key {active W, H, mode, enable}
// and the complete set published for it (output size, scaling or pass-through).
struct resizer_key {
   bool valid = false;  // false: the reset set, derived from no key
   std::uint32_t width = 0, height = 0, scale = 0;
   bool en = false;
   bool operator==(const resizer_key &o) const {
      return valid == o.valid && width == o.width && height == o.height && scale == o.scale && en == o.en;
   }
   bool operator!=(const resizer_key &o) const { return !(*this == o); }
};
struct resizer_set {
   resizer_key key;
   bool scaling = false;
   geometry out{1920, 1080};  // reset value of RESIZER_OUT_W/H
};

class control_unit {
public:
   using block_hook = std::function<void(const register_file::write_effect &, std::uint64_t cycle)>;
   using soft_reset_hook = std::function<void(std::uint64_t cycle)>;

   // A command written by software whose behaviour is implemented by a later
   // milestone. Recorded when no block hook is installed for that register.
   struct command_record {
      std::uint64_t cycle;
      register_file::write_effect effect;
   };

   // Scope of a soft reset (MAS Table 183, p316-317).
   enum class reset_scope {
      common,  // COMMON_CTRL.soft_rst: pixel pipeline and both DMA engines
      dma,     // DMA_CTRL.soft_reset: DMA engines and the frame in flight only
   };

   // 15-core-clock soft-reset pulse (MAS §6.5.3 p33).
   explicit control_unit(unsigned soft_reset_cycles = 15);

   // i_rst_n asserted: registers to reset values, LUTs to their documented
   // power-up contents (the poison pattern, MAS §6.5.2), soft-reset window cancelled.
   void external_reset();
   // i_rst_n released at `cycle`: FRAME_COUNTER starts counting from zero.
   void release_reset(std::uint64_t cycle) { time_base_epoch_ = cycle; }

   // FRAME_COUNTER (MAS Table 160): free-running 32-bit core-clock count since
   // the release of i_rst_n, computed on demand (no per-clock event). It reads
   // zero while i_rst_n is asserted; soft resets do not touch it (VP decision
   // U5-A1).
   std::uint32_t frame_counter(std::uint64_t cycle) const {
      return time_base_epoch_ == settled || cycle < time_base_epoch_
                 ? 0u
                 : static_cast<std::uint32_t>(cycle - time_base_epoch_);
   }

   // CSR port. A read at `cycle` sees hardware state of that edge (the AWB
   // lease becomes readable one core clock after ACQUIRE, MAS §6.22.4.3); the
   // overload without a cycle returns the settled state, as a debugger would.
   std::uint32_t csr_read(std::uint32_t addr) const { return regs_.sw_read(addr); }
   std::uint32_t csr_read(std::uint32_t addr, std::uint64_t cycle) const {
      read_cycle_ = cycle;
      const std::uint32_t v = regs_.sw_read(addr);
      read_cycle_ = settled;
      return v;
   }
   void csr_write(std::uint32_t addr, std::uint32_t data, std::uint32_t lane_mask,
                  std::uint64_t cycle);

   // Level of the o_irq output (HAS Table 9-2).
   bool irq_level() const;

   // Soft reset (MAS §6.5.3, Table 183; decisions §9). The register effects
   // are applied at the write; the engines stay in reset until
   // `soft_reset_end_cycle()`. A rewrite during the pulse restarts it.
   bool soft_reset_active(std::uint64_t cycle) const { return cycle < soft_reset_end_; }
   std::uint64_t soft_reset_end_cycle() const { return soft_reset_end_; }
   // Time has reached `cycle`: the following hardware events (publication,
   // abort) happen on that edge; lifts the command block once the reset
   // window has elapsed.
   void advance_to(std::uint64_t cycle);

   // Pipeline geometry after the Input Formatter phase crop (HAS Table 6-17)
   // and the output geometry derived by the Resizer (HAS §6.20.10).
   geometry active_geometry() const;
   // Published Resizer set (MAS §6.20.4.3): what RESIZER_OUT_W/H report and
   // the next accepted SOF uses. Derived from a stable key in E0-E35 (35
   // clocks); pass-through publishes on the next edge; during a derivation the
   // previous set stays published; a key changed during it is derived again.
   // `cycle` is the observation edge; without one, the settled set.
   resizer_set resizer_published(std::uint64_t cycle) const;
   geometry resizer_output_geometry() const { return resizer_published(settled).out; }
   // The set the frame in flight uses, sampled at its accepted SOF.
   const resizer_set &resizer_frame() const { return rz_frame_; }
   static constexpr unsigned resizer_derivation_cycles = 35;  // E0 .. E35
   // The same derivation from explicit values (frame snapshot at SOF).
   static geometry derive_active(std::uint32_t width, std::uint32_t height, std::uint32_t bayer_pattern);
   static geometry derive_output(const geometry &active, std::uint32_t resizer_ctrl);

   // Hardware events (used by the DMA/pipeline models and by tests).
   void hw_set(std::uint32_t offset, std::uint32_t mask, std::uint64_t cycle);
   void hw_clear(std::uint32_t offset, std::uint32_t mask) { regs_.hw_clear(offset, mask); }
   void hw_write(std::uint32_t offset, std::uint32_t mask, std::uint32_t value) {
      regs_.hw_write(offset, mask, value);
   }
   void hw_hold(std::uint32_t offset, std::uint32_t mask, bool level, std::uint64_t cycle);
   // Accepted SOF: clears the pending frame_start (DEC-16), frame_done (DEC-17),
   // and performs pending commits of the gated configuration sets (DEC-26).
   void accepted_sof(std::uint64_t cycle);

   // Value of a configuration register as the frame starting now samples it.
   // No register is gated by an `updated` bit since CSR/MAS 07/10 (DEC-24/26/27
   // superseded), so this is the live value.
   std::uint32_t committed(std::uint32_t offset) const { return regs_.peek(offset); }

   void set_block_hook(std::uint32_t offset, block_hook hook);
   void set_soft_reset_hook(soft_reset_hook hook) { soft_reset_hook_ = std::move(hook); }
   const std::vector<command_record> &commands() const { return commands_; }
   void clear_commands() { commands_.clear(); }

   register_file &regs() { return regs_; }
   const register_file &regs() const { return regs_; }

   // Table contents (live LUTs read by the pipeline in M3).
   const std::array<std::uint16_t, 256> &gamma_lut() const { return gamma_lut_; }
   // EE resident profiles (MAS §6.18.4.1): tables of a profile (live: a write
   // reaches the rows processed after it), the profile programmed through
   // EE_CTRL.profile_wr_sel, and the set sampled for the frame in flight.
   static constexpr unsigned ee_profiles = 3;
   const std::array<std::uint16_t, 64> &ee_table(unsigned profile, unsigned bank) const {
      return ee_.at(profile).lut.at(bank);
   }
   const std::array<std::uint16_t, 64> &ee_table(unsigned bank) const { return ee_table(ee_frame_.profile, bank); }
   std::uint32_t ee_profile_scalar(unsigned profile, unsigned k) const { return ee_.at(profile).scalar.at(k); }
   const ee_frame_set &ee_frame() const { return ee_frame_; }
   // End of a frame EE processed: radial gain range event (MAS §6.18.4.1).
   void ee_end_of_frame();
   // GTM tone-curve banks (HAS §6.16.4, Table 6-49; ALG-GTM-05/07/08): the
   // datapath reads the read bank; the generator fills the other one, which
   // becomes the read bank at the next accepted SOF.
   const pipe::gtm_curve &gtm_read_bank() const { return gtm_banks_[gtm_read_]; }
   bool gtm_curve_valid() const { return gtm_valid_; }
   // End of frame: KEY, LWHITE and MANUAL are read now (ALG-GTM-09).
   void gtm_end_of_frame(std::uint32_t yavg);
   // Statistics (HAS §6.21-6.23, §9.5; DEC-29; ALG-STAT-*). Configuration seen
   // by the frame that was just accepted (valid after accepted_sof()).
   const aec_config &aec_active() const { return aec_active_; }
   // AWB set of the frame just accepted; `en` is set only if the measurement
   // was admitted into a FREE bank with a legal grid (MAS §6.22.5.5).
   const awb_config &awb_frame_config() const { return awb_sof_; }
   bool af_frame_enabled() const { return af_en_sof_; }
   std::uint32_t frame_id() const { return frame_id_; }
   // The single in-place AEC zone/histogram memory (DEC-30): restarted at the
   // SOF of an enabled frame, updated by the pipeline row by row.
   aec_memory &aec_zone_memory() { return aec_mem_; }
   void debug_set_frame_counter(std::uint32_t value) { frame_counter_ = value; }  // test fixtures only
   // Zone entries of the AWB bank being written (a scratch memory when no
   // measurement was admitted).
   awb_memory &awb_zone_memory() {
      return awb_writing_ >= 0 ? awb_banks_[static_cast<unsigned>(awb_writing_)].mem : awb_scratch_;
   }
   // AWB bank state, for tests and debuggers.
   awb_bank::state awb_bank_state(unsigned bank) const { return awb_banks_.at(bank).st; }
   // End of frame: publish the results of the blocks that measured the frame.
   void stats_publish(const aec_result *aec, const awb_result *awb, const af_result *af);
   // Frame abandoned before its end: no publication, busy bits cleared.
   void stats_abort();
   // Per-block STATUS.busy of the pixel blocks (Demosaic, CCM, Gamma, GTM,
   // 2DNR, EE, CNF, Resizer): 1 while a frame is inside the pipeline,
   // including while it is stalled (DEC-38).
   void blocks_busy(bool busy);
   // 2DNR noise-variance shadow (HAS §6.17.7.2): measured on frame N, used by N+1.
   std::uint32_t nr_variance() const { return nr_var_; }
   void nr_publish(std::uint32_t var);

   // LSC resident profiles (MAS §6.8.4, CSR 07/10): 2..128 nodes per axis,
   // not above the active image size (DEC-31 superseded).
   static constexpr unsigned lsc_profiles = 3;
   static constexpr std::uint32_t lsc_mesh_max = 128;
   const std::vector<std::uint32_t> &lsc_profile(unsigned idx) const { return lsc_.mesh.at(idx); }
   unsigned lsc_active_profile() const { return lsc_.active; }
   bool lsc_profile_valid(unsigned idx) const { return idx < lsc_profiles && lsc_.valid[idx]; }
   bool lsc_geometry_valid() const;

   static constexpr unsigned ee_bank_entries(unsigned bank) { return bank < 2 ? 64u : 32u; }

private:
   void install_hooks();
   void lsc_load_ctrl(const register_file::write_effect &fx);
   void lsc_coef_data(const register_file::write_effect &fx);
   void lsc_profile_sel(const register_file::write_effect &fx);
   void lsc_error(std::uint32_t mask);
   std::uint32_t lsc_expected_count() const;
   resizer_key resizer_live_key() const;
   void resizer_key_written();
   struct resizer_ctrl {  // configuration controller (MAS Figure 135)
      resizer_key live;             // key of the live registers
      std::uint64_t live_since = 0; // edge from which the live key is stable
      std::uint64_t not_before = 0; // end of a reset pulse: nothing starts earlier
      resizer_set published;
      bool calc = false;            // a derivation is running
      std::uint64_t e0 = 0;
      std::uint64_t free_at = 0;    // edge from which the dividers can start again
      resizer_key calc_key;         // the key saved at E0
      void advance(std::uint64_t t);  // process the edges before t
   };
   void dispatch(const register_file::write_effect &fx, std::uint64_t cycle);
   void apply_soft_reset(std::uint64_t cycle, reset_scope scope);
   void on_hw_set(std::uint32_t offset, std::uint32_t newly);
   std::uint32_t error_flags() const;
   void ee_sof();
   void ee_scalar_write(unsigned k, const register_file::write_effect &fx);
   void stats_install_hooks();
   void stats_reset(bool external);
   void stats_sof();
   void awb_admit();
   void awb_loss();
   void awb_result_ctrl(const register_file::write_effect &fx);
   bool awb_lease_readable() const;
   const awb_bank *awb_read_bank() const;

   register_file regs_;
   unsigned soft_reset_cycles_;
   std::uint64_t soft_reset_end_ = 0;
   std::uint64_t current_cycle_ = 0;
   static constexpr std::uint64_t settled = std::numeric_limits<std::uint64_t>::max();
   mutable std::uint64_t read_cycle_ = settled;  // cycle of the CSR read in progress
   std::uint64_t time_base_epoch_ = settled;     // release of i_rst_n; settled while asserted
   std::map<std::uint32_t, block_hook> block_hooks_;
   soft_reset_hook soft_reset_hook_;
   std::vector<command_record> commands_;

   resizer_ctrl rz_;  // advanced at key writes and SOFs; observations advance a copy
   resizer_set rz_frame_;

   struct lsc_state {
      std::array<std::vector<std::uint32_t>, 3> mesh;  // committed profiles
      std::array<bool, 3> valid{};
      std::vector<std::uint32_t> staging;              // load in progress (ALG-LSC-04)
      bool busy = false;
      std::uint32_t count = 0;                         // >= 17 bits (ALG-LSC-07)
      unsigned dest = 0;
      unsigned active = 0;                             // adopted at SOF (ALG-LSC-06)
   } lsc_;

   std::array<std::uint16_t, 256> gamma_lut_{};
   struct ee_profile {
      std::array<std::uint32_t, ee_num_scalars> scalar{};
      std::array<std::array<std::uint16_t, 64>, 4> lut{};
   };
   std::array<ee_profile, ee_profiles> ee_{};
   ee_frame_set ee_frame_{};
   std::array<pipe::gtm_curve, 2> gtm_banks_{};
   unsigned gtm_read_ = 0;
   bool gtm_valid_ = false;    // a curve exists (hard reset: pass-through, p112)
   bool gtm_pending_ = false;  // a rebuilt bank waits for the next SOF
   std::uint32_t nr_var_ = 0;

   // Statistics state.
   aec_config aec_active_{};
   awb_config awb_sof_{};
   bool af_en_sof_ = false;
   std::uint32_t frame_counter_ = 0;  // frames started since reset (#316)
   std::uint32_t frame_id_ = 0;       // bound to the frame in flight (ALG-STAT-01)
   std::uint32_t aec_ctx_ = 0, awb_ctx_ = 0, af_ctx_ = 0;
   aec_result aec_pub_{};
   af_result af_pub_{};
   aec_memory aec_mem_{};
   bool aec_mem_live_ = false;  // restarted by an enabled SOF since the last reset
   // AWB result banks and loss reporting (MAS §6.22.4, §6.22.5.5).
   std::array<awb_bank, 2> awb_banks_{};
   std::deque<unsigned> awb_ready_;  // READY banks in completion order
   int awb_writing_ = -1;            // bank of the measurement in flight
   int awb_leased_ = -1;
   std::uint64_t awb_lease_cycle_ = 0;
   bool awb_overrun_ = false;
   std::uint32_t awb_drops_ = 0;
   std::uint64_t awb_loss_cycle_ = settled;  // edge of the latest loss and the
   std::uint32_t awb_loss_count_ = 0;        // number of losses on that edge
   awb_memory awb_scratch_{};
   // AWB zone read port, synchronous (MAS §6.22.4.3): on the edge that writes
   // AWB_ZONE_ADDR the data still belongs to the previous address.
   std::uint32_t awb_zone_prev_ = 0;
   std::uint64_t awb_zone_cycle_ = settled;
};

}  // namespace cdc::components::fx1_isp
