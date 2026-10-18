// gvu_pipeline_ctrl.h -- Pipeline Controller of the vector unit (MAS V1.0 "Pipeline Controller", figures "GVU Softmax
// Pipeline FSM", "GVU LayerNorm Pipeline FSM", "GVU Residual Add Pipeline FSM"). The controller only sequences stages:
// every stage is fully pipelined, so it does not track the step inside a stage. This model walks the state diagrams and
// returns the visited states and a cycle ESTIMATE; it holds no data and does not change any result.
//
// Terms of the figures: the 32 lanes hold up to 32 rows; "end of row" = the last element of the rows in the lanes has
// streamed through; "end of col" = the last group of rows is done. One visit of a STAGE state = one element (one vector
// of 32 lanes) accepted by the stage.
//   Softmax    stage 1: IDLE, SETUP, per group [FUSED_ATTN ? WAIT_SA : READ_DATA x], STAGE_1 x n, WRITE_DATA row_max
//              stage 2: SETUP, per group READ_DATA x & row_max, STAGE_2 x n, WRITE_DATA row_sigma
//              stage 3: SETUP, per group READ_DATA row_sigma, STAGE_3 x n ; then IDLE
//   LayerNorm  stage 1: IDLE, SETUP, per group READ_DATA x, STAGE_1 x n, WRITE_DATA row_sum
//              stage 2: SETUP, per group READ_DATA x, READ_DATA row_sum, PASS_1 x n (write D), PASS_2 (requant, write var)
//              stage 3: SETUP, READ_DATA var, STAGE_3 x groups        stage 4: SETUP, READ_DATA var+eps, per group
//              READ_DATA D, STAGE_4 x n       stage 5: SETUP, READ_DATA norm, READ_DATA gamma, per group STAGE_5 x n ; END
//   Residual   stage 1: IDLE, SETUP, READ_DATA A & B, STAGE_1 x vectors ; stage 2: SETUP, READ_DATA sum, STAGE_2 x vectors ;
//              IDLE. (The figure prints the two captions "Residual Stage 1 / 2" under the opposite columns.)
// The figures draw the group loop of Softmax stage 3 and LayerNorm stages 3-5 as one "end of col" loop on the STAGE
// state; the per-group READ_DATA above is this model's reading where a stage needs a new row parameter per group.
//
// Cycle ESTIMATE: SETUP = setup cycles (Stream Switch and FU reconfiguration; the multiplier needs a pipeline flush when
// its split mode changes), READ_DATA / WRITE_DATA / WAIT_SA = one cycle each, STAGE = one cycle per element, plus the
// latency of the stage's datapath once per group (the pipeline drains before the end-of-row write). WAIT_SA does not
// include the time the systolic array takes: FUSED_ATTN phase 1 is paced by the array.
#ifndef HAS_GVU_PIPELINE_CTRL_H
#define HAS_GVU_PIPELINE_CTRL_H

#include <cstdint>
#include <string>
#include <vector>

namespace has
{
    enum class PcState { IDLE = 0, SETUP, WAIT_SA, READ_DATA, STAGE, WRITE_DATA, END, COUNT };

    inline const char *pc_state_name(PcState s)
    {
        static const char *n[] = {"IDLE", "SETUP", "WAIT_SA", "READ_DATA", "STAGE", "WRITE_DATA", "END"};
        return n[int(s)];
    }

    struct PcCost
    {
        int setup{1};      // cycles of one SETUP state
        int flush{5};      // extra SETUP cycles when the multiplier split mode changes (deepest multiplier: 5 stages)
        int access{1};     // cycles of READ_DATA / WRITE_DATA / WAIT_SA
    };

    struct PcTrace
    {
        uint64_t visits[int(PcState::COUNT)]{};
        uint64_t stage_visits[6]{};        // STAGE visits per stage number (1-based)
        uint64_t cycles{0};
        std::vector<std::string> log;      // state names in order, kept only while short (unit check on small cases)
        uint64_t at(PcState s) const { return visits[int(s)]; }
    };

    class GvuPipelineCtrl
    {
    public:
        explicit GvuPipelineCtrl(const PcCost &c = PcCost(), size_t log_limit = 256) : c_(c), lim_(log_limit) {}

