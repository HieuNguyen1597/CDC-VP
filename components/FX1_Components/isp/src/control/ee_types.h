// SPDX-License-Identifier: Apache-2.0
// EE resident profiles (MAS §6.18.4.1): the scalar registers each profile
// owns and the set a frame uses, shared by the control unit (storage,
// sampling at SOF) and the pipeline (datapath).
#pragma once

#include <array>
#include <cstdint>

namespace cdc::components::fx1_isp {

// EE profile registers EE_ALPHA .. EE_RADIAL_GAIN_23, in offset order: each of
// the three resident profiles owns one set.
enum ee_scalar : unsigned {
   ee_alpha, ee_beta, ee_clamp, ee_feature_en, ee_radial_center,
   ee_radial_r2_th0, ee_radial_r2_th1, ee_radial_r2_th2, ee_radial_gain_01, ee_radial_gain_23,
   ee_num_scalars
};

// What an EE frame uses, sampled at its accepted SOF: enable, profile and that
// profile's scalars. The profile's LUTs stay live (MAS §6.18.4.1, §9.2).
struct ee_frame_set {
   bool en = false;
   unsigned profile = 0;
   std::array<std::uint32_t, ee_num_scalars> scalar{};
   std::uint32_t width = 0, height = 0;  // active image size of the frame
};

}  // namespace cdc::components::fx1_isp
