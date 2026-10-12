// SPDX-License-Identifier: Apache-2.0
// Statistics configuration and results of AEC (HAS §6.21), AWB (§6.22) and
// AF (§6.23), shared by the pipeline (accumulation) and the control unit
// (publication and readout). Review items: plan/alg/ALG_F.
//
// Three kinds of storage:
// - the AEC zone + histogram memory is a single in-place memory, restarted at
//   the SOF of an enabled frame and updated row by row, so software reads the
//   partial sums of the frame in progress (MAS §6.21.4.3, DEC-30);
// - AWB results live in two banks owned by software through acquire/release
//   (MAS §6.22.4, §6.22.5.3): global, zones and identity of one frame, stable
//   while READY or LEASED;
// - AEC global results and AF scores are replaced at publication.
#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace cdc::components::fx1_isp {

// AEC active configuration: the shadow set transferred at the SOF after
// AEC_CTRL.commit (#296). The image height is the pipeline geometry
// (CSR-17, ALG-AEC-05).
struct aec_config {
   bool en = false;
   std::uint32_t nx = 32, ny = 24, zw = 120, zh = 90;
   std::uint32_t min_clip = 0, max_clip = 4095;
   std::uint32_t th_ue = 256, th_oe = 3840;
   bool zone_ok() const { return nx >= 1 && nx <= 32 && ny >= 1 && ny <= 24 && zw > 0 && zh > 0; }  // ALG-AEC-02
};

// AEC global results (double-buffered).
struct aec_result {
   std::array<std::uint64_t, 4> gsum{};  // R, Gr, Gb, B; 33-bit
   std::array<std::uint32_t, 4> gcnt{};  // 21-bit
};

// AEC zone + green histogram memory (single, in place). After restart()
// every word holds the empty value, so a zone not yet written in the
// current frame reads 0 / min 4095 / max 0 (ALG-AEC-03/04, owner query B20).
struct aec_memory {
   std::vector<std::array<std::uint32_t, 4>> zsum, zcnt;  // per zone and channel; 28 / 16 bit
   std::vector<std::uint32_t> oe, ue, gmin, gmax;          // green, per zone
   std::array<std::uint32_t, 64> hist{};                   // green, 22-bit
   std::uint32_t nx = 0, ny = 0;
   bool zones = false;  // zone path active for the frame (ALG-AEC-02)

   void restart(const aec_config &c) {
      nx = c.nx;
      ny = c.ny;
      zones = c.zone_ok();
      const std::size_t n = zones ? std::size_t{nx} * ny : 0;
      zsum.assign(n, {});
      zcnt.assign(n, {});
      oe.assign(n, 0);
      ue.assign(n, 0);
      gmin.assign(n, 4095);
      gmax.assign(n, 0);
      hist.fill(0);
   }
};

struct awb_config {
   bool en = false;
   std::uint32_t nx = 0, ny = 0;
   std::uint32_t under = 0, sat = 4095;
   bool zone_ok() const { return nx >= 1 && nx <= 64 && ny >= 1 && ny <= 32; }  // ALG-AWB-01
};

// AWB global results (double-buffered).
struct awb_result {
   std::uint64_t r = 0, g = 0, b = 0;  // 35-bit
   std::uint32_t n = 0;                // 23-bit
};

// AWB zone entries of one bank, for the grid captured at admission. An entry
// not touched by the frame reads 0 (written flags cleared at admission).
struct awb_memory {
   std::vector<std::array<std::uint64_t, 3>> zsum;
   std::vector<std::uint32_t> zcnt;
   std::uint32_t nx = 0, ny = 0;
   bool zones = false;

   void restart(const awb_config &c) {
      nx = c.nx;
      ny = c.ny;
      zones = c.zone_ok();
      const std::size_t n = zones ? std::size_t{nx} * ny : 0;
      zsum.assign(n, {});
      zcnt.assign(n, 0);
   }
};

// One AWB result bank (MAS §6.22.4.3, §6.22.5.3): FREE -> WRITING (admitted
// at an enabled SOF) -> READY (published after the frame's last sample) ->
// LEASED (acquired by software) -> FREE (released). READY and LEASED banks are
// never written.
struct awb_bank {
   enum class state { free, writing, ready, leased };
   state st = state::free;
   awb_memory mem;                     // zones of the captured grid (mem.nx, mem.ny)
   awb_result global;                  // global snapshot, published with the bank
   std::uint32_t frame_id = 0, context_id = 0;
   std::uint64_t stamp = 0;            // cycle of the last state change: a bank
                                       // freed or published on an edge is
                                       // eligible from the following edge
};

struct af_result {
   std::array<std::uint32_t, 16> fv{};  // 4x4 zones, raster order, 32-bit
};

}  // namespace cdc::components::fx1_isp
