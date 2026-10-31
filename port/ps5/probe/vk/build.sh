#!/usr/bin/env bash
# The Vulkan probe: PS5_Vulkan's own RADV smoke test (its radv/radv_smoke.c,
# title PPSA99014, "PS5 Vulkan RADV"), built unchanged against the release
# build of its Mesa fork, the driver the game's renderer would link. It
# fills, copies, computes and draws on the GPU and checks each result on
# the CPU (tessellation, geometry shaders, scratch, primitive IDs, mesh and
# task shaders where reported), then presents 60 frames to the TV through
# VK_KHR_display on VideoOut, each cleared to its own gray. Whether that runs
# on this console's firmware is the question: PS5_Vulkan was proved on
# firmware 12.02, 12.09 and 12.70.
#
#   port/ps5/probe/vk/build.sh
#
# builds build/ps5/probe/PPSA99014. Copy it to /data/homebrew/PPSA99014 over
# FTP and start "PS5 Vulkan RADV" from the home screen; the screen should
# turn through grays for a second at the end. Results: /app0/radv-smoke.txt
# (over FTP, /data/homebrew/PPSA99014/radv-smoke.txt) and the kernel log.
#
# Needs PS5_Vulkan (PS5_VULKAN, ~/projects/ps5-deps/PS5_Vulkan) set up by its
# tools/setup-native-dependencies.sh, its Mesa fork beside it (PS5_Mesa), and
# the tools its RADV build needs: meson 1.4 or newer, LLVM 18 with Clang's
# headers, libclc and the SPIR-V translator, and glslangValidator.
# HOSTDEPS_ENV names a script that puts them on the path (where they were
# unpacked without root: ~/projects/ps5-deps/hostdeps/env.sh).
#
# The driver and the title are compiled with clang 22 (LLVM_CONFIG, which the
# SDK's wrappers follow): with clang 18 the PS5 target leaves Mesa's weak
# entry points at default visibility, and the title's link then refuses them
# as imports no system library exports (radv_EnumeratePhysicalDevices first).

set -euo pipefail

repository=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../../.." && pwd)
vulkan=${PS5_VULKAN:-$HOME/projects/ps5-deps/PS5_Vulkan}
hostdeps=${HOSTDEPS_ENV:-$HOME/projects/ps5-deps/hostdeps/env.sh}
# shellcheck disable=SC1090
[[ ! -f $hostdeps ]] || source "$hostdeps"

export LLVM_CONFIG=${LLVM_CONFIG:-/usr/bin/llvm-config-22}
bash "$vulkan/tools/build-radv.sh" release
RADV_ARCHIVE="$vulkan/.deps/native/radv-release/lib/libvulkan_radeon.ps5.a" \
    bash "$vulkan/tools/build-radv-title.sh"
rm -rf -- "$repository/build/ps5/probe/PPSA99014"
mkdir -p "$repository/build/ps5/probe"
cp -a "$vulkan/dist/PPSA99014" "$repository/build/ps5/probe/PPSA99014"
echo "title: $repository/build/ps5/probe/PPSA99014"
