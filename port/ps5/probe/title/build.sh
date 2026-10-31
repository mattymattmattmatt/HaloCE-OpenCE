#!/usr/bin/env bash
# Builds one of the PS5 probes as a title: a directory-style homebrew
# application (eboot.bin, sce_sys/param.json, sce_module/libc.prx) that the
# console runs as a game, which a payload sent to the ELF loader is not. The
# Vulkan driver, VideoOut, the pad and audio are a title's, so the probes that
# ask about them, and the guest that will need them, run as titles.
#
# The packaging is PS5_Vulkan's (github.com/mpereiraesaa/PS5_Vulkan): its
# payload SDK fork (the v0.42 SDK with a platform layer for titles), its C
# runtime (tooling/native/app_crt.cpp), linker script, FSELF tool and libc.prx
# shim, as its tools/build.sh uses them, all GPL-3.0-or-later. Run its
# tools/setup-native-dependencies.sh once first.
#
#   port/ps5/probe/title/build.sh <probe folder>
#
# The probe folder holds its C and assembly sources and param.json, and
# optionally prebuild.sh, run with the build's work folder (where it may
# write headers the sources include), title-files.list, files of this
# repository (one path a line) copied into the title's folder, and
# build-extra.sh, sourced after this script has set its variables and before
# it compiles, which may append to the arrays extra_cflags (C sources),
# extra_cxxflags (C++: the runtime and any .cpp of the probe), extra_objects
# and extra_link (inputs and flags for prospero-lld, after the probe's
# objects) and extra_stubs (stub libraries ps5-native-tool link is given
# besides the SDK's), and may replace linker_script (prospero-lld's -T and
# its search paths). The title is made
# in build/ps5/probe/<TITLE_ID>/; copy that folder to
# /data/homebrew/<TITLE_ID>/ on the console over FTP and start it from the
# home screen (ShadowMountPlus lists it). Results are written beside it, in
# the title's /app0, which FTP reaches.
#
# PS5_VULKAN names the PS5_Vulkan checkout (~/projects/ps5-deps/PS5_Vulkan).

set -euo pipefail

repository=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../../.." && pwd)
probe=$(cd -- "${1:?usage: build.sh <probe folder>}" && pwd)
vulkan=${PS5_VULKAN:-$HOME/projects/ps5-deps/PS5_Vulkan}
sdk="$vulkan/.deps/native/ps5-payload-sdk"
native="$vulkan/tooling/native"
tool="$vulkan/build/host/ps5-native-tool"

[[ -f $sdk/.ps5-sdk-revision ]] ||
    { echo "no SDK fork in $sdk: run $vulkan/tools/setup-native-dependencies.sh" >&2; exit 2; }
[[ -f $probe/param.json ]] || { echo "$probe has no param.json" >&2; exit 2; }
title_id=$(python3 -c 'import json, sys; print(json.load(open(sys.argv[1]))["titleId"])' "$probe/param.json")
[[ $title_id =~ ^PPSA[0-9]{5}$ ]] || { echo "param.json's titleId must be PPSA and five digits" >&2; exit 2; }

work="$repository/build/ps5/probe/work/$title_id"
app="$repository/build/ps5/probe/$title_id"
mkdir -p "$work/obj"

# the FSELF tool, built as PS5_Vulkan's tools/build.sh builds it
if [[ ! -x $tool ]]; then
    zlib_root="$vulkan/.deps/native/zlib/root"
    mkdir -p "${tool%/*}"
    "${CXX:-clang++}" -std=c++20 -O2 -Wall -Wextra -Werror -I "$zlib_root/usr/include" \
        "$native/native_app_builder.cpp" "$native/self_container.cpp" "$native/elf_object.cpp" \
        "$native/sce_module_writer.cpp" "$(find "$zlib_root" -name libz.a -print -quit)" -o "$tool"
fi
[[ -f $vulkan/runtime/libc.prx ]] || bash "$vulkan/tools/rebuild-libc.sh"

