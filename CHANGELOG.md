# Changelog

All notable changes to this project are documented here.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and
this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- **Windows is a supported platform, not a code path.** The whole toolset —
  `ofxwrapper`, `ofxgen`, `ofxprobe`, `ffgltest`, the FFGL→OFX shell and the AE
  guest — builds for Windows x64 and is self-tested on every release: the corpus
  from the OpenFX examples, introspection, CPU render, generation, and
  `ofxgen verify` loading a generated plugin as a host would. Windows CPU render
  produces the same numbers as macOS, digit for digit.
- `scripts/build-test-plugins.ps1`, the Windows counterpart of the corpus script.
- `ofxgen verify` works on Windows, over LoadLibrary/GetProcAddress.
- `ffgltest` builds everywhere. It takes its context and module loader from
  `ffglguest/Platform`, so there is one WGL implementation in the repo, and it
  exits 3 — the existing skip status — when the machine cannot give it a
  context at all.

### Fixed

- **Wrapped bundles put their binary in `Contents/MacOS` on every platform.** An
  OFX host reads `Contents/Win64` or `Contents/Linux-x86-64` and only macOS
  falls back, so every Windows and Linux bundle the generator or the browser
  wrapper produced was invisible to the host it was built for. Now written for
  the shell's own platform.
- **The Windows shell imported `glew32.dll`**, which is on no ordinary machine
  and in no archive shipped — vcpkg's default triplet is dynamic. Everything is
  built `x64-windows-static-md` now, and CI fails the release if any artefact
  imports a library that is not in the zip.
- The host advertised Metal and OpenCL render on platforms where neither interop
  is compiled, so a plugin that took the offer failed mid-render instead of
  being declined at describe time. Both are now gated on the bridge existing,
  the way CUDA always was.
- The After Effects guest built for the host architecture rather than the
  target, so an ARM64 machine cross-compiling x64 handed the linker the wrong
  slice and got an error about missing symbols.
- `OFX_SUPPORTS_MULTITHREAD` is defined on every platform. Our implementation is
  `std::thread` and `std::recursive_mutex`; it had been behind `if(APPLE)`
  because nothing else had ever been compiled.
- A shell wrapped on Windows detects an After Effects guest by its `.aex`
  extension — there is no `CFBundlePackageType` to read there.
- `ffgltest` writes its diagnostics unbuffered and announces the context attempt
  before making it, so a run that dies inside a display driver still says how
  far it got.

## [0.8.1] — 2026-08-03

### Added

- **The any-to-any matrix, written down.** AGENTS.md now draws which host can carry
  which guest, and a video and its thumbnail show the chain working.

### Changed

- The download block regenerated against the release.

## [0.8.0] — 2026-08-03

### Added

- **After Effects as a guest — the last two cells of the matrix.** `source/aeguest/` is
  a minimal AE effect *host* in Rust over the community `-sys` bindings, with no Adobe
  SDK: GLOBAL_SETUP / PARAMS_SETUP / RENDER, the `add_param` interact callback, the
  iterate util callback, the pica Handle Suite, and round-tripped global data. Unknown
  suite requests are refused **by name** to stderr, so extending it stays evidence-driven.
  One Adobe quirk earned a comment in the source: `kPFHandleSuiteVersion1` is literally 2.
- **`ofxgen wrap-ae`** wraps a `.plugin` as a self-contained OFX bundle through the same
  shell as `wrap-ffgl`, dispatching on the manifest's `guestType`. AE gets straight
  colour where FFGL gets premultiplied, and colour parameters cross as packed RGB.
  AE→FFGL is the two bridges composed: `wrap-ae`, then `generate` on the result.

### Fixed

- `rustup target add` now runs before the `aeguest` universal build; CI ships one target.
- The OpenGL deprecation warning on `ffglguest` is silenced — CI promotes it to an error.

## [0.7.0] — 2026-08-03

### Added

- **The other direction: FFGL plugins wrapped as OpenFX bundles.** The founding
  architecture — a prebuilt self-configuring shell, a sidecar manifest, and a generator
  that is really a file-copier — turned out to be direction-agnostic, so this is the
  FFGL→OFX cell of an any-to-any matrix. `source/ffglguest/` hosts an FFGL plugin as a
  guest: it dlopens the bundle, reads its parameter table over `plugMain`, and pushes
  frames through `ProcessOpenGL` in a private offscreen CGL context. `source/ofxshell/`
  is the generic OFX plugin that carries one — parameters declared from the manifest
  (FFGL types map up cleanly: options become choices, events become pushbuttons, 2.2
  ranges are honoured), frames crossing as premultiplied RGBA8, renders serialised by a
  global mutex. `ofxgen wrap-ffgl` writes a self-contained `.ofx.bundle` with the
  untouched guest inside `Contents/Guest`.

  Verified end to end through `ofxprobe`: a wrapped Tinsel renders its real GLSL and
  matches the native CPU port visually, and a wrapped Luma Key agrees with the
  hand-written OFX port byte-for-byte on the probe frame.

### Changed

- Parameter edits are delivered as `instanceChanged` actions, and writes made by the
  plugin are forwarded back.

