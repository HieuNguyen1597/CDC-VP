// tb_gvu_vec_export.cpp -- stage-by-stage vectors of SOFTMAX and LAYERNORM for a cycle-level model of the vector unit.
// The inputs are the heads and LayerNorm rows of ViT-B/16 of the unit-test vectors (attn_vit_*.txt, ln_vit_*.txt); the
// values are those of the functional model (has/gvu_softmax.h, has/gvu_layernorm.h, has/gvu_fused_attn.h) in the default
// configuration of the instruction path: reciprocal / inverse-square-root tables of 256 / 512 entries with the last entry
// held, a zero input of the table returns 0, rounding half up.
// Every intermediate written here is recomputed from the formula of its stage and the final values are compared with the
// model before the file is written; the program fails if they differ.
// usage: tb_gvu_vec_export <vectors dir> <tables dir> <out dir> [rows per case, default 32; 0 = all]
// build: g++ -std=c++17 -O2 -I. tools/has/tb_gvu_vec_export.cpp -o tb_gvu_vec_export
//
// softmax_<head>.txt                                   one head, L elements per row
//   CFG L S s zp P q round      ATTN_Scale, ATTN_Shift, Z_ATTN, P, table format Q, rounding mode (1 = half up)
//   EXP e[0..255]               exponential table, unsigned 8 bit, index = row maximum - x
//   RECIP t[0..255]             reciprocal table image, 16 bit
//   ROW i
//   X x[0..L-1]                 stage 1 input, int8 (the requantized Q.K scores of the head)
//   M m                         stage 1: row maximum
//   E e[0..L-1]                 stage 2: EXP[m - x]
//   SIG sigma                   stage 2: sum of e
//   R r E sh                    stage 3: table result r, exponent E, shift amount sh = E + q - P
//   T t[0..L-1]                 stage 3: round-shift of r * e by sh (a left shift when sh is negative)
//   A a[0..L-1]                 stage 3 output: requant(t, S, s, zp), int8
//
// layernorm_<case>.txt                                 H channels per row
//   CFG H Pre_Shift M0_var TS_var Z_var M0_7 TS_7 E_bias Z_7 M0_div TS_div Z_div Z_mul Z_out floor_flag floor_value round
//   GAMMA g[0..H-1] / BETA b[0..H-1] / M0MUL m[0..H-1] / TSMUL s[0..H-1]     per-channel parameters
//   RSQRT t[0..511]             inverse-square-root table image, 16 bit, address {E[0], index}
//   ROW i
//   X x[0..H-1]                 input, int8
//   S s                         stage 1: sum of x
//   D d[0..H-1]                 stage 2: H * x - S
//   V V                         stage 2: sum of d * d
//   Q v f w r E                 stage 3: requantized variance v, f, table input w; stage 4: table result r, exponent E
//   N n[0..H-1]                 stage 4 output, int8
//   Y y[0..H-1]                 stage 5 output, int8
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>
#include "has/gvu_fused_attn.h"
#include "has/gvu_layernorm.h"

typedef std::vector<std::vector<long>> LMat;
static long g_bad = 0;

static std::map<std::string, LMat> read_head(const std::string &path)
{
    std::map<std::string, LMat> s;
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line))
    {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream in(line);
        std::string tag;
        in >> tag;
        std::vector<long> row;
        long v;
        while (in >> v) row.push_back(v);
        s[tag].push_back(row);
    }
    return s;
}
static has::Mat to_int(const LMat &m)
{
    has::Mat r(m.size());
    for (size_t i = 0; i < m.size(); i++) r[i].assign(m[i].begin(), m[i].end());
    return r;
}
template <typename T> static std::vector<T> read_line(std::istream &f)
{
    std::string line;
    std::getline(f, line);
    std::istringstream in(line);
    std::vector<T> v;
    long long x;
    while (in >> x) v.push_back(T(x));
    return v;
}
template <typename V> static void put(FILE *f, const char *tag, const V &v)
{
    std::fprintf(f, "%s", tag);
    for (const auto &x : v) std::fprintf(f, " %lld", static_cast<long long>(x));
    std::fprintf(f, "\n");
}

