/*
 * DDR3 controller bring-up and test helpers for FX1 firmware on the VP.
 *
 * Reference MMIO code for the FW tests and the BootROM loader (DDR3 VP plan,
 * subtask 5): it follows the programming guide step by step. It is not a driver
 * library; the FW team writes its own driver.
 *
 * Everything here runs from BootROM/SRAM: no code or data in DDR is needed before
 * readiness, so the helpers are also used to recover a failed controller.
 */
#ifndef FX1_DDR3_FW_H
#define FX1_DDR3_FW_H

#include <stdint.h>

#include "fx1_fw.h"
#include "fx1/fx1_ddr_sysctrl.h"
#include "fx1/fx1_memory_map.h"
#include "fx1_ddr3/fx1_ddr3_regs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- register access ------------------------------------------------------ */
static inline uint32_t ddr3_rd(uint32_t offset) { return fx1_read32(FX1_DDR3_CSR_BASE + offset); }
static inline void ddr3_wr(uint32_t offset, uint32_t value) { fx1_write32(FX1_DDR3_CSR_BASE + offset, value); }
static inline uint32_t ddrsc_rd(uint32_t offset) { return fx1_read32(FX1_DDR_SYSCTRL_BASE + offset); }
static inline void ddrsc_wr(uint32_t offset, uint32_t value) { fx1_write32(FX1_DDR_SYSCTRL_BASE + offset, value); }

/* Capture a snapshot, then read one [SNAP] register. */
uint32_t ddr3_snap(uint32_t offset);

/* ---- fault-safe accesses ---------------------------------------------------
 * 0 when the access completed, otherwise the mcause of its access fault (5 load,
 * 7 store). The trap must reach ddr3_probe_trap(): with no handler of its own a
 * test calls ddr3_install_probe_handler(); a test handler calls ddr3_probe_trap()
 * first. Per hart; usable by both harts at once. */
uint32_t ddr3_probe_read32(uintptr_t address, uint32_t* value);
uint32_t ddr3_probe_write32(uintptr_t address, uint32_t value);
/* Non-zero (and *resume set) when the trap is a probed access. */
int ddr3_probe_trap(uint32_t mcause, uint32_t mepc, uint32_t* resume);
void ddr3_install_probe_handler(void);

/* ---- time ------------------------------------------------------------------ */
#define DDR3_TICKS_PER_US (FX1_MTIME_HZ / 1000000u)
/* Sleep in WFI on this hart's CLINT timer (interrupts stay masked meanwhile). */
void ddr3_sleep_us(uint32_t us);
/* Sleep until mtime reaches `ticks`. */
void ddr3_sleep_until(uint64_t ticks);
static inline uint64_t ddr3_ms_to_ticks(uint32_t ms) { return (uint64_t)ms * 1000u * DDR3_TICKS_PER_US; }

/* ---- bring-up (programming guide §4, DDR3-1600 reference image) -------------- */
typedef enum {
    DDR3_OK = 0,
    DDR3_ERR_IDENTITY = 1,    /* IP_ID / ABI / capability mismatch */
    DDR3_ERR_PROGRAM = 2,     /* a shadow or INIT_RW write was rejected */
    DDR3_ERR_COMMIT = 3,      /* a commit returned SLVERR */
    DDR3_ERR_TIMEOUT = 4,     /* no mission readiness in time */
    DDR3_ERR_INIT_FAILED = 5, /* INIT_STATUS failed */
    DDR3_ERR_FATAL = 6,       /* FATAL_STATUS set */
} ddr3_status_t;

/* Details of the last failure. */
typedef struct {
    uint32_t offset;   /* register of the failing write/commit, 0 if none */
    uint32_t value;    /* commit status word, INIT_STATUS or FATAL_STATUS */
    uint32_t elapsed_us;
} ddr3_report_t;
extern ddr3_report_t ddr3_last;

/* Check IP_ID, ABI major 1 and the capability of the FX1 build (ECC, 2 ranks). */
ddr3_status_t ddr3_identify(void);
/* Configure the port-0 QoS sideband through DDR_SYSCTRL (all NORMAL, configured).
 * Needed again after a cold reset or a port soft reset. */
void ddr3_qos_default(void);
/* Write the timing, DFI, INIT_RW and topology shadows (no commit). */
ddr3_status_t ddr3_program(void);
/* Commit topology, timing and DFI, in that order. */
ddr3_status_t ddr3_commit_all(void);
/* Wait for DDR_SYSCTRL STATUS.mission_ready, at most `timeout_ms` (the ECC scrub of the
 * 4 GiB baseline takes about 670 ms). Failure and fatal states end the wait early. */
ddr3_status_t ddr3_wait_ready(uint32_t timeout_ms);
/* identify -> QoS -> program -> commits -> GLOBAL_CTRL.enable -> wait ready. */
ddr3_status_t ddr3_bringup(uint32_t timeout_ms);
/* Print the status and the report on the console. */
void ddr3_log_status(const char* what, ddr3_status_t status);

/* ---- DDR_SYSCTRL sequences ---------------------------------------------------- */
/* Run a P-channel request; returns PWR_STATUS after the handshake. */
uint32_t ddr3_power_request(uint32_t pstate);
/* RST_CTRL sequence (FX1_DDRSC_RST_COLD / _CORE); returns RST_STATUS when done. */
uint32_t ddr3_reset(uint32_t which);

#ifdef __cplusplus
}
#endif

#endif /* FX1_DDR3_FW_H */
