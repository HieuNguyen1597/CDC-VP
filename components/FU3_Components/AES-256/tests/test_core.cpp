#include "vector_file.h"
#include "fu3/aes/aes_controller.h"
#include <iostream>

int sc_main(int argc, char** argv) {
    unsigned checks = 0;
    const std::string arg = argc >= 2 ? argv[1] : "";
    const bool reference = arg != "core" && arg != "init_zero" && arg != "block_zero";
    auto check = [&](bool value, const char* message) {
        ++checks; if (!value) throw std::runtime_error(message);
    };
    try {
        if (argc < 2) throw std::runtime_error("expected core, init_zero, block_zero or vector file");
        if (arg == "init_zero" || arg == "block_zero") {
            fu3::aes::Config cfg;
            if (arg == "init_zero") cfg.init_cycles = 0; else cfg.block_cycles = 0;
            bool rejected = false;
            try { fu3::aes::Aes invalid{"invalid", cfg}; }
            catch (const std::invalid_argument&) { rejected = true; }
            check(rejected, "zero-cycle configuration accepted");
        } else {
            fu3::aes::AesCore core;
            auto expect_uninitialized = [&] {
                for (bool enc : {false, true}) {
                    bool rejected = false;
                    try { (void)core.transform({}, enc); }
                    catch (const std::logic_error&) { rejected = true; }
                    check(rejected, "uninitialized core accepted operation");
                }
            };
            expect_uninitialized();
            if (arg != "core") {
                const auto vectors = load_vectors(arg);
                for (std::size_t i = 0; i < vectors.size(); ++i) {
                    const auto& v = vectors[i];
                    core.initialize(v.key, v.wide);
                    if (core.transform(v.input, true) != v.encrypted ||
                        core.transform(v.input, false) != v.decrypted)
                        throw std::runtime_error("independent core mismatch at vector " + std::to_string(i));
                    checks += 2;
                    if (!v.wide) {
                        auto key = v.key;
                        for (unsigned j = 16; j < 32; ++j) key[j] = static_cast<std::uint8_t>(j ^ 0xa5);
                        core.initialize(key, false);
                        check(core.transform(v.input, true) == v.encrypted, "AES-128 used lower key half");
                    }
                }
                std::cout << "Independent vectors=" << vectors.size() << '\n';
            }
            core.initialize({}, true); core.clear(); expect_uninitialized();
            core.initialize({}, false); core.clear(); expect_uninitialized();
        }
        std::cout << (reference ? "REFERENCE_PASS " : "POLICY_PASS ") << arg << " checks=" << checks << '\n';
        return 0;
    } catch (const std::exception& e) {
        std::cerr << (reference ? "REFERENCE_FAIL: " : "POLICY_FAIL: ") << e.what() << '\n'; return 1;
    }
}
