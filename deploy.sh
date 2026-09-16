#!/usr/bin/env bash
# Stage a runnable game directory next to the built executable.
#
# Copies the installed Amnesia: The Dark Descent assets into
# build-premake/amnesia/<Config>/ and brings in the Redux resources
# (amnesia/resources: .map_delta / .ent_delta patches plus Redux-only assets)
# in one of two ways:
#
#   copy   The overlay is copied as-is. The engine applies the deltas to the
#          retail .map/.ent files at load time; the retail files stay pristine.
#   merge  The deltas are baked into the deployed .map/.ent files with
#          scripts/mapdelta.py (originals stashed as <file>.mapdelta-orig) and
#          only the non-delta overlay files are copied, so the output runs
#          without delta support.
#
# Examples:
#   ./deploy.sh --game-dir "$HOME/.steam/steam/steamapps/common/Amnesia The Dark Descent"
#   ./deploy.sh --resources merge
#   ./deploy.sh --config debug --no-game-assets     # refresh the Redux resources only
#
# Options:
#   --game-dir <path>        Installed game (fallback: $AMNESIA_GAME_DIRECTORY,
#                            then the default Steam library path)
#   --config <c>             release | debug | all (default: all that exist)
#   --resources <mode>       copy (default) | merge | none
#   --no-game-assets         Skip copying the game install
#   --output <dir>           Parent of the <Config> dirs (default: build-premake/amnesia)
#   --overlay <dir>          Redux resources to deploy (default: amnesia/resources)
#
# Files the Redux step placed are listed in <Config>/.redux_overlay_manifest.
# A listed file that the current run no longer places is removed, so deleted
# assets, or delta files left behind when switching copy -> merge, do not linger.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OVERLAY="$ROOT/amnesia/resources"
OUTPUT="$ROOT/build-premake/amnesia"
EDITOR_RESOURCES="$ROOT/HPL2/tools/resources"
MAPDELTA="$ROOT/scripts/mapdelta.py"
MANIFEST_NAME=".redux_overlay_manifest"
BACKUP_SUFFIX=".mapdelta-orig"

GAME_DIR=""
CONFIG="all"
RESOURCES="copy"
GAME_ASSETS=1
while [[ $# -gt 0 ]]; do
    case "$1" in
        --game-dir)        GAME_DIR="${2:-}"; shift 2 ;;
        --game-dir=*)      GAME_DIR="${1#--game-dir=}"; shift ;;
        --config)          CONFIG="${2:-}"; shift 2 ;;
        --config=*)        CONFIG="${1#--config=}"; shift ;;
        --resources)       RESOURCES="${2:-}"; shift 2 ;;
        --resources=*)     RESOURCES="${1#--resources=}"; shift ;;
        --no-game-assets)  GAME_ASSETS=0; shift ;;
        --output)          OUTPUT="${2:-}"; shift 2 ;;
        --output=*)        OUTPUT="${1#--output=}"; shift ;;
        --overlay)         OVERLAY="${2:-}"; shift 2 ;;
        --overlay=*)       OVERLAY="${1#--overlay=}"; shift ;;
        -h|--help)         sed -n '2,32p' "$0"; exit 0 ;;
        *) echo "error: unknown argument '$1'" >&2; exit 1 ;;
    esac
done

case "$CONFIG" in
    all)     CONFIGS=(Debug Release) ;;
    release) CONFIGS=(Release) ;;
    debug)   CONFIGS=(Debug) ;;
    *) echo "error: --config must be release, debug or all" >&2; exit 1 ;;
esac
[[ -d "$OVERLAY" ]] || { echo "error: overlay dir not found: $OVERLAY" >&2; exit 1; }
case "$RESOURCES" in
    copy|merge|none) ;;
    *) echo "error: --resources must be copy, merge or none" >&2; exit 1 ;;
esac

