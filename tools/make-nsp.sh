#!/bin/sh
# Builds the forwarder NSP: nx-hbloader patched to open sdmc:/switch/sh2-nx/sh2-nx.nro (39-bit
# address space), packed by hacBrewPack with the console's keys. The game itself stays an NRO on the
# SD card, so updating it never needs a new NSP.  Usage: tools/make-nsp.sh path/to/prod.keys
set -e
KEYS=${1:?usage: tools/make-nsp.sh path/to/prod.keys}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
N=$ROOT/build/nsp
cd "$N"
rm -rf exefs control hacbrewpack_nsp
mkdir -p exefs control
cp nx-hbloader/hbl.nso exefs/main
cp nx-hbloader/hbl.npdm exefs/main.npdm
cp "$ROOT/build/switch/sh2.nacp" control/control.nacp
cp icon.jpg control/icon_AmericanEnglish.dat
./hacBrewPack/hacbrewpack -k "$KEYS" --noromfs --nologo --titleid 05005348324E0000 \
    --titlename "Silent Hill 2" --titlepublisher "sh2-nx" > hacbrewpack.log 2>&1
cp hacbrewpack_nsp/05005348324e0000.nsp "$ROOT/build/switch/Silent Hill 2 [05005348324E0000].nsp"
echo "$ROOT/build/switch/Silent Hill 2 [05005348324E0000].nsp"
