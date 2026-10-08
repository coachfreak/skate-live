#!/usr/bin/env bash
# Regenerates dylib/title_logo.h: the press-start logo's letters redrawn as
# "skate.live". Reads the stock textures from the installed game's fedynamic.big.
#   tools/title_logo/make.sh [path/to/game/data/big/fedynamic.big]
set -euo pipefail
cd "$(dirname "$0")"
BIG="${1:-/Applications/game/data/big/fedynamic.big}"
WORK="$(mktemp -d)"
cp big.py rx2.py dxt.py neon.py encode.py "$WORK/"
# Text in the rows every letter covers with its tile and bottom strip (movie y -35..38).
swift mask.swift "skate.live" "$WORK/mask.png" rounded 369 85 3 6 366 79 >/dev/null
( cd "$WORK"
  python3 -c "import sys; sys.path.insert(0,'.'); from big import read; open('pressstart.rx2','wb').write(read('$BIG','data/fe/source/screens/bootflow/pressstart.rx2'))"
  python3 neon.py mask.png.raw 1476 369 85 )
sed "s#os.path.dirname(os.path.abspath(__file__))#'$(pwd)'#" encode.py > "$WORK/encode.py"
( cd "$WORK" && python3 encode.py && cp encoded_preview.png "${PREVIEW:-/dev/null}" 2>/dev/null || true )
rm -rf "$WORK"
