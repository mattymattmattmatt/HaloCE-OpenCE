# Sourced by port/ps5/probe/title/build.sh: a stub library for libSceAudioIn,
# which the SDK has none of, so the probe can import its functions at link
# time (the route after sceKernelDlsym, which run 1 found empty). A stub is
# what ps5-native-tool reads of the SDK's (PS5_Vulkan's elf_object.cpp,
# read_stub): a shared object whose SONAME names the module and whose global
# symbols are the functions it exports; their code is never run. The names
# are shadPS4's (src/core/libraries/audio/audioin.cpp's export table).

audio_in_stub="$work/libSceAudioIn.so"
{
    echo ".text"
    for name in sceAudioInInit sceAudioInOpen sceAudioInInput sceAudioInClose sceAudioInGetSilentState; do
        printf '.globl %s\n.type %s,@function\n%s:\n    ret\n' "$name" "$name" "$name"
    done
} > "$work/libSceAudioIn.s"
clang --target=x86_64-unknown-linux-gnu -shared -nostdlib -fuse-ld=lld -Wl,-soname,libSceAudioIn.sprx \
    "$work/libSceAudioIn.s" -o "$audio_in_stub"
extra_link+=("$audio_in_stub")
extra_stubs+=("$audio_in_stub")
