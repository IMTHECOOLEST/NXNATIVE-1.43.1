#!/bin/bash
# Compile-check the iOS controller, touch and front-end sources against the iPhoneOS SDK, and run the controller unit tests. Needs a Mac with Xcode; needs no game files and no keys, and takes a few minutes.
# Nothing is linked or packed: this only answers "does the Objective-C and C code compile" before a full IPA build is spent on it. The CI job .github/workflows/controls-check.yml runs it.
# Usage: dm/ios/tools/check_controls.sh        environment: GAME=<base game title ID> (default 0100000000010000: only its profile header is needed), OUT=<report dir> (default ./check-out),
#   SDL3_INC=<dir with SDL3/SDL.h>: also compile Sources/DmSDL3Pad.c with -DDM_HAVE_SDL3=1 (optional, the SDL3 backend is opt-in), CLANG=<compiler command> (default: xcrun --sdk iphoneos clang)
# Writes $OUT/summary.md, $OUT/errors.txt (every compiler error and failed test line) and $OUT/*.log; exit status is 1 when anything failed.
set -u
cd "$(dirname "$0")/../../.." || exit 1
ROOT=$PWD
GAME=${GAME:-0100000000010000}; export GAME
OUT=${OUT:-$ROOT/check-out}; mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
CLANG=${CLANG:-xcrun --sdk iphoneos clang}
: > "$OUT/errors.txt"; : > "$OUT/summary.md"
fails=0
row() { printf '| %s | %s |\n' "$1" "$2" >> "$OUT/summary.md"; }
note_fail() { fails=$((fails + 1)); grep -E 'error:|fatal error:|FAIL|failed' "$1" 2> /dev/null | head -n 40 | sed "s|^|$2: |" >> "$OUT/errors.txt"; }
printf '| check | result |\n| --- | --- |\n' >> "$OUT/summary.md"

# The include paths of the host, taken from its Makefile the way the iOS build takes them (gen_build.py), plus the generated game profile header
G=$(mktemp -d)/Gen
if python3 dm/ios/tools/gen_build.py dm/host "$G" > "$OUT/xcconfig.txt" 2> "$OUT/gen_build.log"; then
  INC=$(python3 - "$OUT/xcconfig.txt" << 'PY'
import shlex, sys
for l in open(sys.argv[1]):
    if l.startswith('HEADER_SEARCH_PATHS'):
        print(' '.join('-I' + p for p in shlex.split(l.split('=', 1)[1].replace('$(inherited)', ''))))
PY
)
  row "host include paths (gen_build.py)" "ok"
else
  note_fail "$OUT/gen_build.log" "gen_build.py"; row "host include paths (gen_build.py)" "FAILED"; INC=""
fi

# -Wall shows real mistakes in the log; only errors (a non-zero exit) fail the check. The iOS target compiles with -mcpu=apple-a18 and ARC (project.yml)
FLAGS="-fsyntax-only -arch arm64 -miphoneos-version-min=26.0 -mcpu=apple-a18 -std=gnu11 -Wall -Wno-unused-function -Idm/ios/Sources $INC"
cc_check() {   # <file> <language> [extra flags...]
  local f=$1 lang=$2; shift 2
  local log="$OUT/$(basename "$f")${SUFFIX:-}.log"
  # shellcheck disable=SC2086
  if $CLANG $FLAGS "$@" -x "$lang" "$f" > "$log" 2>&1; then row "$f${SUFFIX:-}" "ok ($(grep -c 'warning:' "$log") warnings)"
  else row "$f${SUFFIX:-}" "FAILED"; note_fail "$log" "$(basename "$f")"; fi
}
for f in DmController.m DmGamepad.m front_ios.m main.m; do cc_check "dm/ios/Sources/$f" objective-c -fobjc-arc; done
cc_check dm/ios/Sources/dm_pad_core.c c
cc_check dm/ios/Sources/DmSDL3Pad.c c
if [ -n "${SDL3_INC:-}" ]; then SUFFIX=" (DM_HAVE_SDL3)" cc_check dm/ios/Sources/DmSDL3Pad.c c -DDM_HAVE_SDL3=1 "-I$SDL3_INC"; fi

# Unit tests of the controller core and the HID side (host clang, no device)
if make -C dm/host/tests GAME="$GAME" build/t_pad_core build/t_hid_pad > "$OUT/tests-build.log" 2>&1; then
  for t in t_pad_core t_hid_pad; do
    if "dm/host/tests/build/$t" > "$OUT/$t.log" 2>&1; then row "test $t" "ok ($(tail -n 1 "$OUT/$t.log"))"; else row "test $t" "FAILED"; note_fail "$OUT/$t.log" "$t"; fi
  done
else row "unit tests (build)" "FAILED"; note_fail "$OUT/tests-build.log" "tests build"; fi

# Scripts: shell syntax, Python compiles
for s in dm/ios/tools/build_ipa.sh dm/ios/tools/build_sdl3.sh dm/ios/tools/check_controls.sh; do
  if bash -n "$s" 2> "$OUT/sh.log"; then row "$s syntax" "ok"; else row "$s syntax" "FAILED"; note_fail "$OUT/sh.log" "$s"; fi
done
if python3 -m py_compile dm/tools/aspect_scan.py 2> "$OUT/py.log"; then row "dm/tools/aspect_scan.py compiles" "ok"; else row "dm/tools/aspect_scan.py compiles" "FAILED"; note_fail "$OUT/py.log" "aspect_scan.py"; fi

cat "$OUT/summary.md"
[ -s "$OUT/errors.txt" ] && { echo; echo "errors:"; head -n 80 "$OUT/errors.txt"; }
echo; echo "$fails check(s) failed"
[ "$fails" -eq 0 ]
