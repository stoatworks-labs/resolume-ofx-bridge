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

Assumed / not yet done:
- **Nothing has run inside Resolume.** The FFGL bundle has never been loaded by
  the real host, and `ProcessOpenGL` has never executed — there is no GL context
  in any test, so the readback/blit path is entirely unexercised.
- Premultiplication is asserted, not verified against Resolume's actual output.
- GPU render paths (Metal/CUDA/OpenCL) are not implemented; CPU only, which means
  a full texture round trip per frame.
- Windows and Linux are untried; `ofxgen verify` is macOS/Linux only.
- No generator GUI yet.
