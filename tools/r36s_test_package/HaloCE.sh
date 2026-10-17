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
chmod +x ./halo 2>/dev/null
export LD_LIBRARY_PATH="$PWD/libs${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-kmsdrm}"
export SDL_AUDIODRIVER="${SDL_AUDIODRIVER:-alsa}"
export HALO_DATA_ROOT="$PWD"
echo "SDL_VIDEODRIVER=$SDL_VIDEODRIVER SDL_AUDIODRIVER=$SDL_AUDIODRIVER"
echo "binary: $(ls -l ./halo)"
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
