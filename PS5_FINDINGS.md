# PS5 port: findings

Everything learned on 2026-10-16 while getting the PS5 port started: the
toolchain, how homebrew runs on the console, and what each probe measured on
the console. `PS5_HANDOFF.md` has the shorter summary and the plan for the
host. The raw reports are in `port/ps5/probe/results/`.

## The console

- A jailbroken PS5 on the local network at 192.168.2.157, with:
  - etaHEN;
  - ShadowMountPlus, which puts folders under `/data/homebrew/<TITLE_ID>/`
    on the home screen as titles;
  - ftpsrv on port 2121;
  - an ELF loader on port 9021.
- System software 13.590.001: `sceKernelGetSystemSwVersion` reports
  0x13590001 in a title. The payload SDK's `kernel_get_fw_version`, which
  the payload memory probe used, reads 0x13600007 from the kernel instead.
- The FTP server decrypts SELF files when they are downloaded. A downloaded
  `eboot.bin` or `libc.prx` is the plain ELF, so it is larger than the
  uploaded file and its size matches the ELF from before signing. Compare
  ordinary files by size, and SELFs against the unsigned ELF.
- A title's folder is writable by the title. Files it creates must be
  chmod 0666 for FTP to read them.
- `access()` answers in a title on this firmware: the RADV smoke test found
  `/app0` and its results file (read and write), and got `ENOENT` for a
  missing file. PS5_Vulkan's `docs/PLATFORM_NOTES.md` says it fails with
  `EPERM` for every path, measured on 12.09, so a host should not count on
  it either way.
- Users: `sceUserServiceGetInitialUser` returns 372994645 (0x163b7255), the
  only logged-in user. The other three slots of the login list are -1.

## Payloads and titles

There are two ways to run homebrew, and the game has to be a title.

| | payload | title |
|---|---|---|
| how it runs | sent to the ELF loader (port 9021); runs inside the loader's process | `eboot.bin` (a fake-signed FSELF), `sce_sys/param.json`, `sce_module/libc.prx`, in `/data/homebrew/<ID>/`, started from the home screen |
| GPU, VideoOut, pad, audio | not its own | its own |
| memory below 4 GB | plain `mmap`, with execute at map time or later | direct memory at fixed addresses, made executable afterwards by `sceKernelMprotect` (asking for execute at map time is refused, per PS5_PayloadSDK's `platform/docs/PROBE.md`); a plain `mmap` below 4 GB also works, and one asked for with execute was placed, though no code was run in it |
| built with | ps5-payload-dev SDK v0.42 (`prospero-clang`) | PS5_Vulkan's packaging over its SDK fork |

PS5_Vulkan's driver, PS5 RetroArch and vkQuake all run as titles.

### Building a title

`port/ps5/probe/title/build.sh <probe folder>` builds one. It uses a
PS5_Vulkan checkout (`~/projects/ps5-deps/PS5_Vulkan`, set up by its
`tools/setup-native-dependencies.sh`):

1. Compile with `tooling/prospero-clang18`, pointed at the SDK fork in
   `.deps/native/ps5-payload-sdk`. The fork is PS5_PayloadSDK at commit
   95c08f2, which had to be fetched by commit because it is not on a branch.
2. Link with `prospero-lld -T ps5-pie.ld --eh-frame-hdr --version-script
   app-symbols.map --exclude-libs=ALL -e _start`. The inputs are the fork's
   `app_crt.cpp`, `app_cpp_runtime.cpp`, `libps5platform.a` and the
   system's stub `.so` files (`--as-needed`).
3. Run `ps5-native-tool link --module-sdk 0x02000009 --companion-sdk
   0x08050001`. These loader constants were validated on 6.02 and 12.70, and
   now 13.59.
4. Run `ps5-native-tool self --sign --magic 0x1D3D154F` to make the FSELF.
5. Copy in `param.json`, PS5_Vulkan's icon, and `runtime/libc.prx`, after
   checking its sha256.

`param.json` needs a `titleId` of `PPSA` and five digits, and a
`contentId`. The probes use PPSA99101 and PPSA99102.

`port/ps5/probe/title/deploy.sh <console> <TITLE_ID>` uploads a title over
FTP (`curl --ftp-create-dirs -T`). It sends `eboot.bin` and `param.json`
last, so the title never appears half uploaded.

PS5_Vulkan's `payload/ps5vkctl/ps5vkctl.elf` (built by its
`tools/build-ps5vkctl.sh`) is a payload that stays resident on port 9111. It answers `ping`, `status`, `users`, `procs`, `launch <ID>`,
`kill <ID>` and `restart <ID>`, which would let a run happen with nobody at
the console. It hasn't been used yet.

