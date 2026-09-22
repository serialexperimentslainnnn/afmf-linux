---
title: Install afmf-linux
description: Install the AMD Fluid Motion Frames layer on Linux from RPM, DEB, the AUR or a tarball, or build it from source, and enable it per game in Steam.
---

# Install

afmf-linux is a Vulkan **implicit layer**: once installed it is present in every Vulkan process
but stays dormant until `AFMF_ENABLE=1` is set for a game. Pick one route.

## Packages

From the [latest release](https://github.com/{{ site.repository }}/releases/latest), one file per
system; the `.asc` and `.sha256` next to each are the signature and checksum, not needed to install.

| You run | Download | Install |
|---|---|---|
| Fedora 44 (Nobara, Bazzite and other Fedora-based) | `afmf-linux-<version>-1.fc44.x86_64.rpm` | `sudo dnf install ./afmf-linux-<version>-1.fc44.x86_64.rpm` |
| Debian 13 trixie and derivatives | `afmf-linux_<version>-1_amd64.trixie.deb` | `sudo apt install ./afmf-linux_<version>-1_amd64.trixie.deb` |
| Ubuntu 24.04 noble (Mint 22, Pop!_OS 24 and other 24.04-based) | `afmf-linux_<version>-1_amd64.noble.deb` | `sudo apt install ./afmf-linux_<version>-1_amd64.noble.deb` |
| Arch Linux (CachyOS, EndeavourOS, Manjaro) | `afmf-linux-<version>-1-x86_64.pkg.tar.zst` | `sudo pacman -U afmf-linux-<version>-1-x86_64.pkg.tar.zst` (or the AUR package `afmf-linux`) |
| Anything else (openSUSE, Gentoo, Void, Solus, NixOS by hand...) | `afmf-linux-<version>-x86_64.tar.xz` | `tar xf afmf-linux-<version>-x86_64.tar.xz && afmf-linux-<version>/install.sh` (installs under `~/.local`, no root; `--uninstall` removes it) |

The `-debuginfo`, `-debugsource` and `-debug` packages carry debugging symbols only: you do not
need them to play, only to produce a backtrace for a bug report.

Every asset has a `.sha256` and a detached `.asc` signature made with the project's release key
(`packaging/afmf-linux-release-key.asc` in the repository, certified by the maintainer's keys):

```sh
gpg --import afmf-linux-release-key.asc
gpg --verify afmf-linux-<version>-x86_64.tar.xz.asc afmf-linux-<version>-x86_64.tar.xz
```

## From source

Requirements: CMake 3.28+, Ninja or Make, GCC or Clang (C11), the Vulkan headers and loader,
`glslang`; `spirv-tools` is optional and validates every shader.

```sh
# Fedora
sudo dnf install cmake ninja-build gcc vulkan-headers vulkan-loader-devel glslang spirv-tools
# Ubuntu / Debian
sudo apt install cmake ninja-build gcc libvulkan-dev glslang-tools spirv-tools
# Arch
sudo pacman -S cmake ninja gcc vulkan-headers vulkan-icd-loader glslang spirv-tools

git clone https://github.com/{{ site.repository }}.git && cd afmf-linux
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
sudo cmake --install build --prefix /usr/local
```

To use it from the build tree without installing:

```sh
VK_ADD_IMPLICIT_LAYER_PATH=$PWD/build/layer AFMF_ENABLE=1 vkcube
```

## Enable it for a game

Steam launch options, Proton or native:

```
AFMF_ENABLE=1 %command%
```

Any other launcher: put `AFMF_ENABLE=1` in the game's environment. `DISABLE_AFMF=1` keeps the
layer out of a process even when enabled, which is the quickest way to measure the base frame
rate.

Every pixel of the flow, at about a quarter more GPU cost per generated frame, with the log on:

```
AFMF_ENABLE=1 AFMF_PERFORMANCE_MODE=quality AFMF_SEARCH_MODE=high AFMF_LOG=2 %command% 2>afmf.log
```

The log says what the layer decided for each swapchain; with `AFMF_PROFILE=1` it also reports
real fps, the time spent in the layer and the pacing hold. All variables are on the
[configuration]({{ '/configuration/' | relative_url }}) page.

## Check that it works

```sh
vulkaninfo --summary | grep VK_LAYER_AFMF          # the layer is installed
AFMF_ENABLE=1 AFMF_LOG=2 vkcube --present_mode 1   # "... generated ..." lines on stderr
```

In a game, MangoHud shows `real + generated` frames per second; the layer's log shows how many
of the presents got a companion.

## Uninstall

Remove the package, or `afmf-linux-<version>/install.sh --uninstall` for the tarball, or delete
`libafmf-linux.so` and `afmf-linux.json` from where `cmake --install` put them.
