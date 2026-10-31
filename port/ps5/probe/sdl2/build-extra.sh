# Sourced by port/ps5/probe/title/build.sh (see there): pacbrew's SDL2 for the
# SDL2 probe. PS5_Vulkan's tools/setup-pacbrew-dependencies.sh (called where
# it is) fetches pacbrew v0.40.2 once into PS5_Vulkan's .deps/pacbrew/ and
# checks it against its SHA-256,
# a85f65de418a8e6a898c6c3e3c870d50fff7618a200e4dd59ea9692af6ecec4d, and
# --resolve sdl2 answers pkg-config --static's flags for it: the include
# folders, then libSDL2.a and its transitive archives and system libraries in
# link order, as PS5_Vulkan's tools/build.sh passes PACBREW_PACKAGES to its
# compile and link. That SDL2 is 2.30.12, ps5-payload-dev's port.

sdl2_resolution=$(bash "$vulkan/tools/setup-pacbrew-dependencies.sh" --resolve sdl2)
mapfile -d '' -t sdl2_cflags < <(python3 -c \
    'import json, sys; [print(v, end="\0") for v in json.loads(sys.argv[1])["cflags"]]' "$sdl2_resolution")
mapfile -d '' -t sdl2_libs < <(python3 -c \
    'import json, sys; [print(v, end="\0") for v in json.loads(sys.argv[1])["libs"]]' "$sdl2_resolution")
# (-isystem: SDL's headers are not this probe's to keep free of warnings)
for flag in "${sdl2_cflags[@]}"; do
    [[ $flag == -I* ]] && extra_cflags+=(-isystem "${flag#-I}") || extra_cflags+=("$flag")
done
extra_link+=(-L "$sdk/target/lib" "${sdl2_libs[@]}")

# Linked with pacbrew's flags alone, the title does not link: libiconv.a,
# which SDL2 brings, was built against the payload SDK's libc and wants
# nl_langinfo and ___mb_cur_max, which no system module exports to a title
# (SDL2_PLAIN_LINK=1 builds it that way, to see the errors). So it is linked
# as the host will be linked anyway: with PS5_Vulkan's radv_link_recipe
# (tools/radv-link.sh, sourced where it is), whose bindings of libc's names
# to the platform layer cover nl_langinfo, without RADV's archive (the recipe
# checks it is there, so it is named, then dropped); and ___mb_cur_max is
# answered by libc_extra.c.
if [[ -n ${SDL2_PLAIN_LINK:-} ]]; then
    extra_cflags+=(-DSDL2_PLAIN_LINK)
else
    # shellcheck disable=SC1091
    source "$vulkan/tools/radv-link.sh"
    sdl2_radv_archive="$vulkan/.deps/native/radv-release/lib/libvulkan_radeon.ps5.a"
    radv_link_recipe "$vulkan" "$sdk" "$sdl2_radv_archive" || exit 2
    linker_script=("${radv_linker_script[@]}")
    for input in "${radv_link_inputs[@]}"; do
        [[ $input == "$sdl2_radv_archive" || $input == --whole-archive || $input == --no-whole-archive ]] ||
            extra_link+=("$input")
    done
    extra_link+=("${radv_link_flags[@]}")
fi
