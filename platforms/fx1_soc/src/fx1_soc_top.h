#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include <systemc>

namespace cdc::platforms::fx1_soc {

struct fx1_soc_options {
    // ELF image written into DDR through the bus backdoor before reset. Every
    // hart resets at its entry point (one-image boot, see README).
    std::string firmware;
    // Mirror of the UART TX stream; empty for none.
    std::string uart_log;
    bool uart_stdout = true;
    // UART RX from the host (components/uart_host_tlm): a TCP console on
    // 127.0.0.1:<uart_socket> (-1: off; with uart_wait the run waits for the
    // client at t=0), and/or bytes replayed from uart_rx_file starting at
    // uart_rx_delay_us of simulated time (deterministic, for CI).
    int uart_socket = -1;
    // Throttle: simulated time never runs ahead of wall-clock time.
    bool realtime = false;
    bool uart_wait = false;
    std::string uart_rx_file;
    unsigned uart_rx_delay_us = 0;
    // Print every routed bus transaction.
    bool bus_trace = false;
    // Plan C7 exclusive monitor: real LR/SC reservations shared with SYS_DMA
    // and ISP ODMA. false = upstream VP++ lock model (negative control only).
    bool exclusive_monitor = true;
};

// FX1 SoC for FW/SW bring-up. Stage G1 content: two VP++ RV32IMAFC harts,
// FX1 bus, BootROM, sparse DDR, CLINT, UART and the VP-only sim-control
// finisher. Addresses come from fx1/fx1_memory_map.h.
class fx1_soc_top : public sc_core::sc_module {
public:
    enum class result { running, pass, fail };

    fx1_soc_top(sc_core::sc_module_name name, const fx1_soc_options& options);
    ~fx1_soc_top() override;

    result outcome() const;
    std::uint32_t fail_code() const;
    unsigned harts() const;
    std::uint64_t instret(unsigned hart) const;
    std::uint64_t pc(unsigned hart) const;
    // Copy `length` bytes of DDR at global `address` into `path` (debug
    // artifacts such as ISP NV12 output). Throws if out of range or unwritable.
    void dump_ddr(std::uint64_t address, std::uint64_t length, const std::string& path) const;
    // Copy the file at `path` into DDR at global `address`, before the
    // simulation starts (host-provided data such as a RAW frame or a job
    // descriptor). Throws if out of range or unreadable.
    void load_ddr(std::uint64_t address, const std::string& path);
    // One line of exclusive-monitor counters ("off" without a monitor).
    std::string atomics_report() const;

private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

} // namespace cdc::platforms::fx1_soc
