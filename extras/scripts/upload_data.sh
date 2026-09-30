#!/usr/bin/env bash
#
# Mirror ux0_data/brothersinarms2/ to ux0:data/brothersinarms2/ over
# VitaShell's FTP server (START in VitaShell). Layout:
#
#   libbia2.so   engine (lib/armeabi-v7a/ of the APK)
#   BIA2/        Android cache gameloft/games/BIA2/ (obj, sprites, sounds/...)
#   res/         res/ of the APK
#   saves/       created by the game (BIA2.data, profile.sav, ...)
#   logs/        created by the loader (brothersinarms2_001.log .. _999.log)
#
# libStormGLOFT.so is skipped (third-party Android shim, never loaded), as are
# macOS ._* / .DS_Store junk and .gitkeep placeholders. saves/ and logs/
# contents are never uploaded, so a re-run can't clobber progress or logs.
#
# Usage: extras/scripts/upload_data.sh [VITA_IP] [--so-only]
# ~400 MB: the first run takes a while over Wi-Fi.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SRC="$ROOT/ux0_data/brothersinarms2"
IP="${1:-$(python3 -c "import json;print(json.load(open('$ROOT/.psvita-toolkit.json'))['vita_ip'])")}"
FTP="ftp://$IP:1337/ux0:/data/brothersinarms2"

[ -f "$SRC/libbia2.so" ] || { echo "missing $SRC/libbia2.so"; exit 1; }

echo "[*] libbia2.so"
curl -s --ftp-create-dirs -T "$SRC/libbia2.so" "$FTP/libbia2.so"

if [ "${2:-}" = "--so-only" ]; then
    exit 0
fi

cd "$SRC"
find BIA2 res -type f ! -name '._*' ! -name '.DS_Store' ! -name '.gitkeep' | sort | while read -r f; do
    echo "[*] $f"
    curl -s --ftp-create-dirs -T "$f" "$FTP/$f"
done

echo "[+] Done."
