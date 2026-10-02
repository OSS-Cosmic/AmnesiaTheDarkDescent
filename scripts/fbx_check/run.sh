#!/usr/bin/env bash
# Check hpl::fbx::Import (HPL2/core/sources/impl/FbxImport.cpp) against the
# .msh/.anm caches that ship in the retail AMFP install. Those caches were made
# by AMFP's KFbx-based loader, so they are the reference for what the FBX
# loader must produce. Needs no GPU: FbxImport is engine-free.
#
#   scripts/fbx_check/run.sh            # AMFP_DIR defaults to the Steam install
#
# Prints one DIFF line per mismatching file, a NOCACHE line per .fbx with no
# retail cache, and a summary dict at the end.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
OUT="${TMPDIR:-/tmp}/fbx_check"
mkdir -p "$OUT"
gcc -O2 -c "$ROOT/HPL2/extern/ufbx/ufbx.c" -o "$OUT/ufbx.o"
g++ -std=c++17 -O2 -I"$ROOT/HPL2/core/include" -I"$ROOT/HPL2/extern/ufbx" \
    "$HERE/fbx_dump.cpp" "$ROOT/HPL2/core/sources/impl/FbxImport.cpp" "$OUT/ufbx.o" -o "$OUT/fbx_dump" -lm
FBX_DUMP="$OUT/fbx_dump" python3 "$HERE/compare_retail.py"
