# Ubuntu 24.04 build environment for Amnesia64.
#
# Provides every system dev library the in-tree builds of SDL2, openal-soft,
# and the HPL2 engine pull in via #include. The Linux CI workflow
# (.github/workflows/build.yml) installs a smaller set because GitHub's
# ubuntu-latest image already ships some headers; this Dockerfile lists the
# full set explicitly so a clean container can build. glslang-tools and
# spirv-tools provide the FSR 3.1 SDK shader-compiler and SPIR-V validation tools.
#
# Not a runtime image — intended only as a build sandbox. Invoke via
# ./build-linux-docker.sh, which bind-mounts the source tree and runs the
# premake build inside the container so build outputs land back on the host.
#
# Ships premake5 (drives the gmake2 build) and python3 for the FSR 3.1 SDK
# shader-compiler wrapper / SPIR-V validation scripts, the
# scripts/hpl2_amnesia_patch.py source patch, and the stdlib-only Python test
# suite the build runs. These are in addition to the system dev libraries the
# in-tree SDL2/openal-soft/HPL2 builds need. cmake is still required: SDL2 and
# openal-soft are built via their own CMake as premake "Makefile" projects
# (premake/external.lua).

FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential clang ninja-build python3 glslang-tools spirv-tools \
        git ca-certificates curl \
        libx11-dev libxext-dev libxi-dev libxcursor-dev libxrandr-dev libxss-dev \
        libgl-dev libglu1-mesa-dev libegl1-mesa-dev \
        libwayland-dev libxkbcommon-dev libdecor-0-dev \
        libasound2-dev libpulse-dev libdbus-1-dev libsamplerate0-dev \
        libudev-dev \
        liblcms2-dev \
    && rm -rf /var/lib/apt/lists/*

# cmake: Ubuntu 24.04's apt package is 3.28, but NRD fetches MathLib v11, whose
# CMakeLists.txt requires 3.30. Install a pinned Kitware release instead.
ARG CMAKE_VERSION=3.31.6
RUN curl -fsSL -o /tmp/cmake.tar.gz \
        "https://github.com/Kitware/CMake/releases/download/v${CMAKE_VERSION}/cmake-${CMAKE_VERSION}-linux-$(uname -m).tar.gz" \
    && mkdir -p /opt/cmake \
    && tar -xzf /tmp/cmake.tar.gz -C /opt/cmake --strip-components=1 \
    && ln -sf /opt/cmake/bin/cmake /opt/cmake/bin/ctest /opt/cmake/bin/cpack /usr/local/bin/ \
    && rm /tmp/cmake.tar.gz \
    && cmake --version

# premake5: prebuilt Linux binary from the project's GitHub releases. The asset
# is named premake-<ver>-linux.tar.gz and contains a single `premake5` binary.
ARG PREMAKE_VERSION=5.0.0-beta8
RUN curl -fsSL -o /tmp/premake.tar.gz \
        "https://github.com/premake/premake-core/releases/download/v${PREMAKE_VERSION}/premake-${PREMAKE_VERSION}-linux.tar.gz" \
    && tar -xzf /tmp/premake.tar.gz -C /usr/local/bin premake5 \
    && chmod +x /usr/local/bin/premake5 \
    && rm /tmp/premake.tar.gz \
    && premake5 --version

# The wrapper bind-mounts the host source tree at its real host path. Whichever
# uid ends up owning the files inside the container, treat any mounted repo as
# trusted so submodule init doesn't trip the safe.directory guard.
RUN git config --system --add safe.directory '*'

CMD ["bash"]
