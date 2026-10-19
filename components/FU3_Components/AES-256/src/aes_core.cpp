#include "fu3/aes/aes_core.h"
#include <algorithm>
#include <stdexcept>

namespace fu3::aes {
namespace {
std::uint8_t mul(std::uint8_t a, std::uint8_t b) {
    std::uint8_t out = 0;
    for (unsigned i = 0; i < 8; ++i) {
        if (b & 1) out ^= a;
        a = static_cast<std::uint8_t>((a << 1) ^ ((a & 0x80) ? 0x1b : 0));
        b >>= 1;
    }
    return out;
}
std::uint8_t rot(std::uint8_t x, unsigned n) {
    return static_cast<std::uint8_t>((x << n) | (x >> (8 - n)));
}
struct Substitution {
    std::array<std::uint8_t, 256> forward{}, inverse{};
    Substitution() {
        for (unsigned i = 0; i < 256; ++i) {
            // Multiplicative inverse x^254 in GF(2^8), then affine transform.
            std::uint8_t x = static_cast<std::uint8_t>(i), inv = 1;
            unsigned power = 254;
            while (power) {
                if (power & 1) inv = mul(inv, x);
                x = mul(x, x);
                power >>= 1;
            }
            const auto s = static_cast<std::uint8_t>(inv ^ rot(inv, 1) ^
                rot(inv, 2) ^ rot(inv, 3) ^ rot(inv, 4) ^ 0x63);
            forward[i] = s;
            inverse[s] = static_cast<std::uint8_t>(i);
        }
    }
};
const Substitution& tables() { static const Substitution t; return t; }
void shift(Block& s, bool inverse) {
    const auto old = s;
    for (unsigned r = 0; r < 4; ++r)
        for (unsigned c = 0; c < 4; ++c)
            s[4 * c + r] = old[4 * ((c + (inverse ? 4 - r : r)) % 4) + r];
}
void mix(Block& s, bool inverse) {
    const std::uint8_t forward[4] = {2, 3, 1, 1};
    const std::uint8_t backward[4] = {14, 11, 13, 9};
    const auto* coefficients = inverse ? backward : forward;
    const auto old = s;
    for (unsigned c = 0; c < 4; ++c)
        for (unsigned r = 0; r < 4; ++r) {
            s[4 * c + r] = 0;
            for (unsigned k = 0; k < 4; ++k)
                s[4 * c + r] ^= mul(old[4 * c + k], coefficients[(k + 4 - r) % 4]);
        }
}
}
void AesCore::clear() { round_keys_.fill(0); rounds_ = 0; }
void AesCore::initialize(const Key& key, bool key256) {
    clear();
    const unsigned nk = key256 ? 8 : 4;
    rounds_ = key256 ? 14 : 10;
    std::copy_n(key.begin(), 4 * nk, round_keys_.begin());
    std::uint8_t rcon = 1;
    for (unsigned i = nk; i < 4 * (rounds_ + 1); ++i) {
        std::array<std::uint8_t, 4> t{};
        std::copy_n(round_keys_.begin() + 4 * (i - 1), 4, t.begin());
        if (i % nk == 0) {
            std::rotate(t.begin(), t.begin() + 1, t.end());
            for (auto& b : t) b = tables().forward[b];
            t[0] ^= rcon;
            rcon = mul(rcon, 2);
        } else if (nk == 8 && i % nk == 4) {
            for (auto& b : t) b = tables().forward[b];
        }
        for (unsigned j = 0; j < 4; ++j)
            round_keys_[4 * i + j] = round_keys_[4 * (i - nk) + j] ^ t[j];
    }
}
Block AesCore::transform(Block state, bool encrypt, const TraceSink& trace) const {
    if (!rounds_) throw std::logic_error("AES key schedule is not initialized");
    const auto observe = [&](unsigned round, const char* stage) {
        if (trace) trace(round, stage, state);
    };
    const auto add_key = [&](unsigned round, unsigned step) {
        if (trace) {
            Block key{};
            std::copy_n(round_keys_.begin() + 16 * round, 16, key.begin());
            trace(step, encrypt ? "k_sch" : "ik_sch", key);
        }
        for (unsigned i = 0; i < 16; ++i) state[i] ^= round_keys_[16 * round + i];
    };
    observe(0, encrypt ? "input" : "iinput");
    add_key(encrypt ? 0 : rounds_, 0);
    for (unsigned step = 1; step <= rounds_; ++step) {
        observe(step, encrypt ? "start" : "istart");
        if (!encrypt) {
            shift(state, true);
            observe(step, "is_row");
        }
        for (auto& b : state) b = encrypt ? tables().forward[b] : tables().inverse[b];
        observe(step, encrypt ? "s_box" : "is_box");
        if (encrypt) {
            shift(state, false);
            observe(step, "s_row");
            if (step != rounds_) {
                mix(state, false);
                observe(step, "m_col");
            }
        }
        add_key(encrypt ? step : rounds_ - step, step);
        if (!encrypt && step != rounds_) {
            observe(step, "ik_add");
            mix(state, true);
        }
        if (step == rounds_) observe(step, encrypt ? "output" : "ioutput");
    }
    return state;
}
}
