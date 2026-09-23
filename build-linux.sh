#!/usr/bin/env bash
# Run the premake build directly on the native Linux host.
#
# build-linux-docker.sh is the containerized equivalent and canonical path.
# This wrapper runs `premake5 gmake2` + `make` without Docker or Podman. Stage
# game assets and the Redux resources afterwards with ./deploy.sh.

set -euo pipefail

CONFIG="release"
CLEAN=0
WITH_TEST=0
EXTRA_ARGS=()

usage() {
    cat <<'EOF'
Usage: ./build-linux.sh [release|debug] [options] [-- <extra premake args>]

Options:
    --clean              Remove build-premake/ before generating
    -with-test           Build and run the unit tests (disabled by default)
    -h, --help           Show this help

Anything after `--` is forwarded verbatim to `premake5 gmake2`.

Examples:
    ./build-linux.sh                                # native release
    ./build-linux.sh debug                          # native debug
    ./build-linux.sh debug -with-test               # build and run tests
    ./build-linux.sh release --clean                # wipe build-premake/ and rebuild
    ./build-linux.sh release -- --with-fsr=no
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        release|debug)  CONFIG="$1"; shift ;;
        --clean)        CLEAN=1; shift ;;
        -with-test|--with-test)
                        WITH_TEST=1; shift ;;
        --no-deploy|--game-dir)
                        echo "error: $1 was removed; stage assets with ./deploy.sh" >&2; exit 1 ;;
        -h|--help)      usage; exit 0 ;;
        --)             shift; EXTRA_ARGS=("$@"); break ;;
        *)              echo "error: unknown argument '$1'" >&2; usage >&2; exit 1 ;;
    esac
done

ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"

if [[ "$(uname -s)" != "Linux" ]]; then
    echo "error: build-linux.sh runs on Linux hosts" >&2
    exit 1
fi

if [[ ! -f HPL2/extern/SDL/CMakeLists.txt ]]; then
    echo "==> Initialising git submodules"
    git submodule update --init --recursive
fi

if ! command -v premake5 >/dev/null 2>&1; then
    echo "error: premake5 5.0.0-beta8 is required on PATH (the version pinned by CI and Dockerfile)" >&2
    exit 1
fi

if [[ "$CLEAN" == "1" ]]; then
    echo "==> Cleaning build-premake"
    rm -rf build-premake
fi

echo "==> Generating gmake2 project files"
TEST_OPTION="no"
if [[ "$WITH_TEST" == "1" ]]; then
    TEST_OPTION="yes"
fi
PREMAKE_ARGS=(
    "--with-tests=$TEST_OPTION"
    "--with-python-tests=$TEST_OPTION"
)
if [[ ${#EXTRA_ARGS[@]} -gt 0 ]]; then
    premake5 gmake2 "${PREMAKE_ARGS[@]}" "${EXTRA_ARGS[@]}"
else
    premake5 gmake2 "${PREMAKE_ARGS[@]}"
fi

echo "==> Building ($CONFIG)"
make -C build-premake config="$CONFIG" -j"$(nproc 2>/dev/null || echo 4)"

if [[ "$WITH_TEST" == "1" ]]; then
    # Premake postbuild only runs when the target relinks, so a Python-only
    # edit would otherwise leave these tests untested. They need no game install,
    # GPU, or display.
    echo "==> Running python tests"
    python3 scripts/run_python_tests.py
fi

echo "==> Build complete: build-premake/amnesia/"
