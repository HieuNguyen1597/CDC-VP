// fx1_soc: run one firmware image on the FX1 SoC VP.
//
//   fx1_soc --fw <image.elf> [--timeout-ms N] [--quantum-ns N] [--uart-log FILE]
//           [--quiet-uart] [--trace] [--no-exclusive-monitor] [--load ADDR:FILE ...]
//           [--dump ADDR:LEN:FILE ...] [--uart0-socket PORT [--uart0-wait]]
//           [--uart0-rx-file FILE [--uart0-rx-delay-us N]]
//
// Exit status: 0 PASS, 1 FAIL (firmware reported a code), 2 timeout (no verdict
// within the simulated-time budget), 3 simulator or integration error, 4 usage.

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include <vector>

#include <systemc>
#include <tlm>

#include "fx1_soc_build_info.h"
#include "fx1_soc_top.h"

namespace {

void version()
{
    std::cout << "fx1_soc " << fx1_soc_build::package << " (FX1 SoC VP, 2 x RV32IMAFC VP++ harts)\n"
              << "  CDC-VP revision " << fx1_soc_build::git_revision << " (" << fx1_soc_build::git_state
              << " at configure)\n"
              << "  build type      " << fx1_soc_build::build_type << ", " << fx1_soc_build::cxx_compiler << "\n"
              << "  SystemC         " << sc_core::sc_release() << "\n"
              << "  RISC-V VP++     " << fx1_soc_build::vpp_revision << " + CDC-VP patch series\n"
              << "  firmware ABI    " << fx1_soc_build::firmware_abi << "\n";
}

void usage()
{
    std::cerr << "usage: fx1_soc --fw <image.elf> [--timeout-ms N] [--quantum-ns N] "
                 "[--uart-log FILE] [--quiet-uart] [--trace] [--no-exclusive-monitor]\n"
                 "              [--load ADDR:FILE ...] [--dump ADDR:LEN:FILE ...]\n"
                 "              [--uart0-socket PORT [--uart0-wait]] [--uart0-rx-file FILE [--uart0-rx-delay-us N]]\n"
                 "  --version           print the build identity (package, CDC-VP revision) and exit\n"
                 "  --uart0-socket      UART console over TCP on 127.0.0.1:PORT (connect with nc)\n"
                 "  --uart0-wait        with --uart0-socket: wait for the client before starting\n"
                 "  --uart0-rx-file     inject FILE into UART RX (deterministic console input)\n"
                 "  --uart0-rx-delay-us start the injection N us of simulated time after reset\n"
                 "  --no-exclusive-monitor  upstream VP++ LR/SC bus-lock model, no DMA\n"
                 "                          participation (negative control only)\n"
                 "  --timeout-ms  simulated-time budget in ms (default 1000); 0 = no limit\n"
                 "                (run until the firmware's verdict or Ctrl-C)\n"
                 "  --realtime    never let simulated time run ahead of wall-clock time\n"
                 "                (interactive consoles: an idle system sleeps instead of\n"
                 "                racing through simulated time)\n"
                 "  --quantum-ns  TLM global quantum, a multiple of the 10 ns ISS cycle "
                 "(default 1000)\n"
                 "  --load        before reset, copy FILE into DDR at ADDR (host data such\n"
                 "                as a RAW frame); may be repeated\n"
                 "  --dump        after the run, write LEN bytes of DDR at ADDR (hex or\n"
                 "                decimal) to FILE; may be repeated\n";
}

// Finite number of milliseconds, > 0, or exactly 0 for "no limit"; false on
// anything else.
bool parse_timeout(const std::string& text, double& value)
{
    try {
        std::size_t used = 0;
        value = std::stod(text, &used);
        return used == text.size() && std::isfinite(value) && value >= 0;
    } catch (const std::exception&) {
        return false;
    }
}

// Positive whole multiple of 10 ns (the VP++ ISS cycle).
bool parse_quantum(const std::string& text, unsigned long& value)
{
    try {
        std::size_t used = 0;
        value = std::stoul(text, &used);
        return used == text.size() && value >= 10 && value % 10 == 0;
    } catch (const std::exception&) {
        return false;
    }
}

struct dump_request {
    std::uint64_t address = 0, length = 0;
    std::string path;
};

bool parse_dump(const std::string& text, dump_request& request)
{
    const auto first = text.find(':');
    const auto second = first == std::string::npos ? first : text.find(':', first + 1);
    if (second == std::string::npos) return false;
    try {
        std::size_t used = 0;
        request.address = std::stoull(text.substr(0, first), &used, 0);
        if (used != first) return false;
        const auto len = text.substr(first + 1, second - first - 1);
        request.length = std::stoull(len, &used, 0);
        if (used != len.size() || !request.length) return false;
    } catch (const std::exception&) {
        return false;
    }
    request.path = text.substr(second + 1);
    return !request.path.empty();
}

// Decimal or hex unsigned value no larger than `max`.
bool parse_uint(const std::string& text, unsigned long max, unsigned long& value)
{
    try {
        std::size_t used = 0;
        value = std::stoul(text, &used, 0);
        return used == text.size() && value <= max;
    } catch (const std::exception&) {
        return false;
    }
}

// ADDR:FILE for --load.
bool parse_load(const std::string& text, dump_request& request)
{
    const auto colon = text.find(':');
    if (colon == std::string::npos || colon == 0) return false;
    try {
        std::size_t used = 0;
        request.address = std::stoull(text.substr(0, colon), &used, 0);
        if (used != colon) return false;
    } catch (const std::exception&) {
        return false;
    }
    request.path = text.substr(colon + 1);
    return !request.path.empty();
}

} // namespace

