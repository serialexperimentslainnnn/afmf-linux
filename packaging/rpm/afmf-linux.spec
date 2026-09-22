# Fedora / RPM packaging for afmf-linux. Built in CI from the release tarball of the sources;
# usable as is for COPR.
%global layer_dir %{_datadir}/vulkan/implicit_layer.d

Name:           afmf-linux
Version:        1.3.1
Release:        1%{?dist}
Summary:        AMD Fluid Motion Frames for Linux: Vulkan frame generation layer
License:        MIT
URL:            https://afmf-linux.digitalexperiments.dev/
Source0:        https://github.com/serialexperimentslainnnn/afmf-linux/archive/v%{version}/%{name}-%{version}.tar.gz

BuildRequires:  cmake >= 3.28
BuildRequires:  ninja-build
BuildRequires:  gcc
BuildRequires:  vulkan-headers >= 1.3.250
BuildRequires:  vulkan-loader-devel
BuildRequires:  glslang
BuildRequires:  spirv-tools
Requires:       vulkan-loader

%description
An open-source equivalent of AMD Fluid Motion Frames (AFMF) for Linux: a Vulkan implicit layer
that generates one interpolated frame between every two frames a game presents, from the colour
buffer alone, using FidelityFX Optical Flow. Works with any 64-bit Vulkan application, including
DXVK and vkd3d-proton titles under Proton. Dormant until AFMF_ENABLE=1 is set for a game.

%prep
%autosetup -n %{name}-%{version}

%build
%cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -DAFMF_WERROR=OFF
%cmake_build

%install
%cmake_install

%files
%license LICENSE shaders/fidelityfx/LICENSE.txt
%doc README.md CHANGELOG.md shaders/fidelityfx/NOTICE.md
%{_libdir}/libafmf-linux.so
%{layer_dir}/afmf-linux.json

%changelog
* Tue Sep 22 2026 Lain <lain@digitalexperiments.dev> - 1.3.1-1
- No double image on fast camera motion: motion is believed up to 128 pixels between frames,
  and where the two warped samples disagree the blend leans on the current frame's unmoved
  pixel, so an object the camera follows stays whole
- Device-level commands resolved through vkGetInstanceProcAddr are hooked only where the
  driver has them; a game submitting from several threads no longer serialises on the layer;
  the layer's queue is never protected; vkCreateDevice retries with the game's own request
- The governor paces one-in-n on the frame's own number; AFMF_MIN_FPS clears with a margin;
  the pacing hold has a minimum with pacing on and a 40 ms ceiling
- The HDR luma is clamped before it is stored as a byte; the half-resolution downscale samples
  against the frame's size

* Tue Sep 22 2026 Lain <lain@digitalexperiments.dev> - 1.3.0-1
- The optical flow and the block search follow the resolution by default instead of always
  asking for the most: half resolution and five pyramid levels from 1440p up, 716 to 578 us
  per generated frame at 3440x1440. AFMF_PERFORMANCE_MODE=quality AFMF_SEARCH_MODE=high keeps
  the previous behaviour
- Half resolution reads the game's frame once, as full resolution already did, instead of
  copying it and downscaling the copy

* Tue Sep 22 2026 Lain <lain@digitalexperiments.dev> - 1.2.0-1
- The block search needs evidence before it keeps a vector: a block with no detail to match
  stores zero instead of the coarse level's guess, which is what smeared moving silhouettes
  over fog, snow and sky

* Tue Sep 22 2026 Lain <lain@digitalexperiments.dev> - 1.1.1-1
- AFMF_DUMP_DIR creates the directory it names, writes 10-bit swapchains, and reports what it
  cannot write instead of producing nothing in silence

* Mon Sep 21 2026 Lain <lain@digitalexperiments.dev> - 1.1.0-1
- GPU hang on Intel (ANV): the block search's cross-subgroup reductions now combine every
  subgroup the driver chose; the scene change detector completes before the coarsest search
  and its own reductions are correct on 8- and 16-lane subgroups; generation checks the
  device's subgroup operations and storage formats before starting

* Wed Sep 16 2026 Lain <lain@digitalexperiments.dev> - 1.0.0-1
- Quality defaults (flow at display resolution, seven levels, blend) at the cost of the old
  half-resolution ones: blocks whose coarser-level vector matches skip the search; FIFO
  swapchains move to MAILBOX; HUD detection; direct output; governor; pre-recorded command
  buffers; fixes to the presentation thread's lock and pacing

* Tue Sep 15 2026 Lain <lain@digitalexperiments.dev> - 0.4.0-1
- AFMF_GAMESCOPE=1 for games under Gamescope; verified on RDNA3; tested hardware page

* Tue Sep 15 2026 Lain <lain@digitalexperiments.dev> - 0.3.0-1
- First public release