## [0.6.0] — 2026-08-03

### Added

- **An About surface.** Product name, version, and a button each for the user guide, the
  project page, the source and the support page. Deliberately **not** a window: FFGL 2.x
  has none and cannot make one — a plugin declares parameters, Resolume draws them in
  Resolume's layout, and that is the entire surface a plugin gets. So it is a text
  parameter carrying name and version, plus an event parameter per link, which the host
  draws as a button.
- A user guide, and a Download section with direct per-platform links.

### Fixed

- **Every generated plugin failed to instantiate** after the About surface landed.
- **The exit-time teardown crash.** `createEffect` kept its plugin caches in
  function-local statics, destroyed at process exit — and at exit each plugin module's
  own finalizers run *before* ours, destroying the plugin-side state a Support-library
  plugin still points the cache at. `~PluginCache`'s `kOfxActionUnload` then called
  through freed memory and died at PC=0. Raw-C plugins survived, which made it look
  plugin-specific; it wasn't. The caches are now deliberately leaked, which is the
  standard shape for process-lifetime singletons — a host is not required to send
  Unload on its way out.

### Changed

- `ofxprobe` grows `--size WxH` and `--out FILE.bmp` (input and output side by side).

## [0.5.1] — 2026-07-31

### Changed

- **Paths are shown with `~` for the home directory**, as the rest of macOS
  does — in both fields and in the app's own log lines. The window is read over
  shoulders, screenshotted and filmed; the account name does not need to be in
  any of that. Typing a `~` path by hand works too.

## [0.5.0] — 2026-07-31

### Added

- **OFX Bridge.app** — a window around the generator, for people who would
  rather not open a terminal. Choose a folder of OFX plugins or a single
  `.ofx.bundle`, choose where to install, press Start; a progress bar tracks the
  run and the log is the generator's own output, colour-coded. Cancel stops
  between plugins, and Reveal in Finder opens the result.
- **"Add to" buttons** fill in the `Extra Effects` folder for whichever Resolume
  products are installed. Only Arena and Avenue are known to scan one; Alley and
  Wire get a button and a caveat in the log rather than a silent no-op.
- **`ofxgen generate --bundle PATH`** generates from one OFX bundle instead of a
  whole directory — what the app's single-plugin selection uses.
- **Quarantine is now cleared as part of generation** on macOS, by both the app
  and the CLI. A quarantined plugin is skipped by Resolume silently, so this was
  a manual `xattr -dr` step people had no reason to know about.

### Changed

- Scanning and copying moved out of the `ofxgen` CLI into `source/gen/Generator`,
  so the app and the CLI produce bundles by the same code rather than by two
  implementations that agree today. `ofxgen generate` now also prints the scan
  log it previously discarded.
- A plugin whose bundle cannot be copied is now counted as skipped and the run
  continues, instead of aborting everything after it.

## [0.4.0] — 2026-07-31

### Added

- **OFX OpenCL render path (buffer variant), verified.** GL and OpenCL share a
  pixel buffer object — `clCreateFromGLBuffer` takes a GL buffer, not a texture,
  so a PBO stands in for Metal's IOSurface. No pixel crosses to the CPU.
  **4.2× faster than the CPU path at 1080p** (2.22 ms → 0.53 ms), 2.8× at 4K.
  Metal still wins when a plugin offers both, since OpenCL on macOS is a
  deprecated compatibility layer over Metal.
- **An OpenCL OFX test plugin** (`testplugins/opencl-gain/`), GPU-only, for the
  same reason as the Metal one: no such plugin is publicly available.
- **CUDA detection and render action** — see the caveat below.

### Unverified

**The CUDA path has never been compiled or run.** It needs an NVIDIA GPU, which
macOS has not supported since 10.13 and Apple Silicon has never had. Detection
works and `Effect::renderCuda` is written from the OFX specification, but the
GL↔CUDA interop is deliberately not written blind, and the host does **not**
advertise CUDA support — a CUDA plugin is declined cleanly rather than failing
confusingly mid-render. Build with `-DOFXBRIDGE_ENABLE_CUDA` to opt in on
hardware where it can actually be tested.

Everything else in this project is proven against a real plugin. This is the one
exception, and it is marked as such in the probe output, the headers, the code
and the docs.

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

[0.5.1]: https://github.com/stoatworks-labs/resolume-ofx-bridge/releases/tag/v0.5.1
[0.5.0]: https://github.com/stoatworks-labs/resolume-ofx-bridge/releases/tag/v0.5.0
[0.4.0]: https://github.com/stoatworks-labs/resolume-ofx-bridge/releases/tag/v0.4.0
[0.3.1]: https://github.com/stoatworks-labs/resolume-ofx-bridge/releases/tag/v0.3.1
[0.3.0]: https://github.com/stoatworks-labs/resolume-ofx-bridge/releases/tag/v0.3.0
[0.2.0]: https://github.com/stoatworks-labs/resolume-ofx-bridge/releases/tag/v0.2.0
[0.1.0]: https://github.com/stoatworks-labs/resolume-ofx-bridge/releases/tag/v0.1.0
