// =============================================================================
// File: tools/gen_test_datasets.cpp
// Description: Generates deterministic input, LUT, and golden output dataset
//              files for rtl_ref_gvu_top testbenches:
//                - target/test/residual_add/{tile,vec}/
//                - target/test/gemm_fused/{tile,vec}/
//              Matches the Softmax testbench dataset format (tile: 64x64=4096,
//              vec: 1x64=64) and MAS Section 5.2.1, 5.2.2, and 5.2.6.
// =============================================================================

#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <cstdint>
#include <cassert>
#include "has/rtl_ref_gvu_fu.h"

using namespace rtl_ref_gvu;

// Write vector of values (one per line) to file
template <typename T>
static void write_lines(const std::string& path, const std::vector<T>& vals)
{
    std::ofstream ofs(path);
    if (!ofs.is_open()) {
        std::cerr << "[ERROR] Cannot open file for writing: " << path << "\n";
        std::exit(1);
    }
    for (const auto& v : vals) {
        ofs << static_cast<int64_t>(v) << "\n";
    }
    std::cout << "[GEN] Wrote " << vals.size() << " lines to " << path << "\n";
}

// Generate Residual Add dataset (MAS Section 5.2.6)
static void gen_residual_add(const std::string& dir, int count)
{
    // Quantization parameters matching tb_gvu_top_residual_add.cpp
    const int32_t scale_a   = 192; // 0.75 * 2^8
    const uint8_t shift_a   = 8;
    const int8_t  zp_a      = -4;
    const int32_t scale_b   = 128; // 0.50 * 2^8
    const uint8_t shift_b   = 8;
    const int8_t  zp_b      = 2;
    const int32_t scale_out = 256; // 0.50 * 2^9
    const uint8_t shift_out = 9;
    const int8_t  zp_out    = 5;
    const RoundMode rnd     = RND_NEAR_UP;

    std::vector<int8_t>  in_a(count);
    std::vector<int8_t>  in_b(count);
    std::vector<int32_t> golden_sum(count);
    std::vector<int8_t>  golden_out(count);

    for (int idx = 0; idx < count; ++idx) {
        int8_t a, b;
        if (idx == 0)      { a = -128; b = -128; }
        else if (idx == 1) { a = 127;  b = 127;  }
        else if (idx == 2) { a = -128; b = 127;  }
        else if (idx == 3) { a = 127;  b = -128; }
        else if (idx == 4) { a = 0;    b = 0;    }
        else if (idx == 5) { a = zp_a; b = zp_b; }
        else {
            a = static_cast<int8_t>(((idx * 37 + 13) % 256) - 128);
            b = static_cast<int8_t>(((idx * 73 + 59) % 256) - 128);
        }
        in_a[idx] = a;
        in_b[idx] = b;

        // Stage 1: vbank0 (A) + vbank1 (B) -> vbank2 (int32 sum)
        const uint32_t da = adder32_lane(ADD_16, 0, 1, false,
            static_cast<uint32_t>(static_cast<int32_t>(a)) & 0xFFFFu,
            static_cast<uint32_t>(zp_a) & 0xFFFFu) & 0xFFFFu;
        const uint32_t db = adder32_lane(ADD_16, 0, 1, false,
            static_cast<uint32_t>(static_cast<int32_t>(b)) & 0xFFFFu,
            static_cast<uint32_t>(zp_b) & 0xFFFFu) & 0xFFFFu;

        const uint64_t a40 = static_cast<uint64_t>(static_cast<int64_t>(sext32(da, 16))) & 0xFFFFFFFFFFull;
        const u128 pa = vmul48x40_lane(MB_40X32, a40, static_cast<uint32_t>(scale_a)).out_prod;
        const uint64_t pb = vmul32x32_lane(MA_32X16, static_cast<uint32_t>(scale_b), db);

        const uint32_t ra = vroundshift72_lane(pa, shift_a, rnd, NARROW_SATURATE, NW_32).out_data;
        const uint32_t rb = vroundshift64_lane(pb, shift_b, rnd, NARROW_SATURATE, NW_32).out_data;

        const int32_t sum32 = static_cast<int32_t>(adder32_lane(ADD_32, 0, 0, false, ra, rb));
        golden_sum[idx] = sum32;

        // Stage 2: vbank2 (int32 sum) -> vbank0 (int8 output)
        const uint64_t po = vmul32x32_lane(MA_32X32, static_cast<uint32_t>(sum32), static_cast<uint32_t>(scale_out));
        const uint32_t rs = vroundshift64_lane(po, shift_out, rnd, NARROW_SATURATE, NW_16).out_data & 0xFFFFu;
        const uint32_t zs = adder32_lane(ADD_16, 0, 0, false, rs, static_cast<uint32_t>(zp_out) & 0xFFFFu) & 0xFFFFu;
        const uint32_t cl = clamp_lane(zs, 0xFF80u, 0x007Fu, 16, true) & 0xFFu;
        golden_out[idx] = static_cast<int8_t>(cl);
    }

    write_lines(dir + "/input_a.txt",       in_a);
    write_lines(dir + "/input_b.txt",       in_b);
    write_lines(dir + "/golden_sum.txt",    golden_sum);
    write_lines(dir + "/golden_output.txt", golden_out);
}

