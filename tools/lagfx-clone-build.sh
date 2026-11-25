#!/bin/bash
# LAGFX CLONE BOOT: builds the clone-boot runner (host/ui_runner_vm_clone.c) with the gfx MMIO window handed to libapplegfx-vulkan (static library,
# built with -DLAGFX_NO_PROCESS by .github/workflows/android-g7lagfx.yml). A separate binary and a separate build: tools/ui-runner-vm-clone-build.sh and
# the general / g7exec builds are untouched, and nothing here runs on a push to the other workflows.
# What differs from the normal clone runner: devices/pvg_lite.c and devices/pvg_lagfx.c are compiled with -DHV_WITH_LAGFX -DHV_PVG_LAGFX_DEFAULT_ON (the lagfx hook is on in
# THIS binary; the HV_PVG_LAGFX variable cannot be used because the runner removes every HV_* variable at start), and the link adds the library and the Android Vulkan
# loader. The static safety check is the lagfx copy (tools/check-runner-vm-clone-lagfx-safe.sh): the same check with ONE more allowed shared library, libvulkan.so.
# Link + static check only; nothing is run. Usage: CC=<compiler> [NM=..] [READELF=..] LAGFX_DIR=<library source> LAGFX_A=<libapplegfx-vulkan.a> VULKAN_LIBDIR=<dir with libvulkan.so> \
#          tools/lagfx-clone-build.sh <engine dir (after engine-build.sh + devices-build.sh)> <output dir>
set -u
cd "$(dirname "$0")/.."
E=${1:?engine build dir}; OUT=${2:?output dir}; CC=${CC:?CC}
L=${LAGFX_DIR:?LAGFX_DIR}; A=${LAGFX_A:?LAGFX_A}; VL=${VULKAN_LIBDIR:?VULKAN_LIBDIR}
[ -f "$L/include/libapplegfx-vulkan.h" ] || { echo "FAILED  no library header in $L/include"; exit 1; }
[ -f "$A" ] || { echo "FAILED  no static library $A"; exit 1; }
[ -f "$VL/libvulkan.so" ] || { echo "FAILED  no libvulkan.so in $VL"; exit 1; }
rm -rf "$OUT"; mkdir -p "$OUT/eng"
cp "$E"/*.o "$OUT/eng/" || { echo "FAILED  no engine objects in $E"; exit 1; }
CFLAGS="-std=gnu11 -D_GNU_SOURCE -Wall -Wextra -O2 -g -Icore -Ihost -Idevices -Wno-unused-parameter -DHV_WITH_LAGFX -DHV_PVG_LAGFX_DEFAULT_ON -I$L/include"
for f in pvg_lite pvg_lagfx; do
  $CC $CFLAGS -c devices/$f.c -o "$OUT/eng/dev_$f.o" 2> "$OUT/$f.log" || { echo "FAILED  devices/$f.c"; head -30 "$OUT/$f.log"; exit 1; }
  echo "ok      devices/$f.c (lagfx)"
done
# the library must carry no process-start call: that is what -DLAGFX_NO_PROCESS removed
bad=$(${NM:-nm} "$A" 2>/dev/null | grep -E ' U (system|popen|fork|vfork|execv|execve|execvp|execl|execlp|socket|connect|dlopen)$' || true)
[ -z "$bad" ] || { echo "FAILED  the static library still imports a process-start or network symbol:"; echo "$bad"; exit 1; }
echo "ok      the static library imports no system/popen/fork/exec/socket/connect/dlopen"
CC="$CC" EXTRA_LIBS="${EXTRA_LIBS:--llog} $A -L$VL -lvulkan" PF_THREADLIB="${PF_THREADLIB-}" CLONE_SAFE_CHECK=tools/check-runner-vm-clone-lagfx-safe.sh \
  tools/ui-runner-vm-clone-build.sh "$OUT/eng" "$OUT" || exit 1
strings "$OUT/ui_runner_vm_clone" | grep -q "libapplegfx-vulkan" && echo "ok      the binary names libapplegfx-vulkan" || { echo "FAILED  the binary does not mention libapplegfx-vulkan (hook not linked in?)"; exit 1; }
