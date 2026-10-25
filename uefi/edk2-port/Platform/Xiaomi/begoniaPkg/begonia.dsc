[Defines]
  PLATFORM_NAME                  = begonia
  PLATFORM_GUID                  = 7A10BEE0-242C-4875-B901-BBAB29912A85
  PLATFORM_VERSION               = 0.1
  DSC_SPECIFICATION              = 0x00010019
  OUTPUT_DIRECTORY               = Build/$(PLATFORM_NAME)
  SUPPORTED_ARCHITECTURES        = AARCH64
  BUILD_TARGETS                  = DEBUG|RELEASE
  SKUID_IDENTIFIER               = DEFAULT
  FLASH_DEFINITION               = begoniaPkg/begonia.fdf
  DEVICE_DXE_FV_COMPONENTS       = begoniaPkg/begonia.fdf.inc

[PcdsFixedAtBuild.common]
  gArmTokenSpaceGuid.PcdSystemMemoryBase|0x40000000
  gArmTokenSpaceGuid.PcdSystemMemorySize|0x180000000 # 6GB (also boots on 8GB)
  gArmTokenSpaceGuid.PcdCpuVectorBaseAddress|0x40C40000
  gEmbeddedTokenSpaceGuid.PcdPrePiStackBase|0x40C00000
  gEmbeddedTokenSpaceGuid.PcdPrePiStackSize|0x00040000
  gMediaTekTokenSpaceGuid.PcdUefiMemPoolBase|0x40C50000
  gMediaTekTokenSpaceGuid.PcdUefiMemPoolSize|0x075b0000

  # Begonia 1080x2340 DSI Simple Framebuffer
  gMediaTekTokenSpaceGuid.PcdMipiFrameBufferWidth|1080
  gMediaTekTokenSpaceGuid.PcdMipiFrameBufferHeight|2340
  gMediaTekTokenSpaceGuid.PcdMipiFrameBufferAddress|0xbe000000

[LibraryClasses]
  PlatformMemoryMapLib|begoniaPkg/Library/PlatformMemoryMapLib/PlatformMemoryMapLib.inf

!include Silicon/MediaTek/MT6785Pkg/MT6785.dsc
