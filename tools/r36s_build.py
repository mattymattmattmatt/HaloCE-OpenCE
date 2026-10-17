#!/usr/bin/env python3
"""R36S FEASIBILITY EXPERIMENT (branch r36s-test, throwaway): compile and
link the game for 32-bit ARM Linux (armhf) with the OpenGL ES renderer -
path (a) of the R36S feasibility assessment.

Not wired into configure.py / build.ninja, so the existing linux, windows,
android and web targets (and their CI) are untouched.

    python3 tools/r36s_build.py [--jobs N]

Host needs (Debian/Ubuntu x86-64): clang, gcc-arm-linux-gnueabihf, and the
armhf SDL3 headers/libraries (dpkg --add-architecture armhf, then
libsdl3-dev:armhf). Output is build/r36s/halo plus one .log per failed
unit in build/r36s/logs. The flags mirror tools/linux_build.py's with the
x86 ones replaced (no -m32, -malign-double, -freg-struct-return) and
HALO_GLES=1, which also selects the Xbox-sized texture/sound caches and
the 128 MB memory window a 1 GB handheld needs.
"""

import argparse
import concurrent.futures
import json
import shlex
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

from tools.linux_build import (  # noqa: E402
    EXPAT_DIR, EXPAT_SOURCES, GAME_FLAGS, KCP_DIR, MBEDTLS_DIR, MINIUPNPC_DIR,
    MINIUPNPC_DEFINES, MONOCYPHER_DIR, MUSL_MATH_DIR, PLATFORM_FLAGS, STB_VORBIS_SOURCE,
    TOML_DIR, XDK_INCLUDE, ZLIB_DEFINES, ZLIB_DIR, ZLIB_SOURCES, game_defines_and_includes,
    game_sources, miniupnpc_sources, musl_math_cflags, musl_math_sources, opus_cflags,
    opus_sources,
)

BUILD = ROOT / "build/r36s"
TARGET = ["--target=arm-linux-gnueabihf", "-march=armv8-a", "-mfpu=neon", "-mfloat-abi=hard"]
ABI = TARGET + [
    "-fms-extensions", "-fshort-wchar", "-fcommon", "-fno-pic", "-fno-strict-aliasing",
    "-fwrapv", "-fno-delete-null-pointer-checks", "-fno-omit-frame-pointer",
    "-ffp-contract=off", "-O2", "-g", "-DHALO_GLES=1",
    *[f"-fno-builtin-{n}" for n in (
        "wcslen", "wcsnlen", "wcschr", "wcsrchr", "wcscmp", "wcsncmp", "wcscpy",
        "wcsncpy", "wcscat", "wcsncat", "wmemchr", "wmemcmp", "wmemcpy", "wmemmove", "wmemset")],
]
POSIX = TARGET + ["-std=gnu11", "-D_GNU_SOURCE", "-D_FILE_OFFSET_BITS=64", "-fno-pic", "-O2", "-g", "-Wall"]
SDL_CFLAGS = subprocess.run(["pkg-config", "--cflags", "sdl3"], capture_output=True, text=True).stdout.split() \
    if subprocess.run(["pkg-config", "--exists", "sdl3"], capture_output=True).returncode == 0 else ["-I/usr/include"]


