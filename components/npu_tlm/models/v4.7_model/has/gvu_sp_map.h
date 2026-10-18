// gvu_sp_map.h -- Scratchpad SRAM use of every vector-unit pipeline stage (MAS V1.0 "Scratchpad SRAM": tables "Scratchpad
// SRAM banks & ports detailed arbitrary" and "Scratchpad SRAM banks capacity summary"). Structural model, like gvu_vrf.h:
// it holds no data. It answers two questions:
//   1. ports    -- do the accesses a stage makes in the same cycle fit the banks (one physical port per vbank, two on the
//                  sbank) and does every access use a logical port the bank has (has/gvu_lsu.h lsu_port())?
//   2. capacity -- how many bytes does a pipeline keep in each bank for `rows` rows processed in parallel, and how many
//                  rows fit a given Scratchpad (Knobs::sp_v0 / sp_v1 / sp_v2 / sp_s, bytes per half)?
// tools/has/tb_gvu_mas.cpp checks both against the tables of the MAS.
#ifndef HAS_GVU_SP_MAP_H
#define HAS_GVU_SP_MAP_H

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>
#include "has/gvu_lsu.h"
#include "has/gvu_quant.h"

namespace has
{
    // One Scratchpad access of a stage. Accesses with the same `group` happen in the same cycle; a stage with two groups
    // has a streaming part (group 0) and an end-of-row part (group 1), as the table marks with "Sequence".
    struct SpAccess
    {
        const char *what;
        LsuBank bank;
        LsuMode mode;
        bool write;
        int group;
    };
    struct SpStage
    {
        const char *pipeline;
        std::vector<SpAccess> acc;
    };

    // Table "Scratchpad SRAM banks & ports detailed arbitrary", row by row.
    inline const std::vector<SpStage> &pipeline_sp_access()
    {
        const LsuBank S = LsuBank::SBANK, V0 = LsuBank::VBANK0, V1 = LsuBank::VBANK1, V2 = LsuBank::VBANK2;
        const LsuMode I32 = LsuMode::S32, I8 = LsuMode::S8, V32 = LsuMode::V32I32, V16 = LsuMode::V32I16, V8 = LsuMode::V32I8;
        static const std::vector<SpStage> t = {
            {"Requant", {{"out_scale", S, I32, false, 0}, {"out_shift", S, I8, false, 0}}},
            {"Dequant", {{"in_scale", S, I32, false, 0}, {"in_shift", S, I8, false, 0}}},
            {"GEMM_FUSED", {{"out_scale", S, I32, false, 0}, {"out_shift", S, I8, false, 0}}},
            {"Softmax stage 1", {{"x", V0, V8, false, 0}, {"row_max", V1, V8, true, 1}}},
            {"Softmax stage 2", {{"x", V0, V8, false, 0}, {"row_max", V1, V8, false, 0}, {"e", V2, V8, true, 0}, {"row_sigma", V1, V32, true, 1}}},
            {"Softmax stage 3", {{"row_sigma", V1, V32, false, 0}, {"e", V2, V8, false, 0}, {"attention score", V0, V8, true, 0}}},
            {"FUSED_ATTN stage 1", {{"x", V0, V8, true, 0}, {"mask", V2, V8, false, 0}, {"row_max", V1, V8, true, 0}}},
            {"LayerNorm stage 1", {{"x", V0, V8, false, 0}, {"row_sum", V1, V32, true, 1}}},
            {"LayerNorm stage 2", {{"x", V0, V8, false, 0}, {"row_sum", V1, V32, false, 0}, {"D", V2, V32, true, 0}, {"var", V0, V8, true, 1}}},
            {"LayerNorm stage 3", {{"var", V0, V8, false, 0}, {"var + epsilon", V1, V16, true, 0}}},
            {"LayerNorm stage 4", {{"var + epsilon", V1, V16, false, 0}, {"D", V2, V32, false, 0}, {"norm", V0, V8, true, 0}}},
            {"LayerNorm stage 5", {{"norm", V0, V8, false, 0}, {"gamma", S, I8, false, 0}, {"beta", S, I32, false, 0}, {"result", V1, V8, true, 0}}},
            {"Residual Add stage 1", {{"tensor A", V0, V8, false, 0}, {"tensor B", V1, V8, false, 0}, {"sum", V2, V32, true, 0}}},
            {"Residual Add stage 2", {{"sum", V2, V32, false, 0}, {"result", V0, V8, true, 0}}},
            {"Max pool", {{"tensor", V0, V8, false, 0}, {"result", V1, V8, true, 0}}},
            {"Average pool", {{"tensor", V0, V8, false, 0}, {"result", V1, V8, true, 0}}},
        };
        return t;
    }

