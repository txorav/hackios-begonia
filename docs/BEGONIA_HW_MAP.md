# Hardware Register Map & Platform Specs: Redmi Note 8 Pro (`begonia` / MT6785)

## 1. Memory Layout (DRAM & TrustZone)
- **DRAM Base:** `0x40000000` (4GB / 6GB / 8GB LPDDR4x)
- **Critical Secure / Firmware Reservations (DO NOT MAP AS FREE RAM):**
  - `0x43000000` - `0x43200000`: ARM Trusted Firmware (ATF BL31)
  - `0x43200000` - `0x44600000`: Microtrust / TEE / SecOS
  - `0x44600000` - `0x44800000`: MediaTek SSPM (Smart Sensor Power Manager) & SPM
- **Little Kernel (LK) Relocation Base:** `0x48000000` (or `0x41e00000` depending on MIUI build)
- **Kernel Load Address (Android boot.img):** `0x40080000` (`base: 0x40000000`, `kernel_offset: 0x00080000`)
- **Tags / DTB Load Offset:** `0x0bc00000` (`0x4bc00000`)
- **Ramdisk Load Offset:** `0x07c00000` (`0x47c00000`)

## 2. Interrupt Controller (ARM GICv3)
- **Compatible:** `arm,gic-v3`
- **Distributor Base (`GICD`):** `0x0c000000` (Size: `0x40000`)
- **Redistributor Base (`GICR`):** `0x0c040000` (Size: `0x200000`)
- **Redistributor Stride:** `0x20000` (2x 64KB pages per CPU core, 8 cores total)
- **Maintenance Interrupt:** PPI 9 (Level High)
- **Core Affinity:**
  - `cpu0` - `cpu5`: Cortex-A55 (Cluster 0, `0x0000` - `0x0005`)
  - `cpu6` - `cpu7`: Cortex-A76 (Cluster 1, `0x0100` - `0x0101`)

## 3. MediaTek TopRGU Hardware Watchdog
- **Base Address:** `0x10007000` (Size: `0x1000`)
- **Registers:**
  - `WDT_MODE`: `0x10007000 + 0x0000`
    - Magic Key: `0x22000000` (Clears enable bit, disables hardware reboot timeout)
  - `WDT_LENGTH`: `0x10007000 + 0x0004`
  - `WDT_RESTART`: `0x10007000 + 0x0008`
    - Magic Pet Key: `0x1971` (Writes restart sequence to reset timeout counter)

## 4. Serial Debug Console (UART0 / apuart0)
- **Base Address:** `0x11002000` (Size: `0x400`)
- **Compatible:** `mediatek,mt6577-uart`, `mediatek,mt6785-uart`, `ns16550a`
- **Interrupt:** GIC SPI 91 (Hardware IRQ: `91 + 32 = 123`)
- **Default Baud Rate:** `115200` 8N1
- **Clock Source:** `topckgen CLK_TOP_UART_SEL` (26 MHz default bus clock)

## 5. Display & Framebuffer (LK bootfb)
- **Resolution:** `1080 x 2340`
- **Color Format:** 32-bit BGRA / RGBA (`v_pixelFormat = 0x00020004`)
- **Row Bytes:** `1080 * 4 = 4320` bytes
- **Framebuffer Address:** Passed by Little Kernel via device tree (`/reserved-memory/framebuffer` or `bootfb`) or fixed splash reservation.
- **Display Subsystem Bases:**
  - `mmsys_config`: `0x14000000`
  - `disp_ovl0`: `0x14007000`
  - `disp_rdma0`: `0x14009000`
  - `dsi0`: `0x14012000`
- **Supported Panels:**
  - Tianma: Novatek `nt36672a` (Video mode DSI)
  - Shenchao/BOE: Focaltech `ft8719` (Video mode DSI)

## 6. Storage (UFS 2.1 HCI)
- **Base Address:** `0x11270000`
- **Interrupt:** GIC SPI 104 (Hardware IRQ: `104 + 32 = 136`)
- **Standard:** UFSHCI v2.1
- **Root Partition Target:** `userdata` GPT partition on physical UFS LUN 0

## 7. Input Controls
- **Keypad (`mtk-kpd`):** `0x10010000` (Volume Up / Volume Down / Power Key via PMIC MT6359)
- **Touch IC:**
  - I2C0 Bus: `0x11007000`
  - Interrupt: Dedicated GPIO EINT line
