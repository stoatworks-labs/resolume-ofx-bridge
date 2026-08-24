# Notes

Working notes for this repo: status, decisions, and the traps that have actually bitten.
Migrated out of Claude Code's memory on 2026-08-24, so they are written in the first
person and dated by when each thing was learned — that date is usually the useful part.

Cross-cutting notes that are not specific to this repo live in
[fleet-notes](https://github.com/stoatworks-labs/fleet-notes).

*resolume-ofx-bridge — hosts OpenFX (Resolve) plugins in Resolume as generated per-plugin FFGL bundles; PUBLIC MIT; run in Resolume on real content since 2026-08-02; Windows became a run-and-self-tested platform 2026-08-18, GL render path there still unproven*

**resolume-ofx-bridge** (started 2026-07-31) — run OpenFX plugins, the format
DaVinci Resolve uses, inside Resolume Arena as native FFGL effects.
`~/Projects/resolume-ofx-bridge`, **PUBLIC MIT**, **v0.5.1 released 2026-07-31**
(at the time macOS universal only; Windows became a first-class platform on
2026-08-18, see below).
Video: `https://www.youtube.com/watch?v=-ruvWTiDpa4` (linked from the README *and*
the website's projects.json).

**Architecture decision that shapes everything:** it is a *generator*, not one
dynamic plugin with a plugin dropdown. FFGL 2.2 can change a param's display
name/visibility/dropdown elements at runtime, but `FFGL.h:449` lists range and
default-value change events as **not supported**, and type is fixed at load. A
dropdown wrapper would collapse every OFX param to an anonymous 0–1 slider.

**The trick that makes the generator cheap:** `g_CurrPluginInfo` is a global
pointer read lazily, so ONE prebuilt FFGL binary is *copied* per plugin and reads
a sidecar JSON manifest at dynamic-init time. **End users need no compiler.**

**There is a GUI as of 2026-07-31: `OFX Bridge.app`** (`source/ui/`, plain Cocoa
Obj-C++ in the same CMake build, no new toolchain). Scan-and-copy lives in
`source/gen/Generator.{h,cpp}` so the app and the `ofxgen` CLI generate through
one implementation — put anything that changes bundle *contents* there, not in
either front end. Its "Add to" buttons target `~/Documents/Resolume <product>/
Extra Effects`: **only Arena and Avenue actually scan that folder** (the string
is in the Arena binary, absent from Alley's and Wire's, though all three link the
same FFGL engine), so Alley/Wire buttons carry a caveat.

The app **displays paths tilde-abbreviated** (`stringByAbbreviatingWithTildeInPath`)
and expands them before use. That is a privacy feature, not cosmetics: the window
is screenshotted for the README and filmed for YouTube. **`HOME=` does not change
what it renders** — NSHomeDirectory ignores the environment for a normal .app, so
the abbreviation is the only lever.

Built on `openfx/HostSupport` (BSD-3, the OFX project's own host library, what
Natron uses) — never hand-roll the suites.

**GPU: Metal AND OpenGL render paths are both done.** Metal is the one that
matters (Resolve plugins on macOS use it): 2.14→0.40ms at 1080p, 3.20→0.90ms at
4K, and crucially it makes **GPU-only plugins work at all** — they previously
refused to render. The interop is **IOSurface**: one surface backs both a GL
texture (`CGLTexImageIOSurface2D`, RECTANGLE target only) and an `MTLBuffer`
(`newBufferWithBytesNoCopy`), so zero copies — two on-GPU blits per frame.
IOSurface pads rows (rowBytes != w*4) and gives coherency but NOT ordering.
`testplugins/metal-gain/` is **ours** — no Metal OFX plugin exists publicly to
test against. **OpenCL** also done and verified (0.53ms@1080p): shared object is a **PBO**, not
IOSurface — `clCreateFromGLBuffer` takes a GL buffer not a texture; CL context
must come from the CGL share group or you get CL_INVALID_CONTEXT. Metal wins when
a plugin offers both. **CUDA is written but NEVER COMPILED OR RUN** (no NVIDIA
hardware; macOS dropped it after 10.13) — the host deliberately does NOT advertise
CUDA support unless built with `OFXBRIDGE_ENABLE_CUDA`, so CUDA plugins are
declined cleanly. Don't "fix" that by advertising it.

**Hard limit found:** an OFX GL-render plugin must use *core-profile* GL to work
in Resolume (FFGL 2.x = `#version 410 core`; macOS has no compat profile above
2.1). Immediate-mode GL plugins cannot work there at all — no host-side fix.

**CI cannot verify the GPU paths** — hosted macOS runners are `Apple Software
Renderer` with no GPU. A Metal CI run once "passed" on an entirely black frame
because the check only read the exit code; `ffgltest --expect-centre` now asserts
pixels and `--require-gpu` exits 3 to skip honestly. **All GPU numbers come from
the one M4 Max**, not CI.

Verified (and re-run in CI on every release): probe/introspection, CPU render
(Invert inverts, Gain halves at scale=0.5), generated bundles load via dlopen, and
the full GL path through an offscreen CGL context. Texture orientation needs no
flip — OFX bottom-up and FFGL bottom-left agree. **Generated bundles have been
run inside Resolume itself on real content** as of 2026-08-02 (Allan's own
report), so its real texture sizes, premultiplication and resize behaviour are no
longer the open question they were. Windows/Linux/CUDA remain uncompiled, and
commercial plugins still refuse on host name.

**Nothing OFX is installed on this machine** — Resolve compiles ResolveFX into
the app, so `/Library/OFX/Plugins` is empty. `scripts/build-test-plugins.sh`
builds a test corpus from the OpenFX examples.

Host identifies itself honestly as `com.stoatworks.ofxbridge`; commercial
plugins that gate licences on host name will refuse to load, and **host-name
spoofing is deliberately not implemented** (that would be circumvention).

Traps are catalogued in the repo's AGENTS.md — see [agents md convention](https://github.com/stoatworks-labs/fleet-notes/blob/main/notes/reference_agents_md_convention.md).
Related: [resolume luma keyer](https://github.com/stoatworks-labs/resolume-luma-keyer/blob/main/docs/NOTES.md) (`resolume-luma-keyer`) (the FFGL SDK submodule pattern came from there).

**Param flow changed 2026-08-03 (post-v0.6.0, unreleased):** the FFGL layer
pushes only *dirty* values per frame and follows them with
kOfxActionInstanceChanged as user edits (first push silent except values off
their defaults); `Effect::paramChangedByPlugin` flows plugin-initiated writes
back into the FFGL value table + FF_EVENT_FLAG_VALUE. Do NOT revert to
push-everything-per-frame — it clobbers what a plugin sets on itself (the
fleet's preset dropdowns). `ofxprobe --edit` vs `--set`, comma lists for RGB:
see [plugin factory presets](https://github.com/stoatworks-labs/fleet-notes/blob/main/notes/reference_plugin_factory_presets.md).

**WINDOWS IS A REAL PLATFORM as of 2026-08-18 (post-v0.9.0, unreleased).** The
whole toolset — `ofxwrapper.dll`, `ofxgen`, `ofxprobe`, `ffgltest`, the shell,
the AE guest — builds and **self-tests** on Windows x64, in CI and on the ARM64
Parallels guest. CPU render numbers match macOS digit for digit. Two things had
made Windows impossible and **both failed silently**: wrapped bundles wrote
their binary to `Contents/MacOS` on every platform (only a macOS host falls back
there, so Windows/Linux bundles were *invisible*, not broken), and the shell was
built with vcpkg's default dynamic triplet so it imported a **glew32.dll**
nobody has — everything is `x64-windows-static-md` now and CI asserts it with
`dumpbin /dependents`. **The GL render path on Windows is still NOT run** — the
one Windows machine here is headless in session 0 where WGL has no desktop, and
hosted runners have no GL 4.1 core driver; `ffgltest` exits 3 saying which.
There is **no Windows GUI** and none is planned (Cocoa). Traps are all in the
repo's AGENTS.md under "Windows traps" — `pascal` is an MSVC keyword, compound
literals are a GNU extension, cargo builds for the *host* not the target.

**`ofxprobe --set-string name=value` added 2026-08-04** ([flipbook](https://github.com/stoatworks-labs/flipbook/blob/main/docs/NOTES.md) (`flipbook`)).
`Effect::setParamString` had existed all along; the probe simply never exposed
it, so a plugin whose picture depends on a **file** parameter could only be
render-tested with that file unset — it renders, it does not crash, and it draws
nothing, which is indistinguishable from a broken render path. String overrides
are applied *before* the numeric ones. Any future plugin with a file picker
(sheet, font, LUT) needs this.