### Building the Vulkan driver (RADV)

`port/ps5/probe/vk/build.sh` runs PS5_Vulkan's `tools/build-radv.sh release`,
then `tools/build-radv-title.sh` against the release archive. The tools
didn't build out of the box on this machine (Ubuntu under WSL, no sudo):

- Mesa needs meson 1.4 or newer, LLVM 18 with Clang's headers, libclc 18,
  the SPIR-V translator 18 and glslang. They are unpacked from Ubuntu's
  `.deb` files into `~/projects/ps5-deps/hostdeps`, and its `env.sh` puts
  them on the path.
- Mesa picked LLVM 22, the newest installed, but no SPIR-V translator 22
  exists. The hostdeps `meson` wrapper adds a native file that pins LLVM 18
  for `mesa_clc`.
- The unpacked SPIR-V translator's `.pc` file named the wrong library
  folder (`cannot find -lLLVMSPIRVLib`), so it was rewritten.
- **The RADV archive and the title must be built with clang 22**
  (`LLVM_CONFIG=/usr/bin/llvm-config-22`).
  - With clang 18, the PS5 target leaves Mesa's weak entry points at default
    visibility. `ps5-native-tool link` then refuses them as imports that no
    system library exports, starting with `radv_EnumeratePhysicalDevices`.
  - `-z nodynamic-undefined-weak` is unknown to lld 18, and linking with
    lld 22 alone didn't help.
- PS5_Vulkan's `tools/*.sh` lack execute permission in the checkout, so run
  them with `bash`.

The resulting title, PPSA99014, is 42 MB, with a 24 MB `eboot.bin`.

## Memory

### As a payload (`port/ps5/probe/mem`, two runs)

The full reports are in `results/mem-run1.txt` and `mem-run2.txt`.

- The process's map (`sysctl KERN_PROC_VMMAP`):
  - About 1.5 MB of the loader's memory sits below 4 GB, at an address that
    changes between runs (0x778c8000, then 0x39664000). It is rwx, then r--,
    r--, rw-, plus 1 MB rw-.
  - Everything else is above 4 GB:
    - the payload: code at 0x2006xxxxx, rwx;
    - its stack: around 0x7eexxxxxx;
    - libc and other modules: around 0x80xxxxxxx–0x83xxxxxxx;
    - malloc: around 0x880xxxxxx.
- `MAP_EXCL` is ignored: it mapped over an existing page. `MAP_32BIT`
  without an address placed memory above 4 GB (0x2006c8000). A host must
  read the map before each fixed mapping.
- Every page from 64 KB to 4 GB maps (4094 of 4095 MB), except for the
  loader's megabyte.
- An address hint without `MAP_FIXED` is honoured: 0x3c000000 and
  0x80000000 both mapped where asked.
