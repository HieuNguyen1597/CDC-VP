// SPDX-License-Identifier: Apache-2.0
#pragma once
// Minimal check helpers shared by the DDR3 regressions (repo convention: plain sc_main
// / main executables selected by a case-name argument, no external test framework).
#include <cstdint>
#include <iostream>
#include <sstream>
#include <string>

namespace fx1_ddr3_test {

inline unsigned& checks() { static unsigned n = 0; return n; }
// Set by a bench when its scenario ran to the end; a hung SystemC thread must not pass.
inline bool& completed() { static bool done = false; return done; }
inline unsigned& failures() { static unsigned n = 0; return n; }

inline void expect(bool ok, const std::string& what) {
    ++checks();
    if (!ok) {
        ++failures();
        std::cerr << "FAIL: " << what << '\n';
    }
}

template <typename A, typename B>
void expect_eq(const A& actual, const B& expected, const std::string& what) {
    const bool ok = actual == static_cast<A>(expected);
    if (!ok) {
        std::ostringstream os;
        os << what << " (got 0x" << std::hex << static_cast<std::uint64_t>(actual) << ", expected 0x"
           << static_cast<std::uint64_t>(expected) << ")";
        expect(false, os.str());
    } else {
        expect(true, what);
    }
}

inline int summary(const std::string& name) {
    std::cout << name << ": " << checks() << " checks, " << failures() << " failures\n";
    return failures() == 0 && checks() > 0 ? 0 : 1;
}

} // namespace fx1_ddr3_test
