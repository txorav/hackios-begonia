# Sandbox notes — why relaxations are safe and needed on HackiOS-begonia

Scope: `userland/` documentation. The sandbox enforcement itself lives in the
XNU tree (`xnu/bsd/...`) with `*_sandbox` kernel extensions — kernel-port's
domain. This note records what the userland-side brings up and why.

## 1. What sandboxing normally protects — and what ours does

On stock iOS every LaunchDaemon/LaunchAgent runs inside a per-service sandbox
profile; processes are denied kernel IOKit nodes, file paths, and Mach ports
outside their allowlist. Profiles are compiled (`sb`) and enforced by the
`com.apple.security.sandbox` AMS/AMFI pair (kernel-side *Sandbox.kext* is
Apple-provided; XNU's open source carries the syscall plumbing).

HackiOS-begonia deliberately stops enforcing pieces of that stack during
bring-up, via the boot args set in `docs/hardware_map_mt6785.md` §4:

```
cs_enforcement_disable=1  amfi_get_out_of_my_way=0x1  wpdebug=1 ...
```

* `cs_enforcement_disable=1` — verified in-tree: `xnu/bsd/kern/kern_codesigning.c`
  parses it (`PE_parse_boot_argn("cs_enforcement_disable", ...)`).
* `amfi_get_out_of_my_way=0x1` — makes the AMFI-stub kext (`kexts/AMFIStub`,
  kernel-port scope) approve any signature / entitlement.

Even with AMFI out of the way, **sandbox denials are independent checks**; a
sandbox-restricted daemon can still get EPERM hitting a device node that
stock iOS profiles never allowed (e.g., our `/dev/md0`, IOKit services that
only exist on our MT6785 platform kext set).

## 2. Why we need profile relaxations (specifics)

1. **Missing device services.** The stock service lists in profiles name
   Apple IOKit families (SEP, AppleKeyStore, DCP, PMGR…) that have no
   MT6785 counterpart. Until the stub kexts (`kexts/SEPStub`,
   `kexts/WatchDogStub`) publish compatible nubs, profile IOKit allowlists
   do not match and the daemons crash-loop. Relaxation: under our control,
   allow-list the whole IOKit plane for bring-up services.
2. **Different device paths.** Stock profile path rules for `/var/db/*` etc.
   are fine; what we actually need are relaxations around
   path-virtualization quirks (the `/private/var` overlay described in
   `../ipsw/LayoutNotes.md`) and read-write access to
   `/System/Library/Caches/com.apple.dyld/` for the rootfs-merging flows.
3. **Unsigned companion binaries.** Our static shell and diagnostic tools
   (`../ramdisk/static_shell`, M4 console) ship `ldid -S`-signed (no real
   entitlements). Sandbox default-deny of Mach `bootstrap` gets them nowhere —
   which is exactly why static_shell does pure BSD console I/O instead of
   Mach IPC (see `../ramdisk/README.md` §2).

## 3. What we can relax by construction (preferred) vs. patching profiles

| Mechanism | Where | Cost | Safe? |
| :--- | --- | --- | --- |
| Launch plists run unsandboxed | edit `<daemon>.plist` `EnvironmentVariables`/manifest — rootfs is patchable (fstab rw, no SSV check) | cheap | research-grade acceptable; every daemon runs root |
| disabled profile references | optional; we keep stock profiles and don't touch them | none | N/A |
| whole-IOKit allow | in the replaced plists only | cheap | needed until stub kexts land |
| `sandbox_exec -e my.csbpl` experiments | dev tools only, NOT baked into plists | cheap | yes |

**Do NOT**: disable the sandbox kext entirely (it breaks `launchd`'s own
process-management assumptions via `SandboxConfiguration` checks) and do NOT
edit compiled `*.sb` binaries you didn't inspect — there is no GUI-recovery.

## 4. Console / debugging checklist

* Sandbox denials are logged through `com.apple.xpc.asl`/os_log — divert the
  os-log spam to serial by running `log stream --style syslog > /dev/console`
  from the M4 shell once `logd` is up (see plan §Phase 5 M5).
* First-boot crash-loops of `backboardd`, `UserEventAgent`, and `securityd`
  are all sandbox/IOKit-bootstrap issues typical of a ported platform — work
  device-profile replacements one daemon at a time, never in bulk.

## 5. Cross-references

* Boot args canonicalization: `docs/hardware_map_mt6785.md` §4 (canonical).
* Kernel-flag lore: `xnu/bsd/kern/kern_codesigning.c` (`cs_enforcement_disable`),
  `xnu/osfmk/vm/vm_fault.c` (`cs_debug` paths) — verify against the in-tree
  sources when the kernel bring-up changes these flags.
* Stub-kext ownership matrix: `docs/hardware_map_mt6785.md` §6.
