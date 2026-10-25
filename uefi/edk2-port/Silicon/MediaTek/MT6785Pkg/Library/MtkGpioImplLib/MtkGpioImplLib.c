/*
 * MT6789 GPIO Configuration
 */

#include <Protocol/MtkGpio.h>

#define IOCFG_RT_BASE (0x11ea0000)

MTK_GPIO_PLATFORM_INFO PlatformInfo = {
  .BaseAddr      = 0x10005000,
  .SetOffset     = 0x4, 
  .ResetOffset   = 0x8,
  .DirOffset     = 0x0,
  .DataOutOffset = 0x100,
  .DataInOffset  = 0x200,
  .ModeOffset    = 0x300,
  .MaxPin        = 189
};

struct MTK_GPIO_DRV_TABLE DrvTable[] = {
  { .Pin = 71, .Base = IOCFG_RT_BASE + 0x30, .Mask = 0x7, .Shift = 9 },
  { .Pin = 72, .Base = IOCFG_RT_BASE + 0x30, .Mask = 0x7, .Shift = 12 },
  { .Pin = 73, .Base = IOCFG_RT_BASE + 0x30, .Mask = 0x7, .Shift = 15 },
  { .Pin = 74, .Base = IOCFG_RT_BASE + 0x30, .Mask = 0x7, .Shift = 18 },
  { .Pin = 75, .Base = IOCFG_RT_BASE + 0x30, .Mask = 0x7, .Shift = 21 },
  { .Pin = 76, .Base = IOCFG_RT_BASE + 0x30, .Mask = 0x7, .Shift = 24 },
};

struct MTK_GPIO_R_TABLE RTable[] = {
  { .Pin = 71, .R0Base = IOCFG_RT_BASE + 0xe0,
    .R1Base = IOCFG_RT_BASE+0xf0, .R0Shift = 0, .R1Shift = 0 },
  { .Pin = 72, .R0Base = IOCFG_RT_BASE + 0xe0,
    .R1Base = IOCFG_RT_BASE+0xf0, .R0Shift = 1, .R1Shift = 1 },
  { .Pin = 73, .R0Base = IOCFG_RT_BASE + 0xe0,
    .R1Base = IOCFG_RT_BASE+0xf0, .R0Shift = 2, .R1Shift = 2 },
  { .Pin = 74, .R0Base = IOCFG_RT_BASE + 0xe0,
    .R1Base = IOCFG_RT_BASE+0xf0, .R0Shift = 3, .R1Shift = 3 },
  { .Pin = 75, .R0Base = IOCFG_RT_BASE + 0xe0,
    .R1Base = IOCFG_RT_BASE+0xf0, .R0Shift = 4, .R1Shift = 4 },
  { .Pin = 76, .R0Base = IOCFG_RT_BASE + 0xe0,
    .R1Base = IOCFG_RT_BASE+0xf0, .R0Shift = 5, .R1Shift = 5 },
};

struct MTK_GPIO_PUPD_TABLE PupdTable[] = {
  { .Pin = 71, .Base = IOCFG_RT_BASE+0xb0, .Shift = 0 },
  { .Pin = 72, .Base = IOCFG_RT_BASE+0xb0, .Shift = 1 },
  { .Pin = 73, .Base = IOCFG_RT_BASE+0xb0, .Shift = 2 },
  { .Pin = 74, .Base = IOCFG_RT_BASE+0xb0, .Shift = 3 },
  { .Pin = 75, .Base = IOCFG_RT_BASE+0xb0, .Shift = 4 },
  { .Pin = 76, .Base = IOCFG_RT_BASE+0xb0, .Shift = 5 },
};
