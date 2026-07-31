# Verification

What is actually tested, how to reproduce it, and — just as importantly — what is
not tested.

This file is the authority on the project's status. If it disagrees with the
README, believe this file.

## The test corpus

There is no OFX plugin on a clean machine, even with Resolve installed: Resolve
compiles its own ResolveFX into the application rather than shipping loadable
bundles, so `/Library/OFX/Plugins` is empty and Resolve's own
`OFXPluginCacheV2.xml` is a two-line stub.

So the corpus is built from the OpenFX project's example plugins:

```bash
./scripts/build-test-plugins.sh
```

| Plugin | Why it is in the corpus |
|---|---|
| `Invert` | the minimal filter — no parameters at all |
| `Basic` (Gain) | doubles with display ranges, a boolean, a group, a page |
| `ChoiceParams` | choice parameters with named options |
| `Custom` | General context only — exercises the "cannot host this" path |

## Level 1 — introspection

```bash
./build/ofxprobe --dir build/test-plugins
```

Verified: plugin metadata, parameter types, labels, hints, group parenting,
display ranges, and choice options are all extracted correctly; the manifest JSON
round-trips through Python's `json.load`; and the General-only plugin is
correctly reported as unusable rather than half-loaded.

## Level 2 — CPU render

```bash
./build/ofxprobe --dir build/test-plugins --render uk.co.thefoundry.OfxInvertExample
./build/ofxprobe --dir build/test-plugins --render uk.co.thefoundry.BasicGainPlugin --set scale=0.5
```

This instantiates the plugin and pushes one frame through it with no OpenGL
involved, so a render failure can be attributed without a GPU or a host.

Verified:

- Invert produces exact complements — `0,0,128,255` → `255,255,127,0`.
- Gain at `scale=0.5` halves every channel: `128,128,128,255` → `64,64,64,127`.
- Gain at `scale=2` saturates, and at its default of 1 is bit-exact identity
  (0 of 8192 bytes differ), which is a useful null test.
- ChoiceParams at its default options zeroes red and green and leaves blue and
  alpha alone.

`--set` drives parameters through `Effect::setParamValue` — the same path the
FFGL wrapper uses — so this covers the parameter plumbing as well as the render.

## Level 3 — bundle loading

```bash
./build/ofxgen generate --dir build/test-plugins --out build/generated
./build/ofxgen verify build/generated/OFX_Gain_Example.bundle
```

`verify` `dlopen`s a generated bundle and calls `plugMain` exactly as a host
would, so the whole load path — self-location, manifest parsing, parameter table
construction, FFGL registration — runs without Resolume.

Verified: correct effect type, a deterministic 4-character ID, and the expected
parameter table (group and page dropped, ranges preserved, children grouped under
the group's label, choice elements present and in the plugin's own order).

## Level 4 — OpenGL

```bash
./build/ffgltest build/generated/OFX_Invert_Example.bundle
./build/ffgltest build/generated/OFX_Gain_Example.bundle 0=0.5
```

`ffgltest` creates an offscreen CGL context at OpenGL 4.1 core — matching what
Resolume uses on macOS — uploads a texture, and calls `FF_INSTANTIATE_GL`,
`FF_SET_PARAMETER` and `FF_PROCESS_OPENGL` in the same sequence a host does.

This is the only way to exercise `ProcessOpenGL` at all, and it earned its keep
immediately: it found that `CFFGLPlugin::InitGL` dereferences its viewport
argument unconditionally, which the wrapper was passing as null.

Verified: texture readback, OFX render, upload and blit into the host FBO all
work, and parameters reach the plugin through the FFGL interface.

### Orientation

```bash
./build/ffgltest build/generated/OFX_Invert_Example.bundle --demo docs/demo-invert.png
```

The demo images are written straight out of the plugin's framebuffer. The grey
wedge along the bottom of the test pattern stays along the bottom on both sides,
which confirms empirically that no vertical flip is introduced: OFX's bottom-up
image convention and FFGL's bottom-left texture origin agree, so the code applies
no correction.

## What is *not* verified

- **Nothing has run inside Resolume.** Every GL test uses our own offscreen
  context. Resolume's real texture sizes (it may hand over a padded hardware
  texture larger than the image), its premultiplication behaviour, its resize
  behaviour and its parameter UI rendering are all unconfirmed.
- **Premultiplication is asserted, not measured.** The host tells plugins the
  input is premultiplied RGBA because that is what Resolume is documented to use.
  Nothing has checked it against real Resolume output.
- **No commercial plugin has been tried**, only the OpenFX examples. Real-world
  plugins are larger, more likely to be GPU-only, and more likely to refuse an
  unrecognised host.
- **No performance measurement.** The per-frame GPU→CPU→GPU round trip is
  expected to be the bottleneck, but nobody has profiled it.
- **Windows and Linux are untried.** The code paths exist; `ffgltest` is
  macOS-only, as it creates its context with CGL directly.
- **No sustained run.** Nothing has been left running long enough to surface a
  leak in the per-resize instance rebuild.

## Known limitation

`createEffect` loads every OFX bundle in the target bundle's directory, not just
the one it needs, because HostSupport's `addFileToPath` is directory-scoped. In a
live video process that is wasteful, and lets an unrelated broken plugin
misbehave during a show. Fixing it properly needs a bundle-scoped load path in
HostSupport.
