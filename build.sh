#!/bin/sh
# Regenerate the C code from your own ds.exe files, then build.
#   ./build.sh linux   -> build/demonstar (headless Linux test build)
#   ./build.sh vita    -> vbuild/DemonStar.vpk (needs VITASDK and GeneralUser-GS.sf2)
set -e
cd "$(dirname "$0")"
python3 tools/recomp.py games/ds1/ds.exe ds1 gen
python3 tools/recomp.py games/sm1/ds.exe sm1 gen
python3 tools/gen_stubs.py gen/ds1_shims.txt gen/sm1_shims.txt
case "${1:-vita}" in
  linux) rm -f build/*.a; make -f Makefile.linux CFLAGS="-O2 -g -fno-strict-aliasing -fwrapv -Irt -Igen -w" build/demonstar ;;
  vita)  rm -f vbuild/*.a vbuild/eboot.bin vbuild/*.vpk; make -f Makefile.vita SF2="${SF2:-GeneralUser-GS.sf2}" ;;
esac