if [[ "$GAME_ASSETS" == 1 ]]; then
    GAME_DIR="${GAME_DIR:-${AMNESIA_GAME_DIRECTORY:-$HOME/.local/share/Steam/steamapps/common/Amnesia The Dark Descent}}"
    if [[ ! -d "$GAME_DIR" ]]; then
        echo "error: game dir not found: $GAME_DIR (pass --game-dir or set AMNESIA_GAME_DIRECTORY)" >&2
        exit 1
    fi
    GAME_DIR="$(realpath "$GAME_DIR")"
fi

is_delta() { [[ "$1" == *.map_delta || "$1" == *.ent_delta ]]; }

# Retail binaries and bulky archives stay behind: names beginning with
# "Amnesia", plus .rar, .pdf, .dll and .exe files.
copy_game_assets() {
    local dest="$1"
    (cd "$GAME_DIR" && find . -type f \
        ! -name 'Amnesia*' ! -name '*.rar' ! -name '*.pdf' ! -name '*.dll' ! -name '*.exe' \
        -print0) |
    while IFS= read -r -d '' rel; do
        mkdir -p "$dest/$(dirname "$rel")"
        cp -f "$GAME_DIR/$rel" "$dest/$rel"
    done
}

# Put back every original an earlier merge stashed: merge re-bakes from them,
# and copy/none must not leave baked files under runtime deltas.
restore_merged_originals() {
    local dest="$1"
    (cd "$dest" && find . -type f -name "*$BACKUP_SUFFIX" -print0) |
    while IFS= read -r -d '' rel; do
        mv -f "$dest/$rel" "$dest/${rel%"$BACKUP_SUFFIX"}"
    done
}

deploy_resources() {
    local dest="$1" mode="$2"
    local manifest="$dest/$MANIFEST_NAME"
    local all placed=""
    all="$(cd "$OVERLAY" && find . -type f | sed 's#^\./##' | LC_ALL=C sort)"

    if [[ "$mode" != none ]]; then
        while IFS= read -r rel; do
            [[ -z "$rel" ]] && continue
            if [[ "$mode" == merge ]] && is_delta "$rel"; then
                continue
            fi
            placed+="$rel"$'\n'
        done <<<"$all"
    fi
    placed="$(printf '%s' "$placed" | LC_ALL=C sort)"

    # Remove what an earlier run placed and this one does not.
    if [[ -f "$manifest" ]]; then
        LC_ALL=C comm -23 <(LC_ALL=C sort "$manifest") <(printf '%s\n' "$placed") |
        while IFS= read -r rel; do
            [[ -z "$rel" ]] && continue
            rm -f "$dest/$rel"
            echo "    removed $rel"
        done
    fi

    restore_merged_originals "$dest"
    if [[ "$mode" == merge ]]; then
        echo "    baking deltas into $dest"
        python3 "$MAPDELTA" apply "$dest" "$OVERLAY" --in-place --backup
    fi

    while IFS= read -r rel; do
        [[ -z "$rel" ]] && continue
        mkdir -p "$dest/$(dirname "$rel")"
        cp -f "$OVERLAY/$rel" "$dest/$rel"
    done <<<"$placed"

    if [[ -n "$placed" ]]; then
        printf '%s\n' "$placed" >"$manifest"
    else
        rm -f "$manifest"
    fi
}

deployed=0
for cfg in "${CONFIGS[@]}"; do
    dest="$OUTPUT/$cfg"
    if [[ ! -d "$dest" ]]; then
        [[ "$CONFIG" == all ]] && continue
        echo "error: $dest does not exist; build $cfg first" >&2
        exit 1
    fi
    if [[ "$GAME_ASSETS" == 1 ]]; then
        echo "==> Deploying game assets from $GAME_DIR to $dest"
        copy_game_assets "$dest"
    fi
    # Editor/viewer runtime data wins over whatever the install carries.
    if [[ -d "$EDITOR_RESOURCES" ]]; then
        echo "==> Deploying editor resources to $dest"
        cp -R "$EDITOR_RESOURCES/." "$dest/"
    fi
    echo "==> Redux resources ($RESOURCES) -> $dest"
    deploy_resources "$dest" "$RESOURCES"
    deployed=1
done

if [[ "$deployed" == 0 ]]; then
    echo "error: no $OUTPUT/<Config> directory exists; build first" >&2
    exit 1
fi