    // Number of port problems of one stage; `why` receives the first one. A problem is an access on a logical port the bank
    // does not have, two same-cycle accesses on a single-port bank, or two same-cycle accesses on one sbank port.
    inline int sp_port_conflicts(const SpStage &st, std::string *why = nullptr)
    {
        int bad = 0;
        auto note = [&](const std::string &s) { if (!bad && why) *why = s; bad++; };
        for (size_t i = 0; i < st.acc.size(); i++)
        {
            const SpAccess &a = st.acc[i];
            if (lsu_port(a.bank, a.mode) < 0)
                note(std::string(a.what) + ": " + lsu_bank_name(a.bank) + " has no " + lsu_mode_name(a.mode) + " port");
            for (size_t j = i + 1; j < st.acc.size(); j++)
            {
                const SpAccess &b = st.acc[j];
                if (a.group != b.group || a.bank != b.bank) continue;
                if (!lsu_dual_port(a.bank) || lsu_port(a.bank, a.mode) == lsu_port(b.bank, b.mode))
                    note(std::string(a.what) + " and " + b.what + " use " + lsu_bank_name(a.bank) + " in the same cycle");
            }
        }
        return bad;
    }

    // Bytes per bank (one half of the double buffer).
    struct SpBytes
    {
        uint64_t v0{0}, v1{0}, v2{0}, s{0};
        uint64_t total() const { return v0 + v1 + v2 + s; }
    };

    // Per-bank maxima of the MAS capacity table, as printed (d_model 512 = maximum, 256 = typical). Notes on the printed
    // numbers: vbank#0 "24756" next to vbank#2 "24576" for the same number of int8 elements (32 x 768 = 24 576); the sbank
    // column gives 1280 B at d_model 512 and 2560 B at 256, the reverse of gamma int8 + beta int32 per channel (5 B x d).
    inline SpBytes mas_capacity_table(int d_model)
    {
        SpBytes b;
        b.v0 = 24756;
        b.v1 = 1024;
        b.v2 = d_model >= 512 ? 65536 : 32768;
        b.s = d_model >= 512 ? 1280 : 2560;
        return b;
    }

    enum class SpPipeline { Softmax, LayerNorm, ResidualAdd, Pool };

    // Bytes a pipeline keeps per bank for `rows` rows of `n` elements in parallel (n = sequence length L for Softmax, hidden
    // size H for LayerNorm, vector length for Residual Add; Pool: n = input elements of the window group, rows = outputs).
    //   Softmax     vbank#0 x / attention score rows*n ; vbank#1 row_max rows + row_sigma 4*rows ; vbank#2 e rows*n
    //   LayerNorm   vbank#0 x rows*n + var rows ; vbank#1 row_sum 4*rows (later var + epsilon 2*rows) ;
    //               vbank#2 D 4*rows*n ; sbank gamma n + beta 4*n. The stage-5 result (rows*n int8) is mapped to vbank#1
    //               by the port table but has no size in the capacity table; ln_result_in_v1 = true adds it to vbank#1,
    //               false assumes it leaves through the AXI4 master as it is produced.
    //   Residual    vbank#0 A rows*n ; vbank#1 B rows*n ; vbank#2 sum 4*rows*n ; sbank 3 scales + 3 shifts
    // s_word32 = true: every scalar parameter takes an int32 word of the sbank (the int8 port reads the low byte of a
    // word in the RTL), so gamma + beta need 8 B per channel and the six ADD parameters 24 B.
    inline SpBytes sp_need(SpPipeline p, uint64_t rows, uint64_t n, bool ln_result_in_v1 = false, bool s_word32 = false)
    {
        SpBytes b;
        switch (p)
        {
        case SpPipeline::Softmax: b.v0 = rows * n; b.v1 = rows + 4 * rows; b.v2 = rows * n; break;
        case SpPipeline::LayerNorm:
            b.v0 = rows * n + rows; b.v1 = ln_result_in_v1 ? std::max<uint64_t>(4 * rows, rows * n) : 4 * rows;
            b.v2 = 4 * rows * n; b.s = (s_word32 ? 8 : 5) * n;
            break;
        case SpPipeline::ResidualAdd: b.v0 = rows * n; b.v1 = rows * n; b.v2 = 4 * rows * n; b.s = s_word32 ? 24 : 3 * 4 + 3; break;
        case SpPipeline::Pool: b.v0 = n; b.v1 = rows; break;
        }
        return b;
    }
    inline bool sp_fits(const SpBytes &need, const Knobs &k)
    {
        return need.v0 <= k.sp_v0 && need.v1 <= k.sp_v1 && need.v2 <= k.sp_v2 && (k.sp_s == 0 || need.s <= k.sp_s);
    }
    // Largest number of rows (1..32 lanes) of `n` elements the Scratchpad of `k` holds for pipeline `p`; 0 = not even one.
    inline int sp_max_rows(SpPipeline p, uint64_t n, const Knobs &k)
    {
        for (int rows = 32; rows >= 1; rows--)
            if (sp_fits(sp_need(p, uint64_t(rows), n, false, k.sp_s_word32 != 0), k)) return rows;
        return 0;
    }
} // namespace has

#endif // HAS_GVU_SP_MAP_H
