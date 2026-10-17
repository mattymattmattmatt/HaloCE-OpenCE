HALO CE ON THE R36S - FEASIBILITY TEST PACKAGE
===============================================
This is a throwaway test build (branch "r36s-test"), NOT a finished port.
It already boots to the main menu on a PC emulating the R36S's 32-bit ARM
CPU. This package is to find out what it does on your real handheld.

WHAT'S IN HERE
--------------
halo-r36s-test/
  halo              the game (32-bit ARM, OpenGL ES build, gore mod included)
  libs/             its 32-bit libraries, bundled so the firmware's own
                    libraries (old on ArkOS) don't matter, including its
                    own loader and libc - the COMPLETE matched set,
                    including librt/libpthread/libdl: missing one out
                    makes the loader take the firmware's older copy and
                    the game dies at launch on GLIBC_PRIVATE symbols
                    (that was update 1's fix). The Mali graphics libraries
                    (libEGL/libGLESv2) deliberately come from the system.
  maps/             all 24 stock maps (1.8 GB) - everything the game needs
  custom_maps/      empty; Custom Edition maps would go here (untested)
  config.toml       settings tuned for the R36S: 640x480 original scaling,
                    interpolation off, gore pieces capped at 64
  HaloCE.sh         THE LAUNCHER (same as run.sh). It first checks the
                    firmware can run 32-bit ARM, then starts the game.
                    EVERYTHING is written to halo-r36s.log in this folder.
  README-MATTY.txt  this file
HaloCE.sh (sitting OUTSIDE the folder, in the zip's top level)
                    optional little launcher for the Ports menu - see below.

HOW TO GET IT ON THE HANDHELD
-----------------------------
1. Unzip on your PC. You get a folder "halo-r36s-test" (about 1.9 GB)
   plus that one loose HaloCE.sh file.
2. Copy the whole halo-r36s-test folder onto the SD card. Best place:
   the "ports" folder (ArkOS: /roms/ports/ on the EASYROMS partition).
   Home folder via SSH is fine too - location doesn't matter, the
   launcher finds its own files.
3. Optional, for launching from the Ports menu in EmulationStation:
   also copy the loose HaloCE.sh into that same ports folder, next to
   (not inside) the halo-r36s-test folder. Some firmwares only list
   .sh files that are executable - see the permission fix below.

PERMISSIONS - READ THIS, IT'S THE MOST LIKELY SNAG
--------------------------------------------------
SD cards (FAT/exFAT) throw away Linux "execute" permissions, so the
game and scripts may arrive unable to run ("Permission denied").
The launcher tries to fix the game itself, but something has to run
the launcher first. If launching does nothing, SSH in (or open the
terminal app) and run exactly:

  cd /roms/ports/halo-r36s-test
  chmod +x halo HaloCE.sh run.sh libs/ld-linux-armhf.so.3
  bash HaloCE.sh

(adjust the path if you put the folder somewhere else). Running it with
"bash HaloCE.sh" also works even before any chmod.

HOW TO LAUNCH
-------------
- Ports menu: pick HaloCE (if you copied the loose .sh in), or
- SSH/terminal: cd into the folder and:  bash HaloCE.sh
First run also creates save/profile files in the usual ArkOS home
location. Give the menu a while on first load; if it works you'll see
the Halo main menu at 640x480. Try Campaign -> New Game -> Pillar of
Autumn, wander, shoot things. The gore mod is ON (explosions/vehicles
can gib; point-blank shotgun kills gib; only 64 pieces at once here).

WHAT SUCCESS / FAIL LOOKS LIKE IS WRITTEN IN THE LOG
----------------------------------------------------
halo-r36s.log (in the game folder) starts with device info and:
  "ARMHF CHECK: PASS"  = firmware runs 32-bit ARM (expected on ArkOS)
  "ARMHF CHECK: FAIL"  = this firmware/kernel can't run 32-bit at all.
                         That's a real answer too - send the log.
At the end: "=== HALO R36S TEST: SUCCESS ..." or "... FAIL ...".
The launcher also appends the game's own debug.txt.

WHAT TO SEND BACK
-----------------
1. halo-r36s.log from the game folder (the whole thing).
2. What you saw: black screen? menu? got in-game? roughly how smooth
   (playable / choppy / slideshow), any weird graphics, did the
   controls work (sticks, triggers, Start pauses, Back cycles the
   HUD/reticle display)?
3. How long you played and where it got to.

KNOWN LIMITS OF THIS TEST
-------------------------
- Test build: 640x480, interpolation off, gore capped at 64 pieces,
  small (Xbox-sized) memory profile already compiled in for 1 GB RAM.
- Under PC emulation it warned it could not reserve the Custom Edition
  tag cache, so CE maps may not run; stock maps are unaffected and are
  all that's included/tested here.
- Bundled libraries are from a modern Debian; the launcher runs the
  game through the bundled loader/libc on purpose. The one thing that
  MUST come from the firmware is 32-bit kernel support plus the Mali
  libEGL/libGLESv2 - that's exactly what this test measures.
- No Bink video (every native build of this port skips the .bik intro).
- If the Ports menu returns instantly, it's permissions (above) or the
  ARMHF check failed - either way halo-r36s.log says which.
