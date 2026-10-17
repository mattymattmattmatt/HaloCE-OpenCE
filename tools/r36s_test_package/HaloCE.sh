#!/bin/bash
# Halo CE (OpenCE) - R36S feasibility test launcher (branch r36s-test, throwaway)
# Everything, including the pre-flight armhf checks, goes to halo-r36s.log
# in this folder. Send that file back whatever happens.
cd "$(dirname "$0")" || exit 1
LOG="$PWD/halo-r36s.log"
exec >>"$LOG" 2>&1
echo "================================================================"
echo "=== HALO R36S TEST: START $(date 2>/dev/null) ==="
echo "--- device / firmware ---"
uname -a 2>/dev/null
echo "arch: $(uname -m 2>/dev/null)"
cat /etc/os-release 2>/dev/null | head -3
free -m 2>/dev/null | head -2
echo "--- armhf self-checks ---"
ls -l /lib/ld-linux-armhf.so.3 ./libs/ld-linux-armhf.so.3 2>&1
echo "foreign architectures: $(dpkg --print-foreign-architectures 2>/dev/null || echo '(dpkg n/a)')"
if ./libs/ld-linux-armhf.so.3 --help >/dev/null 2>&1; then
  echo "ARMHF CHECK: PASS (bundled 32-bit loader runs natively - kernel has 32-bit support)"
else
  echo "ARMHF CHECK: FAIL (32-bit loader cannot run - 'Exec format error' class problem;"
  echo "  this firmware/kernel likely has no CONFIG_COMPAT 32-bit support)"
  echo "=== HALO R36S TEST: FAIL (armhf not supported by this firmware) ==="
  exit 1
fi
echo "--- launch ---"
# Graphics stack must be the DEVICE's matched set: our bundled Debian
# libgbm/libdrm (a Mesa frontend + newer libdrm) mixed with the firmware's
# Mali GBM blob killed the first swap in round 4 - SDL talked to one GBM
# while Mali's EGL owned the other. Remove the bundled pair (this script
# can, an unzip-overwrite cannot) so the loader takes the system armhf
# libgbm/libdrm the Mali driver was built against.
if [ -e ./libs/libgbm.so.1 ] || [ -e ./libs/libdrm.so.2 ]; then
  rm -f ./libs/libgbm.so.1 ./libs/libdrm.so.2
  echo "removed bundled libs/libgbm.so.1 and libs/libdrm.so.2 (device graphics stack takes over)"
fi
echo "system graphics libraries on this device:"
ls -l /usr/lib/arm-linux-gnueabihf/libgbm.so.1 /usr/lib/arm-linux-gnueabihf/libdrm.so.2       /usr/local/lib/arm-linux-gnueabihf/libgbm.so.1 /usr/local/lib/arm-linux-gnueabihf/libdrm.so.2 2>&1
chmod +x ./halo 2>/dev/null
export LD_LIBRARY_PATH="$PWD/libs${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-kmsdrm}"
export SDL_AUDIODRIVER="${SDL_AUDIODRIVER:-alsa}"
export HALO_DATA_ROOT="$PWD"
echo "SDL_VIDEODRIVER=$SDL_VIDEODRIVER SDL_AUDIODRIVER=$SDL_AUDIODRIVER"
echo "binary: $(ls -l ./halo)"
echo "--- libraries the loader will use (all glibc must come from ./libs) ---"
./libs/ld-linux-armhf.so.3 --library-path "$PWD/libs" --list ./halo 2>&1
./libs/ld-linux-armhf.so.3 --library-path "$PWD/libs" ./halo
rc=$?
echo "game exited with code $rc"
if [ $rc -eq 0 ]; then
  echo "=== HALO R36S TEST: SUCCESS (game ran and exited cleanly) ==="
else
  echo "=== HALO R36S TEST: FAIL (game exit code $rc - see log above) ==="
fi
echo "--- engine debug.txt (tail) ---"
tail -40 ./debug.txt 2>/dev/null
exit $rc