int sc_main(int argc, char* argv[])
{
    using cdc::platforms::fx1_soc::fx1_soc_top;
    cdc::platforms::fx1_soc::fx1_soc_options options;
    double timeout_ms = 1000.0;
    unsigned long quantum_ns = 1000;
    std::vector<dump_request> dumps, loads;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto value = [&]() -> std::string {
            if (i + 1 >= argc) {
                usage();
                std::exit(4);
            }
            return argv[++i];
        };
        if (arg == "--version") {
            version();
            return 0;
        }
        if (arg == "--help" || arg == "-h") {
            usage();
            return 0;
        }
        if (arg == "--fw") options.firmware = value();
        else if (arg == "--timeout-ms") {
            if (!parse_timeout(value(), timeout_ms)) {
                usage();
                return 4;
            }
        }
        else if (arg == "--quantum-ns") {
            if (!parse_quantum(value(), quantum_ns)) {
                usage();
                return 4;
            }
        }
        else if (arg == "--uart0-socket") {
            unsigned long port = 0;
            if (!parse_uint(value(), 65535, port)) {
                usage();
                return 4;
            }
            options.uart_socket = static_cast<int>(port);
        }
        else if (arg == "--uart0-wait") options.uart_wait = true;
        else if (arg == "--realtime") options.realtime = true;
        else if (arg == "--uart0-rx-file") options.uart_rx_file = value();
        else if (arg == "--uart0-rx-delay-us") {
            unsigned long us = 0;
            if (!parse_uint(value(), 1000000000ul, us)) {
                usage();
                return 4;
            }
            options.uart_rx_delay_us = static_cast<unsigned>(us);
        }
        else if (arg == "--load") {
            dump_request request;
            if (!parse_load(value(), request)) {
                usage();
                return 4;
            }
            loads.push_back(request);
        }
        else if (arg == "--dump") {
            dump_request request;
            if (!parse_dump(value(), request)) {
                usage();
                return 4;
            }
            dumps.push_back(request);
        }
        else if (arg == "--uart-log") options.uart_log = value();
        else if (arg == "--quiet-uart") options.uart_stdout = false;
        else if (arg == "--trace") options.bus_trace = true;
        else if (arg == "--no-exclusive-monitor") options.exclusive_monitor = false;
        else {
            usage();
            return 4;
        }
    }
    if (options.firmware.empty()) {
        usage();
        return 4;
    }

    try {
        // Before the harts are built: the VP++ ISS reads the quantum in its constructor.
        tlm::tlm_global_quantum::instance().set(
            sc_core::sc_time(static_cast<double>(quantum_ns), sc_core::SC_NS));
        fx1_soc_top top("fx1_soc", options);
        for (const auto& l : loads) top.load_ddr(l.address, l.path);
        const auto wall_start = std::chrono::steady_clock::now();
        if (timeout_ms > 0)
            sc_core::sc_start(sc_core::sc_time(timeout_ms, sc_core::SC_MS));
        else
            sc_core::sc_start(); /* until the firmware's verdict (sim-control stops) */
        const double wall_s =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - wall_start).count();

        for (const auto& d : dumps) top.dump_ddr(d.address, d.length, d.path);

        std::uint64_t instret = 0;
        std::cout << "\n[fx1_soc] simulated " << sc_core::sc_time_stamp();
        for (unsigned hart = 0; hart < top.harts(); ++hart) {
            std::cout << ", hart" << hart << " instret=" << top.instret(hart);
            instret += top.instret(hart);
        }
        std::cout << "\n[fx1_soc] quantum " << quantum_ns << " ns, host " << wall_s << " s, "
                  << (wall_s > 0 ? instret / wall_s / 1e6 : 0.0) << " MIPS (all harts)\n";
        std::cout << "[fx1_soc] atomics: " << top.atomics_report() << '\n';

        switch (top.outcome()) {
        case fx1_soc_top::result::pass:
            std::cout << "[fx1_soc] RESULT PASS\n";
            return 0;
        case fx1_soc_top::result::fail:
            std::cout << "[fx1_soc] RESULT FAIL code=0x" << std::hex << top.fail_code() << std::dec
                      << '\n';
            return 1;
        default:
            std::cout << "[fx1_soc] RESULT TIMEOUT after " << timeout_ms << " ms simulated";
            for (unsigned hart = 0; hart < top.harts(); ++hart)
                std::cout << ", hart" << hart << " pc=0x" << std::hex << top.pc(hart) << std::dec;
            std::cout << '\n';
            return 2;
        }
    } catch (const std::exception& error) {
        std::cerr << "[fx1_soc] ERROR " << error.what() << '\n';
        return 3;
    }
}
