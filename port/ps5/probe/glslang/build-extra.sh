# Sourced by port/ps5/probe/title/build.sh (see there): glslang for the
# glslang probe, built static with the SDK fork's CMake toolchain and linked
# whole into the title.
#
# glslang is the Android build's pin (GLSLANG_TAG in tools/android_build.py,
# 16.6.0), cloned into build/ps5/third_party/glslang, or the Android build's
# clone (build/android/third_party/glslang) when it is there at that tag. The
# options are port/android/probe/glslang/CMakeLists.txt's (no optimizer, no
# HLSL, no binaries, no tests), static, with no shared library around it.
# glslang 16 builds one libglslang.a holding what were MachineIndependent,
# GenericCodeGen, OSDependent and SPIRV (libSPIRV.a is an empty archive kept
# for compatibility), and libglslang-default-resource-limits.a.
#
# The compiler is the one the fork's toolchain/prospero.cmake names,
# bin/prospero-clang, which runs the clang that bin/prospero-llvm-config
# finds: LLVM_CONFIG if it is set, else the newest of llvm-config-21 down to
# llvm-config (clang 18 on this machine). GLSLANG_LLVM_CONFIG sets it for this
# build only. libc++, libc++abi and libunwind are the SDK fork's, linked as
# PS5_Vulkan's tools/radv-link.sh links them for ACO.

glslang_tag=16.6.0
glslang_source="$repository/build/ps5/third_party/glslang"
if [[ ! -d $glslang_source ]] &&
    [[ $(git -C "$repository/build/android/third_party/glslang" describe --tags 2>/dev/null) == "$glslang_tag" ]]; then
    glslang_source="$repository/build/android/third_party/glslang"
fi
if [[ ! -d $glslang_source ]]; then
    git clone -q --depth 1 --branch "$glslang_tag" https://github.com/KhronosGroup/glslang.git "$glslang_source"
fi
[[ $(git -C "$glslang_source" describe --tags) == "$glslang_tag" ]] ||
    { echo "$glslang_source is not glslang $glslang_tag" >&2; exit 2; }
glslang_build="$repository/build/ps5/glslang"
(
    [[ -z ${GLSLANG_LLVM_CONFIG:-} ]] || export LLVM_CONFIG=$GLSLANG_LLVM_CONFIG
    if [[ ! -f $glslang_build/build.ninja ]]; then
        cmake -S "$glslang_source" -B "$glslang_build" -G Ninja \
            -DCMAKE_TOOLCHAIN_FILE="$sdk/toolchain/prospero.cmake" -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY -DCMAKE_VERBOSE_MAKEFILE=OFF \
            -DENABLE_OPT=OFF -DENABLE_HLSL=OFF -DENABLE_GLSLANG_BINARIES=OFF -DENABLE_GLSLANG_JS=OFF \
            -DGLSLANG_TESTS=OFF -DBUILD_EXTERNAL=OFF -DBUILD_SHARED_LIBS=OFF > "$glslang_build.configure.log"
    fi
    ninja -C "$glslang_build" glslang glslang-default-resource-limits > "$glslang_build.build.log"
) || { echo "glslang did not build: see $glslang_build.configure.log and $glslang_build.build.log" >&2; exit 1; }

extra_cflags+=(-I "$glslang_source")
# The link is the one the host will have: PS5_Vulkan's radv_link_recipe
# (tools/radv-link.sh, sourced where it is), its linker script, its libc++,
# its bindings of libc's names to the platform layer's (libc++ wants
# newlocale, catopen and the rest, which no system module exports) and its
# allocator wraps, so glslang's allocations go to the platform's heap in
# direct memory as RADV's do; only RADV's archive is left out. The recipe
# checks the archive is there, so it is named, and then dropped.
# shellcheck disable=SC1091
source "$vulkan/tools/radv-link.sh"
glslang_radv_archive="$vulkan/.deps/native/radv-release/lib/libvulkan_radeon.ps5.a"
radv_link_recipe "$vulkan" "$sdk" "$glslang_radv_archive" || exit 2
linker_script=("${radv_linker_script[@]}")
extra_link+=(--whole-archive "$glslang_build/glslang/libglslang.a"
    "$glslang_build/glslang/libglslang-default-resource-limits.a" --no-whole-archive)
for input in "${radv_link_inputs[@]}"; do
    [[ $input == "$glslang_radv_archive" || $input == --whole-archive || $input == --no-whole-archive ]] ||
        extra_link+=("$input")
done
extra_link+=("${radv_link_flags[@]}")
