#include <Library/BaseLib.h>
#include <Library/PlatformMemoryMapLib.h>

/*
 * Begonia (MT6785 / Helio G90T) Verified Memory Map for EDK2 / UEFI
 * Strictly excludes Trustonic TEE (Kinibi) and ARM Trusted Firmware (BL31)
 */
static ARM_MEMORY_REGION_DESCRIPTOR_EX gDeviceMemoryDescriptorEx[] = {
/*                                                    EFI_RESOURCE_ EFI_RESOURCE_ATTRIBUTE_ EFI_MEMORY_TYPE ARM_REGION_ATTRIBUTE_
     MemLabel(32 Char.),  MemBase,    MemSize, BuildHob, ResourceType, ResourceAttribute, MemoryType, CacheAttributes
--------------------- Register ---------------------*/
    {"Periphs",           0x00000000, 0x1B000000,  AddMem, MEM_RES, UNCACHEABLE,  RtCode,   NS_DEVICE},

    //--------------------- DDR --------------------- */
    {"DDR Memory 1",      0x040000000, 0x000C00000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK_XN}, // 12MB (0x40000000 - 0x40C00000)
    {"UEFI Stack",        0x040C00000, 0x000040000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK},    // 256KB
    {"CPU Vectors",       0x040C40000, 0x000010000, AddMem, SYS_MEM, SYS_MEM_CAP, BsCode, WRITE_BACK},    // 64KB
    {"DXE Heap",          0x040C50000, 0x0023B0000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK},    // ~35MB (0x40C50000 - 0x43000000)
    {"ATF BL31",          0x043000000, 0x000200000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},    // 2MB (0x43000000 - 0x43200000)
    {"TrustZone TEE",     0x043200000, 0x001400000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},    // 20MB (0x43200000 - 0x44600000)
    {"SSPM SPM Share",    0x044600000, 0x000200000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},    // 2MB (0x44600000 - 0x44800000)
    {"DDR Memory 2",      0x044800000, 0x00B800000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK},    // 184MB (0x44800000 - 0x50000000)
    {"UEFI FD",           0x050000000, 0x000700000, AddMem, SYS_MEM, SYS_MEM_CAP, BsCode, WRITE_BACK},    // 7MB
    {"DDR Memory 3",      0x050700000, 0x06D900000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK},    // ~1.7GB (0x50700000 - 0xBE000000)
    {"Framebuffer",       0x0BE000000, 0x001000000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_THROUGH_XN}, // 16MB LK Framebuffer
    {"DDR Memory 4",      0x0BF000000, 0x001000000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK},    // 16MB top of 32-bit RAM
    {"DDR Memory High",   0x0C0000000, 0x140000000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK},    // 5GB High RAM

//------------------- Terminator for MMU ---------------------
{"Terminator", 0, 0, 0, 0, 0, 0, 0}};

ARM_MEMORY_REGION_DESCRIPTOR_EX *GetPlatformMemoryMap()
{
  return gDeviceMemoryDescriptorEx;
}
