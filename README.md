# Amnesia: The Dark Descent — Redux

[![Build](https://github.com/flying-swallow/Redux-AmnesiaTheDarkDescent/actions/workflows/build.yml/badge.svg?branch=main)](https://github.com/flying-swallow/Redux-AmnesiaTheDarkDescent/actions/workflows/build.yml)
[![Itch.io](https://img.shields.io/badge/Itch-%23FF0B34.svg?style=for-the-badge&logo=Itch.io&logoColor=white)](https://mpollind.itch.io/amnesia-the-dark-descent-redux)


![Preview](./images/preview.png?raw=true)

A rework of Frictional Games' HPL2 engine. The fixed-function OpenGL renderer has been replaced with a
Vulkan render-interface layer, the shaders are written in [Slang](https://shader-slang.org/), and the
lighting path is being moved onto a hybrid rasterization + ray-tracing renderer with surfel-based global
illumination.

Work in progress. It is playable end to end, but expect rendering artifacts and broken functionality.

## You need the retail game

This repository contains **engine, game, and tool code only**. It ships no game data. To play, you must own a
copy of *Amnesia: The Dark Descent* (Steam, GOG, or the Frictional store) — the maps, entities, sounds,
scripts, and configs all come from your install.

The itch.io and GitHub Release downloads carry the same restriction: rebuilt executables and compiled shaders
only, nothing from the retail game. Frictional's engine source is GPL; its game assets are not.

## Getting a build

- **itch.io** — [mpollind.itch.io/amnesia-the-dark-descent-redux](https://mpollind.itch.io/amnesia-the-dark-descent-redux),
  channels `windows` and `linux`.
- **GitHub Releases** — the same archives, attached to each tagged prerelease.
- **From source** — see [Building](#building).

Copy the contents of the download into your Amnesia install directory and run `Amnesia` / `Amnesia.exe` from
there, so the executable finds the game's data files.

## Renderer

- Vulkan-only render interface (`HPL2/core/include/graphics/RI*.h`) — no OpenGL, no fixed-function path.
- Slang shaders under `amnesia/slang/`, compiled to SPIR-V at build time by a pinned `slangc` that the build
  downloads for you.
- Hybrid renderer (`HPL2/core/include/graphics/HybridRenderer.h`): visibility buffer, deferred and forward
  passes, plus the surfel-GI chain (prepare / update / ray-trace / integrate / generate) driving indirect
  lighting.
- Bindless resource pools and a global managed descriptor set, replacing HPL2's per-draw binding.

## Building

### Prerequisites

- **premake5 5.0.0-beta8** on your `PATH` ([releases](https://github.com/premake/premake-core/releases)).
- **CMake** — not for this project, but SDL2 and openal-soft are built by driving their own CMake
  (`premake/external.lua`).
- Linux: GCC or Clang with C++20, plus the X11/Wayland/GL/audio development packages. The exact apt list is
  kept in [`.github/workflows/linux-build.yml`](.github/workflows/linux-build.yml) — copy it from there, or
  use the container path below.
- Windows: Visual Studio 2022 or newer with the Desktop C++ workload. Enable long paths before cloning; this
  tree has paths that exceed `MAX_PATH`:
  ```
  git config --system core.longpaths true
  ```

### Getting Started

```
git clone --recurse-submodules git@github.com:flying-swallow/Redux-AmnesiaTheDarkDescent.git
cd Redux-AmnesiaTheDarkDescent
```

Dependencies are git submodules under `HPL2/extern/` — clone recursively, or run
`git submodule update --init --recursive` afterwards.

### Linux

```
premake5 gmake2
make -C build-premake config=release -j"$(nproc)"
```

Or build inside the Ubuntu 24.04 container defined by the repo-root [`Dockerfile`](Dockerfile), which ships
premake5 and every dev package the build needs, so nothing has to be installed on the host:

```
docker build -t amnesia-build .
docker run --rm -v "$PWD:$PWD" -w "$PWD" amnesia-build \
    bash -c 'premake5 gmake2 && make -C build-premake config=release -j"$(nproc)"'
```

Mount the tree at its real host path (as above) so `compile_commands.json` and the paths baked into the object
files line up between containerized and native builds.

Two wrappers drive the same premake5 flow end to end — generate, build, run the Python tests, and
optionally deploy your install's assets: [`build-linux-docker.sh`](build-linux-docker.sh) does it in the
container (canonical), [`build-linux.sh`](build-linux.sh) natively on the host. Both take `[release|debug]`
and forward anything after `--` to `premake5 gmake2`.

### Windows

```
premake5 vs2026
msbuild build-premake\Amnesia.sln /p:Configuration=Release /p:Platform=x64 /m
```

Or use the wrapper, which locates MSBuild via `vswhere` so any PowerShell works:

```
.\build-windows.ps1
```

CI generates with `vs2022` instead; `premake5.lua` pins no `_ACTION`, so both produce the same projects.

### Output

Everything lands in `build-premake/amnesia/<Debug|Release>/`: `Amnesia`, the four editors (`LevelEditor`,
`ModelEditor`, `MaterialEditor`, `ParticleEditor`), `MshConverter`, the `compiled_shaders/` directory, and on
Linux the colocated SDL2/OpenAL shared libraries under `libs/` (found via an `$ORIGIN/libs` rpath).

### Options

Pass these to `premake5`:

| Option | Default | Effect |
| --- | --- | --- |
| `--with-tools=yes\|no` | `yes` | Build the editors and converters. |
| `--with-tests=yes\|no` | `yes` | Build and run the headless unit tests. |
| `--with-python-tests=yes\|no` | `yes` | Build and run the Python unit tests. |
| `--with-fsr=yes\|no` | `yes` | Build and link the FidelityFX Super Resolution SDK. |
| `--with-xess=yes\|no` | `yes` | Intel XeSS Vulkan super-resolution backend (Windows only; ignored on Linux). |
| `--graphics-x11=on\|off` | `on` | (Linux) X11 Vulkan surface backend. |
| `--graphics-wayland=on\|off` | `on` | (Linux) Wayland Vulkan surface backend. |
| `--slangc=PATH` | downloads | Use an existing `slangc` instead of the pinned download. |
| `--cmake=PATH` | `cmake` | The CMake used to build SDL2 and openal-soft. |

`premake5 export-compile-commands` writes a `compile_commands.json` for clangd.

## Running

The engine resolves resources relative to the working directory, so the executable must run from a directory
holding the game's `config/`, `entities/`, `maps/`, `core/`, and so on. Two ways to get there:

- Stage your install's assets and the Redux resources next to the build output:
  ```
  ./deploy.sh --game-dir "$HOME/.steam/steam/steamapps/common/Amnesia The Dark Descent"
  ```
  This copies everything except the original binaries, DLLs, and archives into
  `build-premake/amnesia/<Config>/`, then adds `amnesia/resources` (`--resources copy`,
  applied by the engine at load) or bakes its deltas in (`--resources merge`).
- Or copy the build output into your install directory and launch it from there.

On Windows the generated projects set the debugger working directory to `$(ATDD_DIR)`, so set that environment
variable to your install and F5 works directly.

[BUILD.md](BUILD.md) has more detail on the wrappers, the option table, and the shader tooling.

## License

The tree is a mix; `LICENSE` holds the GPL v3 text, but per-component:

- Any code published by **Frictional Games** is under the GNU General Public License.
- Some code from **Open 3D Engine**, under Apache-2.0 OR MIT.
- Any new code under my name uses the Apache-2.0 license.
- Sebastian Aaltonen 2023, MIT (see `LICENSE`).
