# resolume-ofx-bridge — agent onboarding

Host OpenFX (OFX) plugins — the plugin format DaVinci Resolve uses — inside
Resolume Arena/Avenue, which speaks FreeFrameGL (FFGL).

## The shape of the problem

OFX and FFGL disagree about almost everything:

| | OFX | FFGL |
|---|---|---|
| pixels | CPU buffers (or CUDA/Metal/OpenCL via the Blackmagic extension) | OpenGL textures |
| params | typed, named, with real units and ranges | flat floats, mostly 0–1 |
| params known | at instance time, per plugin | **at plugin load, fixed** |
| discovery | host scans directories at runtime | one binary = one effect |

The last row is the load-bearing one — see the next section.

## Why this is a generator, not one dynamic plugin

The obvious design is a single "OFX Host" FFGL plugin with a dropdown listing
discovered plugins. That was evaluated and rejected on evidence:

FFGL 2.2 **can** change a param's display name, visibility, and dropdown
elements at runtime (`FF_EVENT_FLAG_DISPLAY_NAME` / `_VISIBILITY` / `_ELEMENTS`,
raised via `RaiseParamEvent`). But `external/ffgl/source/lib/ffgl/FFGL.h:449`
explicitly lists range and default-value change events as **not supported yet**,
and `dwType` is fixed once set.

So a dynamic wrapper would have to declare a fixed pool of pre-typed 0–1 slots
and relabel them. Every OFX param loses its real units, and a plugin with more
params of a type than the pool has slots simply cannot be represented.

Instead we generate **one FFGL bundle per OFX plugin**, with the parameter table
baked to match that plugin exactly.

### The generator needs no compiler

`g_CurrPluginInfo` (`FFGLPluginInfoData.cpp:13`) is a plain global pointer that
`getInfo()` reads lazily. So a *single prebuilt* FFGL binary can be copied per
plugin and configure itself from a sidecar JSON manifest read at load time.

This is the key architectural fact: the generator is a file-copier, not a build
system. Users do not need Xcode/MSVC.

## Layout

- `source/ofxbridge/` — the OFX host. Shared by the probe and the FFGL wrapper,
  deliberately so: the params a generated wrapper advertises are by construction
  the params the renderer will drive.
  - `Host.{h,cpp}` — host, effect instance, clips, images, suites
  - `Params.{h,cpp}` — concrete param instances (HostSupport leaves these abstract)
  - `Catalog.{h,cpp}` — discovery, describe, manifest JSON
- `source/ffgl/` — the generic FFGL wrapper (manifest, param mapping, GL bridge)
- `source/gen/` — `Generator.{h,cpp}` is the scan-and-copy step with no argv and
  no UI; `main.cpp` is the `ofxgen` CLI around it, including the bundle verifier.
  Anything that changes what a generated bundle *contains* belongs in the former,
  or the app and the CLI drift apart.
- `source/ui/` — `OFX Bridge.app`, a Cocoa window over `Generator`. AppKit only,
  built by the same CMake; it holds no generation logic of its own.
- `source/gltest/` — `ffgltest`, offscreen-GL harness for `ProcessOpenGL`
- `source/probe/` — `ofxprobe` CLI
- `external/openfx` — OFX headers + **HostSupport**, a BSD-3 host implementation
  maintained by the OFX project. Natron is built on it. Do not hand-roll suites.
- `external/ffgl` — Resolume's FFGL SDK

## Traps

- **Nothing OFX is installed on a clean machine.** Resolve compiles ResolveFX
  into the app; `/Library/OFX/Plugins` is empty and `OFXPluginCacheV2.xml` is a
  two-line stub. Run `scripts/build-test-plugins.sh` to get a real corpus.
- **Don't build openfx via its own CMake.** It resolves EXPAT as Conan's
  `expat::expat`, which system `FindEXPAT` doesn't define, so generate fails on a
  plain checkout. Our `CMakeLists.txt` compiles HostSupport from source; the test
  script compiles the Support library directly.
- **`OFX::Host::ImageEffect::Texture` and `flushOpenGLResources` don't exist**
  unless `OFX_SUPPORTS_OPENGLRENDER` is defined. They are not overridable now.
- **Use `getParamList()`, not `getParams()`**, when order matters — the latter is
  a `std::map` and loses declaration order, which is the UI order.
- `CMAKE_OSX_ARCHITECTURES` must be set before `project()` or a "universal" build
  silently ships single-arch. Verify artefacts with `lipo`, not the build log.

### HostSupport traps (each of these cost real debugging time)

- **`Instance::getClipPreferences()` returns `bool`, not `OfxStatus`.** Every
  neighbouring action returns a status, so comparing it against `kOfxStatOK`
  looks right and turns success (`true` == 1) into an error.
- **`ImageEffectPlugin::createInstance()` already calls `populate()`**
  (`ofxhImageEffectAPI.cpp:251`). Calling it again fails on duplicate parameter
  names — and only for plugins that *have* parameters, so a no-param plugin will
  happily hide the bug.
