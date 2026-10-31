# PS5 port: handoff

What was found out about PS5 homebrew on 2026-10-16, what the probes then
answered on a console (Open questions), and the plan for the host (Plan: the
host). The branch is `ps5-x32`, from `main` at Build 101 (`d271f7b32`), with
upstream PR #359 (the x32 guest) merged in and the probes under
`port/ps5/probe`. The projects' claims below are from their READMEs, not
checked against their code; `PS5_FINDINGS.md` has the measurements in full.

## Graphics: Vulkan exists, from the community

There is no Sony SDK in homebrew. Three community Vulkan drivers target the
PS5's GPU (RDNA 2 class, `gfx1013`), all for jailbroken consoles:

| project | Vulkan | state |
|---|---|---|
| [mpereiraesaa/PS5_Vulkan](https://github.com/mpereiraesaa/PS5_Vulkan) | reports 1.4 (RADV); conformanceVersion 0.0.0.0 | the most mature; Mesa's RADV with a PS5 winsys |
| [mpereiraesaa/ps5-vulkan](https://github.com/mpereiraesaa/ps5-vulkan) | reports 1.3, "non-conformant" | older, separate implementation |
| [Swordpdf/HB_Vulkan](https://github.com/Swordpdf/HB_Vulkan) | 1.0 | early; Mesa-derived, drives the console's AGC command API |

### PS5_Vulkan (RADV), the one to use

- Working on hardware: clears, depth test and write, blending,
  render-to-texture, 4x MSAA with resolve, compute and transfer, and a
  swapchain through `VK_KHR_display` on the console's VideoOut (flips at vblank,
  120 Hz where the display allows).
- Conformance: a first full CTS run on the console passed 1,098,388 of
  2,786,062 cases with 12,005 failures; a second run was in progress.
- Users: vkQuake (about 119.88 fps at 4K), PS5 RetroArch (PPSSPP, Dolphin,
  LRPS2 cores; Dolphin stutters while it compiles ubershaders on first run),
  ProsperoEden (a Switch emulator port, Vulkan by default).
- Known gaps that matter to this game:
  - occlusion queries are coarse (`occlusionQueryPrecise` false): the game's
    lens flares and sun glow use visibility counts, so they would have to cope
    with imprecise ones;
  - line lists do not draw correctly on hardware (check what the game draws
    with lines: debug geometry, possibly the motion tracker);
  - no buffer device address; no concurrent queues.
- The driver is linked into the program: the console refuses to load a graphics
  library at run time (`dlopen`). Linking is through its `tools/radv-link.sh`.
- License: GPL-3.0-or-later. A PS5 build linking it is a GPL work as a whole.
  This repository is CC0, which allows that; the PS5 build's source must be
  published with it, as it already is.

### ps5-vulkan (older)

Runtime SPIR-V compilation, native 1080p presentation, no loader/ICD; support
"deliberately bounded" to some formats, stages and resource shapes. Also
GPL-3.0-or-later (static `libps5vk.a`).

### OpenGL ES

No standalone GL ES SDK was found for PS5 homebrew. PS5_Vulkan's build mentions
a "PS5 OpenGL SDK 0.3.0", only as where its shader compiler comes from.

## Toolchain and console setup

- [ps5-payload-dev/sdk](https://github.com/ps5-payload-dev/sdk) (John
  Törnblom's; PS5_Vulkan pins v0.42) with its own clang 18 and lld
  (`prospero-clang`). PS5_Vulkan's author keeps a fork,
  [mihawk-99/PS5_PayloadSDK](https://github.com/mihawk-99/PS5_PayloadSDK),
  for its shared platform layer (used here at commit 95c08f2), and builds
  Mesa 26.2.0 on an Arch/CachyOS host (`make doctor` checks the setup).
- The console: jailbroken, with a homebrew environment (PS5_Vulkan was
  validated with etaHEN, ShadowMountPlus, ftpsrv on port 2121 and klogsrv on
  port 3232).
- **The game has to be a title, not a payload.** A payload sent to the ELF
  loader (port 9021) runs inside the loader's process. PS5_Vulkan's driver,
  PS5 RetroArch and vkQuake all run as *titles*: directory-style homebrew
  applications (`eboot.bin`, a fake-signed FSELF; `sce_sys/param.json`;
  `sce_module/libc.prx`, a clean-room shim) copied to
  `/data/homebrew/<TITLE_ID>/` over FTP and started from the home screen,
  which ShadowMountPlus lists them on. VideoOut, AGC, the pad and audio are a
  title's. A title differs from a payload in what the memory probe found:
  - anonymous memory comes from a budget of flexible memory (448 MB on
    PS5_PayloadSDK's console and on this one), and anything larger from
    direct memory (12 GB, mapped with `sceKernelMapDirectMemory`);
  - direct memory refuses execute at map time and gets it from
    `sceKernelMprotect` (read, write and execute at once is granted); a
    plain `mmap` below 4 GB works too, and one asked for with execute was
    placed, though no code was run in it;
  - `sceKernelJitCreateSharedMemory` is refused; a title starts with
    flush-to-zero and denormals-are-zero set, which `ps5_fp_ieee()` clears;
  - `access()` answered on this console's 13.59 (the smoke test found
    `/app0`, its results file, and `ENOENT` for a missing file); PS5_Vulkan's
    notes say it fails with `EPERM` for every path on 12.09, so don't lean on
    it;
  - files the title creates need mode 0666 or 0777 for FTP's user to read
    them (the title runs as its own user).
  (PS5_PayloadSDK's `platform/docs/PROBE.md` and PS5_Vulkan's
  `docs/PLATFORM_NOTES.md` have their measurements, taken on 12.09;
  `PS5_FINDINGS.md` has this console's.)
- Building a title: PS5_Vulkan's packaging (its C runtime, linker script,
  FSELF tool and libc.prx) over its SDK fork. `port/ps5/probe/title/build.sh`
  uses them from a PS5_Vulkan checkout (`~/projects/ps5-deps/PS5_Vulkan`, set
  up by its `tools/setup-native-dependencies.sh`). Its RADV build needs meson
  1.4+, LLVM 18 with Clang's headers, libclc 18, the SPIR-V translator 18 and
  glslang; here they are unpacked from Ubuntu's packages without root
  (`~/projects/ps5-deps/hostdeps/env.sh`). The driver archive has to be
  compiled with clang 22 (`LLVM_CONFIG=/usr/bin/llvm-config-22`): with clang
  18, Mesa's weak entry points keep default visibility and the title link
  refuses them as imports no system library has.
- PS5_Vulkan's `payload/ps5vkctl` is a small resident payload that starts and
  closes titles over the network, for runs without a person at the console.

## The CPU side is the bigger job

The PS5 is x86-64 (Zen 2) running a FreeBSD-based system. The game is a 32-bit
program, and the port runs it three ways on `main`:

- Windows and Linux: a plain 32-bit x86 program (`i686-pc-windows-msvc`,
  `tools/windows_build.py`; the Linux build likewise). On 64-bit Windows it
  runs under WOW64; the port does nothing special.
- Android and the Switch: no 32-bit userland, so a guest/host split. The game
  (the guest) is built for arm64_32 (64-bit ARM instructions, 32-bit
  pointers); a 64-bit host keeps its memory below 4 GB, widens its pointers on
  the way to the system and answers its system calls
  (`port/switch/host/host_syscall.c`, `host_memory.c`, `host_thread.c`,
  `host_loader.c`).

WOW64's way (a 32-bit code segment, 32-bit system calls) needs the kernel's
help: FreeBSD's `COMPAT_FREEBSD32`. Nothing found says the PS5's kernel has it,
homebrew is 64-bit only as far as is known, and there are no 32-bit system
libraries. Not confirmed; it would take a test on a console.

The workable route is the Switch's: x86's arm64_32 is the x32 ABI (64-bit x86
instructions, 32-bit pointers), which runs in ordinary 64-bit mode and needs
nothing from the kernel. Upstream PR #359 ("64-bit x86 Linux build as an x32
guest", `ninja linux_x86_64`, OpenCommunityEdition/OpenCE; still open there,
merged into this branch at `11eb35b8a`) builds exactly that guest. A PS5 host
would do what the Switch's does: reserve the guest's memory below 4 GB, widen
pointers, answer system calls, with the payload SDK in place of libnx.

## What to reuse

- Renderer: the Android port's Vulkan renderer is the closest start
  (`port/android/guest/d3d8_vk.c` and the host side `port/android/host/host_vk*.c`).
  It would present through `VK_KHR_display` on VideoOut instead of Android's
  surface, and its shader cache would have to suit runtime compilation on the
  console.
- Host: `port/switch/host` is the model for a non-Linux host (system calls,
  threads, futexes, memory, files, the map cache kept in memory:
  `host_memory_files.c`). The Android host is the other reference.
- Build: `tools/switch_build.py` shows how a second guest/host target was made
  from the Android one.

## Open questions, in the order to answer them

1. Can a PS5 homebrew process map memory below 4 GB at fixed addresses, and run
   code there? **Yes** (2026-10-16, `port/ps5/probe/mem`, two runs on a
   console reporting firmware 0x13600007, payload sent to the ELF loader on
   port 9021):
   - every megabyte from 64 KB to 4 GB maps, but for what the loader placed
     there; the image (0x3c000000), Custom Edition's tag cache, the Xbox
     window at 0x80000000 (128 MB and 512 MB), a 256 MB pool and 1 GB from
     0x40000000 all map whole; an address hint without MAP_FIXED is taken;
   - code there runs whichever way it is made executable (PROT_EXEC at
     mmap, mprotect, a shared memory object mapped twice, kernel_mprotect),
     stays readable, loads through 32-bit pointers, jumps to the host's code
     above 4 GB and runs on a stack below 4 GB; a pthread runs on a stack
     below 4 GB. (The payload has the loader's privileges: it was itself
     loaded into rwx anonymous memory.)
   - MAP_EXCL is ignored (it mapped over an existing page) and MAP_32BIT
     places memory above 4 GB. The host must read the map
     (sysctl KERN_PROC_VMMAP) before every fixed mapping.
   - about 1.5 MB of rwx memory from the loader sits below 4 GB at an address
     that moves between runs (0x778c8000, then 0x39664000). It could land in
     the guest's ranges, so the host has to check for it at start-up.
2. Does PR #359's x32 guest run on the PS5? **Its CPU side does**
   (2026-10-16, `port/ps5/probe/guest`, PPSA99102, as a title):
   - a title has PS5_Vulkan's `libc.prx` at 0x80000000-0x80118000, so the
     Xbox window cannot be there. The guest now takes it where the host puts
     it (`HALO_MOVABLE_WINDOW`, `configure.py --linux-x86-64-movable-window`,
     Android's mechanism with the size kept at 512 MB); the probe puts it at
     0x90000000. Everything else below 4 GB is free but the title's code at
     0x400000;
   - the image's range (0x70000000, 64 MB, read, write and execute), the
     window, Custom Edition's range and a 256 MB pool at 0x10000000 map as
     direct memory; code there runs, loads through 32-bit pointers, calls
     the host above 4 GB and runs on a stack below 4 GB, on the main thread
     and a pthread;
   - the game loads, its header and 265 imports check, and it runs its C
     library's start, reserves its window and starts its allocator (14 host
     calls, 0.1 s), up to `host_sdl_get_base_path`, the first import the
     probe does not answer (run 3: with the window at 0x90000000, where
     the probe placed it). Titles start with MXCSR 0x9fc0 (FTZ and DAZ);
     `ps5_fp_ieee()` makes it 0x1f80.
3. How much of `port/switch/host` ports to the payload SDK's libc and FreeBSD
   system calls (threads, futex equivalent `_umtx_op`, mmap, sockets, files)?
   Open; the plan below answers it file by file.
4. Input and audio: **they work** (`port/ps5/probe/io`, PPSA99101).
   - AudioOut at 48 kHz in grains of 256 frames, 16-bit and float stereo,
     opened for the system user (0xff): 2 s of sound took 2 s of calls, the
     slowest 5.4 ms, no errors;
   - the pad (opened for the initial user): every button, both sticks, the
     analog triggers and the touch pad; 871 reads in 15 s, up to 45 samples
     a read, none empty, no errors. Bit 0x80000000 of the buttons
     ("intercepted") is set while the system has the pad (its menu);
   - `scePadSetVibration` answers 0 (whether the pad shook was not asked).
   Networking is not probed.
5. The Vulkan driver on this firmware: **it works**
   (`port/ps5/probe/vk`, PPSA99014, PS5_Vulkan's own smoke test): 102 of 102
   checks passed on system software 13.590.001, where PS5_Vulkan had been
   proved on 12.02, 12.09 and 12.70: RADV NAVI21, Vulkan 1.4, fills,
   copies, compute, tessellation, geometry, scratch, mesh and task shaders,
   acceleration structures, a GPU write to a title's anonymous memory, and
   `VK_KHR_display`: one 3840x2160 display at 59.94 Hz, FIFO presents paced
   by its flips. (The payload memory probe had read 0x13600007 from the
   kernel; titles report 0x13590001 from `sceKernelGetSystemSwVersion`.)

## Plan: the host

Superseded by `port/ps5/PS5.md`, the phased plan worked the way the deko3d
and Vulkan plans were; what follows is the first sketch, kept for the record.

The Switch's way again: the guest is the x32 image of `ninja linux_x86_64`
(with the movable window), and the host is the Android host library
(`port/android/host`) with the files that touch the system replaced, built
as a title with PS5_Vulkan's toolchain (`port/ps5/probe/title/build.sh` is
the first version of that build). Each step ends with something to run on
the console.

### 1. The build (`ninja ps5`)

- `tools/ps5_build.py`, after `tools/switch_build.py`: the guest from
  `tools/linux_guest_build.py`'s x32 target with the movable window and the
  Vulkan renderer (`generate_guest_image(..., vulkan=True)`, which only
  Android uses so far: `d3d8_vk.c` and its texture and shader files in place
  of `d3d8_gl.c`); the host with the title toolchain, linked with RADV's
  archive (clang 22, as `port/ps5/probe/vk/build.sh` builds it); packaged as
  a title with the image beside the eboot.
- A title ID of our own (PPSA, five digits), `param.json`, an icon.
- `tools/ps5_deploy.py`: FTP upload, as `deploy.sh`; start and stop through
  PS5_Vulkan's `ps5vkctl` payload (port 9111), so a run needs no hands on
  the console.
- Logs: the kernel log and a file in the data folder, as the probes do.

### 2. The host's core, no graphics

Shared Android host files used as they are, or nearly: `host_common.c`
(boot structure, environment, logging), `host_loader.c` (the image at
0x70000000, never moved), `host_thread.c` (stacks below 4 GB through
`pthread_attr_setstack`, proved), `host_debug.c` (faults, with the
registers in `uc_mcontext.mc_*`), `host_memory.c`'s bookkeeping.

New files, after the Switch's:

| File | What it does on the PS5 |
|---|---|
| `host_main.c` | title start-up: `ps5_fp_ieee()`, the data folder, the environment, the user service |
| `host_mman.c` | the image's range, the window, Custom Edition's range and a pool as direct memory (`sceKernelAllocateDirectMemory`, `sceKernelMapDirectMemory` fixed, `sceKernelMprotect` for execute), zeroed, and the guest's `mmap`/`munmap`/`mprotect` served inside them, as the probe does. The window (512 MB) goes to 0x80000000 if that is free, else to the next 256 MB boundary where it fits: 0x90000000 on this console |
| `host_syscall.c` | the guest's Linux system calls on the PS5's FreeBSD libc, after the Switch's (which maps them onto newlib): files, `stat` layouts and open flags translated, clocks, `writev` to the log |
| `host_futex.c` | futexes over `_umtx_op` (or a mutex and condition variable per address, as the Switch's) |

Memory budget: a title's own heap (flexible memory) is 448 MB, and RADV,
glslang and the host's tables live in it; the guest's memory and the GPU's
come from the 12 GB of direct memory.

Ends with: the game running headless with SDL answered by stubs, as far as
it goes before it needs a frame.

### 3. SDL: window, events, pad, audio

The guest calls SDL3 through `host_sdl_*` imports (69 on the desktop
guest: Android's 34, and the 35 of `port/linux/host/host_imports.list`:
window, display, renderer and file dialog calls). Two ways:

- **A shim** (`host_sdl_ps5.c`, the Switch's `host_sdl2.c` and
  `host_sdl3_events.c` as the model): one fixed window the size of the
  display, events made from pad samples, the gamepad calls on `scePad*`,
  audio streams on an `sceAudioOut` thread (float stereo, 48 kHz, 256
  frames a grain, as probed), ticks from the TSC, and the desktop-only calls
  (message boxes, file dialogs, the debug renderer, display modes) answered
  as a fixed console would.
- **SDL3 itself**, if PS5_Dusklight's SDL3 port (RADV, Dawn, SDL3) holds up:
  then `port/android/host/host_sdl.c`, which forwards to real SDL3, nearly
  as it is. Look at it first; take the shim if it is not a clean, buildable
  port.

The game's desktop code paths stay (the guest is built without
`HALO_ANDROID`); the self-updater is left out (`host_update_*` answer
"unavailable"), as on a release build without it.

Ends with: menus driven by the pad and heard, with nothing drawn yet.

### 4. Rendering: the Vulkan renderer on RADV

- `host_vk*.c` from the Android host, with two files replaced:
  `host_vk_driver.c` (no adrenotools: RADV is linked in, and its
  `vkGetInstanceProcAddr` is called directly, as PS5_Vulkan's titles do) and
  `host_vk_present.c` (`VK_KHR_display` on VideoOut instead of an Android
  surface; the smoke test's sequence, which passed: display, mode, plane
  surface, FIFO swapchain, replacement).
- glslang compiled for the console, for `host_vk_shader_compile`: Mesa's
  ACO already builds there as C++, so the toolchain handles it; whether
  glslang does without changes is the first thing to try. The fallback is
  SPIR-V compiled on the PC and shipped in the title.
- The SPIR-V and pipeline caches in the data folder (`host_vk_shaders.c`
  already keeps them as files).
- The display is 3840x2160 at 59.94 Hz: render at 1080p and scale, then
  try native.
- PS5_Vulkan's known gaps to check against the game: imprecise occlusion
  queries (lens flares, the sun's glow), line lists.

Ends with: the game on the TV.

### 5. The rest

- Game data: the maps and the ISO in a folder of the console's storage
  reached over FTP (`/data/halo`), named in `config.toml`; saves and
  settings beside them.
- The PS button: the pad's intercepted bit pauses input; suspend and resume
  have to be tried with RADV holding the display.
- Networking (FreeBSD sockets through `host_syscall.c`, the Switch's
  `host_net.c` as the model), last.

### Risks

- glslang on the console (step 4's fallback covers it).
- 448 MB of flexible memory for everything the host allocates.
- Firmware: proved on 13.59 here and 12.x by PS5_Vulkan; other versions
  unknown.
- PS5_Vulkan's toolchain and SDK fork are GPL-3.0-or-later, so the PS5
  build is a GPL work (see above); nothing of theirs is copied into this
  repository.

## Sources

- https://github.com/mpereiraesaa/PS5_Vulkan
- https://github.com/mpereiraesaa/ps5-vulkan
- https://github.com/mpereiraesaa/ps5-vulkan/pull/4 (runtime vertex/fragment compilation)
- https://github.com/Swordpdf/HB_Vulkan
- https://github.com/mihawk-99/PS5_RetroArch
- https://github.com/griches/PS5_Dusklight (RADV, Dawn WebGPU, SDL3)
- https://github.com/leolobato/playden-ps5 (Windows games via DXVK/Zink on Vulkan)
- https://github.com/ps5-payload-dev/sdk (the payload SDK, v0.42)
- https://github.com/mihawk-99/PS5_PayloadSDK (PS5_Vulkan's fork of it, with the platform layer titles use)
- https://github.com/ymatrix1/PortPS5/wiki
