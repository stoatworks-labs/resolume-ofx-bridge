# GPU acceleration

## Measure first

Before building anything, here is what a frame actually costs. M4 Max, the
OpenFX Gain example, pipelined (`ffgltest --bench`):

| | per frame | of a 60fps budget |
|---|---|---|
| 1080p | 2.25 ms | 13% |
| 4K | 2.94 ms | 18% |

Per stage, serialised with `glFinish` (`OFXBRIDGE_TIMING=1`), the transfer costs
about **2.2 ms at both resolutions**. It barely grows when the pixel count goes
up 4×, which means it is bound by synchronisation latency, not bandwidth. That is
unified memory doing the work — the same code on a discrete GPU, crossing PCIe
twice per frame, would look far worse.

So on Apple Silicon the CPU round trip is **not** the main cost. Two things are:

1. **The plugin's own render.** Gain is a multiply, so it is 0.5–2.5 ms. A real
   blur, denoise or optical-flow effect on the CPU is 10–100 ms and blows the
   frame budget by itself. No amount of transfer optimisation touches that.
2. **GPU-only plugins don't work at all.** Plugins targeting Resolve commonly
   implement Metal or CUDA and skip CPU render entirely. Those don't render
   slowly today; they fail. This is a compatibility argument, and it is the
   stronger of the two.

## The three OFX GPU paths

`ofxGPURender.h` defines three, plus the older OpenGL render path:

| Path | How images arrive | Who uses it |
|---|---|---|
| OpenGL render | GL texture id + target | Nuke-oriented plugins; **not** Resolve |
| Metal | `id<MTLBuffer>` — linear memory, not a texture | Resolve on macOS |
| CUDA | device pointer | Resolve on Windows/Linux, NVIDIA |
| OpenCL | `cl_mem` | Resolve on Windows/AMD |

## What is implemented: OpenGL render

This is the path that fits FFGL naturally, because we already hold a GL context
and the plugin can read our texture directly.

HostSupport already implements the host half — `clipLoadTexture`, the `Texture`
class, the context-attached actions — behind `OFX_SUPPORTS_OPENGLRENDER`. That is
now enabled, and `source/ofxbridge/Host.cpp` supplies:

- `kOfxImageEffectPropOpenGLRenderSupported = "true"` on the host
- `Clip::loadTexture`, serving a `Texture` carrying the GL texture name and target
- `Effect::renderGL`, which issues the render action with
  `kOfxImageEffectPropOpenGLEnabled` set

That last one is necessary because HostSupport's `renderAction` has no way to set
it, and without it a GL-capable plugin takes its CPU branch — or refuses
outright, as the OpenFX example does.

Negotiation is per plugin: `ofxprobe` records the plugin's
`OpenGLRenderSupported` into the manifest, and the wrapper takes the GL path only
when both sides agree. On that path it allocates no CPU frames at all, saving
66 MB per instance at 4K.

The host is kept free of GL headers — the texture crosses as plain int
properties — so `ofxbridge` still links into `ofxprobe`, which runs headless.

### Verified

`ffgltest build/generated/OFX_OpenGL_Example.bundle --legacy-gl` renders real
output through the GL path with no CPU round trip: the plugin negotiates GL
render, `clipLoadTexture` returns valid texture ids for both the source and
output clips, and the plugin's drawing appears in the host framebuffer.

## The limitation you need to know about

**The OpenFX OpenGL example draws with immediate mode** — `glBegin`,
`glVertex2f`, `glPushAttrib`. That is illegal in an OpenGL core profile, and
macOS offers no compatibility profile above 2.1. So:

- In a **4.1 core** context (what Resolume uses on macOS, since FFGL 2.x shaders
  are `#version 410 core`), the example produces `GL_INVALID_OPERATION` and a
  black frame.
- In a **legacy 2.1** context, it renders correctly.

Our code is the same in both cases; the difference is entirely the profile. The
practical consequence is real:

> An OFX plugin using the OpenGL render path can only work inside Resolume if it
> draws with **core-profile** GL. An immediate-mode plugin cannot work there at
> all, and no host-side change can fix that.

`--legacy-gl` exists purely to test the plumbing against the one available
GL-render plugin. It is not how Resolume runs, and there is currently **no
core-profile OFX GL plugin to test against** — writing one is the honest way to
close that gap.

There is also a residual `GL_INVALID_OPERATION` in the legacy path, most likely
our own FBO calls wanting the `EXT` entry points under 2.1. It does not affect
the core-profile path, which is the one that ships.

## Why this path may still not help much

Resolve does not use OFX OpenGL render, so plugins written for Resolve — the
stated audience — generally do not implement it. Combined with the core-profile
constraint above, the set of plugins this accelerates today is narrow.

It was still worth building: the negotiation, manifest plumbing, per-path clip
handling and context lifecycle are all reused by Metal, which is the path that
actually matters for Resolve plugins on macOS.

## Next: Metal

Metal passes `id<MTLBuffer>` — linear memory, not a texture — so the chain is:

```
Resolume GL texture
  -> our IOSurface-backed GL texture   (on-GPU copy)
  -> MTLTexture over the same IOSurface (zero copy)
  -> MTLBuffer                          (blit encoder)
  -> plugin renders
  -> back the same way
```

All on-GPU. The hard parts are GL/Metal synchronisation on the legacy macOS GL
driver, and that HostSupport gives no help at all — it declares the Metal
properties and implements none of the plumbing.

**There is no Metal-capable OFX plugin to test against**, since the OpenFX
examples are all CPU or GL. Writing a minimal one is the first step, both to
de-risk the interop and to serve as the permanent test subject — exactly the role
the OpenGL example plays now.
