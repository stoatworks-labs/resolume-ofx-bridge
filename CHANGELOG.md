# Changelog

All notable changes to this project are documented here.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and
this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.3.1] — 2026-07-31

### Fixed

- **CI self-tests now assert pixel values, not exit codes.** The first Metal CI
  run reported success while rendering an entirely black frame, because the check
  only looked at the process status. `ffgltest --expect-centre R,G,B,A` now fails
  on a wrong pixel.
- **GPU self-tests skip honestly rather than passing meaninglessly.** GitHub's
  hosted macOS runners have no GPU (`Apple Software Renderer`), where
  IOSurface-backed GL/Metal sharing cannot work. `--require-gpu` exits 3 and CI
  skips with a notice; any other failure is now fatal.

All GPU measurements in the docs come from real hardware, never from CI.

## [0.3.0] — 2026-07-31

### Added

- **OFX Metal render path** — the one Resolve-targeted plugins on macOS actually
  use. GL texture and Metal buffer share one IOSurface, so no pixel crosses to
  the CPU and there are no copies at all: just two on-GPU blits.
  **5.4× faster at 1080p** (2.14 ms → 0.40 ms) and 3.6× at 4K
  (3.20 ms → 0.90 ms). More importantly, **GPU-only plugins can now run at all** —
  before this they did not render slowly, they refused to render.
- **A Metal OFX test plugin** (`testplugins/metal-gain/`). No such plugin is
  publicly available — the OpenFX examples are all CPU or OpenGL, and commercial
  Metal plugins refuse to load in an unrecognised host — so without it the Metal
  path could not be developed or verified. It is deliberately GPU-only, so a host
  that silently falls back to CPU is caught rather than flattered.

### Verified

Gain at 0.5 halves every channel including alpha; at 2.0 it saturates; at its
default of 1.0 it is bit-exact identity, ruling out a blit-through that would
otherwise look like success. The demo image is visually identical to the CPU
result, confirming channel order survives the BGRA IOSurface round trip.

### Still missing

CUDA and OpenCL, which is what Resolve uses on Windows and Linux — platforms this
project has never been compiled for.

## [0.2.0] — 2026-07-31

### Added

- **OFX OpenGL render path.** When a plugin advertises
  `kOfxImageEffectPropOpenGLRenderSupported`, the wrapper hands it the GL texture
  directly and no pixel crosses to the CPU. Negotiated per plugin and recorded in
  the manifest; the CPU path remains the fallback. On the GL path no CPU frames
  are allocated at all, saving 66 MB per instance at 4K.
- **Per-stage timing** via `OFXBRIDGE_TIMING=1`, and `ffgltest --bench` /
  `--size` for measuring a frame properly.
- `ffgltest --legacy-gl` for testing GL-render plugins that use immediate mode.
- The OpenFX OpenGL example is now part of the test corpus.
- [docs/04-gpu-acceleration.md](docs/04-gpu-acceleration.md), covering the
  measurements, the three OFX GPU paths and what is actually implemented.

### Measured

On an M4 Max, the Gain example pipelined: **2.25 ms/frame at 1080p** (13% of a
60fps budget) and **2.94 ms at 4K** (18%). The transfer costs ~2.2 ms at both
resolutions — it is bound by synchronisation latency, not bandwidth, because
memory is unified. The CPU round trip is therefore not the main cost on Apple
Silicon; a heavy plugin's own CPU render is.

### Known limitation

An OFX plugin using the OpenGL render path can only work inside Resolume if it
draws with **core-profile** GL. The OpenFX example uses immediate mode, which is
illegal in a core profile and unavailable above GL 2.1 on macOS, so it renders
correctly under `--legacy-gl` and produces a black frame in the core context
Resolume actually uses. No host-side change can fix that.

## [0.1.0] — 2026-07-31

First release. A working prototype: verified against real OFX plugins by the
harnesses in this repo, but never loaded into Resolume itself.

### Added

- **OFX host** built on the OpenFX project's BSD-3 `HostSupport` library, with
  concrete parameter instances for every hosted type, the property/parameter/
  clip/message/progress/timeline suites, and a bounded multi-thread suite.
- **`ofxgen`** — scans OFX plugin directories and generates one FFGL bundle per
  plugin. Generation is a copy plus a JSON manifest, so no compiler is needed to
  use it. `ofxgen verify` loads a generated bundle as a host would.
- **`ofxprobe`** — dumps a plugin's parameters, emits its manifest, and with
  `--render` pushes a frame through it entirely on the CPU.
- **`ffgltest`** — drives a generated bundle through a real offscreen OpenGL
  context, the only way to exercise `ProcessOpenGL` without Resolume.
- **Parameter mapping** preserving OFX types, display ranges, group headings,
  colour-component tagging and choice options in declaration order.
- `scripts/build-test-plugins.sh`, which builds a corpus of real OFX plugins from
  the OpenFX examples — necessary because a clean machine has none, even with
  DaVinci Resolve installed.

### Known limitations

- Never loaded into Resolume; its real texture sizes, premultiplication and
  resize behaviour are unconfirmed. See [docs/03-verification.md](docs/03-verification.md).
- CPU rendering only — a full GPU→CPU→GPU round trip per frame. The OFX GPU
  render extensions (Metal/CUDA/OpenCL) are not implemented.
- Only the OFX Filter context is hosted.
- Parametric (curve) parameters are declined rather than misrepresented.
- Commercial plugins that license themselves to specific hosts will refuse to
  load; host-name spoofing is deliberately not implemented.
- `createEffect` loads every bundle in the target's directory, because
  HostSupport's `addFileToPath` is directory-scoped.

[0.3.1]: https://github.com/stoatworks-labs/resolume-ofx-bridge/releases/tag/v0.3.1
[0.3.0]: https://github.com/stoatworks-labs/resolume-ofx-bridge/releases/tag/v0.3.0
[0.2.0]: https://github.com/stoatworks-labs/resolume-ofx-bridge/releases/tag/v0.2.0
[0.1.0]: https://github.com/stoatworks-labs/resolume-ofx-bridge/releases/tag/v0.1.0
