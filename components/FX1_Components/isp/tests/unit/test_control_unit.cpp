// SPDX-License-Identifier: Apache-2.0
// Unit tests of the FX1 ISP register file and control unit (M1, no SystemC).
//
// Reset values in `literal_resets` are copied by hand from the CSR
// spreadsheet, independently of tools/gen_csr.py, so a generator defect cannot
// hide behind the table it produced. The generic sweeps use the generated
// table only to know which bits carry which access type, and check behaviour.

#include <cstdint>
#include <string>
#include <vector>

#include "control/control_unit.h"
#include "pipeline/isp_pipeline.h"
#include "fx1_check.h"
#include "fx1_isp/fx1_isp_csr.h"

using namespace cdc::components::fx1_isp;

namespace {

constexpr std::uint32_t all_lanes = 0xFFFFFFFFu;

std::uint32_t rd(const control_unit &cu, std::uint32_t off) { return cu.csr_read(off); }
void wr(control_unit &cu, std::uint32_t off, std::uint32_t v, std::uint64_t cycle = 1,
        std::uint32_t lanes = all_lanes) {
   cu.csr_write(off, v, lanes, cycle);
}

bool has_read_hook(std::uint32_t off) {
   return off == FX1_ISP_COMMON_STATUS_OFFSET || off == FX1_ISP_GAMMA_LUT_RDATA_OFFSET ||
          off == FX1_ISP_EE_LUT_RDATA_OFFSET || off == FX1_ISP_GTM_LUT_RDATA_OFFSET ||
          off == FX1_ISP_RESIZER_OUT_W_OFFSET || off == FX1_ISP_RESIZER_OUT_H_OFFSET ||
          // Untouched AEC zones read min 4095 (MAS §6.21.4.3), not the CSR reset value 0.
          off == FX1_ISP_AEC_ZONE_GREEN_MIN_MAX_OFFSET ||
          off == FX1_ISP_FRAME_COUNTER_OFFSET;  // core-clock count (test_time_base)
}

// Registers whose writes have block side effects (covered by dedicated tests).
bool triggers_soft_reset(std::uint32_t off) {
   return off == FX1_ISP_COMMON_CTRL_OFFSET || off == FX1_ISP_DMA_CTRL_OFFSET ||
          off == FX1_ISP_LSC_LOAD_CTRL_OFFSET || off == FX1_ISP_LSC_PROFILE_SEL_OFFSET ||
          off == FX1_ISP_AWB_RESULT_CTRL_OFFSET;  // one-clock commands, read 0 (test_stats)
}

void test_table_shape() {
   FX1_CHECK_EQ(csr::num_registers, 254);  // CSR revision of 2026-10-07
   std::size_t fields = 0;
   for (std::size_t i = 0; i < csr::num_registers; ++i) {
      fields += csr::registers[i].num_fields;
   }
   FX1_CHECK_EQ(fields, 336);
}

void test_literal_resets() {
   struct lit {
      std::uint32_t off;
      std::uint32_t value;
   };
   const lit literal_resets[] = {
      {0x0000, 0x18072026}, {0x0004, 0x00010002}, {0x0008, 0x00000000}, {0x0018, 0x00000780},
      {0x001C, 0x00000438}, {0x0020, 0x00000000}, {0x0028, 0x00000000}, {0x0204, 0x00000100},
      {0x0208, 0x000000C8}, {0x021C, 0x000000C8}, {0x0258, 0x00010000}, {0x0264, 0x00010000},
      {0x0404, 0x00000000}, {0x0604, 0x00000001}, {0x0608, 0x00000100}, {0x060C, 0x004D0192},
      {0x0610, 0x000004B0}, {0x0804, 0x00000100}, {0x0808, 0x00000100}, {0x080C, 0x00000100},
      {0x0A04, 0x00000100}, {0x1004, 0x00000200}, {0x1008, 0x00000000}, {0x1014, 0x00000200},
      {0x1024, 0x00000200}, {0x1400, 0x00000000}, {0x160C, 0x0000002E}, {0x1610, 0x00000400},
      {0x1614, 0x0000000B}, {0x1C04, 0x00004000}, {0x1C08, 0x00002000}, {0x1C0C, 0x00001810},
      {0x1C24, 0x80008000}, {0x1C28, 0x80008000}, {0x2014, 0x00000780}, {0x2018, 0x00000438},
      {0x2208, 0x00001820}, {0x220C, 0x005A0078}, {0x2210, 0x0FFF0000}, {0x2214, 0x0F000100},
      {0x2458, 0x00000FFF}, {0x2604, 0x0000FFFF}, {0x2804, 0x00000F00}, {0x2808, 0x00000F00},
      {0x3000, 0x00003F00}, {0x3020, 0x00001E00}, {0x3080, 0x00000F00}, {0x3084, 0x00000F00},
      {0x30DC, 0x00000000},
   };
   control_unit cu;
   for (const lit &l : literal_resets) {
      FX1_CHECK_EQ_CTX(rd(cu, l.off), l.value, "reset @" + fx1_test::hex(l.off));
   }
}

// Every register: reset readback, all-ones write, all-zeros write, against the
// access type of every bit.
void test_access_sweep() {
   for (std::size_t i = 0; i < csr::num_registers; ++i) {
      const csr::reg_desc &d = csr::registers[i];
      if (has_read_hook(d.offset) || triggers_soft_reset(d.offset)) {
         continue;
      }
      control_unit cu;
      const std::string ctx = d.name;
      const std::uint32_t rst = d.reset & ~d.w1sc_mask;
      FX1_CHECK_EQ_CTX(rd(cu, d.offset), rst, ctx + " reset");
      wr(cu, d.offset, 0xFFFFFFFFu);
      const std::uint32_t after_ones = (rst & ~d.rw_mask) | d.rw_mask | d.w1s_mask;
      FX1_CHECK_EQ_CTX(rd(cu, d.offset), after_ones, ctx + " after 0xFFFFFFFF");
      wr(cu, d.offset, 0);
      const std::uint32_t after_zeros = (rst & ~d.rw_mask) | d.w1s_mask;
      FX1_CHECK_EQ_CTX(rd(cu, d.offset), after_zeros, ctx + " after 0x0");
   }
}

void test_unmapped_and_reserved() {
   control_unit cu;
   std::vector<std::uint32_t> before;
   for (std::size_t i = 0; i < csr::num_registers; ++i) {
      before.push_back(cu.regs().peek(csr::registers[i].offset));
   }
   std::size_t unmapped = 0;
   for (std::uint32_t off = 0; off < 0x10000u; off += 4) {
      if (cu.regs().find(off)) {
         continue;
      }
      ++unmapped;
      wr(cu, off, 0xFFFFFFFFu);
      FX1_CHECK_EQ_CTX(rd(cu, off), 0, "unmapped @" + fx1_test::hex(off));
   }
   FX1_CHECK_EQ(unmapped, 16384 - 254);
   for (std::size_t i = 0; i < csr::num_registers; ++i) {
      FX1_CHECK_EQ_CTX(cu.regs().peek(csr::registers[i].offset), before[i],
                       std::string("unmapped writes changed ") + csr::registers[i].name);
   }
   // Reserved bits read zero after an all-ones write (BLC_GAIN_SEL: [1:0], [8]).
   wr(cu, FX1_ISP_BLC_GAIN_SEL_OFFSET, 0xFFFFFFFFu);
   FX1_CHECK_EQ(rd(cu, FX1_ISP_BLC_GAIN_SEL_OFFSET), 0x103);
   // Read-only word ignores writes.
   wr(cu, FX1_ISP_COMMON_VER_ID_OFFSET, 0);
   FX1_CHECK_EQ(rd(cu, FX1_ISP_COMMON_VER_ID_OFFSET), 0x00010002);
}

void test_strobes_and_alias() {
   control_unit cu;
   const std::uint32_t s = FX1_ISP_COMMON_SCRATCH_OFFSET;
   wr(cu, s, 0xAABBCCDDu, 1, 0x0000FF00u);
   FX1_CHECK_EQ(rd(cu, s), 0x0000CC00);
   wr(cu, s, 0x11223344u, 1, 0xFF000000u);
   FX1_CHECK_EQ(rd(cu, s), 0x1100CC00);
   wr(cu, s, 0xFFFFFFFFu, 1, 0);  // no strobes: accepted, changes nothing
   FX1_CHECK_EQ(rd(cu, s), 0x1100CC00);
   // Address bits [1:0] are ignored (HAS Table 7-10).
   wr(cu, s + 3, 0x12345678u);
   FX1_CHECK_EQ(rd(cu, s + 1), 0x12345678);
   // A strobe on a lane holding only W1C bits of a status register.
   cu.hw_set(FX1_ISP_DMA_ERR_OFFSET, 0x1F, 1);
   wr(cu, FX1_ISP_DMA_ERR_OFFSET, 0xFFFFFFFFu, 2, 0xFFFFFF00u);
   FX1_CHECK_EQ(rd(cu, FX1_ISP_DMA_ERR_OFFSET), 0x1F);
}

void test_w1c_semantics() {
   control_unit cu;
   const std::uint32_t e = FX1_ISP_DMA_ERR_OFFSET;
   cu.hw_set(e, 0x15, 10);
   FX1_CHECK_EQ(rd(cu, e), 0x15);
   wr(cu, e, 0x04, 11);  // per-bit clear
   FX1_CHECK_EQ(rd(cu, e), 0x11);
   // Hardware set wins over a software clear in the same cycle.
   cu.hw_set(e, 0x02, 20);
   wr(cu, e, 0x02, 20);
   FX1_CHECK_EQ(rd(cu, e), 0x13);
   wr(cu, e, 0x02, 21);
   FX1_CHECK_EQ(rd(cu, e), 0x11);
   // Software clear first, hardware set later in the same cycle: set survives.
   wr(cu, e, 0x01, 30);
   cu.hw_set(e, 0x01, 30);
   FX1_CHECK_EQ(rd(cu, e), 0x11);
   // Reserved bit 5 is tied to zero.
   cu.hw_set(e, 0x20, 31);
   FX1_CHECK_EQ(rd(cu, e) & 0x20, 0);
}

void test_level_hold() {
   control_unit cu;
   const std::uint32_t e = FX1_ISP_DMA_ERR_OFFSET;
   const std::uint32_t underrun = FX1_ISP_DMA_ERR_ERR_IDMA_UNDERRUN_BIT;
   cu.hw_hold(e, underrun, true, 5);
   wr(cu, e, underrun, 6);
   FX1_CHECK_EQ(rd(cu, e) & underrun, underrun);  // persists while the condition holds
   cu.hw_hold(e, underrun, false, 7);
   FX1_CHECK_EQ(rd(cu, e) & underrun, underrun);  // still sticky
   wr(cu, e, underrun, 8);
   FX1_CHECK_EQ(rd(cu, e) & underrun, 0);
}

void test_irq_aggregation() {
   control_unit cu;
   const std::uint32_t st = FX1_ISP_COMMON_IRQ_STATUS_OFFSET;
   const std::uint32_t en = FX1_ISP_COMMON_IRQ_EN_OFFSET;
   FX1_CHECK(!cu.irq_level());
   cu.hw_set(st, FX1_ISP_COMMON_IRQ_STATUS_FRAME_DONE_IRQ_MASK, 1);
   FX1_CHECK_EQ(rd(cu, st), 1);  // latched while masked
   FX1_CHECK(!cu.irq_level());
   wr(cu, en, FX1_ISP_COMMON_IRQ_EN_FRAME_DONE_EN_MASK, 2);
   FX1_CHECK(cu.irq_level());
   // DMA group: seven sources, bit 0 is CFG_ERROR since the 07/10 map (SPEC-05 superseded).
   cu.hw_set(FX1_ISP_DMA_IRQ_STAT_OFFSET, 0x7F, 3);
   FX1_CHECK_EQ(rd(cu, FX1_ISP_DMA_IRQ_STAT_OFFSET), 0x7F);
   wr(cu, FX1_ISP_DMA_IRQ_EN_OFFSET, 0x7F, 3);
   FX1_CHECK_EQ(rd(cu, FX1_ISP_DMA_IRQ_EN_OFFSET), 0x7F);
   wr(cu, st, 1, 4);
   FX1_CHECK(cu.irq_level());  // DMA group still pending
   wr(cu, FX1_ISP_DMA_IRQ_STAT_OFFSET, 0x3E, 5);
   FX1_CHECK(cu.irq_level());  // bits 6 and 0 remain
   wr(cu, FX1_ISP_DMA_IRQ_STAT_OFFSET, 0x40, 6);
   FX1_CHECK(cu.irq_level());  // CFG_ERROR remains
   wr(cu, FX1_ISP_DMA_IRQ_STAT_OFFSET, FX1_ISP_DMA_IRQ_STAT_IRQ_CFG_ERROR_BIT, 6);
   FX1_CHECK(!cu.irq_level());
   // Unmasking a latched bit raises the line; masking it drops it.
   cu.hw_set(FX1_ISP_DMA_IRQ_STAT_OFFSET, FX1_ISP_DMA_IRQ_STAT_IRQ_ODMA_DONE_BIT, 7);
   wr(cu, FX1_ISP_DMA_IRQ_EN_OFFSET, 0, 8);
   FX1_CHECK(!cu.irq_level());
   wr(cu, FX1_ISP_DMA_IRQ_EN_OFFSET, FX1_ISP_DMA_IRQ_EN_IRQ_EN_ODMA_DONE_BIT, 9);
   FX1_CHECK(cu.irq_level());
}

void test_error_aggregation() {
   struct src {
      std::uint32_t off;
      std::uint32_t bit;
   };
   const src sources[] = {
      {FX1_ISP_LSC_ERROR_OFFSET, FX1_ISP_LSC_ERROR_ACTIVE_LOAD_REJECT_MASK},
      {FX1_ISP_LSC_ERROR_OFFSET, FX1_ISP_LSC_ERROR_LOAD_PROTOCOL_MASK},
      {FX1_ISP_BPC_STATUS_OFFSET, FX1_ISP_BPC_STATUS_CAND_REJECT_OVF_MASK},
      {FX1_ISP_EE_STATUS_OFFSET, FX1_ISP_EE_STATUS_ERROR_MASK},
      {FX1_ISP_DMA_ERR_OFFSET, FX1_ISP_DMA_ERR_ERR_ALIGN_OR_GEOMETRY_BIT},
      {FX1_ISP_DMA_ERR_OFFSET, FX1_ISP_DMA_ERR_ERR_IDMA_UNDERRUN_BIT},
      {FX1_ISP_DMA_ERR_OFFSET, FX1_ISP_DMA_ERR_ERR_ODMA_OVERFLOW_BIT},
      {FX1_ISP_DMA_ERR_OFFSET, FX1_ISP_DMA_ERR_ERR_IDMA_AXI_BIT},
      {FX1_ISP_DMA_ERR_OFFSET, FX1_ISP_DMA_ERR_ERR_ODMA_AXI_BIT},
   };
   const std::uint32_t err_irq = FX1_ISP_COMMON_IRQ_STATUS_ERROR_IRQ_MASK;
   const std::uint32_t err_st = FX1_ISP_COMMON_STATUS_ERROR_MASK;
   for (const src &s : sources) {
      control_unit cu;
      const std::string ctx = fx1_test::hex(s.off) + "/" + fx1_test::hex(s.bit);
      cu.hw_set(s.off, s.bit, 10);
      FX1_CHECK_EQ_CTX(rd(cu, FX1_ISP_COMMON_IRQ_STATUS_OFFSET) & err_irq, err_irq, ctx);
      FX1_CHECK_EQ_CTX(rd(cu, FX1_ISP_COMMON_STATUS_OFFSET) & err_st, err_st, ctx);
      wr(cu, s.off, s.bit, 11);
      FX1_CHECK_EQ_CTX(rd(cu, FX1_ISP_COMMON_STATUS_OFFSET) & err_st, 0, ctx + " status follows flags");
      FX1_CHECK_EQ_CTX(rd(cu, FX1_ISP_COMMON_IRQ_STATUS_OFFSET) & err_irq, err_irq, ctx + " irq sticky");
   }
   // Underrun is level held: error_irq on the rising edge only (CSR-06).
   control_unit cu;
   const std::uint32_t u = FX1_ISP_DMA_ERR_ERR_IDMA_UNDERRUN_BIT;
   cu.hw_hold(FX1_ISP_DMA_ERR_OFFSET, u, true, 1);
   wr(cu, FX1_ISP_COMMON_IRQ_STATUS_OFFSET, err_irq, 2);
   cu.hw_hold(FX1_ISP_DMA_ERR_OFFSET, u, true, 3);  // condition persists
   FX1_CHECK_EQ(rd(cu, FX1_ISP_COMMON_IRQ_STATUS_OFFSET) & err_irq, 0);
   FX1_CHECK_EQ(rd(cu, FX1_ISP_COMMON_STATUS_OFFSET) & err_st, err_st);
   // Non-error status (a W1C statistics done flag) does not raise error_irq.
   control_unit cu2;
   cu2.hw_set(FX1_ISP_AF_STATUS_OFFSET, FX1_ISP_AF_STATUS_FRAME_DONE_MASK, 1);
   FX1_CHECK_EQ(rd(cu2, FX1_ISP_COMMON_IRQ_STATUS_OFFSET) & err_irq, 0);
}

void test_stats_ready() {
   const std::uint32_t sources[][2] = {
      {FX1_ISP_AEC_STATUS_OFFSET, FX1_ISP_AEC_STATUS_STAT_DONE_MASK},
      {FX1_ISP_AWB_STATUS_OFFSET, FX1_ISP_AWB_STATUS_STAT_DONE_MASK},
      {FX1_ISP_AF_STATUS_OFFSET, FX1_ISP_AF_STATUS_FRAME_DONE_MASK},
   };
   for (const auto &s : sources) {
      control_unit cu;
      cu.hw_set(s[0], s[1], 1);
      FX1_CHECK_EQ(rd(cu, FX1_ISP_COMMON_IRQ_STATUS_OFFSET), FX1_ISP_COMMON_IRQ_STATUS_STATS_READY_IRQ_MASK);
   }
}

// Soft reset scopes (MAS Table 183 p316-317, §6.5.3; decisions §9).
void test_soft_reset(std::uint32_t reg, std::uint32_t bit) {
   control_unit cu;
   const bool common = reg == FX1_ISP_COMMON_CTRL_OFFSET;
   const std::string ctx = common ? "COMMON soft_rst" : "DMA soft_reset";
   // Configuration.
   wr(cu, FX1_ISP_COMMON_SCRATCH_OFFSET, 0x55AA55AAu);
   wr(cu, FX1_ISP_BLC_OFS_G0_DFT_OFFSET, 0x40);
   wr(cu, FX1_ISP_DMA_CTRL_OFFSET, 0x3F03);  // IDMA_EN | ODMA_EN
   wr(cu, FX1_ISP_COMMON_IRQ_EN_OFFSET, 0x7);
   // Ownership, commands, status, results.
   wr(cu, FX1_ISP_IDMA_BUF_VALID_OFFSET, 0xF);
   wr(cu, FX1_ISP_ODMA_BUF_FREE_OFFSET, 0xF);
   wr(cu, FX1_ISP_COMMON_CTRL_OFFSET, FX1_ISP_COMMON_CTRL_FRAME_START_MASK | FX1_ISP_COMMON_CTRL_ISP_EN_MASK);
   wr(cu, FX1_ISP_AF_CTRL_OFFSET, FX1_ISP_AF_CTRL_SEARCH_START_MASK);
   wr(cu, FX1_ISP_AEC_CTRL_OFFSET, FX1_ISP_AEC_CTRL_COMMIT_MASK);  // pending until the next accepted SOF
   cu.hw_set(FX1_ISP_ODMA_BUF_DONE_OFFSET, 0x3, 2);
   cu.hw_set(FX1_ISP_DMA_IRQ_STAT_OFFSET, 0x7F, 2);
   cu.hw_hold(FX1_ISP_DMA_ERR_OFFSET, 0x1F, true, 2);
   cu.hw_set(FX1_ISP_COMMON_STATUS_OFFSET, FX1_ISP_COMMON_STATUS_FRAME_DONE_MASK | FX1_ISP_COMMON_STATUS_BUSY_MASK, 2);
   cu.hw_set(FX1_ISP_AEC_STATUS_OFFSET, FX1_ISP_AEC_STATUS_STAT_DONE_MASK, 2);
   // A frame in flight: every pixel block and the AEC/AF accumulators busy.
   cu.blocks_busy(true);
   cu.hw_set(FX1_ISP_AEC_STATUS_OFFSET, FX1_ISP_AEC_STATUS_BUSY_MASK, 2);
   cu.hw_set(FX1_ISP_AF_STATUS_OFFSET, FX1_ISP_AF_STATUS_BUSY_MASK, 2);
   cu.hw_write(FX1_ISP_AEC_GLOBAL_SUM_LO_OFFSET, 0xFFFFFFFFu, 0x1234u);
   cu.hw_write(FX1_ISP_IDMA_FRAME_COUNT_OFFSET, 0xFFFFFFFFu, 7);
   cu.hw_write(FX1_ISP_ODMA_FRAME_COUNT_OFFSET, 0xFFFFFFFFu, 6);
   cu.hw_write(FX1_ISP_AEC_FRAME_ID_OFFSET, 0xFFFFFFFFu, 5);
   wr(cu, FX1_ISP_GAMMA_LUT_ADDR_OFFSET, 9);
   wr(cu, FX1_ISP_GAMMA_LUT_DATA_OFFSET, 0xABC);
   // error_irq from DMA_ERR, stats_ready_irq from AEC stat_done.
   FX1_CHECK_EQ_CTX(rd(cu, FX1_ISP_COMMON_IRQ_STATUS_OFFSET), 0x6, ctx + " setup");

   wr(cu, reg, bit | (common ? FX1_ISP_COMMON_CTRL_ISP_EN_MASK : 0x3F03u), 100);

   // Preserved by both: configuration, LUTs, frame counters, FRAME_IDs.
   FX1_CHECK_EQ_CTX(rd(cu, FX1_ISP_COMMON_SCRATCH_OFFSET), 0x55AA55AA, ctx);
   FX1_CHECK_EQ_CTX(rd(cu, FX1_ISP_BLC_OFS_G0_DFT_OFFSET), 0x40, ctx);
   FX1_CHECK_EQ_CTX(rd(cu, FX1_ISP_DMA_CTRL_OFFSET), 0x3F03, ctx + " enables kept, soft_reset reads 0");
   FX1_CHECK_EQ_CTX(rd(cu, FX1_ISP_COMMON_IRQ_EN_OFFSET), 0x7, ctx);
   FX1_CHECK_EQ_CTX(rd(cu, FX1_ISP_IDMA_FRAME_COUNT_OFFSET), 7, ctx);
   FX1_CHECK_EQ_CTX(rd(cu, FX1_ISP_ODMA_FRAME_COUNT_OFFSET), 6, ctx);
   FX1_CHECK_EQ_CTX(rd(cu, FX1_ISP_AEC_FRAME_ID_OFFSET), 5, ctx);
   FX1_CHECK_EQ_CTX(rd(cu, FX1_ISP_COMMON_VER_ID_OFFSET), 0x00010002, ctx);
   FX1_CHECK_EQ_CTX(cu.gamma_lut()[9], 0xABC, ctx);
   // A pending AEC commit survives both (owner answer Q4).
   FX1_CHECK_EQ_CTX(rd(cu, FX1_ISP_AEC_CTRL_OFFSET) & FX1_ISP_AEC_CTRL_COMMIT_MASK, FX1_ISP_AEC_CTRL_COMMIT_MASK,
                    ctx + " pending AEC commit kept");
   // Cleared by both: DMA ownership, DMA interrupt and error state, busy.
   FX1_CHECK_EQ_CTX(rd(cu, FX1_ISP_IDMA_BUF_VALID_OFFSET), 0, ctx);
   FX1_CHECK_EQ_CTX(rd(cu, FX1_ISP_ODMA_BUF_FREE_OFFSET), 0, ctx);
   FX1_CHECK_EQ_CTX(rd(cu, FX1_ISP_ODMA_BUF_DONE_OFFSET), 0, ctx);
   FX1_CHECK_EQ_CTX(rd(cu, FX1_ISP_DMA_IRQ_STAT_OFFSET), 0, ctx);
   FX1_CHECK_EQ_CTX(rd(cu, FX1_ISP_DMA_ERR_OFFSET), 0, ctx + " holds released");
   FX1_CHECK_EQ_CTX(rd(cu, FX1_ISP_COMMON_STATUS_OFFSET) & FX1_ISP_COMMON_STATUS_BUSY_MASK, 0, ctx);
   // The frame in flight is abandoned by both: its busy flags drop (MAS
   // §6.5.3, §9.1.1; review R1 of 2026-10-09).
   for (const std::uint32_t off : {FX1_ISP_DEMOSAIC_STATUS_OFFSET, FX1_ISP_CCM_STATUS_OFFSET,
                                   FX1_ISP_GAMMA_STATUS_OFFSET, FX1_ISP_GTM_STATUS_OFFSET,
                                   FX1_ISP_NR_2D_STATUS_OFFSET, FX1_ISP_EE_STATUS_OFFSET,
                                   FX1_ISP_CNF_STATUS_OFFSET, FX1_ISP_RESIZER_STATUS_OFFSET}) {
      FX1_CHECK_EQ_CTX(rd(cu, off) & 1u, 0, ctx + " block busy @" + fx1_test::hex(off));
   }
   FX1_CHECK_EQ_CTX(rd(cu, FX1_ISP_AEC_STATUS_OFFSET) & FX1_ISP_AEC_STATUS_BUSY_MASK, 0, ctx + " AEC busy");
   FX1_CHECK_EQ_CTX(rd(cu, FX1_ISP_AF_STATUS_OFFSET) & FX1_ISP_AF_STATUS_BUSY_MASK, 0, ctx + " AF busy");
   if (common) {
      // Pipeline scope: statistics, all interrupt status, commands, status.
      FX1_CHECK_EQ_CTX(rd(cu, FX1_ISP_COMMON_CTRL_OFFSET), FX1_ISP_COMMON_CTRL_ISP_EN_MASK, ctx);
      FX1_CHECK_EQ_CTX(rd(cu, FX1_ISP_COMMON_IRQ_STATUS_OFFSET), 0, ctx);
      FX1_CHECK_EQ_CTX(rd(cu, FX1_ISP_COMMON_STATUS_OFFSET), 0, ctx);
      FX1_CHECK_EQ_CTX(rd(cu, FX1_ISP_AEC_STATUS_OFFSET), 0, ctx);
      FX1_CHECK_EQ_CTX(rd(cu, FX1_ISP_AEC_GLOBAL_SUM_LO_OFFSET), 0, ctx);
      FX1_CHECK_EQ_CTX(rd(cu, FX1_ISP_AF_CTRL_OFFSET), 0, ctx + " command bits");
      FX1_CHECK(!cu.irq_level());
   } else {
      // DMA scope only: statistics and the pipeline-side state are untouched;
      // COMMON_IRQ_STATUS is not a DMA interrupt register (assumption U1-A1).
      FX1_CHECK_EQ_CTX(rd(cu, FX1_ISP_COMMON_CTRL_OFFSET),
                       FX1_ISP_COMMON_CTRL_ISP_EN_MASK | FX1_ISP_COMMON_CTRL_FRAME_START_MASK, ctx);
      FX1_CHECK_EQ_CTX(rd(cu, FX1_ISP_COMMON_IRQ_STATUS_OFFSET), 0x6, ctx);
      FX1_CHECK_EQ_CTX(rd(cu, FX1_ISP_COMMON_STATUS_OFFSET) & FX1_ISP_COMMON_STATUS_FRAME_DONE_MASK,
                       FX1_ISP_COMMON_STATUS_FRAME_DONE_MASK, ctx);
      FX1_CHECK_EQ_CTX(rd(cu, FX1_ISP_AEC_STATUS_OFFSET), FX1_ISP_AEC_STATUS_STAT_DONE_MASK, ctx);
      // (Published results are kept as well; covered over TLM by the G-CONT
      // DMA-reset bursts, where a real publication precedes the reset.)
      FX1_CHECK_EQ_CTX(rd(cu, FX1_ISP_AF_CTRL_OFFSET), FX1_ISP_AF_CTRL_SEARCH_START_MASK, ctx + " command bits");
   }
   // 15-cycle pulse (MAS §6.5.3): W1S/W1C writes are held off, RW writes are accepted.
   FX1_CHECK(cu.soft_reset_active(100));
   FX1_CHECK(cu.soft_reset_active(114));
   FX1_CHECK(!cu.soft_reset_active(115));
   wr(cu, FX1_ISP_IDMA_BUF_VALID_OFFSET, 0x1, 114);
   wr(cu, FX1_ISP_COMMON_SCRATCH_OFFSET, 0x1, 114);
   FX1_CHECK_EQ_CTX(rd(cu, FX1_ISP_IDMA_BUF_VALID_OFFSET), 0, ctx + " W1S held off in pulse");
   FX1_CHECK_EQ_CTX(rd(cu, FX1_ISP_COMMON_SCRATCH_OFFSET), 1, ctx + " RW accepted in pulse");
   wr(cu, FX1_ISP_IDMA_BUF_VALID_OFFSET, 0x1, 115);
   FX1_CHECK_EQ_CTX(rd(cu, FX1_ISP_IDMA_BUF_VALID_OFFSET), 1, ctx + " W1S after pulse");
   // A rewrite starts the pulse again; reads always return zero (W1SC).
   wr(cu, reg, bit | (common ? FX1_ISP_COMMON_CTRL_ISP_EN_MASK : 0x3F03u), 200);
   wr(cu, reg, bit | (common ? FX1_ISP_COMMON_CTRL_ISP_EN_MASK : 0x3F03u), 210);
   FX1_CHECK(cu.soft_reset_active(224));
   FX1_CHECK(!cu.soft_reset_active(225));
   FX1_CHECK_EQ_CTX(rd(cu, reg) & bit, 0, ctx + " W1SC reads zero");
   // A hard reset clears the pending commit.
   cu.external_reset();
   FX1_CHECK_EQ_CTX(rd(cu, FX1_ISP_AEC_CTRL_OFFSET) & FX1_ISP_AEC_CTRL_COMMIT_MASK, 0, ctx + " i_rst_n");
}

// The pending AEC commit kept by a soft reset is consumed at the next accepted SOF.
void test_aec_commit_across_soft_reset() {
   control_unit cu;
   wr(cu, FX1_ISP_AEC_ZONE_CFG_OFFSET, 4u | (3u << FX1_ISP_AEC_ZONE_CFG_NUM_ZONE_Y_SHIFT), 1);
   wr(cu, FX1_ISP_AEC_CTRL_OFFSET, FX1_ISP_AEC_CTRL_EN_MASK | FX1_ISP_AEC_CTRL_COMMIT_MASK, 2);
   wr(cu, FX1_ISP_COMMON_CTRL_OFFSET, FX1_ISP_COMMON_CTRL_SOFT_RST_MASK, 3);
   FX1_CHECK(rd(cu, FX1_ISP_AEC_CTRL_OFFSET) & FX1_ISP_AEC_CTRL_COMMIT_MASK);
   cu.accepted_sof(40);
   FX1_CHECK_EQ(rd(cu, FX1_ISP_AEC_CTRL_OFFSET) & FX1_ISP_AEC_CTRL_COMMIT_MASK, 0);  // consumed: self-clears
}

void test_frame_start_and_done() {
   control_unit cu;
   const std::uint32_t fs = FX1_ISP_COMMON_CTRL_FRAME_START_MASK;
   wr(cu, FX1_ISP_COMMON_CTRL_OFFSET, fs);
   FX1_CHECK_EQ(rd(cu, FX1_ISP_COMMON_CTRL_OFFSET), fs);  // DEC-16: pending
   wr(cu, FX1_ISP_COMMON_CTRL_OFFSET, 0);                 // writing 0 does not clear W1S
   FX1_CHECK_EQ(rd(cu, FX1_ISP_COMMON_CTRL_OFFSET), fs);
   cu.hw_set(FX1_ISP_COMMON_STATUS_OFFSET, FX1_ISP_COMMON_STATUS_FRAME_DONE_MASK, 5);
   FX1_CHECK_EQ(rd(cu, FX1_ISP_COMMON_STATUS_OFFSET), FX1_ISP_COMMON_STATUS_FRAME_DONE_MASK);
   cu.accepted_sof(6);
   FX1_CHECK_EQ(rd(cu, FX1_ISP_COMMON_CTRL_OFFSET), 0);
   FX1_CHECK_EQ(rd(cu, FX1_ISP_COMMON_STATUS_OFFSET), 0);  // DEC-17
   // CSR-07: a W1S field with no hardware stays set.
   wr(cu, FX1_ISP_AF_CTRL_OFFSET, FX1_ISP_AF_CTRL_SEARCH_ABORT_MASK);
   cu.accepted_sof(7);
   FX1_CHECK_EQ(rd(cu, FX1_ISP_AF_CTRL_OFFSET), FX1_ISP_AF_CTRL_SEARCH_ABORT_MASK);
}

void test_commands_recorded() {
   control_unit cu;
   wr(cu, FX1_ISP_IDMA_BUF_VALID_OFFSET, 0x5, 3);
   FX1_CHECK_EQ(rd(cu, FX1_ISP_IDMA_BUF_VALID_OFFSET), 0x5);  // until the IDMA consumes it (M2)
   FX1_CHECK_EQ(cu.commands().size(), 1);
   FX1_CHECK_EQ(cu.commands().back().effect.w1s_set, 0x5);
   FX1_CHECK_EQ(cu.commands().back().cycle, 3);
   // An installed block hook receives the command instead.
   int hooked = 0;
   cu.set_block_hook(FX1_ISP_ODMA_BUF_FREE_OFFSET,
                     [&](const register_file::write_effect &fx, std::uint64_t) { hooked += fx.w1s_set != 0; });
   wr(cu, FX1_ISP_ODMA_BUF_FREE_OFFSET, 0x2, 4);
   FX1_CHECK_EQ(hooked, 1);
   FX1_CHECK_EQ(cu.commands().size(), 1);
}

// Review finding: pending commands must not survive a reset.
// Pending commands do not survive a hard reset or a COMMON soft reset; a
// DMA-only soft reset keeps the pipeline's (MAS Table 183).
void test_commands_cleared_on_reset() {
   control_unit cu;
   wr(cu, FX1_ISP_IDMA_BUF_VALID_OFFSET, 0x1, 1);
   FX1_CHECK_EQ(cu.commands().size(), 1);
   cu.external_reset();
   FX1_CHECK_EQ(cu.commands().size(), 0);

   wr(cu, FX1_ISP_ODMA_BUF_FREE_OFFSET, 0x3, 10);
   FX1_CHECK_EQ(cu.commands().size(), 1);
   wr(cu, FX1_ISP_DMA_CTRL_OFFSET, FX1_ISP_DMA_CTRL_SOFT_RESET_MASK | FX1_ISP_DMA_CTRL_IDMA_EN_MASK, 11);
   FX1_CHECK_EQ_CTX(cu.commands().size(), 1, "after DMA soft reset");
   wr(cu, FX1_ISP_COMMON_CTRL_OFFSET, FX1_ISP_COMMON_CTRL_SOFT_RST_MASK, 100);
   FX1_CHECK_EQ_CTX(cu.commands().size(), 0, "after COMMON soft reset");
}

void test_gamma_lut_port() {
   control_unit cu;
   wr(cu, FX1_ISP_GAMMA_LUT_ADDR_OFFSET, 0);
   for (std::uint32_t i = 0; i < 256; ++i) {
      wr(cu, FX1_ISP_GAMMA_LUT_DATA_OFFSET, (i * 16u + 3u) & 0xFFFu);
   }
   FX1_CHECK_EQ(rd(cu, FX1_ISP_GAMMA_LUT_ADDR_OFFSET), 0);  // wrapped after 256 writes
   FX1_CHECK_EQ(rd(cu, FX1_ISP_GAMMA_LUT_DATA_OFFSET), (255u * 16u + 3u) & 0xFFFu);  // shadow
   bool all = true;
   for (std::uint32_t i = 0; i < 256; ++i) {
      wr(cu, FX1_ISP_GAMMA_LUT_ADDR_OFFSET, i);
      all = all && rd(cu, FX1_ISP_GAMMA_LUT_RDATA_OFFSET) == ((i * 16u + 3u) & 0xFFFu);
   }
   FX1_CHECK(all);
   // Partial strobe merges into the data register, then writes the entry.
   wr(cu, FX1_ISP_GAMMA_LUT_ADDR_OFFSET, 5);
   wr(cu, FX1_ISP_GAMMA_LUT_DATA_OFFSET, 0x0FF0);
   wr(cu, FX1_ISP_GAMMA_LUT_ADDR_OFFSET, 5);
   wr(cu, FX1_ISP_GAMMA_LUT_DATA_OFFSET, 0xAB, 1, 0x000000FFu);
   FX1_CHECK_EQ(cu.gamma_lut()[5], 0xFAB);
   FX1_CHECK_EQ(rd(cu, FX1_ISP_GAMMA_LUT_ADDR_OFFSET), 6);
   wr(cu, FX1_ISP_GAMMA_LUT_DATA_OFFSET, 0x123, 1, 0);  // no strobes: no write, no increment
   FX1_CHECK_EQ(rd(cu, FX1_ISP_GAMMA_LUT_ADDR_OFFSET), 6);
   FX1_CHECK_EQ(cu.gamma_lut()[6], (6u * 16u + 3u) & 0xFFFu);
}

void test_ee_lut_port() {
   control_unit cu;
   const std::uint32_t sel_shift = FX1_ISP_EE_LUT_CTRL_LUT_SEL_SHIFT;
   FX1_CHECK_EQ(rd(cu, FX1_ISP_EE_LUT_RDATA_OFFSET), cu.ee_table(0)[0]);  // poison after i_rst_n (MAS §6.5.2)
   wr(cu, FX1_ISP_EE_LUT_CTRL_OFFSET, 2u << sel_shift);        // CONTRAST_POS, 32 entries
   wr(cu, FX1_ISP_EE_LUT_ADDR_OFFSET, 33);                     // top bit ignored -> entry 1
   wr(cu, FX1_ISP_EE_LUT_WDATA_OFFSET, 0x1234);
   FX1_CHECK_EQ(rd(cu, FX1_ISP_EE_LUT_ADDR_OFFSET), 34);
   FX1_CHECK_EQ(cu.ee_table(2)[1], 0x1234);
   wr(cu, FX1_ISP_EE_LUT_ADDR_OFFSET, 1);
   FX1_CHECK_EQ(rd(cu, FX1_ISP_EE_LUT_RDATA_OFFSET), 0x1234);
   wr(cu, FX1_ISP_EE_LUT_CTRL_OFFSET, 0);  // LUMA, 64 entries
   wr(cu, FX1_ISP_EE_LUT_ADDR_OFFSET, 33);
   FX1_CHECK_EQ(rd(cu, FX1_ISP_EE_LUT_RDATA_OFFSET), ((33u * 0x9E37u) ^ 0xA5A5u) & 0xFFFFu);  // unwritten: poison
   wr(cu, FX1_ISP_EE_LUT_ADDR_OFFSET, 63);
   wr(cu, FX1_ISP_EE_LUT_WDATA_OFFSET, 0x7);
   FX1_CHECK_EQ(rd(cu, FX1_ISP_EE_LUT_ADDR_OFFSET), 0);  // wraps at 64
   FX1_CHECK_EQ(cu.ee_table(0)[63], 0x7);
   FX1_CHECK_EQ(cu.ee_table(1)[63], (((64u + 63u) * 0x9E37u) ^ 0xA5A5u) & 0xFFFFu);  // untouched bank
   // GTM read-back port: zero at reset, out-of-table index reads zero.
   wr(cu, FX1_ISP_GTM_LUT_ADDR_OFFSET, 70);
   FX1_CHECK_EQ(rd(cu, FX1_ISP_GTM_LUT_RDATA_OFFSET), 0);
}

// EE resident profiles (MAS §6.18.4.1, §9.2; milestone U3).
void test_ee_profiles() {
   control_unit cu;
   std::uint64_t cyc = 1;
   const std::uint32_t ctrl = FX1_ISP_EE_CTRL_OFFSET;
   const auto wr_sel = [](unsigned p) { return p << FX1_ISP_EE_CTRL_PROFILE_WR_SEL_SHIFT; };
   const auto sel = [](unsigned p) { return p << FX1_ISP_EE_CTRL_PROFILE_SEL_SHIFT; };
   // Reset: every profile holds the CSR reset values (ALPHA 0.5, BETA 0.25,
   // clamps 16/24, radial gains 1.0; MAS p189).
   for (unsigned p = 0; p < control_unit::ee_profiles; ++p) {
      FX1_CHECK_EQ(cu.ee_profile_scalar(p, ee_alpha), 0x4000);
      FX1_CHECK_EQ(cu.ee_profile_scalar(p, ee_beta), 0x2000);
      FX1_CHECK_EQ(cu.ee_profile_scalar(p, ee_clamp), 0x1810);
      FX1_CHECK_EQ(cu.ee_profile_scalar(p, ee_radial_gain_23), 0x80008000u);
   }
   // Writes land in the profile named by PROFILE_WR_SEL; a scalar reads back
   // the last value written through it, whichever profile it went to.
   wr(cu, ctrl, wr_sel(1), cyc++);
   wr(cu, FX1_ISP_EE_ALPHA_OFFSET, 0x1111, cyc++);
   wr(cu, ctrl, wr_sel(2), cyc++);
   wr(cu, FX1_ISP_EE_ALPHA_OFFSET, 0x2222, cyc++);
   FX1_CHECK_EQ(cu.ee_profile_scalar(0, ee_alpha), 0x4000);
   FX1_CHECK_EQ(cu.ee_profile_scalar(1, ee_alpha), 0x1111);
   FX1_CHECK_EQ(cu.ee_profile_scalar(2, ee_alpha), 0x2222);
   FX1_CHECK_EQ(rd(cu, FX1_ISP_EE_ALPHA_OFFSET), 0x2222);
   // A partial write merges the bytes last written, here profile 2's (MAS §9.2).
   wr(cu, ctrl, wr_sel(1), cyc++);
   wr(cu, FX1_ISP_EE_ALPHA_OFFSET, 0x0055, cyc++, 0x000000FFu);
   FX1_CHECK_EQ(cu.ee_profile_scalar(1, ee_alpha), 0x2255);
   // Code 3: writes ignored, readback unchanged.
   wr(cu, ctrl, wr_sel(3), cyc++);
   wr(cu, FX1_ISP_EE_ALPHA_OFFSET, 0x3333, cyc++);
   wr(cu, FX1_ISP_EE_CLAMP_OFFSET, 0x0101, cyc++);
   FX1_CHECK_EQ(rd(cu, FX1_ISP_EE_ALPHA_OFFSET), 0x2255);
   FX1_CHECK_EQ(rd(cu, FX1_ISP_EE_CLAMP_OFFSET), 0x1810);
   for (unsigned p = 0; p < control_unit::ee_profiles; ++p) {
      FX1_CHECK_EQ(cu.ee_profile_scalar(p, ee_clamp), 0x1810);
   }
   // The table port follows PROFILE_WR_SEL for writes and read-back.
   const std::uint32_t lut_sel = 1u << FX1_ISP_EE_LUT_CTRL_LUT_SEL_SHIFT;  // ACTIVITY
   for (unsigned p = 0; p < 3; ++p) {
      wr(cu, ctrl, wr_sel(p), cyc++);
      wr(cu, FX1_ISP_EE_LUT_CTRL_OFFSET, lut_sel, cyc++);
      wr(cu, FX1_ISP_EE_LUT_ADDR_OFFSET, 5, cyc++);
      wr(cu, FX1_ISP_EE_LUT_WDATA_OFFSET, 0x100 + p, cyc++);
   }
   for (unsigned p = 0; p < 3; ++p) {
      FX1_CHECK_EQ(cu.ee_table(p, 1)[5], 0x100 + p);
      wr(cu, ctrl, wr_sel(p), cyc++);
      wr(cu, FX1_ISP_EE_LUT_ADDR_OFFSET, 5, cyc++);
      FX1_CHECK_EQ(rd(cu, FX1_ISP_EE_LUT_RDATA_OFFSET), 0x100 + p);
   }
   wr(cu, ctrl, wr_sel(3), cyc++);  // code 3: data ignored, address advances, read-back 0
   wr(cu, FX1_ISP_EE_LUT_ADDR_OFFSET, 5, cyc++);
   FX1_CHECK_EQ(rd(cu, FX1_ISP_EE_LUT_RDATA_OFFSET), 0);
   wr(cu, FX1_ISP_EE_LUT_WDATA_OFFSET, 0x999, cyc++);
   FX1_CHECK_EQ(rd(cu, FX1_ISP_EE_LUT_ADDR_OFFSET), 6);
   for (unsigned p = 0; p < 3; ++p) {
      FX1_CHECK_EQ(cu.ee_table(p, 1)[5], 0x100 + p);
   }
   // PROFILE_SEL, EN and the profile's scalars are sampled at the accepted
   // SOF; code 3 selects profile 0.
   wr(cu, ctrl, FX1_ISP_EE_CTRL_EN_MASK | sel(2), cyc++);
   FX1_CHECK(!cu.ee_frame().en);
   cu.accepted_sof(cyc++);
   FX1_CHECK(cu.ee_frame().en);
   FX1_CHECK_EQ(cu.ee_frame().profile, 2);
   FX1_CHECK_EQ(cu.ee_frame().scalar[ee_alpha], 0x2222);
   FX1_CHECK(&cu.ee_table(1) == &cu.ee_table(2, 1));  // the datapath reads the frame's profile
   wr(cu, ctrl, FX1_ISP_EE_CTRL_EN_MASK | sel(2) | wr_sel(2), cyc++);
   wr(cu, FX1_ISP_EE_ALPHA_OFFSET, 0x7777, cyc++);  // the active profile: next frame
   FX1_CHECK_EQ(cu.ee_frame().scalar[ee_alpha], 0x2222);
   wr(cu, ctrl, sel(3), cyc++);
   FX1_CHECK_EQ(cu.ee_frame().profile, 2);
   cu.accepted_sof(cyc++);
   FX1_CHECK_EQ(cu.ee_frame().profile, 0);
   FX1_CHECK(!cu.ee_frame().en);
   wr(cu, ctrl, FX1_ISP_EE_CTRL_EN_MASK | sel(2), cyc++);
   cu.accepted_sof(cyc++);
   FX1_CHECK_EQ(cu.ee_frame().scalar[ee_alpha], 0x7777);
   // Soft reset keeps every profile (configuration); i_rst_n restores them.
   wr(cu, FX1_ISP_COMMON_CTRL_OFFSET, FX1_ISP_COMMON_CTRL_SOFT_RST_MASK, cyc);
   cyc += 64;
   FX1_CHECK_EQ(cu.ee_profile_scalar(1, ee_alpha), 0x2255);
   FX1_CHECK_EQ(cu.ee_table(2, 1)[5], 0x102);
   cu.external_reset();
   FX1_CHECK_EQ(cu.ee_profile_scalar(2, ee_alpha), 0x4000);
   FX1_CHECK_EQ(cu.ee_table(1, 1)[5], ((256u + 64u + 5u) * 0x9E37u ^ 0xA5A5u) & 0xFFFFu);
}

// EE_STATUS.error: one event per frame EE processes with radial gain enabled
// and a gain of the frame's profile above 1.5 (MAS §6.18.4.1, §9.2); not a
// level of the live registers.
void test_ee_radial_error() {
   control_unit cu;
   std::uint64_t cyc = 1;
   const std::uint32_t err = FX1_ISP_EE_STATUS_ERROR_MASK;
   const auto frame = [&] {
      cu.accepted_sof(cyc++);
      cu.ee_end_of_frame();
   };
   wr(cu, FX1_ISP_COMMON_FRAME_WIDTH_OFFSET, 64, cyc++);
   wr(cu, FX1_ISP_COMMON_FRAME_HEIGHT_OFFSET, 48, cyc++);
   wr(cu, FX1_ISP_EE_FEATURE_EN_OFFSET, FX1_ISP_EE_FEATURE_EN_RADIAL_EN_MASK, cyc++);
   wr(cu, FX1_ISP_EE_RADIAL_GAIN_23_OFFSET, 0x8000C001u, cyc++);  // gain 2 just above 1.5
   FX1_CHECK_EQ(rd(cu, FX1_ISP_EE_STATUS_OFFSET) & err, 0);  // a write raises nothing
   frame();
   FX1_CHECK_EQ(rd(cu, FX1_ISP_EE_STATUS_OFFSET) & err, 0);  // EE disabled: not processed
   wr(cu, FX1_ISP_EE_CTRL_OFFSET, FX1_ISP_EE_CTRL_EN_MASK, cyc++);
   frame();
   FX1_CHECK_EQ(rd(cu, FX1_ISP_EE_STATUS_OFFSET) & err, err);
   FX1_CHECK(rd(cu, FX1_ISP_COMMON_IRQ_STATUS_OFFSET) & FX1_ISP_COMMON_IRQ_STATUS_ERROR_IRQ_MASK);
   wr(cu, FX1_ISP_EE_STATUS_OFFSET, err, cyc++);  // W1C: stays clear until the next frame
   FX1_CHECK_EQ(rd(cu, FX1_ISP_EE_STATUS_OFFSET) & err, 0);
   frame();
   FX1_CHECK_EQ(rd(cu, FX1_ISP_EE_STATUS_OFFSET) & err, err);  // the illegal gain is still programmed
   wr(cu, FX1_ISP_EE_STATUS_OFFSET, err, cyc++);
   wr(cu, FX1_ISP_EE_RADIAL_GAIN_23_OFFSET, 0x8000C000u, cyc++);  // exactly 1.5: legal
   frame();
   FX1_CHECK_EQ(rd(cu, FX1_ISP_EE_STATUS_OFFSET) & err, 0);
   wr(cu, FX1_ISP_EE_RADIAL_GAIN_01_OFFSET, 0xC0018000u, cyc++);  // gain 1 above 1.5, radial off
   wr(cu, FX1_ISP_EE_FEATURE_EN_OFFSET, 0, cyc++);
   frame();
   FX1_CHECK_EQ(rd(cu, FX1_ISP_EE_STATUS_OFFSET) & err, 0);
   wr(cu, FX1_ISP_EE_FEATURE_EN_OFFSET, FX1_ISP_EE_FEATURE_EN_RADIAL_EN_MASK, cyc++);
   wr(cu, FX1_ISP_COMMON_FRAME_HEIGHT_OFFSET, 4, cyc++);  // below 5 x 5: not processed
   frame();
   FX1_CHECK_EQ(rd(cu, FX1_ISP_EE_STATUS_OFFSET) & err, 0);
   // The gain of another profile does not count; the frame's profile does.
   wr(cu, FX1_ISP_COMMON_FRAME_HEIGHT_OFFSET, 48, cyc++);
   wr(cu, FX1_ISP_EE_CTRL_OFFSET, FX1_ISP_EE_CTRL_EN_MASK | (1u << FX1_ISP_EE_CTRL_PROFILE_SEL_SHIFT), cyc++);
   frame();  // profile 1: reset gains, radial off
   FX1_CHECK_EQ(rd(cu, FX1_ISP_EE_STATUS_OFFSET) & err, 0);
   // Profile 1 with radial on and legal gains; profile 0 keeps an illegal gain 1.
   const std::uint32_t wr1 = 1u << FX1_ISP_EE_CTRL_PROFILE_WR_SEL_SHIFT, sel1 = 1u << FX1_ISP_EE_CTRL_PROFILE_SEL_SHIFT;
   wr(cu, FX1_ISP_EE_CTRL_OFFSET, FX1_ISP_EE_CTRL_EN_MASK | sel1 | wr1, cyc++);
   wr(cu, FX1_ISP_EE_FEATURE_EN_OFFSET, FX1_ISP_EE_FEATURE_EN_RADIAL_EN_MASK, cyc++);
   FX1_CHECK_EQ(cu.ee_profile_scalar(0, ee_radial_gain_01), 0xC0018000u);
   frame();
   FX1_CHECK_EQ(rd(cu, FX1_ISP_EE_STATUS_OFFSET) & err, 0);  // profile 0's gain does not count
   wr(cu, FX1_ISP_EE_RADIAL_GAIN_01_OFFSET, 0x8000FFFFu, cyc++);  // profile 1 gain 0 above 1.5
   frame();
   FX1_CHECK_EQ(rd(cu, FX1_ISP_EE_STATUS_OFFSET) & err, err);
}

// GTM table writes reach the memory only while GTM_CTRL.EN = 0, whatever
// MANUAL says (MAS p146; U3).
void test_gtm_lut_gate() {
   control_unit cu;
   std::uint64_t cyc = 1;
   const auto load = [&](std::uint32_t ctrl, std::uint32_t v) {
      wr(cu, FX1_ISP_GTM_CTRL_OFFSET, ctrl, cyc++);
      wr(cu, FX1_ISP_GTM_LUT_ADDR_OFFSET, 3, cyc++);
      wr(cu, FX1_ISP_GTM_LUT_DATA_OFFSET, v, cyc++);
      FX1_CHECK_EQ(rd(cu, FX1_ISP_GTM_LUT_ADDR_OFFSET), 4);  // the address advances anyway
      wr(cu, FX1_ISP_GTM_LUT_ADDR_OFFSET, 3, cyc++);
      return rd(cu, FX1_ISP_GTM_LUT_RDATA_OFFSET);
   };
   FX1_CHECK_EQ(load(FX1_ISP_GTM_CTRL_MANUAL_MASK, 0x123), 0x123);      // EN = 0: written
   FX1_CHECK_EQ(load(0, 0x124), 0x124);
   FX1_CHECK_EQ(load(FX1_ISP_GTM_CTRL_EN_MASK | FX1_ISP_GTM_CTRL_MANUAL_MASK, 0x555), 0x124);  // discarded
   FX1_CHECK_EQ(load(FX1_ISP_GTM_CTRL_EN_MASK, 0x556), 0x124);
}

void test_resizer_geometry() {
   control_unit cu;
   auto set_geom = [&](std::uint32_t w, std::uint32_t h, std::uint32_t bayer) {
      wr(cu, FX1_ISP_COMMON_FRAME_WIDTH_OFFSET, w);
      wr(cu, FX1_ISP_COMMON_FRAME_HEIGHT_OFFSET, h);
      wr(cu, FX1_ISP_COMMON_BAYER_OFFSET, bayer);
   };
   auto out = [&]() {
      return std::pair<std::uint32_t, std::uint32_t>(rd(cu, FX1_ISP_RESIZER_OUT_W_OFFSET),
                                                     rd(cu, FX1_ISP_RESIZER_OUT_H_OFFSET));
   };
   auto resizer = [&](bool en, std::uint32_t scale) {  // settled reads: the published set
      wr(cu, FX1_ISP_RESIZER_CTRL_OFFSET, (en ? 1u : 0u) | (scale << FX1_ISP_RESIZER_CTRL_SCALE_SHIFT));
   };
   set_geom(2688, 1520, FX1_ISP_COMMON_BAYER_PATTERN_BGGR);  // DEC-04 input
   FX1_CHECK_EQ(out().first, 2686);
   FX1_CHECK_EQ(out().second, 1518);
   resizer(true, FX1_ISP_RESIZER_CTRL_SCALE_RES_1920X1080);
   FX1_CHECK_EQ(out().first, 1920);
   FX1_CHECK_EQ(out().second, 1080);
   resizer(true, FX1_ISP_RESIZER_CTRL_SCALE_RES_3840X2160);  // larger than input: FullRes
   FX1_CHECK_EQ(out().first, 2686);
   resizer(true, 0xF);  // reserved: FullRes
   FX1_CHECK_EQ(out().second, 1518);
   resizer(false, FX1_ISP_RESIZER_CTRL_SCALE_RES_640X480);  // disabled: pass-through
   FX1_CHECK_EQ(out().first, 2686);
   resizer(true, FX1_ISP_RESIZER_CTRL_SCALE_RES_640X480);
   FX1_CHECK_EQ(out().first, 640);
   FX1_CHECK_EQ(out().second, 480);
   resizer(false, 0);
   set_geom(2688, 1520, FX1_ISP_COMMON_BAYER_PATTERN_GRBG);  // H shift only
   FX1_CHECK_EQ(out().first, 2686);
   FX1_CHECK_EQ(out().second, 1520);
   set_geom(2688, 1520, FX1_ISP_COMMON_BAYER_PATTERN_GBRG);  // V shift only
   FX1_CHECK_EQ(out().first, 2688);
   FX1_CHECK_EQ(out().second, 1518);
   set_geom(2689, 1521, FX1_ISP_COMMON_BAYER_PATTERN_RGGB);  // odd input rounds down
   FX1_CHECK_EQ(out().first, 2688);
   FX1_CHECK_EQ(out().second, 1520);
}

void lsc_load(control_unit &cu, unsigned dest, std::uint32_t count, std::uint32_t value, std::uint64_t &cyc) {
   wr(cu, FX1_ISP_LSC_LOAD_CTRL_OFFSET, dest | FX1_ISP_LSC_LOAD_CTRL_LOAD_BEGIN_MASK, cyc++);
   for (std::uint32_t i = 0; i < count; ++i) {
      wr(cu, FX1_ISP_LSC_COEF_DATA_OFFSET, value + i, cyc++);
   }
   wr(cu, FX1_ISP_LSC_LOAD_CTRL_OFFSET, dest | FX1_ISP_LSC_LOAD_CTRL_LOAD_VALIDATE_MASK, cyc++);
}

// LSC load sequencer (MAS §6.8.4.1 Table 31; owner Q5).
// Growing the mesh during a load must never validate an undersized bank.
void test_lsc_resize_during_load() {
   control_unit cu;
   std::uint64_t cyc = 1;
   wr(cu, FX1_ISP_LSC_MESH_NODES_OFFSET, 0x0202, cyc++);
   wr(cu, FX1_ISP_LSC_LOAD_CTRL_OFFSET, 1u | FX1_ISP_LSC_LOAD_CTRL_LOAD_BEGIN_MASK, cyc++);
   wr(cu, FX1_ISP_LSC_MESH_NODES_OFFSET, 0x0303, cyc++);
   for (unsigned i = 0; i < 36; ++i) {
      wr(cu, FX1_ISP_LSC_COEF_DATA_OFFSET, 0x40000, cyc++);
   }
   wr(cu, FX1_ISP_LSC_LOAD_CTRL_OFFSET, 1u | FX1_ISP_LSC_LOAD_CTRL_LOAD_VALIDATE_MASK, cyc++);
   FX1_CHECK(!cu.lsc_profile_valid(1));
   FX1_CHECK(rd(cu, FX1_ISP_LSC_ERROR_OFFSET) & FX1_ISP_LSC_ERROR_COEF_COUNT_MASK);
   // A fresh load in the new geometry recovers normally.
   lsc_load(cu, 1, 36, 0x40000, cyc);
   FX1_CHECK(cu.lsc_profile_valid(1));
   FX1_CHECK_EQ(cu.lsc_profile(1).size(), 36);
}

void test_lsc_loader() {
   control_unit cu;
   std::uint64_t cyc = 1;
   const std::uint32_t st = FX1_ISP_LSC_PROFILE_STATUS_OFFSET, err = FX1_ISP_LSC_ERROR_OFFSET;
   FX1_CHECK_EQ(rd(cu, FX1_ISP_LSC_LOAD_STATUS_OFFSET), 0);
   // Invalid geometry at reset: begin refused.
   wr(cu, FX1_ISP_LSC_LOAD_CTRL_OFFSET, 1u | FX1_ISP_LSC_LOAD_CTRL_LOAD_BEGIN_MASK, cyc++);
   FX1_CHECK_EQ(rd(cu, err), FX1_ISP_LSC_ERROR_GEOMETRY_INVALID_MASK);
   FX1_CHECK_EQ(rd(cu, FX1_ISP_LSC_LOAD_CTRL_OFFSET) & 0x700u, 0);  // commands self-clear
   wr(cu, err, 0x3F, cyc++);
   // 2..128 nodes per axis (CSR 07/10, DEC-31 superseded), not above the active size.
   wr(cu, FX1_ISP_LSC_MESH_NODES_OFFSET, 129u | (2u << 8), cyc++);
   FX1_CHECK_EQ(rd(cu, st) & FX1_ISP_LSC_PROFILE_STATUS_GEOMETRY_VALID_MASK, 0);
   wr(cu, FX1_ISP_LSC_MESH_NODES_OFFSET, 128u | (128u << 8), cyc++);
   FX1_CHECK(rd(cu, st) & FX1_ISP_LSC_PROFILE_STATUS_GEOMETRY_VALID_MASK);
   wr(cu, FX1_ISP_COMMON_FRAME_WIDTH_OFFSET, 100, cyc++);  // 128 nodes on a 100-pixel line
   FX1_CHECK_EQ(rd(cu, st) & FX1_ISP_LSC_PROFILE_STATUS_GEOMETRY_VALID_MASK, 0);
   wr(cu, FX1_ISP_COMMON_FRAME_WIDTH_OFFSET, 1920, cyc++);
   wr(cu, FX1_ISP_LSC_MESH_NODES_OFFSET, 3u | (2u << 8), cyc++);  // 3 x 2 nodes -> 24 coefficients
   FX1_CHECK_EQ(rd(cu, st), FX1_ISP_LSC_PROFILE_STATUS_GEOMETRY_VALID_MASK);
   // The active index is never a destination, even while invalid (owner Q5):
   // at boot, profile 0 is active and loading it is refused.
   wr(cu, FX1_ISP_LSC_LOAD_CTRL_OFFSET, FX1_ISP_LSC_LOAD_CTRL_LOAD_BEGIN_MASK, cyc++);
   FX1_CHECK_EQ(rd(cu, err), FX1_ISP_LSC_ERROR_ACTIVE_LOAD_REJECT_MASK);
   FX1_CHECK_EQ(rd(cu, FX1_ISP_LSC_LOAD_STATUS_OFFSET) & FX1_ISP_LSC_LOAD_STATUS_LOAD_BUSY_MASK, 0);
   wr(cu, err, 0x3F, cyc++);
   // Boot sequence: load profile 1, validate, select it; adopted at the next SOF.
   lsc_load(cu, 1, 24, 0x40000, cyc);
   FX1_CHECK_EQ(rd(cu, err), 0);
   FX1_CHECK_EQ(rd(cu, st) & 0x47u, 0x02u);  // valid[1]; the active profile (0) is not valid
   FX1_CHECK_EQ(cu.lsc_profile(1)[23], 0x40000 + 23);
   FX1_CHECK_EQ(rd(cu, FX1_ISP_LSC_LOAD_STATUS_OFFSET),
                24u | FX1_ISP_LSC_LOAD_STATUS_LOAD_FULL_MASK | (1u << FX1_ISP_LSC_LOAD_STATUS_LOAD_PROFILE_SHIFT));
   wr(cu, FX1_ISP_LSC_PROFILE_SEL_OFFSET, 1, cyc++);
   FX1_CHECK_EQ(rd(cu, err), 0);
   FX1_CHECK_EQ(cu.lsc_active_profile(), 0);
   cu.accepted_sof(cyc++);
   FX1_CHECK_EQ(cu.lsc_active_profile(), 1);
   FX1_CHECK_EQ(rd(cu, st) & FX1_ISP_LSC_PROFILE_STATUS_ACTIVE_PROFILE_MASK, 1u << 4);
   FX1_CHECK(rd(cu, st) & FX1_ISP_LSC_PROFILE_STATUS_ACTIVE_VALID_MASK);
   // Now profile 1 is active: loading it is refused.
   wr(cu, FX1_ISP_LSC_LOAD_CTRL_OFFSET, 1u | FX1_ISP_LSC_LOAD_CTRL_LOAD_BEGIN_MASK, cyc++);
   FX1_CHECK_EQ(rd(cu, err), FX1_ISP_LSC_ERROR_ACTIVE_LOAD_REJECT_MASK);
   wr(cu, err, 0x3F, cyc++);
   // A short load fails at validate; the destination is invalid (MAS Table 31).
   lsc_load(cu, 2, 24, 0x10, cyc);
   FX1_CHECK(cu.lsc_profile_valid(2));
   lsc_load(cu, 2, 23, 0x99, cyc);
   FX1_CHECK_EQ(rd(cu, err), FX1_ISP_LSC_ERROR_COEF_COUNT_MASK);
   FX1_CHECK(!cu.lsc_profile_valid(2));
   wr(cu, err, 0x3F, cyc++);
   // An extra write also fails.
   lsc_load(cu, 2, 25, 0x77, cyc);
   FX1_CHECK_EQ(rd(cu, err), FX1_ISP_LSC_ERROR_COEF_COUNT_MASK);
   FX1_CHECK(!cu.lsc_profile_valid(2));
   wr(cu, err, 0x3F, cyc++);
   // A begin invalidates its destination at once; abort leaves it invalid,
   // and abort wins over a simultaneous validate. The active profile is unchanged.
   lsc_load(cu, 2, 24, 0x20, cyc);
   FX1_CHECK(cu.lsc_profile_valid(2));
   wr(cu, FX1_ISP_LSC_LOAD_CTRL_OFFSET, 2u | FX1_ISP_LSC_LOAD_CTRL_LOAD_BEGIN_MASK, cyc++);
   FX1_CHECK(!cu.lsc_profile_valid(2));
   wr(cu, FX1_ISP_LSC_COEF_DATA_OFFSET, 5, cyc++);
   FX1_CHECK_EQ(rd(cu, FX1_ISP_LSC_LOAD_STATUS_OFFSET),
                1u | FX1_ISP_LSC_LOAD_STATUS_LOAD_BUSY_MASK | (2u << FX1_ISP_LSC_LOAD_STATUS_LOAD_PROFILE_SHIFT));
   wr(cu, FX1_ISP_LSC_LOAD_CTRL_OFFSET, 2u | FX1_ISP_LSC_LOAD_CTRL_LOAD_ABORT_MASK |
                                             FX1_ISP_LSC_LOAD_CTRL_LOAD_VALIDATE_MASK, cyc++);  // abort wins
   FX1_CHECK(!cu.lsc_profile_valid(2));
   FX1_CHECK(cu.lsc_profile_valid(1) && cu.lsc_active_profile() == 1);
   FX1_CHECK_EQ(rd(cu, err), 0);
   // Protocol violations: data without a load (VP assumption, owner query U0-Q6),
   // validate without begin (MAS Table 31).
   wr(cu, FX1_ISP_LSC_COEF_DATA_OFFSET, 1, cyc++);
   FX1_CHECK_EQ(rd(cu, err), FX1_ISP_LSC_ERROR_LOAD_PROTOCOL_MASK);
   wr(cu, err, 0x3F, cyc++);
   wr(cu, FX1_ISP_LSC_LOAD_CTRL_OFFSET, FX1_ISP_LSC_LOAD_CTRL_LOAD_VALIDATE_MASK, cyc++);
   FX1_CHECK_EQ(rd(cu, err), FX1_ISP_LSC_ERROR_LOAD_PROTOCOL_MASK);
   wr(cu, err, 0x3F, cyc++);
   // Coefficient above 4.0 flags coef_range but is stored.
   lsc_load(cu, 2, 24, 0x100001, cyc);
   FX1_CHECK_EQ(rd(cu, err), FX1_ISP_LSC_ERROR_COEF_RANGE_MASK);
   FX1_CHECK(cu.lsc_profile_valid(2));
   wr(cu, err, 0x3F, cyc++);
   // Selection: unloaded profile or code 3 refused (value kept).
   wr(cu, FX1_ISP_LSC_PROFILE_SEL_OFFSET, 3, cyc++);
   FX1_CHECK_EQ(rd(cu, err), FX1_ISP_LSC_ERROR_PROFILE_SEL_REJECT_MASK);
   FX1_CHECK_EQ(rd(cu, FX1_ISP_LSC_PROFILE_SEL_OFFSET), 1);
   wr(cu, err, 0x3F, cyc++);
   wr(cu, FX1_ISP_LSC_PROFILE_SEL_OFFSET, 0, cyc++);  // profile 0 was never loaded
   FX1_CHECK_EQ(rd(cu, err), FX1_ISP_LSC_ERROR_PROFILE_SEL_REJECT_MASK);
   FX1_CHECK_EQ(rd(cu, FX1_ISP_LSC_PROFILE_SEL_OFFSET), 1);
   wr(cu, err, 0x3F, cyc++);
   // A COMMON soft reset keeps meshes and validity and drops a load in progress;
   // a DMA-only soft reset does not touch the loader (MAS Table 183).
   wr(cu, FX1_ISP_LSC_LOAD_CTRL_OFFSET, FX1_ISP_LSC_LOAD_CTRL_LOAD_BEGIN_MASK, cyc++);
   FX1_CHECK(rd(cu, FX1_ISP_LSC_LOAD_STATUS_OFFSET) & FX1_ISP_LSC_LOAD_STATUS_LOAD_BUSY_MASK);
   wr(cu, FX1_ISP_DMA_CTRL_OFFSET, FX1_ISP_DMA_CTRL_SOFT_RESET_MASK, cyc);
   cyc += 64;
   FX1_CHECK(rd(cu, FX1_ISP_LSC_LOAD_STATUS_OFFSET) & FX1_ISP_LSC_LOAD_STATUS_LOAD_BUSY_MASK);
   wr(cu, FX1_ISP_COMMON_CTRL_OFFSET, FX1_ISP_COMMON_CTRL_SOFT_RST_MASK, cyc);
   cyc += 64;
   FX1_CHECK_EQ(rd(cu, FX1_ISP_LSC_LOAD_STATUS_OFFSET) & FX1_ISP_LSC_LOAD_STATUS_LOAD_BUSY_MASK, 0);
   FX1_CHECK(cu.lsc_profile_valid(1) && cu.lsc_profile_valid(2));
   FX1_CHECK(!cu.lsc_profile_valid(0));  // invalidated by the begin before the reset
   // Maximum mesh: 128 x 128 nodes = 65536 coefficients. LOAD_FULL and validity
   // are checked; the 16-bit LOAD_ADDR readback at the terminal count is not,
   // pending the owner (follow-up of 2026-10-09).
   wr(cu, FX1_ISP_LSC_MESH_NODES_OFFSET, 128u | (128u << 8), cyc++);
   FX1_CHECK_EQ(rd(cu, st) & 0x7u, 0);  // a geometry change invalidates every profile
   lsc_load(cu, 2, 4u * 128u * 128u, 0x40000, cyc);
   FX1_CHECK(cu.lsc_profile_valid(2));
   FX1_CHECK(rd(cu, FX1_ISP_LSC_LOAD_STATUS_OFFSET) & FX1_ISP_LSC_LOAD_STATUS_LOAD_FULL_MASK);
   FX1_CHECK_EQ(cu.lsc_profile(2).size(), 65536);
   // External reset clears validity.
   cu.external_reset();
   FX1_CHECK(!cu.lsc_profile_valid(2));
}

// The selection is re-checked at the accepted SOF (MAS Table 31, Figure 29;
// review R2 of 2026-10-09): a selector accepted while its profile was valid,
// whose profile a later begin, abort or count mismatch invalidated, leaves the
// previous profile active and reports PROFILE_SEL_REJECT at every SOF while it
// differs from the active one. A later successful validate lets it take effect.
void test_lsc_selection_at_sof() {
   control_unit cu;
   std::uint64_t cyc = 1;
   const std::uint32_t st = FX1_ISP_LSC_PROFILE_STATUS_OFFSET, err = FX1_ISP_LSC_ERROR_OFFSET;
   const std::uint32_t reject = FX1_ISP_LSC_ERROR_PROFILE_SEL_REJECT_MASK;
   const auto active_is = [&](unsigned p, const std::string &where) {
      FX1_CHECK_EQ_CTX(cu.lsc_active_profile(), p, where);
      FX1_CHECK_EQ_CTX(rd(cu, st) & FX1_ISP_LSC_PROFILE_STATUS_ACTIVE_PROFILE_MASK,
                       p << FX1_ISP_LSC_PROFILE_STATUS_ACTIVE_PROFILE_SHIFT, where);
      FX1_CHECK_EQ_CTX(rd(cu, st) & FX1_ISP_LSC_PROFILE_STATUS_ACTIVE_VALID_MASK,
                       FX1_ISP_LSC_PROFILE_STATUS_ACTIVE_VALID_MASK, where + " active valid");
   };
   wr(cu, FX1_ISP_LSC_MESH_NODES_OFFSET, 0x0202, cyc++);
   lsc_load(cu, 1, 16, 0x40000, cyc);
   wr(cu, FX1_ISP_LSC_PROFILE_SEL_OFFSET, 1, cyc++);
   cu.accepted_sof(cyc++);
   active_is(1, "boot");

   // 1. Begin after the selection: the SOF arrives while the destination loads.
   lsc_load(cu, 2, 16, 0x50000, cyc);
   wr(cu, FX1_ISP_LSC_PROFILE_SEL_OFFSET, 2, cyc++);
   FX1_CHECK_EQ(rd(cu, err), 0);  // valid when written
   wr(cu, FX1_ISP_LSC_LOAD_CTRL_OFFSET, 2u | FX1_ISP_LSC_LOAD_CTRL_LOAD_BEGIN_MASK, cyc++);
   cu.accepted_sof(cyc++);
   active_is(1, "SOF during the load");
   FX1_CHECK_EQ_CTX(rd(cu, err), reject, "SOF during the load");
   wr(cu, err, 0x3F, cyc++);

   // 2. Abort: the destination stays invalid; every SOF keeps profile 1.
   wr(cu, FX1_ISP_LSC_LOAD_CTRL_OFFSET, 2u | FX1_ISP_LSC_LOAD_CTRL_LOAD_ABORT_MASK, cyc++);
   cu.accepted_sof(cyc++);
   active_is(1, "SOF after abort");
   FX1_CHECK_EQ_CTX(rd(cu, err), reject, "SOF after abort");
   wr(cu, err, 0x3F, cyc++);
   cu.accepted_sof(cyc++);
   FX1_CHECK_EQ_CTX(rd(cu, err), reject, "reported again at the next SOF");
   wr(cu, err, 0x3F, cyc++);

   // 3. Count mismatch: still invalid.
   lsc_load(cu, 2, 15, 0x60000, cyc);
   FX1_CHECK_EQ(rd(cu, err), FX1_ISP_LSC_ERROR_COEF_COUNT_MASK);
   wr(cu, err, 0x3F, cyc++);
   cu.accepted_sof(cyc++);
   active_is(1, "SOF after a count mismatch");
   FX1_CHECK_EQ_CTX(rd(cu, err), reject, "SOF after a count mismatch");
   wr(cu, err, 0x3F, cyc++);

   // 4. A successful validate: the pending selection takes effect at the SOF,
   // with the new mesh, and nothing more is reported.
   lsc_load(cu, 2, 16, 0x70000, cyc);
   FX1_CHECK_EQ(rd(cu, err), 0);
   active_is(1, "validated, before the SOF");
   cu.accepted_sof(cyc++);
   active_is(2, "SOF after validate");
   FX1_CHECK_EQ(cu.lsc_profile(2)[0], 0x70000);
   cu.accepted_sof(cyc++);
   FX1_CHECK_EQ_CTX(rd(cu, err), 0, "no rejection once the selection is active");
}

// Resizer configuration controller (MAS §6.20.4.3-4, Table 123-124; U4):
// stable key -> E0..E35 -> publication; observations at an edge see what was
// published before it. DEC-24/26 (commit gate) superseded.
void test_resizer_publication() {
   control_unit cu;
   const std::uint32_t en = FX1_ISP_RESIZER_CTRL_EN_MASK, sh = FX1_ISP_RESIZER_CTRL_SCALE_SHIFT;
   const auto ow = [&](std::uint64_t c) { return cu.csr_read(FX1_ISP_RESIZER_OUT_W_OFFSET, c); };
   const auto oh = [&](std::uint64_t c) { return cu.csr_read(FX1_ISP_RESIZER_OUT_H_OFFSET, c); };
   // Reset set: the CSR reset values (the reset input geometry, unscaled).
   FX1_CHECK_EQ(ow(0), 1920);
   FX1_CHECK_EQ(oh(0), 1080);
   // Pass-through (scaling disabled): the key written on edge w is stable from
   // w + 1 (E0) and published on the next edge, so visible from w + 3.
   wr(cu, FX1_ISP_COMMON_FRAME_WIDTH_OFFSET, 3840, 10);
   wr(cu, FX1_ISP_COMMON_FRAME_HEIGHT_OFFSET, 2160, 11);
   FX1_CHECK_EQ(oh(13), 1080);
   FX1_CHECK_EQ(ow(14), 3840);
   FX1_CHECK_EQ(oh(14), 2160);
   // A stable scaling key: E0 on the edge after the write, published at E35.
   wr(cu, FX1_ISP_RESIZER_CTRL_OFFSET, en | (FX1_ISP_RESIZER_CTRL_SCALE_RES_1920X1080 << sh), 20);
   const std::uint64_t e0 = 21, e35 = e0 + control_unit::resizer_derivation_cycles;
   FX1_CHECK_EQ(ow(e35), 3840);      // during the derivation the previous set stays
   FX1_CHECK_EQ(ow(e35 + 1), 1920);  // published
   FX1_CHECK_EQ(oh(e35 + 1), 1080);
   FX1_CHECK_EQ(cu.resizer_published(e35 + 1).scaling ? 1 : 0, 1);
   // A SOF on the publication edge samples the previous set; one edge later the new.
   {
      control_unit a;
      wr(a, FX1_ISP_COMMON_FRAME_WIDTH_OFFSET, 3840, 10);
      wr(a, FX1_ISP_COMMON_FRAME_HEIGHT_OFFSET, 2160, 11);
      wr(a, FX1_ISP_RESIZER_CTRL_OFFSET, en | (FX1_ISP_RESIZER_CTRL_SCALE_RES_1920X1080 << sh), 20);
      a.accepted_sof(e35);
      FX1_CHECK_EQ(a.resizer_frame().out.width, 3840);
      FX1_CHECK_EQ(a.resizer_frame().scaling ? 1 : 0, 0);
      a.accepted_sof(e35 + 1);
      FX1_CHECK_EQ(a.resizer_frame().out.width, 1920);
      FX1_CHECK_EQ(a.resizer_frame().key.width, 3840);
   }
   // A key changed during a derivation: the stale result is not published; a
   // new derivation starts when the dividers are free (at that E35).
   wr(cu, FX1_ISP_RESIZER_CTRL_OFFSET, en | (FX1_ISP_RESIZER_CTRL_SCALE_RES_1280X720 << sh), 100);  // E0 101
   wr(cu, FX1_ISP_RESIZER_CTRL_OFFSET, en | (FX1_ISP_RESIZER_CTRL_SCALE_RES_640X480 << sh), 110);
   FX1_CHECK_EQ(ow(136), 1920);
   FX1_CHECK_EQ(ow(137), 1920);  // HD was derived but is stale: never published
   FX1_CHECK_EQ(ow(171), 1920);
   FX1_CHECK_EQ(ow(172), 640);   // VGA: E0 136, E35 171
   FX1_CHECK_EQ(oh(172), 480);
   // Changed and changed back during a derivation: the saved key matches again.
   wr(cu, FX1_ISP_RESIZER_CTRL_OFFSET, en | (FX1_ISP_RESIZER_CTRL_SCALE_RES_1280X720 << sh), 200);  // E0 201
   wr(cu, FX1_ISP_RESIZER_CTRL_OFFSET, en | (FX1_ISP_RESIZER_CTRL_SCALE_RES_960X540 << sh), 210);
   wr(cu, FX1_ISP_RESIZER_CTRL_OFFSET, en | (FX1_ISP_RESIZER_CTRL_SCALE_RES_1280X720 << sh), 220);
   FX1_CHECK_EQ(ow(237), 1280);
   // Pass-through has priority (MAS §6.20.4.3, review U4-R2): disabling during
   // a derivation publishes the bypass set on schedule (E0 + 1 of the new key),
   // and the derivation's completion does not restore the stale mode.
   wr(cu, FX1_ISP_RESIZER_CTRL_OFFSET, en | (FX1_ISP_RESIZER_CTRL_SCALE_RES_640X360 << sh), 300);  // E0 301
   FX1_CHECK_EQ(ow(320), 1280);
   wr(cu, FX1_ISP_RESIZER_CTRL_OFFSET, FX1_ISP_RESIZER_CTRL_SCALE_RES_640X360 << sh, 320);   // EN = 0
   FX1_CHECK_EQ(ow(322), 1280);
   FX1_CHECK_EQ(ow(323), 3840);  // stable at 321, published at 322
   FX1_CHECK_EQ(cu.resizer_published(323).scaling ? 1 : 0, 0);
   {
      control_unit b;  // the same, with a SOF after the bypass deadline
      wr(b, FX1_ISP_COMMON_FRAME_WIDTH_OFFSET, 3840, 10);
      wr(b, FX1_ISP_COMMON_FRAME_HEIGHT_OFFSET, 2160, 11);
      wr(b, FX1_ISP_RESIZER_CTRL_OFFSET, en | (FX1_ISP_RESIZER_CTRL_SCALE_RES_1280X720 << sh), 20);
      wr(b, FX1_ISP_RESIZER_CTRL_OFFSET, en | (FX1_ISP_RESIZER_CTRL_SCALE_RES_640X360 << sh), 100);
      wr(b, FX1_ISP_RESIZER_CTRL_OFFSET, FX1_ISP_RESIZER_CTRL_SCALE_RES_640X360 << sh, 120);
      b.accepted_sof(122);
      FX1_CHECK_EQ(b.resizer_frame().scaling ? 1 : 0, 1);  // the bypass is published on 122 itself
      b.accepted_sof(123);
      FX1_CHECK_EQ(b.resizer_frame().scaling ? 1 : 0, 0);
      FX1_CHECK_EQ(b.resizer_frame().out.width, 3840);
   }
   FX1_CHECK_EQ(ow(337), 3840);  // the derivation ended at 336: its stale 640 x 360 is dropped
   FX1_CHECK_EQ(ow(400), 3840);
   // Oversized, FullRes and reserved codes: pass-through on the next edge.
   wr(cu, FX1_ISP_COMMON_FRAME_WIDTH_OFFSET, 2688, 400);
   wr(cu, FX1_ISP_COMMON_FRAME_HEIGHT_OFFSET, 1520, 401);
   wr(cu, FX1_ISP_RESIZER_CTRL_OFFSET, en | (FX1_ISP_RESIZER_CTRL_SCALE_RES_3840X2160 << sh), 402);
   FX1_CHECK_EQ(ow(405), 2688);
   FX1_CHECK_EQ(cu.resizer_published(405).scaling ? 1 : 0, 0);
   wr(cu, FX1_ISP_RESIZER_CTRL_OFFSET, en | (0xFu << sh), 410);
   FX1_CHECK_EQ(ow(413), 2688);
   // The Bayer trim is part of the key (active size).
   wr(cu, FX1_ISP_COMMON_BAYER_OFFSET, FX1_ISP_COMMON_BAYER_PATTERN_BGGR, 420);
   FX1_CHECK_EQ(ow(422), 2688);
   FX1_CHECK_EQ(ow(423), 2686);
   FX1_CHECK_EQ(oh(423), 1518);
   // `updated` has no effect.
   wr(cu, FX1_ISP_RESIZER_CTRL_OFFSET, en | FX1_ISP_RESIZER_CTRL_UPDATED_MASK |
                                           (FX1_ISP_RESIZER_CTRL_SCALE_RES_1920X1080 << sh), 500);
   FX1_CHECK_EQ(ow(536), 2686);
   FX1_CHECK_EQ(ow(537), 1920);
   // COMMON soft reset: the reset set, then a derivation from the live
   // configuration after the 15-cycle pulse; a frame started earlier is not scaled.
   wr(cu, FX1_ISP_COMMON_CTRL_OFFSET, FX1_ISP_COMMON_CTRL_SOFT_RST_MASK, 600);
   FX1_CHECK_EQ(ow(601), 1920);
   FX1_CHECK_EQ(oh(601), 1080);
   FX1_CHECK_EQ(cu.resizer_published(601).key.valid ? 1 : 0, 0);
   cu.accepted_sof(605);
   FX1_CHECK_EQ(cu.resizer_frame().key.valid ? 1 : 0, 0);
   const std::uint64_t r0 = 600 + 15;  // E0 at the end of the pulse
   FX1_CHECK_EQ(ow(r0 + 35), 1920);
   FX1_CHECK_EQ(cu.resizer_published(r0 + 35).key.valid ? 1 : 0, 0);
   FX1_CHECK_EQ(cu.resizer_published(r0 + 36).key.valid ? 1 : 0, 1);
   FX1_CHECK_EQ(cu.resizer_published(r0 + 36).key.width, 2686);
   // The DMA-only soft reset leaves the Resizer alone.
   wr(cu, FX1_ISP_DMA_CTRL_OFFSET, FX1_ISP_DMA_CTRL_SOFT_RESET_MASK, 700);
   FX1_CHECK_EQ(cu.resizer_published(701).key.valid ? 1 : 0, 1);
   FX1_CHECK_EQ(ow(701), 1920);
   // A settled (debug) read shows what will be published.
   {
      // Two writes before one core edge give one key: only the last one is
      // derived (review U4-R3); E0 on the next edge, published at E35.
      control_unit d;
      wr(d, FX1_ISP_COMMON_FRAME_WIDTH_OFFSET, 3840, 10);
      wr(d, FX1_ISP_COMMON_FRAME_HEIGHT_OFFSET, 2160, 11);
      wr(d, FX1_ISP_RESIZER_CTRL_OFFSET, en | (FX1_ISP_RESIZER_CTRL_SCALE_RES_1280X720 << sh), 100);
      wr(d, FX1_ISP_RESIZER_CTRL_OFFSET, en | (FX1_ISP_RESIZER_CTRL_SCALE_RES_640X480 << sh), 100);
      FX1_CHECK_EQ(d.csr_read(FX1_ISP_RESIZER_OUT_W_OFFSET, 136), 3840);
      FX1_CHECK_EQ(d.csr_read(FX1_ISP_RESIZER_OUT_W_OFFSET, 137), 640);
      // A key written during the soft-reset pulse: the derivation starts from
      // the live key at the end of the pulse (MAS Table 124).
      wr(d, FX1_ISP_COMMON_CTRL_OFFSET, FX1_ISP_COMMON_CTRL_SOFT_RST_MASK, 200);  // pulse to 215
      wr(d, FX1_ISP_RESIZER_CTRL_OFFSET, en | (FX1_ISP_RESIZER_CTRL_SCALE_RES_960X540 << sh), 205);
      FX1_CHECK_EQ(d.csr_read(FX1_ISP_RESIZER_CTRL_OFFSET, 206) >> sh & 0xFu,
                   FX1_ISP_RESIZER_CTRL_SCALE_RES_960X540);  // RW accepted in the pulse
      FX1_CHECK_EQ(d.resizer_published(250).key.valid ? 1 : 0, 0);
      FX1_CHECK_EQ(d.csr_read(FX1_ISP_RESIZER_OUT_W_OFFSET, 251), 960);  // E0 215, E35 250
      FX1_CHECK_EQ(d.resizer_published(251).key.valid ? 1 : 0, 1);
      // Worst case for software: a key changed just after E0 of a running
      // derivation is published within two derivations of the change.
      wr(d, FX1_ISP_RESIZER_CTRL_OFFSET, en | (FX1_ISP_RESIZER_CTRL_SCALE_RES_1280X720 << sh), 300);  // E0 301
      wr(d, FX1_ISP_RESIZER_CTRL_OFFSET, en | (FX1_ISP_RESIZER_CTRL_SCALE_RES_640X360 << sh), 302);
      FX1_CHECK_EQ(d.csr_read(FX1_ISP_RESIZER_OUT_W_OFFSET, 371), 960);  // 336 stale, then E0 336, E35 371
      FX1_CHECK_EQ(d.csr_read(FX1_ISP_RESIZER_OUT_W_OFFSET, 372), 640);
      FX1_CHECK(372 - 302 <= 2u * (control_unit::resizer_derivation_cycles + 1));
      // A pass-through key written during the pulse is not published before
      // the pulse ends: the reset set stays until then.
      wr(d, FX1_ISP_COMMON_CTRL_OFFSET, FX1_ISP_COMMON_CTRL_SOFT_RST_MASK, 400);  // pulse to 415
      wr(d, FX1_ISP_RESIZER_CTRL_OFFSET, FX1_ISP_RESIZER_CTRL_SCALE_RES_640X360 << sh, 405);  // EN = 0
      FX1_CHECK_EQ(d.resizer_published(416).key.valid ? 1 : 0, 0);
      FX1_CHECK_EQ(d.resizer_published(417).key.valid ? 1 : 0, 1);  // E0 415, published 416
      FX1_CHECK_EQ(d.csr_read(FX1_ISP_RESIZER_OUT_W_OFFSET, 417), 3840);
   }
   wr(cu, FX1_ISP_RESIZER_CTRL_OFFSET, en | (FX1_ISP_RESIZER_CTRL_SCALE_RES_640X480 << sh), 800);
   FX1_CHECK_EQ(rd(cu, FX1_ISP_RESIZER_OUT_W_OFFSET), 640);
   FX1_CHECK_EQ(ow(801), 1920);
}

// The frame scales only with a set derived for its own raster (U4-A1): a frame
// started before the derivation of a new geometry finished passes unscaled.
void test_resizer_frame_raster() {
   control_unit cu;
   wr(cu, FX1_ISP_COMMON_FRAME_WIDTH_OFFSET, 1280, 1);
   wr(cu, FX1_ISP_COMMON_FRAME_HEIGHT_OFFSET, 720, 2);
   wr(cu, FX1_ISP_RESIZER_CTRL_OFFSET,
      FX1_ISP_RESIZER_CTRL_EN_MASK | (FX1_ISP_RESIZER_CTRL_SCALE_RES_640X360 << FX1_ISP_RESIZER_CTRL_SCALE_SHIFT), 3);
   cu.accepted_sof(100);  // published: 1280 x 720 -> 640 x 360
   pipe::pipeline_config cfg = pipe::snapshot_config(cu);
   const pipe::frame_geometry same = pipe::output_geometry(cfg, 1280, 720);
   FX1_CHECK_EQ(same.width, 640);
   FX1_CHECK_EQ(same.height, 360);
   const pipe::frame_geometry other = pipe::output_geometry(cfg, 800, 600);  // raster of another key
   FX1_CHECK_EQ(other.width, 800);
   FX1_CHECK_EQ(other.height, 600);
   // After a COMMON soft reset, before the derivation: the reset set scales nothing.
   wr(cu, FX1_ISP_COMMON_CTRL_OFFSET, FX1_ISP_COMMON_CTRL_SOFT_RST_MASK, 200);
   cu.accepted_sof(210);
   cfg = pipe::snapshot_config(cu);
   const pipe::frame_geometry early = pipe::output_geometry(cfg, 1280, 720);
   FX1_CHECK_EQ(early.width, 1280);
   FX1_CHECK_EQ(early.height, 720);
}

// CCM and CNF configuration as the frame samples it (no commit gate since 07/10).
void test_commit_gates() {
   control_unit cu;
   // CCM and CNF have no gate (owner Q1, DEC-24/27 superseded): the value the
   // pipeline samples at SOF is the current register value, `updated` or not.
   wr(cu, FX1_ISP_CCM_CRR_OFFSET, 0x100, 11);
   FX1_CHECK_EQ(cu.committed(FX1_ISP_CCM_CRR_OFFSET), 0x100);
   wr(cu, FX1_ISP_CNF_LUMA_TH_OFFSET, 20, 14);
   FX1_CHECK_EQ(cu.committed(FX1_ISP_CNF_LUMA_TH_OFFSET), 20);
   // Soft reset preserves configuration.
   wr(cu, FX1_ISP_COMMON_CTRL_OFFSET, FX1_ISP_COMMON_CTRL_SOFT_RST_MASK, 16);
   FX1_CHECK_EQ(cu.committed(FX1_ISP_CCM_CRR_OFFSET), 0x100);
   // External reset returns them to the register reset values.
   cu.external_reset();
   FX1_CHECK_EQ(cu.committed(FX1_ISP_CCM_CRR_OFFSET), 0x200);
   FX1_CHECK_EQ(cu.committed(FX1_ISP_CNF_LUMA_TH_OFFSET), 0);
}

void test_external_reset() {
   control_unit cu;
   wr(cu, FX1_ISP_COMMON_SCRATCH_OFFSET, 0x1234);
   wr(cu, FX1_ISP_GAMMA_LUT_DATA_OFFSET, 0x777);
   wr(cu, FX1_ISP_EE_LUT_WDATA_OFFSET, 0x1);
   cu.hw_write(FX1_ISP_IDMA_FRAME_COUNT_OFFSET, 0xFFFFFFFFu, 9);
   cu.hw_hold(FX1_ISP_DMA_ERR_OFFSET, 0x2, true, 1);
   wr(cu, FX1_ISP_DMA_CTRL_OFFSET, FX1_ISP_DMA_CTRL_SOFT_RESET_MASK, 50);
   cu.external_reset();
   FX1_CHECK_EQ(rd(cu, FX1_ISP_COMMON_SCRATCH_OFFSET), 0);
   FX1_CHECK_EQ(rd(cu, FX1_ISP_IDMA_FRAME_COUNT_OFFSET), 0);
   FX1_CHECK_EQ(rd(cu, FX1_ISP_DMA_ERR_OFFSET), 0);
   FX1_CHECK_EQ(cu.regs().held(FX1_ISP_DMA_ERR_OFFSET), 0);
   // LUTs: undefined after i_rst_n (MAS §6.5.2), the deterministic poison
   // pattern of the model and the reference (CSR-16 superseded).
   FX1_CHECK_EQ(cu.gamma_lut()[0], 0xA5A5u & 0xFFFu);
   FX1_CHECK_EQ(cu.gamma_lut()[1], (0x9E37u ^ 0xA5A5u) & 0xFFFu);
   FX1_CHECK_EQ(cu.ee_table(0)[0], 0xA5A5u);
   FX1_CHECK_EQ(cu.ee_table(1)[0], ((64u * 0x9E37u) ^ 0xA5A5u) & 0xFFFFu);
   FX1_CHECK(!cu.soft_reset_active(51));
   wr(cu, FX1_ISP_IDMA_BUF_VALID_OFFSET, 0x2, 51);  // command block lifted by i_rst_n
   FX1_CHECK_EQ(rd(cu, FX1_ISP_IDMA_BUF_VALID_OFFSET), 0x2);
}

}  // namespace

