/*
 * DDR_SYSCTRL: FX1 VP SoC glue around the DDR3 controller (DDR3 VP plan DEC-04).
 *
 * VP PLACEHOLDER, NOT AN FTEL IP. The DDR3 controller has no CSR for its readiness pins,
 * its P-channel, its QoS/reserved-rate sideband or its resets (HAS Table 1-3/1-4, §7.1).
 * In the FX1 VP this block, at FX1_DDR_SYSCTRL_BASE (fx1_memory_map.h), provides firmware
 * access to them.
 * It holds no fault injection: faults are injected from the host only.
 *
 * Access: 32-bit aligned words. Unmapped offsets answer an address error; writes to
 * read-only words answer a slave error.
 */
#ifndef FX1_DDR_SYSCTRL_H
#define FX1_DDR_SYSCTRL_H

#if defined(__ASSEMBLER__) || defined(FX1_LINKER_SCRIPT)
#define FX1_DDRSC_U32(v) v
#else
#define FX1_DDRSC_U32(v) v##u
#endif

#define FX1_DDRSC_ID                 FX1_DDRSC_U32(0x000) /* RO 0x44445343 ("DDSC") */
#define FX1_DDRSC_STATUS             FX1_DDRSC_U32(0x004) /* RO controller output levels */
#define FX1_DDRSC_PWR_REQ            FX1_DDRSC_U32(0x010) /* RW: start a P-channel request */
#define FX1_DDRSC_PWR_STATUS         FX1_DDRSC_U32(0x014) /* RO */
#define FX1_DDRSC_PWR_CTRL           FX1_DDRSC_U32(0x018) /* RW */
#define FX1_DDRSC_PWR_COUNT          FX1_DDRSC_U32(0x01C) /* RO */
#define FX1_DDRSC_RST_CTRL           FX1_DDRSC_U32(0x020) /* WO-CMD (reads 0) */
#define FX1_DDRSC_RST_STATUS         FX1_DDRSC_U32(0x024) /* RO */
#define FX1_DDRSC_QOS_LUT(p)        (FX1_DDRSC_U32(0x040) + 4u * (p)) /* RW, port 0..3 */
#define FX1_DDRSC_QOS_CFG            FX1_DDRSC_U32(0x050) /* RW */
#define FX1_DDRSC_RSV_RATE(p)        (FX1_DDRSC_U32(0x060) + 8u * (p)) /* RW [19:0] */
#define FX1_DDRSC_RSV_DEPTH(p)       (FX1_DDRSC_U32(0x064) + 8u * (p)) /* RW [19:0] */

#define FX1_DDRSC_ID_VALUE           FX1_DDRSC_U32(0x44445343)

/* STATUS: the controller's output levels (HAS Table 1-3, BE08) and the boot mode. */
#define FX1_DDRSC_ST_MISSION_READY   (1u << 0)
#define FX1_DDRSC_ST_INIT_COMPLETE   (1u << 1)
#define FX1_DDRSC_ST_GLOBAL_IDLE     (1u << 2)
#define FX1_DDRSC_ST_FE_DRAINED      (1u << 3)
#define FX1_DDRSC_ST_IRQ             (1u << 4)
#define FX1_DDRSC_ST_PREINIT         (1u << 8)  /* the platform initialized the DDR (preinit) */

/* PWR_REQ: write [1:0] PSTATE (fx1_ddr3/provisional_encodings.h FX1_DDR3_PSTATE_*) with
 * [8] START. DDR_SYSCTRL then runs the 4-phase P-channel handshake on its own: it raises
 * PREQ, waits for PACCEPT or PDENY, records the result and lowers PREQ. A START while a
 * handshake runs is ignored (PWR_STATUS[0]). */
