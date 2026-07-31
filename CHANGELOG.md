# Changelog

All notable changes to this project are documented here.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and
this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

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

[0.1.0]: https://github.com/stoatworks-labs/resolume-ofx-bridge/releases/tag/v0.1.0
