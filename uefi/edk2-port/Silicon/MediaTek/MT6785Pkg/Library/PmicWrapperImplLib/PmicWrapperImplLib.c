//
// MT6789 PWRAP Configuration
//

#include <Protocol/MtkPmicWrapper.h>

MTK_PWRAP_REG PwrapRegs[] = {
  0x0,  // INIT_DONE2
  0x880, // WACS2_CMD
  0x8a4, // WACS2_VLDCLR
  0x8a8, // WACS2_RDATA
  0x884, // SWINF_2_WDATA_31_0
  0x894, // SWINF_2_RDATA_31_0
};

MTK_PWRAP_PLATFORM_INFO PlatformInfo = {
  0x10026000, // Base
  PwrapRegs,
  TRUE        // Have ARB Capability
};
