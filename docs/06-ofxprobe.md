# ofxprobe as a test host

`ofxprobe` started as the generator's introspection half: list what a bundle
contains, render one frame at time 0 on the CPU. It is also a command-line
OpenFX test host for checking ports of plugins to OFX: stills and image
sequences in, any time, the Transition and Generator contexts, keyframes,
push buttons, batches of renders in one instance, and a host personality that
imitates DaVinci Resolve's Fusion page.

None of this changes what the original flags do (see
[Compatibility](#compatibility)), and none of it reaches the wrapper or the app:
the knobs it sets are set only by `ofxprobe`, and their defaults reproduce what
the bridge always reported.

## Quick start

```sh
P=./build/ofxprobe
BUILD=/path/to/plugin/build      # the directory that CONTAINS Foo.ofx.bundle

# What does the bundle contain: identifiers, contexts, params?
$P --no-system-dirs --dir $BUILD

# Filter: one frame of an image, written alone and side by side
$P --no-system-dirs --dir $BUILD --render com.example.foo --in in.png \
   --set amount=0.7 --out-only out.ppm --out sbs.bmp

# Temporal: a sequence, rendered at frame 9
$P --no-system-dirs --dir $BUILD --render com.example.foo --seq 'frames/f%04d.ppm' --time 9 --out-only f9.ppm

# Generator
$P --no-system-dirs --dir $BUILD --render com.example.foo --context generator --size 1280x720 --time 30 --out-only g.png

# Transition, 0 at frame 0 rising to 1 at frame 24, rendered at frame 12
$P --no-system-dirs --dir $BUILD --render com.example.foo --context transition \
   --from a.ppm --to b.ppm --transition-ramp 0:24 --time 12 --out-only x.ppm

# The Fusion page's missing frame rate
$P --no-system-dirs --dir $BUILD --render com.example.foo --quirks fusion --time 12
```

**Pass `--no-system-dirs` when testing a build.** Without it the host also
scans `/Library/OFX/Plugins`, `~/Library/OFX/Plugins` and `$OFX_PLUGIN_PATH`,
and an installed copy of the plugin can be loaded alongside the build, or
instead of it.

## Flags

Flags marked "per render" can also go on a `--batch` line.

### Discovery and modes (process-wide)

| flag | meaning |
|---|---|
| `--dir PATH` | Adds PATH to the scan, recursively, looking for `*.ofx.bundle`. Repeatable. If PATH is itself a `.ofx.bundle`, its parent directory is scanned. |
| `--no-system-dirs` | Scans only the `--dir` paths: no `/Library/OFX/Plugins`, no `~/Library/OFX/Plugins`, no `OFX_PLUGIN_PATH` -- including the copy HostSupport's `PluginCache` constructor adds on its own. It sets `OFXBRIDGE_NO_SYSTEM_DIRS=1`, which the bridge's caches honour. |
| *(no mode flag)* | Lists every plugin found, with its contexts and params. Plugins unusable in the chosen `--context` (default Filter) are marked UNUSABLE. |
| `--json` | The manifest JSON for every usable plugin. |
| `--manifest ID` | The manifest JSON for one plugin. |
| `--quiet` | Hides the scan log in listing mode. |
| `--render ID` | Creates an instance and renders it. Everything below applies to this mode. |
| `--context C` | `filter` (default), `transition`, `generator` or `general`. The host advertises Filter plus C, and describes and instantiates the plugin in C. |
| `--frame-rate R` | The frame rate the effect and every clip report. Default **25**. (The wrapper and the app report 60.) |
| `--depth byte\|float` | Image depth. Default `byte`. See [Pixels](#pixels). |
| `--temporal 0\|1` | The host's `kOfxImageEffectPropTemporalClipAccess`. Default 1 if any `--seq*` is used, else 0, as in the bridge. Fetches at any time are answered either way. |
| `--range FIRST:LAST` | Overrides the timeline. See [Time](#time-sequences-and-temporal-access). |
| `--identity` | Calls `kOfxImageEffectActionIsIdentity` first. If the plugin says identity, the named clip's frame at the returned time is copied to the output and render is skipped. Off by default, as in the bridge. |
| `--frames-needed` | Prints what `getFramesNeeded` returns for each input at the render time. |
| `--strict-frames` | Fetches outside the declared frames-needed ranges (or outside the render time, if the plugin does not answer) get **no image**, plus a `STRICT:` line. Catches an under-declared `getFramesNeeded`. |
| `--allow-link-press` | `--press` normally refuses `stoatworksAboutLink*` buttons and anything in the `stoatworksAbout` group, because those open a browser. This allows them. Don't. |
| `--batch FILE` | One render per line, all in one instance. See [Batch](#batch). |
| `--quirks fusion` | Withholds frame-rate, frame-range and render-status properties the way DaVinci Resolve's Fusion page does, and more strictly. See [Quirks](#quirks---quirks-fusion). Applies to the whole process, so every `--batch` line gets it; it is not allowed on a batch line. `none` is accepted and means no quirks. |

### Per-render flags

| flag | meaning |
|---|---|
| `--set name=v[,v…]` | Sets a numeric param to a constant. Multi-component params take a comma list. An unknown name is a WARNING, not a failure. |
| `--set-string name=text` | Sets a string param. |
| `--edit name=v` | `--set`, then `kOfxActionInstanceChanged` (user edit) at the current time, so presets and other param-driven logic run. Numeric params only. Anything the plugin writes back is printed as `plugin set X = v`. |
| `--press NAME` | `kOfxActionInstanceChanged` (user edit) for a push button, at `--time`. Repeatable, applied in order. A non-button name gets a NOTE and is delivered anyway; an unknown name or a refused About link fails the run (exit 1). |
| `--key name=t:v,t:v…` | Linear keyframes for a numeric param, times in frames, components separated by `/`. Values hold flat outside the first and last key. Replaces any earlier keys or constant. `--key centre=0:0.2/0.5,24:0.8/0.5` |
| `--size WxH` | Output size. Default: the size of the first input file or sequence frame, else 64x32. Inputs of another size are resampled nearest-neighbour. A generator without `--size` renders 64x32 and prints a NOTE. |
| `--in FILE` | Still image for `Source` (Filter/General). |
| `--seq PATTERN` | printf-style image sequence for `Source`. |
| `--seq-first N` | First frame number of every sequence in this render. Default 0 if that file exists, else 1. |
| `--time T` | Render time in frames (fractions allowed). Default 0. Edits and presses are delivered at T too. |
| `--from FILE` / `--to FILE` | Transition `SourceFrom` / `SourceTo` stills. Default: a ramp / an 8px checker. |
| `--seq-from P` / `--seq-to P` | Transition inputs as sequences. |
| `--transition V` | Sets the `Transition` param to a constant. |
| `--transition-ramp FIRST:LAST` | Keys `Transition` 0 at FIRST and 1 at LAST, linear between and held outside. Pick the frame with `--time`. |
| `--out FILE` | Side by side, as a 24-bit BMP whatever the extension: the inputs at T, then the output, with 8px grey gaps. Filter: `input \| output` (the original layout). Transition: `SourceFrom \| SourceTo \| output`. Generator: `output`. A missing input frame is a grey panel. |
| `--out-only FILE` | Only the output frame. `.bmp` is 24-bit BMP, `.png` RGBA PNG (macOS, ImageIO), anything else binary PPM P6. |

Give a still or a sequence for any one clip, not both (exit 2).

## Pixels

- **Default:** 8-bit RGBA, rows bottom-up (OFX order), `rowBytes = width*4`.
  Bounds and RoD are always `(0,0,W,H)` -- what the bridge negotiates.
- **`--depth float`:** 32-bit float RGBA, 0..1. Inputs are converted from
  8-bit; the output is clamped and rounded to 8-bit for writing and hashing.
- After each clip-preferences pass, every clip is made to report the depth it
  is actually delivered. If the plugin does not support the chosen depth, the
  other one is used and a NOTE is printed.
- Components are always RGBA and every clip says premultiplied. PPM and 24-bit
  BMP inputs are opaque. A 32-bit BMP keeps its alpha unless every alpha byte
  is 0, which reads as opaque.
- **Formats in:** binary PPM P6 and PGM P5 (maxval up to 65535), BMP 24/32-bit
  (BI_RGB/BI_BITFIELDS, either row order), and on macOS anything ImageIO reads
  (PNG, JPEG, TIFF...). ImageIO decodes in the file's own RGB space with no
  conversion, and CoreGraphics hands alpha back premultiplied -- which matches
  what the clip says.
- **Formats out:** PPM and BMP drop alpha. PNG writes the RGBA bytes exactly as
  rendered, so premultiplied data is stored as if it were straight.
- Render scale is 1. No tiles, one view, no fields, `sequentialRender=0`,
  `interactive=0`. The render window is the full frame.

## Time, sequences and temporal access

- OFX time is in **frames**. `--frame-rate` is only what is reported, via
  `kOfxImageEffectPropFrameRate` on the effect and every clip.
- **Sequence:** frame time N shows file `PATTERN % N`. The clip's frame range
  is `[first, last]`: `first` is `--seq-first` (or 0, else 1) and `last` the last
  consecutive file that exists. `fetchImage(t)` anywhere in the range returns
  that frame, non-integer times rounding to the nearest. **Outside the range
  there is no image** -- `clipGetImage` fails with `kOfxStatFailed` and the
  Support library's `fetchImage` returns `nullptr`, as a host does past a
  clip's ends. Frames load on first use and stay cached; each image's unique id
  is `<Clip>@frame<N>`.
- **Still** (`--in`, `--from`, `--to`, or the default ramp/checker): the same
  image at every time. Its frame range is the timeline; its unique id is
  `<Clip>@still` (`@ramp`/`@checker` for the defaults).
- **Timeline:** `--range` if given, else the union of every sequence's range,
  every `--time` and every ramp end in the run, plus frame 0 when no sequence
  is used. It is reported as the effect duration, the output clip's frame range
  and `timeLineGetBounds`, and is fixed for the whole run, so a render does not
  depend on which other renders share the process.
- Fetches at **any** time are answered, whatever `getFramesNeeded` declared,
  unless `--strict-frames` is given.

## Params and time

- With no keys, every param is one constant at all times -- exactly what the
  bridge has always done, and all the wrapper and the app ever use.
- With keys (`--key`, `--transition-ramp`):
  - `getValueAtTime(t)` interpolates: Double, RGB, RGBA and Double2D/3D
    linearly; Integer and Integer2D/3D linearly, then rounded; Choice and
    Boolean step (hold the previous key). Values hold flat outside the keys.
  - `getValue()` with no time is the value at the current render or edit time.
  - `kOfxParamPropIsAnimating` is 1; `derive` is the segment slope and
    `integrate` the exact piecewise integral.
  - The keyframe suite works: `getNumKeys`, `getKeyTime`, `getKeyIndex`
    (direction 0, <0, >0), `deleteKey`, `deleteAllKeys`.
- A plain `--set` (or the plugin's own `setValue`) makes the param constant
  again and drops its keys. The plugin's `setValueAtTime` on a keyed param adds
  or replaces a key; on an unkeyed one it sets the constant, as before.
- String and custom params are not animated. Parametric params are still
  declined, as in the bridge.

## Transition context

- Clips are `SourceFrom`, `SourceTo` and `Output`, each input fed like
  `Source` above, as a still or a sequence, with temporal fetches.
- The plugin's `Transition` double is driven by `--transition V` (a constant),
  `--transition-ramp a:b` (0 to 1 over frames a..b) or `--key Transition=…`
  (any curve). With none of them it keeps the plugin's default, with a NOTE.

## Generator context

There is no source clip; only `Output` is rendered. Give `--size` (default
64x32); `--time` drives any animation. A plugin that defines an optional
`Source` in this context sees it unconnected.

## What a render prints

```
  host: contexts Filter Transition, temporal clip access 1, system plugin dirs NOT scanned
  instance from /…/Foo.ofx.bundle (Transition context)      <- the bundle actually instantiated
  timeline [0, 11] at 25 fps (effect duration 12 frames)
  clip SourceFrom <- sequence a/%04d.ppm, frames 0..11
  key Transition: 0 at frame 0 -> 1 at frame 10 (linear, held outside)
  press poke at t=5 -> kOfxStatReplyDefault
  plugin set pokes = 1                                       <- params the plugin wrote back
rendered 160x90 through com.example.foo
  time 5  context Transition  depth 8-bit RGBA  frame rate 25  render 0.398 ms
  out [0,0] / out [centre]
  out mean       RGBA r g b a
  out hash       fnv1a64 <16 hex> (8-bit RGBA)   <- compare renders by this
  N of M bytes differ from the input              (Filter only)
  plugin said: …                                  (the plugin's message suite)
  wrote …
```

The render time is wall time for begin + render + end. The plugin's own stderr
is interleaved in order, because stdout is line-buffered.

**Exit codes:** 0 OK. 1 for: plugin not found or unusable in the context;
createInstance, init or render failed (the message names the OFX status); an
output could not be written; a `--press`, `--key` or `--transition` target was
missing or refused; any batch line failed. 2 for a usage error.

If more than one scanned bundle carries the identifier, a WARNING lists them
all and the instance is taken from the `--dir` bundle that was described --
not, as before, whichever the scan found first, which could be an installed
copy.

## Quirks: `--quirks fusion`

A port that reads the frame rate unguarded fails in DaVinci Resolve 21.1's
Fusion page: the Support library throws `PropertyUnknownToHost` on a property
the host does not have, render returns `kOfxStatErrMissingHostFeature`, and the
comp "could not be processed". Measured there on 2026-10-04 with a raw-API
plugin: the effect has a frame rate (it follows the timeline), but no clip
does, and clips have no unmapped rate or range either.

`--quirks fusion` is deliberately **stricter** than Fusion, so a plugin that
survives it survives there:

| property | where | default host | `--quirks fusion` |
|---|---|---|---|
| `kOfxImageEffectPropFrameRate` | effect instance and every clip | `--frame-rate` | **removed**: the dimension query and get both answer `kOfxStatErrUnknown` |
| `kOfxImageEffectPropFrameRange` | every clip | sequence range or timeline | **`[0, 0]`** |
| `kOfxImageEffectPropUnmappedFrameRate` / `…UnmappedFrameRange` | every clip | real values | **dimension 0**; a get answers `kOfxStatErrBadIndex` |
| `kOfxImageEffectPropSequentialRenderStatus`, `…InteractiveRenderStatus` | inArgs of begin-sequence, render and end-sequence | present, 0 | **absent** |

Everything else is unchanged: effect duration, the begin/end frame range
`[t, t]`, frame step, render scale, draft quality, project size, temporal
fetches, clip preferences, and the host's own C++ view of the frame rate.

**In a port:** read the frame rate inside try/catch -- the output clip, then
the source clip, then the effect, then a constant -- and do not trust a clip's
FrameRange or the unmapped pair to bound fetches. Under the quirk, a guarded
plugin renders exactly what a host reporting its fallback rate renders, which
is the check the fleet's `verify.sh` scripts run.

## Batch

```sh
cat > renders.txt <<'EOF'
# one render per line; the per-render flags above; # starts a comment
--time 0 --out-only o0.ppm
--time 5 --out-only o5.ppm --press resetTrail
--time 9 --out-only o9.ppm --out o9_sbs.bmp --set amount=0.2
EOF
$P --no-system-dirs --dir $BUILD --render com.example.foo --seq 'f/%04d.ppm' --batch renders.txt
```

- One instance renders every line in order, so describe and createInstance
  run once and param state persists from line to line, as in a host.
- Each line inherits the command line's sources (`--in`/`--seq`/`--from`/
  `--to`/`--seq-*`, `--seq-first`), `--size` and `--time` unless it sets its
  own.
- Outputs and actions (`--set`/`--edit`/`--press`/`--key`/`--transition*`)
  belong to the line; actions on the command line run once, before the first
  line. `--out`/`--out-only` on the command line are ignored with `--batch`.
- Process-wide flags (`--dir`, `--context`, `--depth`, `--frame-rate`,
  `--temporal`, `--range`, `--identity`, `--frames-needed`, `--strict-frames`,
  `--no-system-dirs`, `--quirks`) are not allowed on a line.
- It ends with `batch: N render(s), K failed`. Afterglow's frame 9 rendered
  alone is byte-identical to frame 9 rendered after 0..8 in one batch.

## Compatibility

Every original flag behaves as before. Checked against the probe built from the
previous main, with only the original flags: the corpus from
`scripts/build-test-plugins.sh` and four fleet filters give byte-identical
`--out` images (two fleet generators are refused identically in the default
Filter context), and every line the old probe printed is still printed --
renders gain informational lines (host, instance, timeline, mean, hash, time)
and a failed render now names its OFX status. Listing, `--json` and
`--manifest` are identical.

What did change for the same flags:

- The reported frame rate defaults to 25 (it was 60, which the wrapper and the
  app still report).
- Clips report the depth they are actually delivered.
- Inputs are connected during the first clip-preferences pass.
- The instance comes from the exact bundle that was described.
- `--dir` on a `.ofx.bundle` path scans its parent.
- `--edit` is delivered at `--time` (0 by default, as before).

The wrapper and the app are unaffected: wrappers generated before and after
render byte-identical frames through `ffgltest`, with and without parameters
set.

## Limits

- **CPU only.** The host still advertises the bridge's OpenGL, Metal and OpenCL
  support, but the probe never enables them in a render, so a GPU-only plugin
  fails to render.
- No RoD/RoI negotiation: `getRegionOfDefinition` and `getRegionsOfInterest`
  are never called and every image is the full frame. No tiles, proxy or render
  scale, one view, no interacts or overlays.
- `isIdentity` is called only with `--identity`; `getFramesNeeded` is enforced
  only with `--strict-frames`.
- Instance-changed is sent only for `--edit` and `--press`. A clip changing
  source between batch lines is not announced, and clip preferences re-run only
  when the plugin marks them dirty.
- Inputs are resampled nearest-neighbour. No colour management: values pass
  straight through.
- 16-bit (`OfxBitDepthShort`) images are never delivered.
- PNG/JPEG/TIFF input and PNG output need ImageIO, so macOS only. PPM and BMP
  need nothing platform-specific. The test-host flags have only ever been run
  on macOS.
