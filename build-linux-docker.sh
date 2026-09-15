#!/usr/bin/env bash
# Run the premake build inside a Docker / Podman container.
#
# Bind-mounts the project tree at its REAL host path inside the container so
# absolute paths in the generated build-premake/ makefiles line up identically
# between host and container runs (you can switch between a native premake
# build and this wrapper without a full clean).
#
# The wrapper accepts the same options as a native premake build and runs
# `premake5 gmake2` + `make` in the container. Stage game assets and the Redux
# resources afterwards with ./deploy.sh.
# Examples:
#   ./build-linux-docker.sh                          # release
#   ./build-linux-docker.sh debug --clean
#   ./build-linux-docker.sh release -- --slangc=/path/to/slangc
#
# Options:
#   release | debug     Build configuration (default: release)
#   --clean             Remove build-premake/ before generating
#   --compile-commands  Run `premake5 export-compile-commands` and symlink its
#                       output to compile_commands.json in the repo root (for
#                       clangd/LSP tooling). Reads the project model directly,
#                       so it doesn't need a build and every path in the
#                       output is absolute.
#   -- <args>           Extra args forwarded to `premake5 gmake2`
#
# Extra bind mounts (for tools that live outside the project tree, e.g. a
# locally-built slangc):
#
#   AMNESIA_DOCKER_MOUNTS="$HOME/projects/slang" \
#       ./build-linux-docker.sh release -- \
#       --slangc="$HOME/projects/slang/build/Release/bin/slangc"
#
# Multiple paths can be colon-separated. Each is mounted at the same path
# inside the container, so args referring to host paths "just work".

set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
IMAGE="${AMNESIA_DOCKER_IMAGE:-amnesia64-build:ubuntu-24.04}"

# Rootless podman maps container-root back to the host user via subuid/subgid
# — passing --user there would shift to a different in-container uid that
# CAN'T touch files owned by the host user. Real docker (rootful) needs
# --user or every artifact ends up owned by root on the host.
# Probe for the runtime. The previous `docker --version | grep podman` test
# wasn't reliable across podman-docker shim variants, so check for the
# `podman` binary on PATH directly. Override with AMNESIA_DOCKER_RUNTIME=docker
# if both podman and real docker are installed and you want real docker.
RUNTIME="${AMNESIA_DOCKER_RUNTIME:-}"
if [[ -z "$RUNTIME" ]]; then
    if command -v podman >/dev/null 2>&1; then
        RUNTIME="podman"
    else
        RUNTIME="docker"
    fi
fi

"$RUNTIME" build -t "$IMAGE" -f "$ROOT/Dockerfile" "$ROOT"

USER_ARGS=()
if [[ "$RUNTIME" == "podman" ]]; then
    echo "==> Runtime: podman (rootless) — container root maps to host uid $(id -u)."
else
    echo "==> Runtime: docker — running as --user $(id -u):$(id -g) to keep artifacts host-owned."
    USER_ARGS+=(--user "$(id -u):$(id -g)")
fi

# Allocate a TTY only when stdout is one, so CI / pipes still work.
TTY_ARGS=()
if [ -t 1 ]; then
    TTY_ARGS+=("-t")
fi

# Project tree mounted at its real host path so absolute paths in the generated
# build-premake/ makefiles line up identically inside and out.
MOUNT_ARGS=(
    --mount "type=bind,source=$ROOT,target=$ROOT"
)

# Parse build options. Everything after `--` is forwarded verbatim to
# `premake5 gmake2`.
CONFIG="release"
CLEAN=0
COMPILE_COMMANDS=0
EXTRA_ARGS=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        release|debug)       CONFIG="$1"; shift ;;
        --clean)             CLEAN=1; shift ;;
        --compile-commands)  COMPILE_COMMANDS=1; shift ;;
        --)            shift; EXTRA_ARGS=("$@"); break ;;
        --no-deploy|--game-dir)
                       echo "error: $1 was removed; stage assets with ./deploy.sh" >&2; exit 1 ;;
        *)             echo "error: unknown argument '$1'" >&2; exit 1 ;;
    esac
done

ENV_ARGS=(-e HOME=/tmp)
# Any extra host paths the user wants visible — typically a locally-built
# slangc tree referenced via -- --slangc=...
if [[ -n "${AMNESIA_DOCKER_MOUNTS:-}" ]]; then
    IFS=':' read -r -a _extra <<<"$AMNESIA_DOCKER_MOUNTS"
    for p in "${_extra[@]}"; do
        [[ -e "$p" ]] || { echo "error: AMNESIA_DOCKER_MOUNTS path does not exist: $p" >&2; exit 1; }
        MOUNT_ARGS+=(--mount "type=bind,source=$p,target=$p")
    done
fi

ENV_ARGS+=(
    -e "PM_CONFIG=$CONFIG"
    -e "PM_CLEAN=$CLEAN"
    -e "PM_COMPILE_COMMANDS=$COMPILE_COMMANDS"
)

# Run the premake build inside the container. Build options arrive via PM_*
# env vars; the `--`-forwarded extra args are passed as positional args to the
# inner shell ($0 is a label, $@ is the extra args) and handed to gmake2.
exec "$RUNTIME" run --rm "${TTY_ARGS[@]}" "${USER_ARGS[@]}" \
    "${ENV_ARGS[@]}" \
    "${MOUNT_ARGS[@]}" \
    -w "$ROOT" \
    "$IMAGE" \
    bash -lc '
        set -euo pipefail
        [[ "$PM_CLEAN" == 1 ]] && rm -rf build-premake
        JOBS="$(nproc 2>/dev/null || echo 4)"
        echo "==> Generating gmake2 project files"
        premake5 gmake2 "$@"
        if [[ "$PM_COMPILE_COMMANDS" == 1 ]]; then
            # Reads the already-resolved premake project model directly, so
            # it needs no compile step and every path it emits (`directory`
            # and each `-I`) is absolute -- no base-directory ambiguity for
            # tools consuming the database from outside build-premake/.
            echo "==> Exporting compile_commands.json ($PM_CONFIG)"
            premake5 export-compile-commands
            ln -sf "build-premake/compile_commands/$PM_CONFIG.json" compile_commands.json
        fi
        echo "==> Building ($PM_CONFIG)"
        make -C build-premake config="$PM_CONFIG" -j"$JOBS"
        # Premake postbuild only runs when the target relinks, so a Python-only
        # edit would otherwise leave these tests untested. They need no game
        # install, GPU, or display.
        echo "==> Build complete: build-premake/amnesia/"
    ' premake-build "${EXTRA_ARGS[@]}"
