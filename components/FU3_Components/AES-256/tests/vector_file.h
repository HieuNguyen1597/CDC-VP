#pragma once
#include "fu3/aes/aes_core.h"
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

struct ReferenceVector {
    bool wide = false;
    fu3::aes::Key key{};
    fu3::aes::Block input{}, encrypted{}, decrypted{};
};
template <std::size_t N>
void decode_hex(const std::string& s, std::array<std::uint8_t, N>& data, unsigned bytes) {
    if (s.size() != 2 * bytes || bytes > N) throw std::runtime_error("invalid vector length");
    const auto nibble = [](char c) -> unsigned {
        if (c >= '0' && c <= '9') return static_cast<unsigned>(c - '0');
        if (c >= 'a' && c <= 'f') return static_cast<unsigned>(c - 'a' + 10);
        throw std::runtime_error("invalid vector hex digit");
    };
    for (unsigned i = 0; i < bytes; ++i)
        data[i] = static_cast<std::uint8_t>((nibble(s[2*i]) << 4) | nibble(s[2*i+1]));
}
inline std::vector<ReferenceVector> load_vectors(const std::string& path) {
    std::ifstream file(path);
    if (!file) throw std::runtime_error("cannot open independent vector file: " + path);
    std::vector<ReferenceVector> out;
    std::string line;
    unsigned small = 0, wide = 0;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream row(line);
        unsigned bits = 0;
        std::string key, block, encrypted, decrypted, extra;
        if (!(row >> bits >> key >> block >> encrypted >> decrypted) || (row >> extra) ||
            (bits != 128 && bits != 256)) throw std::runtime_error("invalid independent vector row");
        ReferenceVector v;
        v.wide = bits == 256;
        decode_hex(key, v.key, bits / 8); decode_hex(block, v.input, 16);
        decode_hex(encrypted, v.encrypted, 16); decode_hex(decrypted, v.decrypted, 16);
        out.push_back(v);
        if (v.wide) ++wide; else ++small;
    }
    if (small != 515 || wide != 643) throw std::runtime_error("incomplete reference corpus");
    return out;
}
