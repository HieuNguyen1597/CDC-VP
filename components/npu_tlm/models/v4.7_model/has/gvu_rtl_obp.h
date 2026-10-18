// gvu_rtl_obp.h -- optional execution of the GEMM_FUSED epilogue on the cycle-level pipeline of
// has/rtl_ref_gvu_requant.h (GvuGemmEpilogue: Requant pipeline, v32int8 register, LUT_act through the ported LUT unit).
// Off unless the environment variable HAS_OBP_RTL is set to a non-zero value; with it off nothing computed by the model
// changes. When on, the epilogue on the PSM -> SRAM-C write path (has::HasObp::apply_inline) first computes its own
// result, then sends the same beat through the pipeline:
//   values   the lanes written to the SRAM are those of the pipeline; every masked lane is compared with the model result
//            and counted as "same" or "differs";
//   cycles   the tail behind the last write of a core pass is the depth of the pipeline (GvuGemmEpilogue::kLatency)
//            instead of the latency constant of the model.
// What stays as in the model: the write still lands in the cycle of the PSM write (the pipeline is run to completion
// inside the call, one beat at a time), the per-channel parameters are set by the instruction path without bus cycles,
// and the activation table is loaded over the host port of the model.
// Not run on the pipeline (model result kept, counted as "not expressible"): a scale outside int32, a shift outside
// 0 .. 63, a zero point outside 16 bits, narrowing other than saturation to int16.
// A table is printed when the program exits ("[obp-rtl] ..." lines).
// HAS_OBP_DUMP=<file> (with HAS_OBP_RTL): every activation table and every beat run on the pipeline are also written to
// the file, in order, for replay outside the core (tools/has/tb_gvu_top_replay.cpp). Records, little endian:
//   'T' + 256 bytes                  activation table (index = int8 value + 128)
//   'B' + 9 int32 + 32 int32 + 32 int8  lane mask, scale, shift, zero point, rounding mode (has::RoundMode), activation
//                                    on, cycle of the beat (0 = not known), 2 reserved; the 32 psums; the 32 results of the
//                                    pipeline (masked lanes; the others 0)
// Writing the file changes no value and no cycle.
#ifndef HAS_RTL_OBP_H
#define HAS_RTL_OBP_H

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include "has/rtl_ref_gvu_requant.h"

namespace has
{
    class RtlObp
    {
    public:
        typedef rtl_ref_gvu::GvuGemmEpilogue Unit;
        static constexpr int kLatency = Unit::kLatency;

        static bool on() { return inst().on_; }
        static void table_loaded(const int8_t *table = nullptr)
        {
            RtlObp &p = inst();
            p.st_.tables++;
            if (table && p.dump_) { std::fputc('T', p.dump_); std::fwrite(table, 1, 256, p.dump_); }
        }
        // One beat of 32 lanes. out holds the model result on entry and the pipeline result on return (masked lanes).
        // round_mode: has::RoundMode. Returns false (out untouched) when the parameters do not fit the pipeline.
        template <class Vec>
        static bool beat(Unit &ep, const Vec &in, Vec &out, uint32_t mask, int64_t S, int s, int zp, int round_mode, bool sat16, bool act)
        {
            namespace R = rtl_ref_gvu;
            Stat &st = inst().st_;
            st.beats++;
            if (!sat16 || !fit(S, 32) || s < 0 || s > 63 || !fit(zp, 16) || round_mode < 0 || round_mode > 6) { st.not_expr++; return false; }
            Unit::In i;
            i.i_valid = true;
            for (size_t l = 0; l < size_t(R::GVU_LANES); l++) i.i_data[l] = static_cast<uint32_t>(static_cast<int32_t>(in[l]));
            i.i_scale = static_cast<int32_t>(S); i.i_shift = static_cast<unsigned>(s); i.i_zp = zp; i.i_round = map_round(round_mode);
            i.i_lane_mask = mask; i.i_act_en = act;
            int n = 0;
            Unit::Out o = ep.eval(i);
            while (!o.o_valid && n < 4 * kLatency)
            {
                ep.tick(i);
                i.i_valid = false;
                n++;
                o = ep.eval(i);
            }
            ep.tick(i);   // the beat leaves; the pipeline is empty again
            if (!o.o_valid) { st.no_out++; st.not_expr++; return false; }
            if (n != kLatency) st.lat_other++;
            for (size_t l = 0; l < size_t(R::GVU_LANES); l++)
            {
                if (!((mask >> l) & 1u)) continue;
                const int h = static_cast<int8_t>(o.o_data[l] & 0xFFu), m = static_cast<int>(out[l]);
                st.lanes++;
                if (h != m)
                    st.miss("x=%d S=%lld s=%d zp=%d act=%d model=%d pipeline=%d", static_cast<int>(in[l]), static_cast<long long>(S), s, zp, int(act), m, h);
                out[l] = h;
            }
            st.cycles += uint64_t(n);
            if (std::FILE *f = inst().dump_)
            {
                int32_t h[9] = {static_cast<int32_t>(mask), static_cast<int32_t>(S), s, zp, round_mode, act ? 1 : 0, 0, 0, 0};
                int32_t ps[R::GVU_LANES];
                int8_t res[R::GVU_LANES];
                for (size_t l = 0; l < size_t(R::GVU_LANES); l++)
                {
                    ps[l] = static_cast<int32_t>(in[l]);
                    res[l] = ((mask >> l) & 1u) ? static_cast<int8_t>(out[l]) : static_cast<int8_t>(0);
                }
                std::fputc('B', f); std::fwrite(h, 4, 9, f); std::fwrite(ps, 4, R::GVU_LANES, f); std::fwrite(res, 1, R::GVU_LANES, f);
                st.dumped++;
            }
            return true;
        }

