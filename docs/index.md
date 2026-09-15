---
title: AMD Fluid Motion Frames for Linux
description: Open-source AFMF for Linux. A Vulkan frame generation layer that doubles the frame rate of any game, Proton or native, on AMD RDNA GPUs with RADV.
permalink: /
faq:
  - q: Is afmf-linux really AMD Fluid Motion Frames?
    a: It is the same idea built the same way, driver-level frame generation from the colour buffer at present time using AMD's FidelityFX Optical Flow for the motion and an interpolation pass, applied to any game. It is not AMD's code for the interpolation and it is not affiliated with AMD.
  - q: How much latency does it add?
    a: Half a frame time, the same as AFMF. At 120 real fps that is about 4 ms. AFMF_PACING=0 removes the hold at the cost of an uneven cadence.
  - q: Does it work with Proton and DirectX games?
    a: Yes. DXVK and vkd3d-proton present through Vulkan, which is where the layer sits. It has been measured with DirectX 12 titles under Proton.
  - q: Does it need an AMD GPU?
    a: No. It needs a Vulkan 1.1 driver with compute queues and 32-bit image atomics. Tuned on RDNA4 and verified on RDNA3, both with RADV; RDNA2, Intel and NVIDIA are untested.
  - q: Is it a kernel module or a Mesa patch?
    a: Neither. It is a Vulkan implicit layer, installed like MangoHud, enabled per game with AFMF_ENABLE=1.
---

# AMD Fluid Motion Frames for Linux

<p class="lead">afmf-linux is an open-source frame generation layer for Linux gaming: the
equivalent of AMD Fluid Motion Frames (AFMF) as a Vulkan implicit layer. It generates one
interpolated frame between every two frames a game presents, so the frame rate on screen doubles.
Any game, Proton or native. No kernel module, no driver patch.</p>

<a class="cta" href="{{ '/install/' | relative_url }}">Install</a>
<a class="cta secondary" href="https://github.com/{{ site.repository }}" rel="noopener">Source on GitHub</a>

<div class="numbers">
  <div><strong>2&times;</strong><span>frames on screen: one generated for every real frame</span></div>
  <div><strong>0.43 ms</strong><span>GPU time per frame at 3440&times;1440 on an RX 9070 XT, off the game's queue</span></div>
  <div><strong>~80 &micro;s</strong><span>of the game's thread per frame; the rest runs on the layer's own thread</span></div>
  <div><strong>4 ms</strong><span>added latency at 120 fps: the half frame AMD's AFMF also pays</span></div>
</div>

## What it does

AMD's AFMF is not a hardware feature: on Windows it is compute-shader work the Radeon driver inserts
at present time, for any game, from the colour buffer alone. afmf-linux does the same on Linux:

1. It hooks swapchain creation and asks for a few extra images.
2. On every present it runs **FidelityFX Optical Flow** (AMD's own shaders, vendored under MIT)
   between the previous frame and the new one, and synthesises the frame in between.
3. A presentation thread of the layer shows the generated frame at once and the real frame half a
   frame later, so both land evenly spaced. The game's own thread returns as soon as the work is
   submitted; the GPU work runs on a compute queue of the layer's own.

It works with **DirectX games under Proton** (DXVK, vkd3d-proton) and native Vulkan titles, on
**RADV / AMD RDNA** GPUs and, in principle, any Vulkan 1.1 driver.

## Get started

```sh
# Fedora
sudo dnf install ./afmf-linux-0.3.0-1.fc44.x86_64.rpm
# Steam launch options for the game
AFMF_ENABLE=1 %command%
```

DEB, Arch and a plain tarball are on the [release page](https://github.com/{{ site.repository }}/releases/latest);
the [install guide]({{ '/install/' | relative_url }}) covers every route, including building from source.

## Tested hardware

**AMD Radeon RX 9070 XT (RDNA4)** and **RX 7800 XT (RDNA3)**, Mesa 26.1.8 RADV, KDE Plasma
Wayland, 3440&times;1440 at 165 Hz, with Monster Hunter Wilds and Cyberpunk 2077 under
vkd3d-proton. Other GPUs and drivers: untested, reports welcome.

## Measured

On the RX 9070 XT unless stated, Mesa 26.1.8, KDE Plasma Wayland, 3440&times;1440:

| Game | Base (Linux, no layer) | With afmf-linux |
|---|---|---|
| Monster Hunter Wilds, Native AA, vkd3d-proton | 120 fps | 250 moving / 271 still |
| Cyberpunk 2077, RT Ultra, FSR 4 Quality, vkd3d-proton | ~100-125 fps | ~200-250 |
| Cyberpunk 2077 on an RX 7800 XT (RDNA3), same settings | ~60-110 fps | ~120-220 |

The ceiling is 2&times; the base the game reaches on Linux without the layer; the layer costs
0.43 ms of GPU time per frame and about 80 &micro;s of the game's thread. Details, per-stage numbers
and how they were measured: [performance]({{ '/performance/' | relative_url }}).

## FAQ

{% for item in page.faq %}
**{{ item.q }}** {{ item.a }}
{% endfor %}

More in the [full FAQ]({{ '/faq/' | relative_url }}).