- **`PluginCache::addFileToPath()` takes a directory, not a bundle.** Passing a
  `.ofx.bundle` path finds nothing, with no error.
- HostSupport declares every `Param::*Instance` abstract; the host supplies the
  storage. See `Params.{h,cpp}`.
- **`CFFGLPlugin::InitGL` dereferences its viewport argument unconditionally**
  (`FFGLPluginSDK.h:59`, `currentViewport = *vp`). Passing null traps.

### OpenGL render path

- **HostSupport's `renderAction` cannot set `kOfxImageEffectPropOpenGLEnabled`.**
  `Effect::renderGL` reissues the action itself, mirroring
  `Instance::renderAction` (`ofxhImageEffect.cpp:918`) with that property added.
  Without it a GL-capable plugin takes its CPU branch, or refuses outright.
- **The OFX GL contract is "draw into whatever is bound".** The plugin fetches
  the output texture id for reference but renders into the current framebuffer,
  so the host must bind it and set the viewport before the render action.
- **Immediate-mode GL plugins cannot work in Resolume.** Resolume uses a core
  profile (FFGL 2.x shaders are `#version 410 core`) and macOS has no
  compatibility profile above 2.1. `ffgltest --legacy-gl` exists only to test
  such plugins; it is not how Resolume runs.

### Metal render path

- **`CGLTexImageIOSurface2D` accepts only `GL_TEXTURE_RECTANGLE`**, never
  `GL_TEXTURE_2D`. macOS constraint, not a choice.
- **IOSurface pads rows**, so `rowBytes != width * 4`. A kernel assuming
  otherwise shears the image.
- **IOSurface gives coherency, not ordering.** `glFlush` after the GL blit before
  Metal reads; wait on the Metal queue before GL reads the output. The OFX
  contract explicitly lets a plugin return before its work completes.
- **`newBufferWithBytesNoCopy` over `IOSurfaceGetBaseAddress`** is what makes this
  zero-copy. Base addresses are page-aligned, which that call requires.
- `MetalBridge.mm` needs `-fobjc-arc`, or the Metal objects it holds are not
  retained.

### OpenCL and CUDA

- **`clCreateFromGLBuffer` takes a GL buffer, not a texture**, so the shared
  object is a PBO rather than an IOSurface. `glReadPixels` into a pixel-pack
  buffer stays on the GPU.
- **The CL context must come from the CGL share group**
  (`CL_CONTEXT_PROPERTY_USE_CGL_SHAREGROUP_APPLE`) or `clCreateFromGLBuffer`
  returns `CL_INVALID_CONTEXT`.
- Ownership is explicit: `clEnqueueAcquireGLObjects` / `ReleaseGLObjects`, plus
  `clFinish` before GL reads the output.
- **CUDA is UNVERIFIED** — never compiled, never run, no NVIDIA hardware. The
  host deliberately does not advertise `kOfxImageEffectPropCudaRenderSupported`
  unless built with `OFXBRIDGE_ENABLE_CUDA`, so CUDA plugins are declined cleanly
  instead of failing mid-render. Do not "fix" that by advertising it.

### Known limitation

`createEffect` loads every OFX bundle in the target bundle's directory, because
`addFileToPath` is directory-scoped. In a live video process that is wasteful and
lets an unrelated broken plugin misbehave mid-show. A proper fix needs a
bundle-scoped load path in HostSupport.

### App traps

- **`PluginCache::addFileToPath()` is directory-scoped**, so "generate from this
  one plugin" cannot be done by pointing the scanner at a `.ofx.bundle`. The
  scanner takes the parent directory and `Options::onlyBundlePath` filters the
  results — which means selecting one plugin still loads every plugin beside it.
- **Quarantine is inherited from the writing process**, not copied from the
  source file: a downloaded, still-quarantined `OFX Bridge.app` marks everything
  it writes, and Resolume then skips those plugins silently. The generator clears
  the attribute unconditionally, because that case cannot be reproduced from a
  local build.
- **Paths are displayed `~`-abbreviated and expanded before use.** The window ends
  up in screenshots and in a published video, and the account name does not
  belong in either. Note that `HOME=` does not help: `NSHomeDirectory()` ignores
  the environment for a normal .app, which was measured, so abbreviating on
  display is the only lever there is.
- **Only Arena and Avenue scan `Extra Effects`.** `strings` finds that path in
  the Arena binary and not in Alley's or Wire's, even though all three link the
  same FFGL engine — so buttons for those two are offered with a caveat, not as
  a promise.

### FFGL traps

- **`FFInstanceID` is `void*`, not a 32-bit id.** Declaring `plugMain`'s third
  parameter as `FFUInt32` corrupts the call frame on 64-bit.
- **`FF_FAIL` is `0xFFFFFFFF` returned in the `FFMixed` union's integer member.**
  Reading that back as `PointerValue` gives a non-null pointer that faults on
  dereference. Filter every pointer result.
