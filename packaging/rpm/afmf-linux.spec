# Fedora / RPM packaging for afmf-linux. Built in CI from the release tarball of the sources;
# usable as is for COPR.
%global layer_dir %{_datadir}/vulkan/implicit_layer.d

Name:           afmf-linux
Version:        0.3.0
Release:        1%{?dist}
Summary:        AMD Fluid Motion Frames for Linux: Vulkan frame generation layer
License:        MIT
URL:            https://serialexperimentslainnnn.github.io/afmf-linux/
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
* Tue Sep 15 2026 Lain <lain@digitalexperiments.dev> - 0.3.0-1
- First public release
