# resolume-ofx-bridge

Run OpenFX plugins — the format DaVinci Resolve uses — inside Resolume
Arena/Avenue as native FFGL effects.

A standalone generator scans your OFX plugin directories and emits one FFGL
bundle per plugin, each exposing that plugin's real parameters in Resolume's UI.

> **Status: early.** The OFX host and introspection work and are tested against
> real plugins. The FFGL wrapper and the generator app are not built yet, so
> nothing runs inside Resolume today. See [AGENTS.md](AGENTS.md) for exactly what
> is verified versus assumed.

## Why one bundle per plugin

FFGL fixes a plugin's parameter types, ranges, and defaults at load time — it can
only change *labels* and *visibility* afterwards. A single wrapper with a plugin
dropdown would therefore have to flatten every OFX parameter into anonymous 0–1
sliders. Generating a bundle per plugin keeps each parameter's real type, unit,
and range.

The generated bundles are copies of one prebuilt binary plus a JSON manifest, so
**you do not need a compiler** to use the generator.

## Building

```bash
git submodule update --init --recursive
cmake -S . -B build && cmake --build build -j8
```

## Trying it

There is probably no OFX plugin on your machine — Resolve compiles its own
ResolveFX into the application rather than installing loadable bundles. Build a
test corpus from the OpenFX examples:

```bash
./scripts/build-test-plugins.sh
```

Then inspect them:

```bash
./build/ofxprobe --dir build/test-plugins
```

```
uk.co.thefoundry.BasicGainPlugin
  label      : OFX Gain Example
  contexts   : OfxImageEffectContextFilter OfxImageEffectContextGeneral
  parameters : 8
    scale                    OfxParamTypeDouble     scale  (0..100)
    scaleComponents          OfxParamTypeBoolean    Scale Individual Components
    componentScales          OfxParamTypeGroup      Components
    scaleR                   OfxParamTypeDouble     red  (0..100)
    ...
```

`ofxprobe --json` (or `--manifest <identifier>`) emits the manifest a generated
wrapper will read.

Scanning with no `--dir` uses the conventional OFX locations for your platform,
plus `OFX_PLUGIN_PATH`.

## Compatibility

The bridge identifies itself honestly as its own host. Some commercial OFX
plugins only license themselves to hosts they recognise and will decline to load
here; that is the vendor's decision and is not worked around.

Only the OFX **Filter** context is hosted, since that is what maps onto an effect
slot in a Resolume clip.

## Licence

MIT. Bundles OpenFX (BSD-3-Clause) and the Resolume FFGL SDK as submodules.