def run(cmd, log=None):
    result = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    if result.returncode and log:
        log.parent.mkdir(parents=True, exist_ok=True)
        log.write_text(" ".join(cmd) + "\n\n" + result.stdout + result.stderr)
    return result.returncode == 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--jobs", type=int, default=8)
    args = ap.parse_args()
    config = json.loads((ROOT / "port/linux/port.json").read_text())
    obj_root = BUILD / "obj"
    logs = BUILD / "logs"

    for name, scan in (("halo_msvc_semantics.h", ["--all-inlines", "--tags", "source", "--inlines", "source", "--inlines", str(XDK_INCLUDE)]),
                       ("platform_msvc_semantics.h", ["--inlines", str(XDK_INCLUDE)])):
        out = BUILD / name
        out.parent.mkdir(parents=True, exist_ok=True)
        if not run([sys.executable, "tools/linux_msvc_semantics.py", "--output", str(out), *scan], logs / f"{name}.log"):
            sys.exit(f"failed generating {name}; see {logs}")
    assets = BUILD / "hud_hires_assets.c"
    if not run([sys.executable, "tools/embed_assets.py", str(assets)], logs / "embed_assets.log"):
        sys.exit("failed generating HUD assets")

    prefix = "port/linux/include/halo_linux_prefix.h"
    game_cflags = ABI + GAME_FLAGS + [
        "-include", prefix, "-include", str(BUILD / "halo_msvc_semantics.h"),
        "-Iport/linux/include", "-iquote", config["game_sources"],
        *shlex.split(game_defines_and_includes(config)), f"-idirafter", str(XDK_INCLUDE)]
    platform_cflags = ABI + PLATFORM_FLAGS + [
        "-include", prefix, "-include", str(BUILD / "platform_msvc_semantics.h"),
        "-Iport/linux/src", "-Iport/linux/include", f"-I{TOML_DIR}", f"-I{EXPAT_DIR}",
        f"-I{KCP_DIR}", f"-I{MONOCYPHER_DIR}", f"-I{ZLIB_DIR}", "-Isource", "-Isource/cseries",
        *SDL_CFLAGS, f"-idirafter", str(XDK_INCLUDE),
        # Debian trixie's SDL is 3.2; the port's own is 3.4, which added this hint name
        '-DSDL_HINT_ENABLE_STEAM_SCREEN_KEYBOARD="SDL_ENABLE_STEAM_SCREEN_KEYBOARD"']

    jobs = []  # (source, cflags)
    for src in game_sources(config):
        jobs.append((src, game_cflags))
    for src in sorted(Path(config["game_sources"]).glob("*.c")):
        jobs.append((src, game_cflags))
    jobs.append((STB_VORBIS_SOURCE, game_cflags))
    mbedtls_include = f"-I{MBEDTLS_DIR / 'include'}"
    for src in sorted(Path(config["platform_sources"]).glob("*.c")):
        if src.name.startswith("posix_"):
            extra = [mbedtls_include] if src.name in ("posix_update.c", "posix_dtls.c") else []
            if src.name == "posix_upnp.c":
                extra += [f"-I{MINIUPNPC_DIR / 'include'}", "-DMINIUPNP_STATICLIB"]
            jobs.append((src, POSIX + [f"-Iport/linux/src"] + extra))
        else:
            jobs.append((src, platform_cflags))
    jobs.append((Path("build/r36s/hud_hires_assets.c"), platform_cflags))
    jobs.append((Path("tools/r36s_stubs.c"), platform_cflags))
    for src in sorted((MBEDTLS_DIR / "library").glob("*.c")):
        jobs.append((src, POSIX + [mbedtls_include, f"-I{MBEDTLS_DIR / 'library'}", "-fno-builtin-wcslen", "-w"]))
    for src in miniupnpc_sources():
        jobs.append((src, POSIX + MINIUPNPC_DEFINES + [f"-I{MINIUPNPC_DIR / 'include'}", f"-I{MINIUPNPC_DIR / 'src'}", "-fno-builtin-wcslen", "-w"]))
    jobs.append((TOML_DIR / "tomlc17.c", ABI + ["-std=gnu11", "-w"]))
    for name in EXPAT_SOURCES:
        jobs.append((EXPAT_DIR / name, ABI + ["-std=gnu11", f"-I{EXPAT_DIR}", "-w"]))
    jobs.append((KCP_DIR / "ikcp.c", ABI + ["-std=gnu11", "-w"]))
    for src in opus_sources():
        jobs.append((src, shlex.split(opus_cflags(" ".join(ABI)))))
    for name in ("monocypher.c", "monocypher-ed25519.c"):
        jobs.append((MONOCYPHER_DIR / name, ABI + ["-std=gnu11", "-w"]))
    for name in ZLIB_SOURCES:
        jobs.append((ZLIB_DIR / name, ABI + ["-std=gnu11", *ZLIB_DEFINES, "-w"]))
    for src in musl_math_sources():
        jobs.append((src, shlex.split(musl_math_cflags(" ".join(ABI)))))

    def compile_one(job):
        src, cflags = job
        obj = obj_root / Path(src).with_suffix(".o")
        obj.parent.mkdir(parents=True, exist_ok=True)
        ok = run(["clang", *cflags, "-c", str(src), "-o", str(obj)], logs / (Path(src).as_posix().replace("/", "_") + ".log"))
        return src, ok, obj

    failed, objects = [], []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        for src, ok, obj in pool.map(compile_one, jobs):
            (objects if ok else failed).append(obj if ok else src)
    print(f"compiled {len(objects)}/{len(jobs)} units; {len(failed)} failed")
    for src in failed:
        print(f"  FAILED {src}  (log: build/r36s/logs/{Path(src).as_posix().replace('/', '_')}.log)")
    if failed:
        sys.exit(1)

    out = BUILD / "halo"
    rsp = BUILD / "objects.rsp"
    rsp.write_text("\n".join(f'"{o}"' for o in objects))
    cmd = ["clang", *TARGET, "-no-pie", "-g", "-o", str(out), f"@{rsp}",
           "-L/usr/lib/arm-linux-gnueabihf", "-lSDL3", "-lGLESv2", "-lEGL", "-lm", "-lpthread", "-ldl"]
    if not run(cmd, logs / "link.log"):
        sys.exit("link failed; see build/r36s/logs/link.log")
    print(f"linked {out}")


if __name__ == "__main__":
    main()
