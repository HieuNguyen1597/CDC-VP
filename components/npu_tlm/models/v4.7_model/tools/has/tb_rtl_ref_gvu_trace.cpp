// tb_rtl_ref_gvu_trace.cpp -- digest of the vector files of the ported vector-unit blocks, so that "the ported blocks give
// the outputs the RTL simulation gave" can be checked on a machine without a Verilog simulator.
// The vector files (written by tb_rtl_ref_gvu_fu --dump, tb_rtl_ref_gvu_g2dump and tb_rtl_ref_gvu_bdump) hold, per beat,
// the stimulus and the outputs of the ported block. tools/has/gvu_oracle/run_all.sh replays them on the RTL; when every
// block reports 0 mismatches the files are what the RTL produced on that stimulus, and run_all.sh stores their digest
// (tools/has/gvu_oracle/rtl_trace_digest.txt). The generators are deterministic, so regenerating the files and comparing
// digests shows whether the ported blocks still produce those outputs.
//   digest <dir>                 print the digest of every *.txt file of <dir>: "<file> <first line of the chunk> <lines> <fnv-1a 64>"
//                                per chunk of 10 000 lines (a difference is located to a chunk)
//   check <dir> <digest file>    compare <dir> with a stored digest; prints one line per file and RESULT: PASS / FAIL
// build: g++ -std=c++17 -O2 -Wall tools/has/tb_rtl_ref_gvu_trace.cpp -o tb_rtl_ref_gvu_trace
#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

struct Chunk { uint64_t first, lines, hash; };
static const uint64_t kChunk = 10000;

static std::vector<Chunk> digest_file(const std::string &path)
{
    std::vector<Chunk> out;
    std::ifstream f(path, std::ios::binary);
    std::string line;
    Chunk c{0, 0, 0xcbf29ce484222325ull};
    uint64_t n = 0;
    while (std::getline(f, line))
    {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        for (unsigned char ch : line) { c.hash ^= ch; c.hash *= 0x100000001b3ull; }
        c.hash ^= '\n'; c.hash *= 0x100000001b3ull;
        c.lines++; n++;
        if (c.lines == kChunk) { out.push_back(c); c = Chunk{n, 0, 0xcbf29ce484222325ull}; }
    }
    if (c.lines) out.push_back(c);
    return out;
}

static std::vector<std::string> txt_files(const std::string &dir)
{
    std::vector<std::string> v;
    for (const auto &e : std::filesystem::directory_iterator(dir))
        if (e.is_regular_file() && e.path().extension() == ".txt") v.push_back(e.path().filename().string());
    std::sort(v.begin(), v.end());
    return v;
}

int main(int argc, char **argv)
{
    if (argc >= 3 && !strcmp(argv[1], "digest"))
    {
        for (const std::string &name : txt_files(argv[2]))
            for (const Chunk &c : digest_file(std::string(argv[2]) + "/" + name))
                printf("%s %" PRIu64 " %" PRIu64 " %016" PRIx64 "\n", name.c_str(), c.first, c.lines, c.hash);
        return 0;
    }
    if (argc >= 4 && !strcmp(argv[1], "check"))
    {
        std::map<std::string, std::vector<Chunk>> ref;
        std::vector<std::string> order;
        std::ifstream f(argv[3]);
        if (!f) { printf("cannot open %s\nRESULT: FAIL\n", argv[3]); return 1; }
        std::string line;
        while (std::getline(f, line))
        {
            if (line.empty() || line[0] == '#') continue;
            std::istringstream is(line);
            std::string name, h; Chunk c{};
            if (!(is >> name >> c.first >> c.lines >> h)) continue;
            c.hash = std::strtoull(h.c_str(), nullptr, 16);
            if (!ref.count(name)) order.push_back(name);
            ref[name].push_back(c);
        }
        int bad_files = 0; uint64_t total = 0;
        for (const std::string &name : order)
        {
            const std::vector<Chunk> got = digest_file(std::string(argv[2]) + "/" + name);
            const std::vector<Chunk> &exp = ref[name];
            uint64_t lines = 0; int64_t first_bad = -1;
            for (size_t i = 0; i < exp.size(); i++)
            {
                lines += exp[i].lines;
                if (first_bad < 0 && (i >= got.size() || got[i].lines != exp[i].lines || got[i].hash != exp[i].hash)) first_bad = static_cast<int64_t>(exp[i].first);
            }
            if (first_bad < 0 && got.size() != exp.size()) first_bad = static_cast<int64_t>(lines);
            total += lines;
            if (first_bad < 0) printf("same    %-18s %9" PRIu64 " beats\n", name.c_str(), lines);
            else { printf("DIFFERS %-18s %9" PRIu64 " beats, first difference in the lines %" PRId64 " .. %" PRId64 "\n", name.c_str(), lines, first_bad, first_bad + static_cast<int64_t>(kChunk) - 1); bad_files++; }
        }
        printf("files %zu, beats %" PRIu64 ", files that differ %d\n", order.size(), total, bad_files);
        printf(bad_files || order.empty() ? "RESULT: FAIL\n" : "RESULT: PASS\n");
        return bad_files || order.empty() ? 1 : 0;
    }
    printf("usage: %s digest <dir> | check <dir> <digest file>\n", argv[0]);
    return 2;
}