if [[ -f $probe/title-files.list ]]; then
    while read -r file; do
        [[ -z $file || -f $repository/$file ]] ||
            { echo "$file is missing: build it first (title-files.list)" >&2; exit 2; }
    done < "$probe/title-files.list"
fi
[[ ! -x $probe/prebuild.sh ]] || "$probe/prebuild.sh" "$work"

cc() { PS5_PAYLOAD_SDK="$sdk" sh "$vulkan/tooling/prospero-clang18" "$@"; }
extra_cflags=()
extra_cxxflags=()
extra_objects=()
extra_link=()
extra_stubs=()
linker_script=(-T "$native/ps5-pie.ld")
# shellcheck disable=SC1091
[[ ! -f $probe/build-extra.sh ]] || source "$probe/build-extra.sh"

cflags=(-std=c11 -O2 -g -Wall -Wextra -Werror -ffunction-sections -fdata-sections
    -I "$repository/port/ps5/probe/title" -I "$repository/port/android/include" -I "$work")
objects=()
for source in "$probe"/*.c "$repository/port/ps5/probe/title/probe_report.c"; do
    object="$work/obj/$(basename "$source").o"
    cc "${cflags[@]}" "${extra_cflags[@]}" -c "$source" -o "$object"
    objects+=("$object")
done
for source in "$probe"/*.S; do
    [[ -e $source ]] || continue
    object="$work/obj/$(basename "$source").o"
    cc -c "$source" -o "$object"
    objects+=("$object")
done
cxxflags=(-std=c++20 -O2 -Wall -Wextra -fno-exceptions -fno-rtti -ffunction-sections -fdata-sections)
cxxflags+=("${extra_cxxflags[@]}")
for source in "$probe"/*.cpp; do
    [[ -e $source ]] || continue
    object="$work/obj/$(basename "$source").o"
    cc "${cxxflags[@]}" -I "$repository/port/ps5/probe/title" -c "$source" -o "$object"
    objects+=("$object")
done
cc "${cxxflags[@]}" -c "$native/app_crt.cpp" -o "$work/obj/app_crt.o"
cc "${cxxflags[@]}" -c "$native/app_cpp_runtime.cpp" -o "$work/obj/app_cpp_runtime.o"

"$sdk/bin/prospero-lld" "${linker_script[@]}" --eh-frame-hdr \
    --version-script "$native/app-symbols.map" --exclude-libs=ALL \
    -e _start -o "$work/llvm-pie.elf" \
    "$work/obj/app_crt.o" "$work/obj/app_cpp_runtime.o" "${objects[@]}" "${extra_objects[@]}" \
    "${extra_link[@]}" \
    "$sdk/target/lib/libps5platform.a" \
    --as-needed "$sdk"/target/lib/*.so
# (the loader constants PS5_Vulkan's titles use, validated on firmware 6.02
# and 12.70)
stub_arguments=()
for stub in "${extra_stubs[@]}"; do
    stub_arguments+=(--stub "$stub")
done
"$tool" link --in "$work/llvm-pie.elf" --out "$work/eboot.elf" --stub-dir "$sdk/target/lib" \
    "${stub_arguments[@]}" --module-sdk 0x02000009 --companion-sdk 0x08050001 --file-name eboot.elf

rm -rf -- "$app"
mkdir -p "$app/sce_sys" "$app/sce_module"
"$tool" self --sign --in "$work/eboot.elf" --out "$app/eboot.bin" --magic 0x1D3D154F
cp "$probe/param.json" "$app/sce_sys/param.json"
cp "$vulkan/sce_sys/icon0.png" "$app/sce_sys/icon0.png"
(cd "$vulkan/runtime" && sha256sum --check --strict --quiet libc.prx.sha256)
cp "$vulkan/runtime/libc.prx" "$app/sce_module/libc.prx"
if [[ -f $probe/title-files.list ]]; then
    while read -r file; do
        [[ -z $file ]] || cp "$repository/$file" "$app/"
    done < "$probe/title-files.list"
fi
"$tool" self --inspect --file "$app/eboot.bin" > /dev/null
echo "title: $app"
