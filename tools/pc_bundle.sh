#!/bin/zsh
# pc_bundle.sh MAC_APP PC_DIR OUT [--pc-sound]
#
# Builds OUT/"Assassin's Creed 2.app", a copy-free AC2 bundle (symlinks only)
# whose archives and videos are the Windows release's, for the port's
# `build/ac2 OUT/"Assassin's Creed 2.app" ...` (docs/PC_DATA.md):
#   MAC_APP  the Mac "Assassin's Creed 2.app" (its executable and everything
#            the PC release lacks: Info.plist, media/, DefaultBindings.map)
#   PC_DIR   the Windows release's folder (DataPC*.forge, SoundData/pc, Videos)
# The engine opens Data<platform>_<world>.forge, so DataPC_X.forge is linked
# as DataMAC_X.forge. Sound packs are the Mac's unless --pc-sound: the PC packs
# hold Wwise banks of an older version (44) that the Mac engine's Wwise (48)
# does not load, so with them the game is silent.
set -e
if (( $# < 3 )); then
    echo "usage: $0 MAC_APP PC_DIR OUT [--pc-sound]" >&2
    exit 2
fi
MAC="${1:A}/Contents"
PC="${2:A}"
OUT="${3:A}/Assassin's Creed 2.app/Contents"
[[ -f "$MAC/MacOS/scimitar_sf" ]] || { echo "no $MAC/MacOS/scimitar_sf" >&2; exit 1; }
[[ -f "$PC/DataPC.forge" ]] || { echo "no $PC/DataPC.forge" >&2; exit 1; }

rm -rf "${3:A}/Assassin's Creed 2.app"
mkdir -p "$OUT/Resources/sounddata"
for f in MacOS Info.plist PkgInfo version.plist; do ln -s "$MAC/$f" "$OUT/$f"; done
for f in "$MAC/Resources"/*; do
    case ${f:t} in
    DataMAC*|sounddata|videos) ;;
    *) ln -s "$f" "$OUT/Resources/${f:t}" ;;
    esac
done
for f in "$PC"/DataPC*.forge; do ln -s "$f" "$OUT/Resources/${${f:t}/DataPC/DataMAC}"; done
if [[ "$4" == --pc-sound ]]; then
    ln -s "$PC/SoundData/pc" "$OUT/Resources/sounddata/Mac"
else
    ln -s "$MAC/Resources/sounddata/Mac" "$OUT/Resources/sounddata/Mac"
fi
ln -s "$PC/Videos" "$OUT/Resources/videos"
echo "${3:A}/Assassin's Creed 2.app"