static void export_softmax(const std::string &vdir, const std::string &tdir, const std::string &odir, size_t max_rows, const has::Knobs &k)
{
    const has::LutIndirect recip(14, true, true);
    std::ifstream pf(tdir + "/attn_params.txt");
    std::string line;
    while (std::getline(pf, line))
    {
        std::istringstream in(line);
        std::string name;
        has::AttnParams p;
        long Mqk, Mav;
        int mz, zc, az;
        in >> name >> p.Zq >> p.Zk >> p.Zv >> p.Zqk >> p.Zav >> Mqk >> p.TSqk >> Mav >> p.TSav >> mz >> zc >> az;
        if (name.compare(0, 4, "vit_") != 0) continue;
        p.Mqk = Mqk; p.Mav = Mav; p.mask_e_zero = mz; p.av_zv_corr = zc; p.asym_zp = az;
        std::vector<int32_t> exp;
        { std::ifstream ef(tdir + "/attn_exp_" + name + ".txt"); long v; while (ef >> v) exp.push_back(int32_t(v)); }
        auto s = read_head(vdir + "/attn_" + name + ".txt");
        if (exp.size() < 256 || s["Q"].empty()) { std::printf("softmax %s: input files missing\n", name.c_str()); g_bad++; continue; }
        const has::AttnResult r = has::fused_attn(to_int(s["Q"]), to_int(s["K"]), to_int(s["V"]), to_int(s["M"]), p, exp.data(), recip, k);
        const has::SoftmaxCfg c;                                  // the requant parameters fused_attn uses for the scores
        FILE *f = std::fopen((odir + "/softmax_" + name + ".txt").c_str(), "w");
        const size_t L = r.X[0].size(), rows = max_rows && max_rows < r.X.size() ? max_rows : r.X.size();
        std::fprintf(f, "CFG %zu %lld %d %d %d %d %d\n", L, static_cast<long long>(c.S), c.s, c.zp, c.P, c.q, k.round_mode);
        put(f, "EXP", std::vector<int32_t>(exp.begin(), exp.begin() + 256));
        put(f, "RECIP", recip.mas_recip_image());
        long masked = 0;
        for (size_t i = 0; i < rows; i++)
        {
            const std::vector<int> &x = r.X[i];
            const has::SoftmaxRow &o = r.stats[i];
            std::vector<int32_t> e(L);
            std::vector<long long> t(L);
            long long sigma = 0;
            const int sh = o.E + c.q - c.P;
            for (size_t j = 0; j < L; j++)
            {
                const bool z = p.mask_e_zero && s["M"][i][j] == -128;
                masked += z;
                e[j] = z ? 0 : exp[static_cast<size_t>(o.m - x[j])];
                sigma += e[j];
                const has::i128 pr = static_cast<has::i128>(o.r) * e[j];
                t[j] = static_cast<long long>(sh <= 0 ? (pr << -sh) : has::rshift(pr, sh, k.round_mode));
                if (has::requant(t[j], c.S, c.s, c.zp, k) != o.a[j]) g_bad++;
            }
            if (sigma != o.sigma) g_bad++;
            std::fprintf(f, "ROW %zu\n", i);
            put(f, "X", x);
            std::fprintf(f, "M %d\n", o.m);
            put(f, "E", e);
            std::fprintf(f, "SIG %lld\n", sigma);
            std::fprintf(f, "R %d %d %d\n", o.r, o.E, sh);
            put(f, "T", t);
            put(f, "A", o.a);
        }
        std::fclose(f);
        std::printf("softmax_%s.txt: %zu rows of %zu elements (head has %zu rows), masked positions with e forced to 0: %ld\n", name.c_str(), rows, L, r.X.size(), masked);
    }
}

