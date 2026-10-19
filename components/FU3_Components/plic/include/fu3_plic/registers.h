#pragma once
#include <stdint.h>

// FU3 AP_PLIC (ftel_plic HDS V1.0) - offset/bit cho firmware
// Cửa sổ 64 KiB trên PERIBUS_1
#define FU3_PLIC_BASE UINT32_C(0x40100000)

#define FU3_PLIC_CONFIG_0_OFFSET    0x0000u   // RO: [15:0] SOURCES, [31:16] TARGETS
#define FU3_PLIC_CONFIG_1_OFFSET    0x0004u   // RO: [15:0] PRIORITIES, [23:16] MAX_PENDING_COUNT, [24] HAS_THRESHOLD 
#define FU3_PLIC_EL_OFFSET          0x0008u   // RW: bit k = irq_sources_i[k] (ID k+1): 0 level, 1 rising edge 
#define FU3_PLIC_PRIORITY_0_OFFSET  0x000Cu   // RW: ID 1-8, 4 bit mỗi ID (dùng [2:0]) 
#define FU3_PLIC_PRIORITY_1_OFFSET  0x0010u   // ID 9-16
#define FU3_PLIC_PRIORITY_2_OFFSET  0x0014u   // ID 17-24
#define FU3_PLIC_PRIORITY_3_OFFSET  0x0018u   // ID 25-32
#define FU3_PLIC_IE_0_OFFSET        0x001Cu   // RW: bit k bật ID k+1 cho target 0 
#define FU3_PLIC_THRESHOLD_0_OFFSET 0x0020u   // RW [2:0]: chỉ báo ngắt khi priority > threshold 
#define FU3_PLIC_ID_0_OFFSET        0x0024u   // Đọc = claim (trả ID, 0 = không có) và Ghi ID = complete 

#define FU3_PLIC_SOURCES            32u
#define FU3_PLIC_TARGETS            1u
#define FU3_PLIC_PRIORITIES         8u
#define FU3_PLIC_MAX_PENDING_COUNT  8u

// Offset thanh ghi PRIORITY và vị trí bit cho Source ID (1..32) 
#define FU3_PLIC_PRIORITY_OFFSET(id) (FU3_PLIC_PRIORITY_0_OFFSET + 4u * (((id) - 1u) / 8u))
#define FU3_PLIC_PRIORITY_SHIFT(id)  (4u * (((id) - 1u) % 8u))
#define FU3_PLIC_SOURCE_BIT(id)      (1u << ((id) - 1u))

// Source ID theo FU3 HAS v2.0 Table 11-1: ID n <- irq_sources_i[n-1]
#define FU3_PLIC_ID_CRM          1u
#define FU3_PLIC_ID_UART0        2u
#define FU3_PLIC_ID_AES          3u
#define FU3_PLIC_ID_QSPI         4u
#define FU3_PLIC_ID_DMA0         5u
#define FU3_PLIC_ID_TIMER1       6u
#define FU3_PLIC_ID_TIMER2       7u
#define FU3_PLIC_ID_TIMER3       8u
#define FU3_PLIC_ID_SPI0         9u
#define FU3_PLIC_ID_UART1        10u
#define FU3_PLIC_ID_UART2        11u
#define FU3_PLIC_ID_GPIOA        12u
#define FU3_PLIC_ID_GPIOB        13u
#define FU3_PLIC_ID_GPIOC        14u
#define FU3_PLIC_ID_I2C0         15u
#define FU3_PLIC_ID_SPI1         16u
#define FU3_PLIC_ID_ADC          17u
#define FU3_PLIC_ID_WDT1         18u
#define FU3_PLIC_ID_GPIO0_WAKE   19u
#define FU3_PLIC_ID_EDHOC_OSCORE 20u
#define FU3_PLIC_ID_RNG          21u
#define FU3_PLIC_ID_SPI2         22u
#define FU3_PLIC_ID_SPI3         23u
