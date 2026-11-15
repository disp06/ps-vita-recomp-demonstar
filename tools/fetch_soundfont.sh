#!/bin/sh
# Downloads the GeneralUser GS soundfont (free, see its LICENSE) used for MIDI music.
set -e
cd "$(dirname "$0")/.."
[ -f GeneralUser-GS.sf2 ] && exit 0
git clone --depth 1 https://github.com/mrbumpy409/GeneralUser-GS /tmp/GeneralUser-GS
cp /tmp/GeneralUser-GS/GeneralUser-GS.sf2 .
