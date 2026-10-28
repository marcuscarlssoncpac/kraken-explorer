# Build environment for Kraken Explorer: the same Ubuntu and packages as CI
# (.github/workflows/cmake.yml, BUILD_DEPS + GUI_DEPS), plus git for the version tag.
FROM ubuntu:24.04

RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build pkg-config git ca-certificates \
        libusb-1.0-0-dev zlib1g-dev libnl-3-dev libnl-route-3-dev python3-dev pybind11-dev libgl-dev \
        libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev \
        libwayland-dev libxkbcommon-dev wayland-protocols \
    && rm -rf /var/lib/apt/lists/*
