# Milestone checklist (plan §4)

Status legend: `[ ]` not started · `[~]` code written, device validation pending · `[x]` achieved on device.

CI coverage (private repo `github.com/txorav/hackios-begonia`, Actions): `host-tests` (loader FDT/ADT unit
tests + py/sh lint), `loader-aarch64` (cross-build + M1 packer smoke),
`qemu-smoke` (`qemu/run_qemu.sh` boot under `-M virt`). See docs/BOOTING.md
§4 for the run loop. Phase A of the reference guide (QEMU first) is thus
enforced on every push.

| M | Target | Success criteria on `begonia` | Components | Status |
| :- | :--- | --- | --- | --- |
| **M1** | Stage-2 loader | `fastboot boot boot.img` draws test pattern + `xnuloader` logs on the 1080×2340 LCD | `xnuloader/*`, `mkxnuimg.py`, kexts/MT6785Framebuffer test-pattern path | `[~]` |
| **M2** | Early XNU entry | XNU enables 16 KiB MMU tables, `PE_init_printf`, version banner on screen | `xnu` port (`MT6785` board config), `boot_args` + ADT via `/chosen/memory-map` | `[~]` |
| **M3** | Interrupts & SMP | Generic Timer ticks (`sched_timeslicing`), all 8 cores online via `PSCI_CPU_ON` | kexts/MT6785GICv3, PSCI in `osfmk/arm64`, CNTFRQ-driven timebase | `[~]` |
| **M4** | Single-user shell | RAMDisk root (`rd=md0`), `/bin/bash` PID 1 console over USB/UART/framebuffer | `userland/ramdisk`, LxStorage (msdc1/ufs path) for later stages | `[~]` |
| **M5** | iOS 16.5 launchd | Stock `/sbin/launchd` boots, loads `dyld_shared_cache_arm64`, spawns logd/configd/sshd | `userland/*` (IPSW tooling), AMFIStub, SEPStub, fstab/MobileGestalt patches | `[ ]` |
| **M6** | UI & Touch | `backboardd` + SpringBoard render Lock/Home Screen with working I2C touch | MT6785Framebuffer, FocalTouchIOHID, wifi/lx drivers | `[ ]` |

Bring-up guidance:
* M1–M2 use `fastboot boot` only. M3+ needs embedded kernel changes; expect
  panic→ramoops→TWRP loops; the `xl` tool automates reboot-and-diagnose.
* M4 is the first time userland runs; `userland/ramdisk` ships the two static
  binaries needed (`bash` drop-in, `dmesg`-style XNU log dumper via sysctl).
* M5 requires a real iPhone10,3 20F66 IPSW (see `userland/README.md`).
