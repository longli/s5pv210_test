#pragma once

#include <arm/s5pv210_register.h>

#define KHz(n) (1000 * (n))
#define MHz(n) (1000 * KHz(n))

#define S5PV210_SRAM_BASE 0x20000000
#define S5PV210_SRAM_SIZE 0x00004000

#define SDRAM_BASE  0x20000000
#define SDRAM_SIZE  0x40000000

#define ATAG_BASE   SDRAM_BASE

#define MPU_CLK    MHz(720)
#define L3_CLK     MHz(133)
#define L4_CLK     MHz(66.5)
#define HCLK_RATE  L3_CLK

#define INT_VECTOR_RAM_BASE 0x4020FFC8

#ifndef __ASSEMBLY__
#include <types.h>

int omap3_irq_init(void);
#endif
