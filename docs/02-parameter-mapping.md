# Parameter mapping

How each OFX parameter type becomes one or more FFGL parameters. Implemented in
`source/ffgl/Manifest.cpp` (`buildParamTable`).

## The table

| OFX type | FFGL result |
|---|---|
| `Double` | one `FF_TYPE_STANDARD`, with range |
| `Integer` | one `FF_TYPE_INTEGER`, with range |
| `Boolean` | one `FF_TYPE_BOOLEAN` |
| `Choice` | one `FF_TYPE_OPTION` with named elements |
| `String`, `Custom` | one `FF_TYPE_TEXT` |
| `PushButton` | one `FF_TYPE_EVENT` |
| `RGB` | three params tagged `FF_TYPE_RED/GREEN/BLUE` |
| `RGBA` | four, adding `FF_TYPE_ALPHA` |
| `Double2D`, `Integer2D` | two params, suffixed " X" / " Y" |
| `Double3D`, `Integer3D` | three params, suffixed " X" / " Y" / " Z" |
| `Group` | no parameter — becomes the `group` heading on its children |
| `Page` | no parameter — dropped |
| `Parametric` | **declined** (see below) |

Declaration order is preserved, because that is the order the plugin's author
intended the UI to read in. This is why the code uses HostSupport's
`getParamList()` rather than `getParams()` — the latter is a `std::map` keyed by
name and silently alphabetises.

## Groups become headings, not parameters

An OFX `Group` is a container. FFGL has no container type, but it does let a
parameter name the group it belongs to via `SetParamGroup`. So a group parameter
produces no slot of its own; instead each of its children gets the group's
**label** as a heading.

The label, not the internal name: the OpenFX Gain example has a group named
`componentScales` labelled `Components`, and `Components` is what belongs in the
UI.

## Colours

FFGL has no colour type, but it tags scalar parameters as
`FF_TYPE_RED`/`GREEN`/`BLUE`/`ALPHA` so a host can recognise consecutive
components and present a colour picker. An OFX `RGBA` therefore becomes four
tagged parameters rather than four anonymous sliders.

## Ranges: display beats hard

OFX distinguishes two ranges:

- `kOfxParamPropMin`/`Max` — what the plugin will *accept*
- `kOfxParamPropDisplayMin`/`Max` — what a UI should *offer*

Plugins routinely leave the hard range unbounded. The OpenFX Gain example
declares `scale` with a hard maximum of `1.79769e+308` — `DBL_MAX` — and a
display range of 0–100. A slider spanning `DBL_MAX` is useless, so the display
range wins whenever it is present and sane.

When neither is usable, `chooseRange` spans the default value symmetrically
rather than pinning it to an edge — a parameter defaulting to 5 with no declared
range gets 0–10, not 0–1 with the handle jammed against the top.

## Multi-component values on the way back

Splitting one OFX parameter into several FFGL slots means reassembling them
before each render. `OfxFFGLPlugin::applyParams` walks the parameter table in
order, gathering consecutive slots that share an OFX name into a single
component vector, and pushes each complete parameter across in one call.

The FFGL parameter *name* is suffixed too (`colour.0`, `colour.1`, …), not just
the display name — FFGL names must be unique, and Resolume serialises by them.

Display names can change freely between versions; **names must not**, or saved
compositions lose their parameter values.

## What is declined, and why

**Parametric parameters** — curve editors — have no FFGL equivalent. They could
be flattened into N sample points, but that would present the user with something
that is not the plugin's interface and does not behave like it. The factory in
`source/ofxbridge/Params.cpp` returns `nullptr` for them, which surfaces as the
plugin being unusable rather than as a silently wrong UI.

**Secret parameters** (`kOfxParamPropSecret`) are created and driven normally but
marked invisible, since a plugin may still depend on their values.

## Plugin identity

FFGL requires a unique 4-character ID per plugin. It is derived by hashing the
OFX plugin identifier (FNV-1a, rendered in base 62), so the same plugin always
produces the same ID on every machine — a counter would produce IDs that depend
on scan order and break saved compositions when the plugin set changes.

The FFGL plugin *name* is capped at 16 characters, so it is truncated
deliberately rather than left to whatever the SDK's fixed-size copy leaves
behind.
