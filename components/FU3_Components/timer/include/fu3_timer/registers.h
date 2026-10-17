#pragma once
#include <stdint.h>

/* Firmware-visible offsets. Upper local address bits alias these registers. */
#define FU3_TIMER_COUNT_OFFSET   0x00u
#define FU3_TIMER_CONTROL_OFFSET 0x04u
#define FU3_TIMER_COMPARE_OFFSET 0x08u
#define FU3_TIMER_ENABLE         0x01u
#define FU3_TIMER_PRESCALER_SHIFT 3u
#define FU3_TIMER_PRESCALER_MASK  0x38u
#define FU3_TIMER0_BASE UINT32_C(0x44060000)
#define FU3_TIMER1_BASE UINT32_C(0x44070000)
#define FU3_TIMER2_BASE UINT32_C(0x50060000)
#define FU3_TIMER3_BASE UINT32_C(0x50070000)
