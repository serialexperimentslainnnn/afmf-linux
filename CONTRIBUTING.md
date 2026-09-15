# Contributing to afmf-linux

Thanks for looking. The project is small on purpose; the bar for changes is that they are
measured and tested, not that they are large.

## Build and test

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure      # needs a GPU and the Khronos validation layer
cmake -S . -B build-asan -G Ninja -DAFMF_SANITIZE=ON && cmake --build build-asan && ctest --test-dir build-asan
shellcheck -x -s bash tests/smoke.sh
```

Warnings are errors (`AFMF_WERROR`, on by default). Before opening a pull request, all of the
above must pass on your machine: CI cannot run the GPU tests, so it only builds, lints and runs
the static analyser.

## What a change needs

- **A number.** Performance changes come with the before/after from `AFMF_PROFILE=1` (GPU time
  per stage, host time per present) at the resolution you tested, and the game or the headless
  test it was measured with. `docs/performance.md` has the register; add a row.
- **The golden test still passes.** `tests/headless.c` checks that the generated frame shows the
  synthetic square exactly halfway between the real ones; if your change moves that, say why.
- **Validation clean.** The headless tests run under `VK_LAYER_KHRONOS_validation`, thread safety
  included, and fail on any validation error.
- **No graphics-only commands** on the layer's command buffers (they run on a compute family).
- **Nothing under `shaders/fidelityfx/`** is edited beyond the one marked block
  `shaders/fidelityfx/NOTICE.md` lists: it is vendored from the FidelityFX SDK as is.
  A new local edit needs the `afmf-linux:` mark on every line and a line in that NOTICE.

## Style

C11 (`gnu11`), the warning set in `CMakeLists.txt`, 100-column lines, comments that say why.
Every symbol with external linkage carries the `afmf_` prefix. Commit messages follow
[Conventional Commits](https://www.conventionalcommits.org/) (`feat(swapchain): ...`,
`perf(framegen): ...`, `fix: ...`) with a body that explains the reason when the diff does not.

## Reporting a problem

Use the bug report template. It asks for the things every diagnosis has needed so far: GPU,
Mesa version, Proton build, compositor, the full launch line and the layer's log with
`AFMF_LOG=2 AFMF_PROFILE=1`.