    private:
        struct Stat
        {
            uint64_t beats{0}, not_expr{0}, no_out{0}, lat_other{0}, lanes{0}, differ{0}, cycles{0}, tables{0}, dumped{0};
            char first[160]{0};
            template <typename... A> void miss(const char *fmt, A... a) { if (!differ) std::snprintf(first, sizeof first, fmt, a...); differ++; }
        };
        bool on_{false};
        Stat st_;
        std::FILE *dump_{nullptr};    // HAS_OBP_DUMP

        static RtlObp &inst() { static RtlObp p; return p; }
        RtlObp()
        {
            const char *v = std::getenv("HAS_OBP_RTL");
            on_ = v && std::atoi(v) != 0;
            if (!on_) return;
            if (const char *d = std::getenv("HAS_OBP_DUMP"))
                if (*d && !(dump_ = std::fopen(d, "wb"))) std::fprintf(stderr, "[obp-rtl] WARNING: cannot write %s; no beats are dumped\n", d);
            std::atexit(&RtlObp::report);
        }
        static bool fit(int64_t v, int bits) { const int64_t h = static_cast<int64_t>(1) << (bits - 1); return v >= -h && v < h; }
        static rtl_ref_gvu::RoundMode map_round(int m)
        {
            namespace R = rtl_ref_gvu;
            static const R::RoundMode t[7] = {R::RND_FLOOR, R::RND_NEAR_UP, R::RND_NEAR_AWAY, R::RND_NEAR_EVEN, R::RND_CEIL, R::RND_TRUNC, R::RND_AWAY};
            return t[m];
        }
        static void report()
        {
            const Stat &s = inst().st_;
            std::printf("[obp-rtl] GEMM_FUSED epilogue on the cycle-level pipeline (HAS_OBP_RTL), depth %d cycles: beats %llu | not expressible %llu | "
                        "lanes %llu | differ from the model %llu | beats with another depth %llu | activation tables loaded %llu | %s\n", kLatency,
                        static_cast<unsigned long long>(s.beats), static_cast<unsigned long long>(s.not_expr), static_cast<unsigned long long>(s.lanes),
                        static_cast<unsigned long long>(s.differ), static_cast<unsigned long long>(s.lat_other),
                        static_cast<unsigned long long>(s.tables), s.differ ? s.first : (s.no_out ? "a beat did not come out" : "-"));
            if (inst().dump_) { std::fclose(inst().dump_); inst().dump_ = nullptr; std::printf("[obp-rtl] beats written to HAS_OBP_DUMP: %llu\n", static_cast<unsigned long long>(s.dumped)); }
            std::fflush(stdout);
        }
    };
} // namespace has

#endif // HAS_RTL_OBP_H
