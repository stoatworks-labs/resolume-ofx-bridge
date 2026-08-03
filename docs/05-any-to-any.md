# The any-to-any matrix

The bridge began life carrying OpenFX plugins into Resolume. The architecture
that made that work — a prebuilt self-configuring shell, a sidecar manifest,
and a generator that is a **file-copier** — turned out to be
direction-agnostic, and as of v0.8.0 the matrix is closed:

| guest ↓ / host → | FFGL (Resolume) | OpenFX (Resolve, Vegas, Nuke, Natron) | AE / Premiere |
|---|---|---|---|
| **OpenFX** | `ofxgen generate` | native | planned |
| **FFGL** | native | `ofxgen wrap-ffgl` | planned |
| **After Effects** | `wrap-ae`, then `generate` | `ofxgen wrap-ae` | native |

Every cell is generated the same way: describe the guest through the bridge's
own host for that format, write a manifest, copy a prebuilt shell binary, and
carry the untouched guest bundle inside `Contents/Guest/`. The output is
self-contained; the person running the generator never needs a compiler.

## FFGL → OpenFX (`wrap-ffgl`)

```
ofxgen wrap-ffgl --bundle Tinsel.bundle --out /Library/OFX/Plugins
```

The wrapped plugin runs its **real GLSL** inside the new host: the shell owns
a private offscreen GL context (4.1 core, the profile Resolume itself uses),
uploads each frame, calls `ProcessOpenGL`, and reads the result back. FFGL
parameter types map up cleanly — options become choices, events become
pushbuttons, FFGL 2.2 ranges and groups are honoured.

What the crossing costs, stated on the tin:

- **8 bits per channel.** FFGL is an 8-bit world; frames cross as RGBA8
  premultiplied whatever depth the OFX host works in.
- **One render at a time.** Wrapped renders serialise through a global mutex,
  because GL contexts and render thread pools disagree about threads.
- **Stateful guests scrub like live sources.** Time is forwarded, but an
  effect that integrates its own clock cannot be random-accessed. Stateless
  effects are exact: a wrapped Luma Key agrees **byte-for-byte** with the
  hand-written OpenFX port of the same maths.

## After Effects → OpenFX (`wrap-ae`)

```
ofxgen wrap-ae --bundle "Luma Key.plugin" --out /Library/OFX/Plugins
```

This one runs on the bridge's own **minimal After Effects host** — the same
idea as its OFX host, applied to Adobe's API. It supplies the slice of the
environment a well-behaved CPU effect actually touches: parameter setup, the
`iterate` render callback, the Handle memory suite, and per-instance state.

Minimal is a design position, not a temporary apology:

- Effects that render on the legacy CPU path work, and work exactly — the
  fleet's own AE build of Luma Key renders **byte-for-byte identically**
  through this host and through the native OpenFX port.
- Effects that demand deeper application services — GPU suites, custom UI
  drawing, AEGP application control, licence checks against a running After
  Effects — are **refused by name**: the missing suite is printed to stderr,
  so the report is "this plugin needs X", never a crash.

## After Effects → Resolume: compose the bridges

There is no third shell for this cell, on purpose. `wrap-ae` writes a valid
OpenFX bundle, and `generate` has always turned OpenFX bundles into FFGL:

```
ofxgen wrap-ae   --bundle "Luma Key.plugin" --out staging
ofxgen generate  --bundle staging/Luma_Key_AE.ofx.bundle --out "…/Extra Effects"
```

The chain — an After Effects plugin inside an OpenFX shell inside an FFGL
wrapper — instantiates and renders through a real GL context exactly the way
Resolume loads plugins, within 8-bit rounding of the effect running natively.
Composability is the pay-off of the shells all speaking the same manifest.

## Why there is no AE-as-host column entry yet

The remaining "planned" cells put existing guests inside an After Effects
*shell* — an `.aex`/`.plugin` the bridge generates. The machinery is designed
(the same Rust route the fleet's own AE plugins use), but it ships only after
the pipe-cleaner AE plugin has been verified inside a real After Effects,
because an unverified host family is how silent all-plugins-broken bugs
happen. Ask the repository's history how it knows.

## Platforms

| | macOS | Windows | Linux |
|---|---|---|---|
| FFGL guest | CGL, **run and verified** | WGL, compiled only | surfaceless EGL, compiled only |
| After Effects guest | yes | yes, compiled only | **no — Adobe has never shipped After Effects for Linux** |
| Desktop tools (`ofxgen`, `ofxprobe`, the app) | yes | not built | not built |
| Browser wrapper shell | served | served | served |

The Windows and Linux shells are built by CI on every release and served by
the browser wrapper, which tells the visitor which one they are getting and
that it is untested. They compile; nobody here has a Windows or Linux machine
to run them on. That is a different claim from "it works", and the project
says which one it is making.

The Linux column has an honest wrinkle worth stating: FFGL is Resolume's
format and Resolume has no Linux build, so a Linux FFGL plugin barely exists
in the wild. The shell is there because OpenFX hosts on Linux are real
(Natron, Nuke, Resolve) and because refusing to build it would have been a
guess about what nobody has; it is not there because there is a queue of
people waiting for it.
