#pragma once
#include <stdint.h>

/* FU3_SYSCSR_v1 - offset/bit cho firmware. Cửa sổ 64 KiB, thanh ghi 32 bit. */
#define FU3_SYSCSR_BASE UINT32_C(0x44010000)

#define FU3_SYSCSR_VERSION_OFFSET        0x0000u
#define FU3_SYSCSR_LEFT_IO_CFG_OFFSET    0x0010u
#define FU3_SYSCSR_BOTTOM_IO_CFG_OFFSET  0x0014u
#define FU3_SYSCSR_RIGHT_IO_CFG_OFFSET   0x0018u
#define FU3_SYSCSR_TOP_IO_CFG_OFFSET     0x001Cu
#define FU3_SYSCSR_STATUS_OFFSET         0x0070u
#define FU3_SYSCSR_SYS_CTRL_OFFSET       0x0074u
#define FU3_SYSCSR_CPU_CTRL_OFFSET       0x0078u   /* Reserved trong FU3 */
#define FU3_SYSCSR_CPU_SYSTICK_OFFSET    0x007Cu   /* Reserved trong FU3 */
#define FU3_SYSCSR_OSC_PAD_CTRL_OFFSET   0x0080u
#define FU3_SYSCSR_ROM_CTRL_OFFSET       0x0088u

/* syscsr_status (RO, live từ IO_PAD) */
#define FU3_SYSCSR_STATUS_BOOT           (1u << 0)
#define FU3_SYSCSR_STATUS_MODE_0         (1u << 1)
#define FU3_SYSCSR_STATUS_MODE_1         (1u << 2)
#define FU3_SYSCSR_STATUS_TEST_MODE      (1u << 3)
#define FU3_SYSCSR_STATUS_TEST_MODE_SEL  (1u << 4)

/* syscsr_sys_ctrl (RW) */
#define FU3_SYSCSR_SW_RESET_REQ          (1u << 0)
#define FU3_SYSCSR_SW_GLOBAL_RST_REQ     (1u << 4)
#define FU3_SYSCSR_JTAGMUX_SEL           (1u << 8)
#define FU3_SYSCSR_JTAGMUX_BLOCK         (1u << 12)

/* rom_ctrl */
#define FU3_SYSCSR_ROM_LVEN              (1u << 0)