- Whole ranges mapped and were written and read back at both ends:
  - 16 MB at 0x3c000000 (the image's address under the older ABI);
  - Custom Edition's 23 MB at 0x40440000;
  - the window as 128 MB, and as 512 MB, at 0x80000000;
  - 256 MB at 0x10000000;
  - 1 GB at 0x40000000.
- Code below 4 GB ran no matter how it was made executable:
  - `PROT_EXEC` at `mmap`;
  - `mprotect` to read and execute;
  - a shared memory object mapped writable above 4 GB and read-execute
    below it;
  - the SDK's `kernel_mprotect`.

  In every case it was still readable, loaded through a 32-bit pointer,
  jumped to host code above 4 GB, and ran on a stack below 4 GB.
- A pthread ran on a stack given below 4 GB (0x3c200000–0x3c240000).

### As a title (`port/ps5/probe/guest`, sections 1–4)

The full reports are in `results/guest-run1.txt` and `guest-run2.txt`.

- **Flexible memory:** 448 MB configured, 442 MB available at start. It
  backs malloc (0x880034c00) and anonymous `mmap` (0x20001c000).
- **Direct memory:** 12288 MB, with the largest free block 12286 MB at
  physical 0x200000.
- **The map** (`sceKernelVirtualQuery` with flag 1 to find the next region;
  a 72-byte info struct; 15 regions):

  | range | what |
  |---|---|
  | 0x400000–0x410000 | the title's eboot: code r-x, data r--, r--, rw- |
  | **0x80000000–0x80118000** | **PS5_Vulkan's `libc.prx`**: code, then data |
  | 0x200000000–0x20001c000 | system mappings (one is ShellCoreUtil) |
  | 0x7eedf8000–0x7ef000000 | the main thread's 2 MB stack, with a guard page, and SceSysCore |

- **Free space below 4 GB.** Measured by reserving 1 MB at each megabyte
  with `sceKernelReserveVirtualRange`, which reserves at the address only
  if it is free, otherwise moves the reservation or refuses:
  - 0x100000–0x400000 free;
  - 0x400000–0x500000 taken (the eboot);
  - 0x500000–0x80000000 free (2043 MB);
  - 0x80000000–0x80200000 taken (libc.prx);
  - 0x80200000–0x100000000 free (2046 MB).
- **Plain `mmap` in a title:**
  - `MAP_FIXED` read-write at 0xff000000 is placed exactly, written and
    read back;
  - `MAP_FIXED` read-write-execute at 0xff000000 is also placed (the probe
    only checked the placement; it ran no code there);
  - with no address, it lands above 4 GB.
- **Direct memory at fixed addresses.** Steps:
  1. reserve the range exactly (`sceKernelReserveVirtualRange`, then check
     it was placed at the address asked);
  2. `sceKernelAllocateDirectMemory(0, the direct memory's size, length,
     64 KB alignment, CPU type)`;
  3. `sceKernelMapDirectMemory` with flag 0x10 (fixed), read-write;
  4. `sceKernelMprotect(..., 7)` for read, write and execute, where code
     has to run.

  This worked for:
  - the image: 64 MB at 0x70000000, rwx, in 1 ms;
  - the Xbox window: 512 MB at 0x90000000, in 14 ms (0x80000000 is
    refused because of `libc.prx`; the probe steps up by 256 MB, the
    window's alignment, until 512 MB map);
  - Custom Edition: 23 MB at 0x40440000;
  - a pool: 256 MB at 0x10000000, in 7 ms.

  Direct memory comes back holding whatever it held before, so the host
  must zero it.
- `sceKernelJitCreateSharedMemory` is refused in a title.
- **Code below 4 GB in a title (section 5):**
  - code copied to the top of the image's range ran (40 + 2 = 42);
  - it loaded through a 32-bit pointer into the window;
  - it called the host above 4 GB;
  - it read its stack through `%esp` on a stack below 4 GB;
  - `pthread_attr_setstack(0x1ff80000)` and `pthread_create` returned 0,
    and the thread ran with its stack at 0x1ffbffbc.

## Floating point

- A title starts with MXCSR 0x9fc0 (flush-to-zero and denormals-are-zero
  set). `ps5_fp_ieee()` from PS5_Vulkan's platform layer makes it 0x1f80.
- PS5_Vulkan's smoke test reports 0x1f80 on its main thread and on a new
  thread after that call. The host has to clear these flags on every
  thread the guest uses.

## The x32 guest on the PS5 (`port/ps5/probe/guest`, section 6)

The image is `build/linux_x86_64/halo_guest.elf` from `ninja linux_x86_64`
(upstream PR #359, guest ABI 4), built with clang 22. It is ELF32 for
EM_X86_64. Its segments load from 0x70000000 to 0x72a10ff0:

| segment | range | contents |
|---|---|---|
| 0 | 0x70000000–0x70000030 | the header (r) |
| 1 | 0x70000030–0x703e2cd1 | code (r-x), 4.0 MB |
| 2 | 0x703e2ce0–0x70e94b23 | read-only data, 11.2 MB |
| 3 | 0x70ea0000–0x71175214 | data (rw), 3.0 MB |
| 4 | 0x71175280–0x72a10ff0 | bss, about 25 MB |

The header reads magic 0x4f4c4148 ("HALO"), ABI 4, image end 0x72a11000.
The entry points:

| | address |
|---|---|
| `start` | 0x703d0470 |
| `thread_start` | 0x702343b0 |
| `attach` | 0x702344a0 |
| `detach` | 0x70234530 |
| constructors | 0x7117520c–0x71175214 |

There are 265 imports.

How the probe runs it:

- Each import is a thunk at the top of the image's range: `mov r10d,
  <index>`, `movabs r11, import_dispatch`, `jmp r11`. The guest's call
  leaves its arguments in their registers (the 7th on the stack), so a jump
  into C passes them all on.
- The boot structure is built in the pool:
  - argv "halo", an empty environment;
  - page size 0x4000;
  - the window's base and size of 512 MB;
  - an image shift of 0.
- `start` runs on a pthread with a 2 MB stack below 4 GB.

**Run 1:** stopped before the game. The window couldn't go at 0x80000000
because of libc.prx, and a bug in the probe found no room for the pool.

**Run 2** (with the window movable):

```
host_set_tp(0x728c870c)
host_syscall gettid() = 1
host_syscall mmap(0xa0000000, 0x20000000, PROT_NONE, MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE|MAP_FIXED_NOREPLACE)
  the game writes to stderr: halo-linux: Xbox contiguous memory window: 512 MB at 0xa0000000
host_syscall brk(0) = 0, brk(0x2000) = 0
host_syscall mmap(0, 0x2000, PROT_NONE, MAP_PRIVATE|MAP_ANONYMOUS) = 0x10011000
host_syscall mprotect(0x10011000, 0x1000, read|write) = 0
host_syscall mmap(0, 0x1000, read|write, MAP_PRIVATE|MAP_ANONYMOUS) = 0x10021000
the first import the probe does not answer: host_sdl_get_base_path(0x728c87a0, 0x400)
```

That is 14 host calls in 0.1 s: musl's start-up, the thread pointer, the
window's reservation, malloc's first pages, and then SDL.

The window reads 0xa0000000 rather than 0x90000000 because of a second
probe bug. Its placement loop stepped the address once more after the
mapping succeeded, so the probe told the game a base where nothing was
mapped.

**Run 3** (`results/guest-run3.txt`, the placement fixed): the same 14
calls, with the window where the probe mapped it. The game reserves
`mmap(0x90000000, 512 MB, ...)`, which the probe answers with its own
mapping, and prints "Xbox contiguous memory window: 512 MB at 0x90000000".
It stops at the same place, `host_sdl_get_base_path`. Everything before
SDL runs on the console.

What the runs tell a host:

- The game reserves its window with `MAP_FIXED_NOREPLACE` (0x100000) and
  no `MAP_FIXED`, so a host has to recognise that flag and hand back the
  range it already mapped.
- Musl calls `brk` first and falls back to `mmap` when it gets 0.
- Its first anonymous `mmap` asks for `PROT_NONE` and follows with
  `mprotect` to read-write, so the pool has to serve `mprotect` too.

### The movable window

A title has `libc.prx` at 0x80000000, so the Xbox window has to go
elsewhere. Android already had the mechanism, because its Java runtime
holds 0x80000000:

- the host places the window and passes the base in
  `halo_guest_boot.contiguous_base`;
- the guest reads it into `platform_contiguous_base` before any
  constructor runs;
- the game moves map data's addresses with `PORT_WINDOW_REBASE` and
  related code.

That code was only enabled for Android builds (`HALO_ANDROID`). The
`HALO_MOVABLE_WINDOW` switch now enables it for the x32 guest too, turned
on by `configure.py --linux-x86-64-movable-window`. It moves only the base;
the size stays 512 MB. The gates that changed:

- `halo_port_window.h`, `platform.h`, `xbox_memory.c`, `guest_start.c`;
- `cache_files.c` (7 sites);
- `tag_groups.c` and `tag_groups.h`;
- `rasterizer_geometry.c` and `rasterizer_geometry.h`;
- the transparent geometry, `tag_validate.c` and `object_mesh.c`.

`xbox_memory.c`'s check that the caches fit the window now measures from
0x80000000, where the game data is linked, instead of from the base. The
desktop Linux host already passes the base through Android's shared boot
code (`host_common.c`), so the movable image also runs there, though that
hasn't been tried. `tools/test_guest_layout.py` and
`tools/test_linux_guest_build.py` pass.

## Input (`port/ps5/probe/io`)

The full report is in `results/io-probe.txt`. The declarations follow PS5
RetroArch's `input_ps5.cpp`.

- `sceUserServiceInitialize` returns 0. `scePadInit` returns 0.
- `scePadOpen(initial user, 0 standard port, 0, NULL)` returns handle
  0x030c0700.
- `scePadRead(handle, samples, 64)`: 871 reads in 15 s, none empty, no
  errors, 9648 samples, at most 45 in one read. The controller fills the
  buffer fast, so the game should read it every frame and keep the latest
  sample.
- A sample is 120 bytes:
  - buttons: u32 at 0;
  - sticks: u8 left x, left y, right x, right y at 4;
  - triggers: u8 left, right at 8;
  - touch count at 0x34;
  - two touches (u16 x, u16 y, u8 id) at 0x3c;
  - `connected`: i32 at 0x4c;
  - timestamp: u64 µs at 0x50;
  - connected count at 0x68.
- Button bits:

  | bit | button | bit | button |
  |---|---|---|---|
  | 0x2 | L3 | 0x400 | L1 |
  | 0x4 | R3 | 0x800 | R1 |
  | 0x8 | Options | 0x1000 | triangle |
  | 0x10 | up | 0x2000 | circle |
  | 0x20 | right | 0x4000 | cross |
  | 0x40 | down | 0x8000 | square |
  | 0x80 | left | 0x100000 | touch pad click |
  | 0x100 | L2 (digital) | 0x80000000 | the system has the pad (its menu) |
  | 0x200 | R2 (digital) | | |

  All of them were seen in the run.
- Sticks rest near 127–130 / 121–123, so they need a dead zone. They reach
  0 and 255. Triggers go from 0 to 255, and L2/R2's digital bits come on
  at a small press.
- Rumble: `scePadSetVibrationMode(handle, 2)`, then
  `scePadSetVibration(128, 128)` and `(0, 0)`, all return 0. The report
  can't tell whether the controller shook.
- `scePadClose` returns 0.

## Audio (`port/ps5/probe/io`)

- `sceAudioOutInit` returns 0.
- `sceAudioOutOpen(0xff system user, 0 main, 0, 256 frames, 48000, format)`
  returns 0x20000000 for format 1 (S16 stereo) and format 4 (float stereo).
- `sceAudioOutOutput` blocks one grain at a time. 375 grains (2.000 s of
  sound) took 1.992 s for S16 and 1.994 s for float. The slowest call was
  5.4 ms, about one grain (5.33 ms), and there were no errors.
- The game mixes 48 kHz float stereo, so format 4 needs no conversion.
- `sceAudioOutClose` returns 0.

## The GPU (`port/ps5/probe/vk`)

PS5_Vulkan's own `radv/radv_smoke.c`, unchanged, as title PPSA99014. The
full report is in `results/vk-radv-smoke.txt`.

- **102 passed, 0 failed.**
- Device: "PlayStation 5 GPU (RADV NAVI21)", Vulkan 1.4.354, Mesa 26.2.0.
- Passed:
  - fill, copy, and compute (64 workgroups of 64);
  - a triangle, read back texel for texel;
  - tessellation: from coordinates, from control points, with varyings,
    levels 2–9;
  - geometry shaders: strips, primitive IDs, early return, indexed,
    indirect, indirect count, primitive restart;
  - scratch memory up to 1024 floats;
  - CPU/GPU coherence both ways with no flush or invalidate;
  - the fragment shader's primitive ID;
  - acceleration structures built on the GPU;
  - mesh and task shaders, direct and indirect.
- Not reported by the driver: `VK_KHR_fragment_shader_barycentric` and
  `VK_KHR_fragment_shading_rate`.
- Coherent memory: 16 MiB read in 1.05 ms and written in 1.29 ms, the same
  as heap memory.
- **The display:**
  - one 3840x2160 display with a single mode at 59.940 Hz;
  - `vkCreateDisplayPlaneSurfaceKHR` and a FIFO swapchain of 3–5 images;
  - 60 frames presented; the last 50 took 0.834 s, exactly the refresh
    rate;
  - the last frame read back correctly;
  - a replacement swapchain worked, and the retired one reports out of
    date.
- A shader wrote a title's anonymous memory through its address, after
  `sceKernelMprotect` with GPU access returned 0.
- RADV's shader cache goes in `/app0/radv-shader-cache`, which FTP can
  reach.
- Known gaps from PS5_Vulkan's README, not tested here:
  - imprecise occlusion queries, which matter for lens flares and the
    sun's glow;
  - line lists don't draw correctly;
  - no buffer device address;
  - no concurrent queues.

## What the probes are

| title | folder | what it does |
|---|---|---|
| (payload) | `port/ps5/probe/mem` | memory below 4 GB as a payload |
| PPSA99014 | `port/ps5/probe/vk` | PS5_Vulkan's RADV smoke test |
| PPSA99101 | `port/ps5/probe/io` | user service, audio (2 s each of 440 Hz S16 and 660 Hz float), 15 s of pad input, 1 s of rumble |
| PPSA99102 | `port/ps5/probe/guest` | the map, free space, direct memory at the guest's addresses, code and a thread below 4 GB, then the x32 game until its first import the probe doesn't answer |

The title probes write `/app0/<name>.txt` (chmod 0666) and the kernel log
(`sceKernelDebugOutText`), and show a notification at each step
(`sceKernelSendNotificationRequest`). The shared code is
`port/ps5/probe/title/probe_report.c`.

The guest probe answers these host calls:

- `host_get_tp`, `host_set_tp`, `host_errno`, `host_log`;
- `host_abort` and `host_exit`, which end the run;
- system calls:
  - `mmap`: anonymous only, from the pool (a file mapping gets `-ENOSYS`);
    fixed or fixed-noreplace inside the guest's ranges, else `-ENOMEM` or
    `-EEXIST`;
  - `munmap`, `mprotect`, `madvise`, signal masks and actions,
    `sigaltstack`, `sched_yield`: answered 0;
  - `brk`: answered 0;
  - the ids: `gettid`, `getpid` and `set_tid_address` answer 1, `getppid`
    0, the user and group ids 1000;
  - `clock_gettime`, `clock_getres`;
  - futex wait (answered `-EAGAIN`);
  - `write` and `writev` to stdout and stderr;
  - `getcwd` (`/app0`);
  - the open/stat family (`-ENOENT`);
  - `exit`.

Anything else ends the run with the call reported. A fault is reported
with its registers. The run ends after 60 seconds.
