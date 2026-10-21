/*
 * psci.h — PSCI interface (call conduit = SMC into MediaTek BL31).
 */
#ifndef _XLDR_PSCI_H_
#define _XLDR_PSCI_H_

#include <stdint.h>

#define PSCI_VERSION            0x84000000ul
#define PSCI_CPU_OFF            0x84000002ul
#define PSCI_CPU_ON_SMC64       0xC4000003ul
#define PSCI_SYSTEM_OFF         0x84000008ul
#define PSCI_SYSTEM_RESET       0x84000009ul

#define PSCI_RET_SUCCESS        0
#define PSCI_RET_NOT_SUPPORTED  -1

int64_t psci_version(void);
int64_t psci_cpu_on(uint64_t mpidr, uint64_t entry, uint64_t ctx);
int64_t psci_system_off(void);
int64_t psci_system_reset(void);

#endif /* _XLDR_PSCI_H_ */
