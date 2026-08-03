# resolume-ofx-bridge — command reference

Host OpenFX (DaVinci Resolve) plugins inside Resolume as generated FFGL bundles.
For the *why*, the invariants and the traps, read **[AGENTS.md](AGENTS.md)**.

## Build

```bash
git submodule update --init --recursive
cmake -S . -B build && cmake --build build -j8
```

## Test corpus

A clean machine has no OFX plugins, even with Resolve installed. Build some:

```bash
./scripts/build-test-plugins.sh          # -> build/test-plugins
```

## The loop

```bash
open "build/OFX Bridge.app"                                       # the app
./build/ofxprobe --dir build/test-plugins                         # what's there
./build/ofxprobe --dir build/test-plugins --render <id> --set k=v # CPU render
./build/ofxprobe --dir DIR --render <id> --edit preset=2          # a real user edit: fires instanceChanged (presets)
./build/ofxprobe --dir DIR --render <id> --size 640x360 --out out.bmp # input|output image
./build/ofxgen generate --dir build/test-plugins --out build/generated
./build/ofxgen generate --bundle <one.ofx.bundle> --out DIR       # just one
./build/ofxgen wrap-ffgl --bundle <FFGL.bundle> --out DIR # FFGL -> OFX, the other direction
./build/ofxgen wrap-ae --bundle <AE.plugin> --out DIR     # AE -> OFX (minimal AE host)
# AE -> FFGL is the two composed: wrap-ae, then generate on the result
./build/ofxprobe --dir DIR --render com.stoatworks.ffglwrap.<name> # drive a wrapped FFGL plugin
./build/ofxgen verify build/generated/<Name>.bundle               # load as a host
./build/ffgltest build/generated/<Name>.bundle 0=0.5              # real GL
./build/ffgltest build/generated/<Name>.bundle --demo out.bmp     # demo image
```

`OFXBRIDGE_DEBUG=1` logs each parameter instance the host creates.
`OFXBRIDGE_TIMING=1` reports per-stage frame timing.

```bash
./build/ffgltest <bundle> --size 3840x2160 --bench 60   # measure
./build/ffgltest <bundle> --legacy-gl                   # immediate-mode GL plugins
./build/ffgltest <bundle> --expect-centre 64,64,64,127  # assert, exit 1 on mismatch
./build/ffgltest <bundle> --require-gpu                 # exit 3 if software renderer
```

## Release

Tag-triggered; `.github/workflows/release.yml` builds macOS universal only
(Windows/Linux paths exist but have never been compiled).

```bash
git tag -a vX.Y.Z -m "..." && git push origin vX.Y.Z   # latest released: v0.5.1
```
