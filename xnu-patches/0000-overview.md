# xnu-patches/ — the applied XNU → MT6785 port series (overview)

The XNU 8796 (`hackios_source`) vendored tree in `../xnu/` already carries
the whole MT6785 (`begonia`) port — the patches below are **applied
in-tree** (see the file lists), and this directory documents them so a
future re-vendor of upstream keeps the changes bisectable and
reviewable.  The canonical build invocation is in `xnu/config/MT6785.mk`.

## Series (what was applied, where it lives today)

| # | Topic                                 | Where it is now (in-tree)                                             |
|---|---------------------------------------|-----------------------------------------------------------------------|
| 0001 | Board-config for MT6785             | `xnu/pexpert/pexpert/arm64/board_config.h` — `#ifdef ARM64_BOARD_CONFIG_MT6785` block (plan §3.1) |
| 0002 | Machine definitions & build config  | `xnu/config/MT6785.mk` (+ `makedefs/` hooks per its own header comments) |
| 0003 | Platform register surface           | `xnu/pexpert/pexpert/arm64/mt6785.h` — architectural-only sysreg table; deliberately NOT derived from `apple_arm64_common.h` (no HID/APRR/PPL/KTRR/CTRR, no Apple fast-IPI, no AMX/CPM — plan §3.1) |
| 0004 | `proc_reg.h` neutralization         | `xnu/osfmk/arm64/proc_reg.h` `#elif defined(MT6785)` branch at line 246 (plan §3.1 bullet on HID/aprr/ppl stubs) |
| 0005 | Early `arm64_init_cpu` tunables     | `xnu/osfmk/arm64/tunables/tunables.s` — Apple tunable writes gated off for MT6785 (plan §3.1: disable `arm64_init_cpu_hid`) |
| 0006 | GICv3 interrupts & PSCI power       | `kexts/MT6785GICv3` + `kexts/MT6785PlatformExpert` (kext tree lives outside `xnu/`) — plan §3.3/§3.4 |

## Why in-tree, and not a patch file

The port is invasive enough (a disjoint board family that must co-exist
with stock Apple boards) that maintaining deltas as `patch`/`quilt`
files against a moving upstream tree would go stale in one rebase; the
series above is instead visible per-branch via the repository history:
one pair per patch topic.  `git log -p -- xnu/pexpert/pexpert/arm64/`
shows the 0001/0003 deltas.  `config/MT6785.mk` documents the build
assertions that keep Apple-proprietary defines from leaking back in.

## Build

```
cd xnu
make SDKROOT=iphoneos PLATFORM=iPhoneOS SDK_VERSION=16.5 \
     TARGET_CONFIGS="development arm64 MT6785" RC_ProjectName=xnu
```

The kernel's `boot_args`/ADT/loader contract (physBase 0x40000000,
virtBase `0xffffffe040000000`, 16 KiB granule, ADT at `/chosen`)
is documented in `../docs/hardware_map_mt6785.md` and implemented by
`../xnuloader/` — update the two together.

## Verification checklist (PR checklist / AGENTS.md)

- [x] `ARM64_BOARD_CONFIG_MT6785` never defines Apple board macros
      (asserted by `config/MT6785.mk` `MT6785_FORBIDDEN_DEFINES`)
- [ ] Kernel boots through `xnuloader` with `-v` banner on the LCD (M2)
- [ ] GICv3 timer PPI dispatches (`sched_timeslicing` live, M3)
- [ ] 8 CPUs online via PSCI `CPU_ON` (`ml_parse_cpu_topology`, M3)