// Generate GEMM Fused dataset (MAS Section 5.2.1 & 5.2.2)
static void gen_gemm_fused(const std::string& dir, int count)
{
    // Quantization parameters matching tb_gvu_top_gemm_fused.cpp
    const int32_t gemm_scale = 320; // 1.25 * 2^8
    const int8_t  gemm_shift = 8;
    const int8_t  gemm_zp    = -3;

    // 256-entry Activation LUT (Leaky-ReLU: x < 0 ? x / 4 : x, indexed by addr = (uint8_t(x) + 128) & 0xFF)
    std::vector<uint8_t> lut_act(256);
    for (int addr = 0; addr < 256; ++addr) {
        int x_q = addr - 128; // signed [-128, 127]
        int y_q = (x_q < 0) ? (x_q / 4) : x_q;
        if (y_q < -128) y_q = -128;
        if (y_q > 127)  y_q = 127;
        lut_act[addr] = static_cast<uint8_t>(static_cast<int8_t>(y_q));
    }

    std::vector<int32_t> input_psum(count);
    std::vector<int8_t>  golden_out(count);

    const uint32_t zp16 = sext8to16(static_cast<uint32_t>(static_cast<uint8_t>(gemm_zp)));

    for (int idx = 0; idx < count; ++idx) {
        int32_t psum;
        if (idx == 0)      psum = -200;
        else if (idx == 1) psum = 200;
        else if (idx == 2) psum = -100;
        else if (idx == 3) psum = 100;
        else if (idx == 4) psum = 0;
        else if (idx == 5) psum = 24;
        else {
            psum = ((idx * 41 + 19) % 321) - 160; // range [-160, 160]
        }
        input_psum[idx] = psum;

        const uint64_t prod = vmul32x32_lane(MA_32X32, static_cast<uint32_t>(psum), static_cast<uint32_t>(gemm_scale));
        const uint32_t rs16 = vroundshift64_lane(prod, static_cast<unsigned>(gemm_shift) & 0x3Fu, RND_NEAR_AWAY, NARROW_WRAP, NW_16).out_data;
        const uint32_t add16 = adder32_lane(ADD_16, 0, 0, false, rs16 & 0xFFFFu, zp16) & 0xFFFFu;
        const uint32_t cl8 = clamp_lane(add16, static_cast<uint32_t>(-128), 127, 16, true) & 0xFFu;
        const unsigned lut_addr = ((cl8 & 0xFFu) + 128u) & 0xFFu;
        golden_out[idx] = static_cast<int8_t>(lut_act[lut_addr]);
    }

    write_lines(dir + "/lut_act.txt",       lut_act);
    write_lines(dir + "/input.txt",         input_psum);
    write_lines(dir + "/golden_output.txt", golden_out);
}

int main()
{
    gen_residual_add("target/test/residual_add/tile", 64 * 64);
    gen_residual_add("target/test/residual_add/vec",  1 * 64);
    gen_gemm_fused("target/test/gemm_fused/tile",     64 * 64);
    gen_gemm_fused("target/test/gemm_fused/vec",      1 * 64);
    return 0;
}
