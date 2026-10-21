# uefi/ — optional UEFI (edk2) stage-2 for MT6785, SKELETON package

**Status: SKELETON — not buildable yet.** The `.dsc`/`.fdf`/sources in
`edk2-mt6785/` describe a minimal UEFI DXE stage for the Redmi Note 8 Pro
(`begonia`, MediaTek Helio G90T / MT6785). They are provided as:

1. an **alternate stage-2** for bring-up diagnostics (see `bootnotes.md`
   for *why* this is kept separate from the main `xnuloader` path), and
2. a fixed shape (`Mt6785Pkg`) so any later UEFI-side work lands where
   the EDK2 tooling expects it, without re-deriving the memory layout —
   every address here is a mirror of `docs/hardware_map_mt6785.md` §2/§5.

Nothing in this directory is referenced by the primary boot flow
(`boot.img` → `xnuloader` → XNU); treat it as *another agent's parallel
debug vehicle* for MT6785 platform bring-up.

## Layout

| Path | Purpose |
| :--- | :--- |
| `bootnotes.md` | design rationale: why a UEFI stage, entry contract, memory story, payload layout |
| `edk2-mt6785/Mt6785Pkg.dsc` | package DSC: library set + the one DXE component |
| `edk2-mt6785/Mt6785Pkg.fdf` | package FDF: `DXEFV` firmware volume layout (skeleton) |
| `edk2-mt6785/Mt6785BootDxe/Mt6785BootDxe.inf` | INF of the boot driver (DXE_DRIVER) |
| `edk2-mt6785/Mt6785BootDxe/Mt6785BootDxe.c` | driver: prints to UART0, dumps memory map, probes payload candidates on SimpleFileSystem |
| `edk2-mt6785/Mt6785SerialPortLib/Mt6785SerialPortLib.inf` | INF: serial library (SerialPortLib implementation) |
| `edk2-mt6785/Mt6785SerialPortLib/Mt6785SerialPortLib.c` | 16550-style TX-only-ish serial to UART0 `0x11002000`, 4-byte stride, line pre-configured by `lk` 115200 8N1 |

GUIDs in the INFs/DSC/FDF were **randomly generated once** for this
skeleton; regenerate if they ever collide with real packages.

## Fetching edk2 (operator-run only, never in-build)

The repo policy is offline-first: nothing here fetches code during the
build. The edk2 checkout is a once-per-workstation setup step:

```
git clone https://github.com/tianocore/edk2.git
cd edk2
git checkout edk2-stable202302        # pinned tag this skeleton was written against
git submodule update --init --recursive
```

## Pointing the build at this package

Keep our package out-of-tree (not inside the edk2 checkout) and surface
it with `PACKAGES_PATH`:

```
#  from a checkout containing: hackios-begonia/ and the cloned edk2/
export PACKAGES_PATH=/path/to/hackios-begonia/uefi/edk2-mt6785:/path/to/edk2
source /path/to/edk2/edksetup.sh
python3 /path/to/edk2/BaseTools/BuildEnv     # or let edksetup do it
build -a AARCH64 -t GCC5 -p Mt6785Pkg/Mt6785Pkg.dsc -b DEBUG
```

Expected artifacts (after the skeleton actually builds — it does not yet):

```
Build/Mt6785Pkg/DEBUG_GCC5/AARCH64/Mt6785BootDxe.efi
Build/Mt6785Pkg/DEBUG_GCC5/FV/DXEFV.Fv
```

By default `check_env.sh` treats edk2 as **absent** — no FAIL, because
this path is optional. If you want it in the matrix, note that the
`build` command is the observable, not `which edk2`.

## Known skeleton limitations (on purpose)

* No platform drivers (GICv3, generic timer, watchdog) are wired in —
  `TimerDxe`-equivalent bring-up belongs to whoever resumes this path.
* `Mt6785BootDxe.c` *probes* payload candidates and would-be load
  addresses but does not yet relocate code.
* No device-tree authoring occurs in this stage; the ADT belongs to
  `xnuloader/fdt2adt` (`docs/hardware_map_mt6785.md` §3 is the contract).
* The DSC links the bare minimum; production would add `MdeModulePkg`
  DXE core + architectural drivers and a host of `*.dec` includes — left
  out so the *shape* is honest about being a skeleton.