static void export_layernorm(const std::string &vdir, const std::string &tdir, const std::string &odir, size_t max_rows, const has::Knobs &k)
{
    const has::LutIndirect lut(14, true, true);
    std::ifstream cf(tdir + "/ln_cases.txt");
    std::string name;
    while (cf >> name)
    {
        if (name.compare(0, 4, "vit_") != 0) continue;
        std::ifstream pf(tdir + "/ln_params_" + name + ".txt");
        const std::vector<long long> s = read_line<long long>(pf);
        if (s.size() < 19) { std::printf("layernorm %s: parameter file missing\n", name.c_str()); g_bad++; continue; }
        has::LnParams p;
        p.H = int(s[0]); p.Pre_Shift = int(s[1]); p.M0_var = s[2]; p.TS_var = int(s[3]); p.Z_var = int(s[4]);
        p.M0_7 = s[5]; p.TS_7 = int(s[6]); p.E_bias = s[7]; p.Z_7 = int(s[8]); p.M0_div = s[9]; p.TS_div = int(s[10]);
        p.Z_div = int(s[11]); p.Z_mul = int(s[12]); p.Z_out = int(s[13]); p.r8_floor_z_out = s[14] != 0; p.r8_z_out = int(s[15]);
        p.r9_exact = s[16] != 0; p.r9_lsb_bits = int(s[17]); p.out_int16 = s[18] != 0;
        p.gamma = read_line<int>(pf); p.beta = read_line<int64_t>(pf); p.M0_mul = read_line<int64_t>(pf); p.TS_mul = read_line<int>(pf);
        FILE *f = std::fopen((odir + "/layernorm_" + name + ".txt").c_str(), "w");
        std::fprintf(f, "CFG %d %d %lld %d %d %lld %d %lld %d %lld %d %d %d %d %d %d %d\n", p.H, p.Pre_Shift, static_cast<long long>(p.M0_var), p.TS_var, p.Z_var,
                     static_cast<long long>(p.M0_7), p.TS_7, static_cast<long long>(p.E_bias), p.Z_7, static_cast<long long>(p.M0_div), p.TS_div, p.Z_div, p.Z_mul, p.Z_out,
                     p.r8_floor_z_out ? 1 : 0, p.r8_z_out, k.round_mode);
        put(f, "GAMMA", p.gamma); put(f, "BETA", p.beta); put(f, "M0MUL", p.M0_mul); put(f, "TSMUL", p.TS_mul);
        put(f, "RSQRT", lut.mas_rsqrt_image());
        std::ifstream vf(vdir + "/ln_" + name + ".txt");
        std::string line;
        size_t rows = 0, total = 0;
        while (std::getline(vf, line))
        {
            if (line.empty() || line[0] != 'X') continue;
            total++;
            if (max_rows && rows >= max_rows) continue;
            std::istringstream in(line);
            std::string tag;
            in >> tag;
            std::vector<int> x;
            long long v;
            while (in >> v) x.push_back(int(v));
            const has::LnRow o = has::layernorm_row(x, p, lut, k);
            std::vector<long long> D(x.size());
            long long S = 0, V = 0;
            for (int q : x) S += q;
            for (size_t j = 0; j < x.size(); j++) { D[j] = static_cast<long long>(p.H) * x[j] - S; V += D[j] * D[j]; }
            if (S != o.S || V != o.V) g_bad++;
            std::fprintf(f, "ROW %zu\n", rows);
            put(f, "X", x);
            std::fprintf(f, "S %lld\n", S);
            put(f, "D", D);
            std::fprintf(f, "V %lld\n", V);
            std::fprintf(f, "Q %d %d %d %d %d\n", o.v, o.f, o.w, o.r, o.E);
            put(f, "N", o.n);
            put(f, "Y", o.y);
            rows++;
        }
        std::fclose(f);
        std::printf("layernorm_%s.txt: %zu rows of %d channels (case has %zu rows)\n", name.c_str(), rows, p.H, total);
        if (rows == 0) g_bad++;
    }
}

int main(int argc, char **argv)
{
    if (argc < 4) { std::fprintf(stderr, "usage: %s <vectors dir> <tables dir> <out dir> [rows per case, default 32; 0 = all]\n", argv[0]); return 2; }
    const size_t max_rows = argc > 4 ? static_cast<size_t>(std::atol(argv[4])) : 32;
    has::Knobs k;
    k.spec_default();
    export_softmax(argv[1], argv[2], argv[3], max_rows, k);
    export_layernorm(argv[1], argv[2], argv[3], max_rows, k);
    std::printf("RESULT: %s (values that differ from the model: %ld)\n", g_bad == 0 ? "PASS" : "FAIL", g_bad);
    return g_bad == 0 ? 0 : 1;
}
