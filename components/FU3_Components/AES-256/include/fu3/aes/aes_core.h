#pragma once
#include <array>
#include <cstdint>
#include <functional>

namespace fu3::aes {
using Block = std::array<std::uint8_t, 16>;
using Key = std::array<std::uint8_t, 32>;
// Optional read-only observation of the actual transform, for debugging and
// comparison against the FIPS intermediate-state examples. No timing/state writes.
using TraceSink = std::function<void(unsigned round, const char* stage, const Block&)>;

// Functional simulation primitive, not a constant-time cryptography library.
class AesCore {
public:
    void initialize(const Key&, bool key256);
    Block transform(Block, bool encrypt, const TraceSink& trace = {}) const;
    void clear();
private:
    std::array<std::uint8_t, 240> round_keys_{};
    unsigned rounds_ = 0;
};
}