// FRAME_COUNTER (MAS Table 160): core clocks since the release of i_rst_n,
// modulo 2^32, computed at the read edge; zero while i_rst_n is asserted;
// unaffected by soft resets (VP decision U5-A1).
void test_time_base() {
   control_unit cu;
   FX1_CHECK_EQ(cu.csr_read(FX1_ISP_FRAME_COUNTER_OFFSET, 0), 0u);
   FX1_CHECK_EQ(cu.csr_read(FX1_ISP_FRAME_COUNTER_OFFSET, 12345), 12345u);
   FX1_CHECK_EQ(cu.csr_read(FX1_ISP_FRAME_COUNTER_OFFSET, (1ull << 32) + 7), 7u);  // wraps
   FX1_CHECK_EQ(cu.csr_read(FX1_ISP_FRAME_COUNTER_OFFSET, (1ull << 32) - 1), 0xFFFFFFFFu);
   // A settled read sees the last edge the control unit processed.
   cu.csr_write(FX1_ISP_COMMON_SCRATCH_OFFSET, 1, 0xFFFFFFFFu, 500);
   FX1_CHECK_EQ(cu.csr_read(FX1_ISP_FRAME_COUNTER_OFFSET), 500u);
   // Soft resets do not restart it.
   cu.csr_write(FX1_ISP_DMA_CTRL_OFFSET, FX1_ISP_DMA_CTRL_SOFT_RESET_MASK, 0xFFFFFFFFu, 600);
   FX1_CHECK_EQ(cu.csr_read(FX1_ISP_FRAME_COUNTER_OFFSET, 700), 700u);
   cu.csr_write(FX1_ISP_COMMON_CTRL_OFFSET, FX1_ISP_COMMON_CTRL_SOFT_RST_MASK, 0xFFFFFFFFu, 800);
   FX1_CHECK_EQ(cu.csr_read(FX1_ISP_FRAME_COUNTER_OFFSET, 900), 900u);
   // i_rst_n: zero while asserted, then counting from the release edge.
   cu.external_reset();
   FX1_CHECK_EQ(cu.csr_read(FX1_ISP_FRAME_COUNTER_OFFSET, 1000), 0u);
   FX1_CHECK_EQ(cu.frame_counter(5000), 0u);
   cu.release_reset(2000);
   FX1_CHECK_EQ(cu.frame_counter(1999), 0u);
   FX1_CHECK_EQ(cu.frame_counter(2000), 0u);
   FX1_CHECK_EQ(cu.frame_counter(2001), 1u);
   FX1_CHECK_EQ(cu.csr_read(FX1_ISP_FRAME_COUNTER_OFFSET, 2000 + (1ull << 32) + 3), 3u);
   // Read-only: the stored word does not change the count.
   cu.csr_write(FX1_ISP_FRAME_COUNTER_OFFSET, 0xFFFFFFFFu, 0xFFFFFFFFu, 2100);
   FX1_CHECK_EQ(cu.csr_read(FX1_ISP_FRAME_COUNTER_OFFSET, 2100), 100u);
   FX1_CHECK_EQ(cu.regs().peek(FX1_ISP_FRAME_COUNTER_OFFSET), 0u);
}

int main() {
   test_time_base();
   test_table_shape();
   test_literal_resets();
   test_access_sweep();
   test_unmapped_and_reserved();
   test_strobes_and_alias();
   test_w1c_semantics();
   test_level_hold();
   test_irq_aggregation();
   test_error_aggregation();
   test_stats_ready();
   test_soft_reset(FX1_ISP_COMMON_CTRL_OFFSET, FX1_ISP_COMMON_CTRL_SOFT_RST_MASK);
   test_soft_reset(FX1_ISP_DMA_CTRL_OFFSET, FX1_ISP_DMA_CTRL_SOFT_RESET_MASK);
   test_aec_commit_across_soft_reset();
   test_frame_start_and_done();
   test_commands_recorded();
   test_commands_cleared_on_reset();
   test_gamma_lut_port();
   test_ee_lut_port();
   test_ee_profiles();
   test_ee_radial_error();
   test_gtm_lut_gate();
   test_resizer_geometry();
   test_resizer_publication();
   test_resizer_frame_raster();
   test_commit_gates();
   test_lsc_resize_during_load();
   test_lsc_loader();
   test_lsc_selection_at_sof();
   test_external_reset();
   return fx1_test::summary("fx1_isp_test_control");
}
