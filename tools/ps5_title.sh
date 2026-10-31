#!/usr/bin/env bash
# The PS5 title's link and signing, for the rules of `ninja ps5`
# (tools/ps5_build.py; port/ps5/PS5.md, phase 1), grown from the probes'
# port/ps5/probe/title/build.sh. Its inputs and outputs are its arguments, so
# that ninja owns the dependencies:
#
#   tools/ps5_title.sh link --vulkan DIR --sdk DIR --archive RADV.a --tool PS5-NATIVE-TOOL
#                           --out EBOOT.ELF --title-elf TITLE.ELF [--stub LIB.so]... OBJECT...
#                           [-- LINK-INPUT...]
#   tools/ps5_title.sh self --tool PS5-NATIVE-TOOL --in TITLE.ELF --out EBOOT.BIN
#
# link sources PS5_Vulkan's tools/radv-link.sh where it is and links the
# title as its titles that link RADV are linked: prospero-lld with the
# recipe's linker script, the C runtime and host objects (OBJECT), the
# recipe's flags (the allocator, thread and stdio wraps, libc's names bound
# to the platform layer's), the stub libraries (--stub: the AGC ones, which
# name the driver's imports), the recipe's inputs (RADV's archive whole, the
# SDK fork's libc++), the rest (LINK-INPUT: glslang's archives, SDL2's), the
# platform layer and the SDK's system stubs as needed, into EBOOT.ELF (with
# its symbols and a map: the file to look a crash's addresses up in). Then
# ps5-native-tool link makes the title's ELF of it, TITLE.ELF (which has no
# symbol table), with the loader constants PS5_Vulkan's titles use (validated
# on firmware 6.02, 12.70 and 13.59).
#
# self signs the ELF as a fake-signed SELF (eboot.bin) and inspects the
# result. PS5_CLANG names the clang whose runtime library the recipe takes
# (tools/ps5_build.py gives the one the host is compiled with).

set -euo pipefail

usage() {
    sed -n '7,10p' "${BASH_SOURCE[0]}" >&2
    exit 2
}

link_title() {
    local vulkan='' sdk='' archive='' tool='' out='' title_elf=''
    local -a stubs=() objects=() inputs=()
    while (($#)); do
        case $1 in
            --vulkan) vulkan=$2; shift 2 ;;
            --sdk) sdk=$2; shift 2 ;;
            --archive) archive=$2; shift 2 ;;
            --tool) tool=$2; shift 2 ;;
            --out) out=$2; shift 2 ;;
            --title-elf) title_elf=$2; shift 2 ;;
            --stub) stubs+=("$2"); shift 2 ;;
            --) shift; inputs=("$@"); break ;;
            *) objects+=("$1"); shift ;;
        esac
    done
    [[ -n $vulkan && -n $sdk && -n $archive && -n $tool && -n $out && -n $title_elf ]] || usage
    local native="$vulkan/tooling/native"

    # shellcheck disable=SC1091
    source "$vulkan/tools/radv-link.sh"
    radv_link_recipe "$vulkan" "$sdk" "$archive" || exit 2

    "$sdk/bin/prospero-lld" "${radv_linker_script[@]}" --eh-frame-hdr \
        --version-script "$native/app-symbols.map" --exclude-libs=ALL \
        -e _start -o "$out" -Map "$out.map" \
        "${objects[@]}" \
        "${radv_link_flags[@]}" "${stubs[@]}" "${radv_link_inputs[@]}" \
        "${inputs[@]}" \
        "$sdk/target/lib/libps5platform.a" \
        --as-needed "$sdk"/target/lib/*.so

    local -a stub_arguments=()
    local stub
    for stub in "${stubs[@]}"; do
        stub_arguments+=(--stub "$stub")
    done
    "$tool" link --in "$out" --out "$title_elf" --stub-dir "$sdk/target/lib" \
        "${stub_arguments[@]}" --module-sdk 0x02000009 --companion-sdk 0x08050001 --file-name eboot.elf \
        > "$title_elf.log"
}

sign_title() {
    local tool='' in='' out=''
    while (($#)); do
        case $1 in
            --tool) tool=$2; shift 2 ;;
            --in) in=$2; shift 2 ;;
            --out) out=$2; shift 2 ;;
            *) usage ;;
        esac
    done
    [[ -n $tool && -n $in && -n $out ]] || usage
    mkdir -p "$(dirname -- "$out")"
    "$tool" self --sign --in "$in" --out "$out" --magic 0x1D3D154F > /dev/null
    "$tool" self --inspect --file "$out" > /dev/null
}

case ${1:-} in
    link) shift; link_title "$@" ;;
    self) shift; sign_title "$@" ;;
    *) usage ;;
esac
