# Sourced by port/ps5/probe/title/build.sh (see there): RADV linked into the
# host probe the way PS5_Vulkan's tools/build-radv-title.sh links it into its
# smoke test, by sourcing its tools/radv-link.sh where it is and taking
# radv_link_recipe's linker script, inputs and flags: the RADV archive whole,
# the SDK fork's libc++ for ACO, the platform layer's libc and its allocator
# and thread wraps. The archive is PS5_Vulkan's release build
# (port/ps5/probe/vk/build.sh makes it: its tools/build-radv.sh release, with
# clang 22), RADV_ARCHIVE to name another. The AGC stub libraries that name
# the driver's imports are compiled from PS5_Vulkan's own stub sources into
# the build's work folder, as build-radv-title.sh compiles them, and given to
# the link and to ps5-native-tool. Vulkan's headers are PS5_Mesa's, beside
# PS5_Vulkan (PS5_MESA_FORK to name another checkout).

host_archive=${RADV_ARCHIVE:-$vulkan/.deps/native/radv-release/lib/libvulkan_radeon.ps5.a}
host_mesa=${PS5_MESA_FORK:-$vulkan/../PS5_Mesa}
[[ -f $host_archive ]] || { echo "no RADV archive at $host_archive: run port/ps5/probe/vk/build.sh" >&2; exit 2; }
[[ -f $host_mesa/include/vulkan/vulkan.h ]] || { echo "no Vulkan headers in $host_mesa/include" >&2; exit 2; }

mkdir -p "$work/stubs"
host_stub() {
    local library=$1 source=$2
    cc -std=c11 -O2 -fPIC -c "$vulkan/$source" -o "$work/obj/${library}_stub.o"
    "$sdk/bin/prospero-lld" --shared -soname "${library}.prx" -o "$work/stubs/${library}.so" \
        "$work/obj/${library}_stub.o"
}
host_stub libSceAgc vendor/ps5/sdk/stubs/agc_canary_link_stub.c
host_stub libSceAgcDriver vendor/ps5/sdk/stubs/agc_driver_canary_link_stub.c

# shellcheck disable=SC1091
source "$vulkan/tools/radv-link.sh"
radv_link_recipe "$vulkan" "$sdk" "$host_archive" || exit 2
linker_script=("${radv_linker_script[@]}")
extra_cflags+=(-isystem "$host_mesa/include")
extra_link+=("${radv_link_flags[@]}" "$work/stubs/libSceAgc.so" "$work/stubs/libSceAgcDriver.so"
    "${radv_link_inputs[@]}")
extra_stubs+=("$work/stubs/libSceAgc.so" "$work/stubs/libSceAgcDriver.so")
