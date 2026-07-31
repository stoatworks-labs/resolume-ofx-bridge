# Architecture

## The mismatch

OFX and FFGL were designed for different worlds, and they disagree about nearly
everything that matters here.

| | OFX | FFGL |
|---|---|---|
| Pixels | CPU buffers (or CUDA/Metal/OpenCL via Blackmagic's extension) | OpenGL textures |
| Parameters | typed and named, with real units and ranges | scalars, mostly 0–1 |
| When parameters are known | at instance time, per plugin | **when the plugin loads, and fixed thereafter** |
| Discovery | the host scans directories at runtime | one binary is one effect |

The third row is the one that decides the design.

## Why a generator rather than one dynamic plugin

The intuitive design is a single "OFX Host" FFGL effect with a dropdown listing
whatever plugins you have installed, reconfiguring itself when you pick one. That
was evaluated first and rejected on evidence.

FFGL 2.2 genuinely does support some runtime change. A plugin can raise
`FF_EVENT_FLAG_DISPLAY_NAME`, `FF_EVENT_FLAG_VISIBILITY` and
`FF_EVENT_FLAG_ELEMENTS` to tell a stateful host that a parameter's label,
visibility or dropdown options have changed.

But `external/ffgl/source/lib/ffgl/FFGL.h` lists exactly what is *not* supported,
in a comment next to those flags:

```c
//Not supported yet, but possibly in the future we would like these events as well:
//static const FFUInt64 FF_EVENT_FLAG_DEFAULT_VALUE = 0x??;
//static const FFUInt64 FF_EVENT_FLAG_RANGE         = 0x??;
```

A parameter's **type** is likewise set once, in `SetParamInfo`, and never
revisited.

So a dropdown wrapper would have to declare a fixed pool of pre-typed slots —
say 24 floats, 8 integers, 8 booleans — normalised to 0–1, and rename them when
you pick a plugin. Two consequences follow, and neither is acceptable:

1. Every parameter loses its real units. A blur radius in pixels, an angle in
   degrees and a gain in stops all become "0.00 – 1.00".
2. Any plugin with more parameters of a type than the pool has slots simply
   cannot be represented.

Generating one bundle per plugin sidesteps both. Each bundle declares exactly the
parameters its plugin has, with the types and ranges the plugin's author
declared. As a bonus, each OFX plugin appears in Resolume's effect browser under
its own name, which is how you would actually want to find it.

## How a copied binary knows which plugin it is

The generator does not compile anything. It copies one prebuilt binary per
plugin and writes a JSON manifest beside it.

That works because of how the FFGL SDK registers a plugin.
`FFGLPluginInfoData.cpp` declares a plain global pointer:

```cpp
CFFGLPluginInfo* g_CurrPluginInfo = NULL;
```

which `getInfo()` dereferences lazily, when the host first asks. The pointer is
set by the constructor of a global `CFFGLPluginInfo` object — and **the arguments
to that constructor are evaluated during dynamic initialisation**, after the
loader has mapped the binary but before the host has called anything.

That is a wide enough window to do real work. `source/ffgl/PluginMain.cpp` uses it
to locate its own file and read the manifest:

```cpp
const std::string gPluginId   = PluginContext::get().pluginId;
const std::string gPluginName = shortName();

static CFFGLPluginInfo PluginInfo(
    PluginFactory< OfxFFGLPlugin >,
    gPluginId.c_str(),
    gPluginName.c_str(),
    ... );
```

"Its own file" means `dladdr` on the address of a function known to live in this
binary — not `argv[0]`, which would give the host executable. See
`source/ffgl/SelfPath.cpp`.

On macOS the manifest lives at `Contents/Resources/manifest.json` inside the
bundle; on Windows and Linux, where FFGL plugins are bare shared libraries, it is
a sidecar file with the same basename.

The practical payoff: **end users need no toolchain.** The generator is a
file-copier.

## The pixel path

FFGL hands the plugin an OpenGL texture and expects it to render into the host's
framebuffer. OFX wants a CPU buffer. So each frame:

1. Attach the input texture to an FBO and `glReadPixels` it into a CPU frame.
2. Push current parameter values into the OFX instance.
3. Run the OFX render action.
4. Upload the result to a texture, and `glBlitFramebuffer` it into the host FBO.

This is a full round trip per frame, which is the honest cost of hosting a CPU
image API inside a GPU one. It is comfortable at 1080p and will not be at 4K.

Two details worth knowing:

- **Blit, not a shader.** Nothing here needs to sample or transform, so a
  framebuffer blit avoids a shader, a quad, and the vertex plumbing that goes
  with them — along with several ways to get texture coordinates subtly wrong.
- **Orientation lines up by luck of convention, not by correction.** OFX images
  are bottom-up, and FFGL textures are bottom-left-origin by default, so no flip
  is applied anywhere. The demo images in the README confirm this empirically:
  the grey wedge stays along the bottom edge on both sides.

The effect instance is rebuilt on resize rather than resized in place, because
clip preferences are negotiated against a specific image size.

## Contexts

Only the OFX **Filter** context is hosted. Filter means "one input image, one
output image", which is precisely what an effect slot in a Resolume clip is.

General, Generator, Transition, Paint and Retimer either need inputs Resolume
cannot supply through FFGL's effect interface, or imply a timeline model that
does not exist here. Plugins offering only those contexts are reported and
skipped rather than loaded and half-worked.

## Layering

```
source/ofxbridge/   the OFX host        (no OpenGL — runs headless)
source/ffgl/        the FFGL wrapper    (manifest, param mapping, GL bridge)
source/gen/         ofxgen              (generate + verify)
source/probe/       ofxprobe            (introspect + CPU render test)
source/gltest/      ffgltest            (offscreen-GL harness)
```

The host is deliberately shared between the probe and the wrapper. Because the
generator describes a plugin with the same code the wrapper later renders with,
the parameters a bundle advertises are by construction the parameters the
renderer will drive — they cannot drift apart.

The host itself is built on `openfx/HostSupport`, the BSD-3 host implementation
maintained by the OpenFX project (and the basis of Natron). Its property,
parameter, clip and plugin-cache machinery is the bulk of an OFX host, and
hand-rolling it would have been both slow and wrong. What it leaves to the host —
concrete parameter *storage* — is `source/ofxbridge/Params.cpp`.

## Host identity

The host reports itself as `com.stoatworks.ofxbridge`.

Some commercial OFX plugins check the host name and refuse to run in a host they
do not recognise. Impersonating Resolve or Nuke would defeat those checks; that
is licence circumvention rather than compatibility work, and it is deliberately
not implemented. Plugins that decline to load are reported as unusable.
