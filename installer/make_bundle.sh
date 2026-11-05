#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# The installer's prebuilt half, from public source: the runtime as one archive, the Switch libraries,
# and the headers generated C includes. Needs Linux or WSL with devkitPro (switch-dev, deko3d), a build
# of Makefile.switch for the runtime's objects, and a translated package for the header list.
#
#   make_bundle.sh <repository> <package folder> <folder of runtime and shader objects> <output folder>
set -eu
REPO=$1 PKG=$2 OBJECTS=$3 OUT=$4
DKP=${DEVKITPRO:-/opt/devkitpro}
CROSS=$DKP/devkitA64/bin/aarch64-none-elf-
GCCLIB=$DKP/devkitA64/lib/gcc/aarch64-none-elf/$("${CROSS}gcc" -dumpversion)
mkdir -p "$OUT/lib" "$OUT/include/sys" "$OUT/include/src"

rm -f "$OUT/lib/libruntime.a"
"${CROSS}ar" rcs "$OUT/lib/libruntime.a" "$OBJECTS"/*.o
for file in crti.o crtbegin.o crtend.o crtn.o libgcc.a; do cp "$GCCLIB/pic/$file" "$OUT/lib/"; done
for file in libc.a libm.a libsysbase.a; do cp "$DKP/devkitA64/aarch64-none-elf/lib/pic/$file" "$OUT/lib/"; done
cp "$DKP/libnx/lib/libnx.a" "$DKP/libnx/lib/libdeko3d.a" "$DKP/libnx/switch.ld" "$OUT/lib/"
"$DKP/tools/bin/nacptool" --create "Ben 10: Protector of Earth" LaLemniscata 1.0.0 "$OUT/lib/ben10-psp.nacp" >/dev/null

cd "$REPO"
FLAGS="-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE -D__SWITCH__ -w -DSR_FLIGHT_RECORDER_LINKED
       -include src/rt/switch/nk_switch_compat.h -I$PKG -Isrc/rt/switch/sdl_shim -Isrc/rt -I$DKP/libnx/include"
for source in "$PKG"/*.c; do
  # shellcheck disable=SC2086
  "${CROSS}gcc" $FLAGS -M "$source"
done | sed -e 's/\\$//' | tr ' ' '\n' | grep -vE ':$|^$' | sort -u | while read -r path; do
  case "$path" in
    "$PKG"*|"$GCCLIB"*) ;;
    "$DKP/devkitA64/aarch64-none-elf/include/"*)
      target=$OUT/include/sys/${path#"$DKP"/devkitA64/aarch64-none-elf/include/}
      mkdir -p "$(dirname "$target")"; cp "$path" "$target" ;;
    /*) echo "unexpected header: $path" >&2; exit 1 ;;
    *)
      target=$OUT/include/src/$(realpath --relative-to=. "$path")
      mkdir -p "$(dirname "$target")"; cp "$path" "$target" ;;
  esac
done
