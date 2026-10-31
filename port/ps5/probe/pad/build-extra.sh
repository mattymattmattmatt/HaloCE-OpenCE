# Sourced by port/ps5/probe/title/build.sh: pacbrew's SDL2, linked as the SDL2
# probe links it (its build-extra.sh, and its libc_extra.c for the
# ___mb_cur_max libiconv wants), since this probe holds the pad through SDL2
# as the title does.

# shellcheck disable=SC1091
source "$repository/port/ps5/probe/sdl2/build-extra.sh"
cc -std=c11 -O2 -Wall -Wextra -Werror "${extra_cflags[@]}" \
    -c "$repository/port/ps5/probe/sdl2/libc_extra.c" -o "$work/obj/libc_extra.c.o"
extra_objects+=("$work/obj/libc_extra.c.o")