        // Softmax of `rows` rows of `L` elements. lat1..lat3 = datapath latency of the three stages (drawing cuts: 3, 5,
        // 14 + LUT stages). fused = phase 1 of FUSED_ATTN: stage 1 waits for the array instead of reading the Scratchpad.
        PcTrace softmax(uint64_t rows, uint64_t L, bool fused, int lat1 = 3, int lat2 = 5, int lat3 = 18) const
        {
            PcTrace t;
            const uint64_t G = groups(rows);
            go(t, PcState::IDLE, 0);
            setup(t, false);
            for (uint64_t g = 0; g < G; g++)
            {
                go(t, fused ? PcState::WAIT_SA : PcState::READ_DATA, c_.access);
                stage(t, 1, L, lat1);
                go(t, PcState::WRITE_DATA, c_.access);
            }
            setup(t, false);
            for (uint64_t g = 0; g < G; g++)
            {
                go(t, PcState::READ_DATA, c_.access);
                stage(t, 2, L, lat2);
                go(t, PcState::WRITE_DATA, c_.access);
            }
            setup(t, true);
            for (uint64_t g = 0; g < G; g++)
            {
                go(t, PcState::READ_DATA, c_.access);
                stage(t, 3, L, lat3);
            }
            go(t, PcState::IDLE, 0);
            return t;
        }

        // LayerNorm of `rows` rows of `H` elements. Stage 2 has two passes (D per element, then the variance requant at
        // the end of the row); stage 3 handles one variance per row, so one vector per group.
        PcTrace layernorm(uint64_t rows, uint64_t H, int lat1 = 3, int lat2 = 5, int lat3 = 10, int lat4 = 10, int lat5 = 10) const
        {
            PcTrace t;
            const uint64_t G = groups(rows);
            go(t, PcState::IDLE, 0);
            setup(t, false);
            for (uint64_t g = 0; g < G; g++)
            {
                go(t, PcState::READ_DATA, c_.access);
                stage(t, 1, H, lat1);
                go(t, PcState::WRITE_DATA, c_.access);
            }
            setup(t, true);
            for (uint64_t g = 0; g < G; g++)
            {
                go(t, PcState::READ_DATA, c_.access);
                go(t, PcState::READ_DATA, c_.access);
                stage(t, 2, H, lat2);   // pass 1
                stage(t, 2, 1, lat2);   // pass 2
            }
            setup(t, true);
            go(t, PcState::READ_DATA, c_.access);
            stage(t, 3, G, lat3);
            setup(t, true);
            go(t, PcState::READ_DATA, c_.access);
            for (uint64_t g = 0; g < G; g++)
            {
                go(t, PcState::READ_DATA, c_.access);
                stage(t, 4, H, lat4);
            }
            setup(t, true);
            go(t, PcState::READ_DATA, c_.access);
            go(t, PcState::READ_DATA, c_.access);
            for (uint64_t g = 0; g < G; g++) stage(t, 5, H, lat5);
            go(t, PcState::END, 0);
            return t;
        }

        // Residual Add of `elems` int8 elements: 32 per vector, two stages through the Scratchpad.
        PcTrace residual(uint64_t elems, int lat1 = 6, int lat2 = 5) const
        {
            PcTrace t;
            const uint64_t V = (elems + 31) / 32;
            go(t, PcState::IDLE, 0);
            setup(t, true);
            go(t, PcState::READ_DATA, c_.access);
            stage(t, 1, V, lat1);
            setup(t, true);
            go(t, PcState::READ_DATA, c_.access);
            stage(t, 2, V, lat2);
            go(t, PcState::IDLE, 0);
            return t;
        }

    private:
        PcCost c_;
        size_t lim_;
        static uint64_t groups(uint64_t rows) { return (rows + 31) / 32; }
        void go(PcTrace &t, PcState s, int cyc) const
        {
            t.visits[int(s)]++;
            t.cycles += uint64_t(cyc);
            if (t.log.size() < lim_) t.log.push_back(pc_state_name(s));
        }
        void setup(PcTrace &t, bool flush) const { go(t, PcState::SETUP, c_.setup + (flush ? c_.flush : 0)); }
        void stage(PcTrace &t, int no, uint64_t n, int lat) const
        {
            t.visits[int(PcState::STAGE)] += n;
            t.stage_visits[no] += n;
            t.cycles += n + uint64_t(lat);
            if (t.log.size() < lim_) t.log.push_back(std::string("STAGE_") + char('0' + no) + " x" + std::to_string(n));
        }
    };
} // namespace has

#endif // HAS_GVU_PIPELINE_CTRL_H