- **`FF_GET_PARAM_GROUP` takes a `GetStringStruct*`, not an index** — the caller
  supplies the buffer, and the plugin does not nul-terminate.
- **`FF_EFFECT` is 0 and `FF_SOURCE` is 1**, which is the opposite of the guess.
- **`SetOptionParamInfo()` does not set a range.** Without a following
  `SetParamRange()`, a choice param reports 0..1 no matter how many options.

## Host identification

The host reports itself honestly as `com.stoatworks.ofxbridge`. Some commercial
OFX plugins gate their licence on host name and will refuse to load. **Do not add
host-name spoofing** — that is licence circumvention, not compatibility work.

## Verified vs assumed

Verified on this machine (macOS arm64), against four OFX plugins built from the
OpenFX examples:

- `ofxprobe` extracts plugin metadata, param types, labels, hints, group
  parenting, display ranges and choice options; manifest JSON round-trips
  through `json.load`. A General-context-only plugin is correctly declined.
- **CPU rendering works.** `ofxprobe --render` pushes a frame through a real
  plugin: Invert produces exact complements; Gain with `--set scale=0.5` halves
  every channel (128 -> 64) and with `scale=2` saturates; ChoiceParams zeroes red
  and green at its default options.
- **Parameters reach the plugin**, via the same `Effect::setParamValue` path the
  FFGL wrapper uses.
- `ofxgen generate` emits loadable FFGL bundles, and `ofxgen verify` dlopens one
  exactly as a host would: correct effect type, deterministic 4-char id, and the
  expected parameter table (group and page params dropped, ranges preserved,
  children grouped under the OFX group's label, choice elements in plugin order).

- **Texture orientation needs no correction.** OFX images are bottom-up and FFGL
  textures are bottom-left-origin, so they agree and the code applies no flip.
  Confirmed empirically: in the demo images the grey wedge stays along the bottom
  edge on both sides.
- **The full GL path works.** `ffgltest` creates an offscreen CGL context
  (GL 4.1 core, as Resolume uses), hands the wrapper a texture, and calls
  `FF_INSTANTIATE_GL` / `FF_SET_PARAMETER` / `FF_PROCESS_OPENGL` exactly as a
  host would. Invert inverts; Gain is identity by default and halves every
  channel at `0=0.5`. That covers texture readback, OFX render, upload and blit
  into the host FBO.

- **The app generates what the CLI does.** Driven end to end on this machine
  against `build/test-plugins`: 6 generated, 1 skipped — the same result as
  `ofxgen generate` — and a bundle it wrote passes `ofxgen verify` with the
  expected parameter table.

Assumed / not yet done:
- **The app's quarantine clearing is unproven in the case that matters.** A
  generated bundle carries no `com.apple.quarantine`, including when the wrapper
  template has one, but the real path — a downloaded app passing the flag to
  files it writes — needs a signed-and-downloaded build to test.
- **Nothing has run inside Resolume itself.** Every GL test uses our own
  offscreen context, so Resolume's actual texture orientation, premultiplication
  and resize behaviour are still unconfirmed.
- Premultiplication is asserted, not verified against Resolume's actual output.
- GPU render paths (Metal/CUDA/OpenCL) are not implemented; CPU only, which means
  a full texture round trip per frame.
- Windows and Linux are untried; `ofxgen verify` is macOS/Linux only.
- CUDA is written but never compiled or run (no NVIDIA hardware).
- No generator GUI yet.

## Parameter edits are actions, not just values

Since the preset work (2026-08-03) the host delivers `kOfxActionInstanceChanged`
the way a real UI does, and that changed the parameter flow in three ways worth
knowing before touching `applyParams()`:

- **Only changed values cross per frame.** The FFGL layer keeps a dirty flag per
  slot; pushing every value every frame (the old behaviour) would overwrite
  values the plugin set on itself — a preset choice filling in the sliders is
  exactly that. After the push, each changed param gets `instanceChanged` with
  `kOfxChangeUserEdited`, bracketed by begin/end.
- **The first push is setup, not an edit** — except for values that already
  differ from their declared defaults, which happens when the operator (or
  `ffgltest`) set a param before the first frame rendered. Those are delivered
  as edits, or a preset picked before playback would be silently swallowed.
- **Plugin-initiated changes flow back.** HostSupport routes a plugin's own
  `paramSetValue` to `Effect::paramChangedByPlugin`, which the FFGL layer hooks
  to refresh its copy (so the per-frame push does not undo it) and raise
  `FF_EVENT_FLAG_VALUE` so Resolume re-reads the control. A host that ignores
  the event still renders correctly and merely shows a stale knob.

`ofxprobe` grew `--edit name=value` alongside `--set`: `--set` writes the value
store silently (what project load looks like), `--edit` delivers a user edit
with the action (what the inspector looks like). Behaviour a plugin hangs off
`changedParam` — presets — only runs under `--edit`. Both accept comma lists
for multi-component params (`--set colour=1,0.72,0.2`).