#define FX1_DDRSC_PWR_REQ_VAL(pstate) ((((pstate) & 3u)) | (1u << 8))
/* PWR_STATUS */
#define FX1_DDRSC_PWR_BUSY           (1u << 0)  /* handshake in progress */
#define FX1_DDRSC_PWR_ACCEPTED       (1u << 1)  /* last handshake accepted */
#define FX1_DDRSC_PWR_DENIED         (1u << 2)  /* last handshake denied */
#define FX1_DDRSC_PWR_AUTO_WAKE_DENIED (1u << 3) /* the last automatic wake was denied; it is
                                                   retried after a back-off (1 us doubling to
                                                   1 ms) until accepted or a reset */
#define FX1_DDRSC_PWR_LAST_REQ(v)    (((v) >> 4) & 3u)   /* PSTATE of the last handshake */
#define FX1_DDRSC_PWR_RESIDENT(v)    (((v) >> 8) & 3u)   /* resident state of the DDR */
#define FX1_DDRSC_PWR_PACTIVE        (1u << 16) /* the controller needs the active state */
/* PWR_CTRL */
#define FX1_DDRSC_PWR_AUTO_WAKE      (1u << 0)  /* reset 1: on PACTIVE in a low-power state,
                                                   request ACTIVE automatically. A denied
                                                   request (e.g. recovery required after a
                                                   failed wake) is retried with a back-off and
                                                   reported in PWR_STATUS[3]; recover with
                                                   RST_CTRL */
/* PWR_COUNT: [15:0] accepted low-power entries, [31:16] accepted automatic wakes (both wrap). */
#define FX1_DDRSC_PWR_ENTRIES(v)     ((v) & 0xFFFFu)
#define FX1_DDRSC_PWR_AUTO_WAKES(v)  (((v) >> 16) & 0xFFFFu)

/* RST_CTRL: the SoC reset controller of the DDR3 subsystem (HAS §7.1). Write exactly one
 * bit; DDR_SYSCTRL then runs the sequence on its own (RST_STATUS[0] busy):
 *  COLD  pulse the cold reset (whole IP: CSRs, images, QoS tables, quarantine, fatal).
 *  CORE  coordinated core reset: raise the reset request, wait for reset-safe, pulse the
 *        core reset, drop the request. Held transactions are aborted with SLVERR. If
 *        reset-safe does not come within 1 ms the request is dropped (RST_STATUS[2]).
 * After either reset the controller needs a full initialization (programming guide §4);
 * after a cold reset the QoS tables must be configured again (QOS_LUT, QOS_CFG). DRAM
 * contents are kept, but with ECC the startup scrub zeroes them.
 * Write two bits, or write while busy: slave error, nothing started. */
#define FX1_DDRSC_RST_COLD           (1u << 0)
#define FX1_DDRSC_RST_CORE           (1u << 1)
/* RST_STATUS */
#define FX1_DDRSC_RST_BUSY           (1u << 0)
#define FX1_DDRSC_RST_DONE           (1u << 1)  /* last sequence completed */
#define FX1_DDRSC_RST_SAFE_TIMEOUT   (1u << 2)  /* last core reset: no reset-safe in 1 ms */
#define FX1_DDRSC_RST_COLD_COUNT(v)  (((v) >> 8) & 0xFFu)
#define FX1_DDRSC_RST_CORE_COUNT(v)  (((v) >> 16) & 0xFFu)

/* QOS_LUT(p): 2-bit class of QoS index i in bits [2i+1:2i] (codes:
 * fx1_ddr3/provisional_encodings.h FX1_DDR3_QOS_CLASS_*). A write programs all 16 entries;
 * a read returns the controller's table (a port soft reset restores 0x55555555 = NORMAL). */
#define FX1_DDRSC_QOS_LUT_ALL_NORMAL FX1_DDRSC_U32(0x55555555)
/* QOS_CFG: bit p = the QoS table of port p is configured. A port admits no transaction
 * until its bit is set (HAS FE05); a port soft reset clears it. */

#endif /* FX1_DDR_SYSCTRL_H */
