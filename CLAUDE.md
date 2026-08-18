# resolume-ofx-bridge — command reference

Host OpenFX (DaVinci Resolve) plugins inside Resolume as generated FFGL bundles.
For the *why*, the invariants and the traps, read **[AGENTS.md](AGENTS.md)**.

## Build

```bash
git submodule update --init --recursive
cmake -S . -B build && cmake --build build -j8
```

Windows (x64, vcpkg for GLEW and expat — the static-md triplet, or the artefacts
import a glew32.dll nobody has):

```bash
vcpkg install glew:x64-windows-static-md expat:x64-windows-static-md
cmake -S . -B build -A x64 -DVCPKG_TARGET_TRIPLET=x64-windows-static-md -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release --parallel
```

Everything but `OFX Bridge.app` builds there. Binaries land in `build\Release\`,
except `ffglofxshell.ofx`, which is pinned to `build\`.

## Test corpus

A clean machine has no OFX plugins, even with Resolve installed. Build some:

```bash
./scripts/build-test-plugins.sh          # -> build/test-plugins
```

```powershell
powershell -ExecutionPolicy Bypass -File scripts\build-test-plugins.ps1   # Windows; edit both
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
# exit 3 also means "no context could be created here" — a headless runner, or a
# Windows session with no desktop. It is the skip status, not a failure.
```

## Release

Tag-triggered; `.github/workflows/release.yml` builds macOS universal and
Windows x64, each self-tested against a corpus built from the OpenFX examples,
plus the Linux FFGL->OFX shell. The GL render path is asserted on macOS only —
no runner has a GL 4.1 core driver on Windows, where `ffgltest` exits 3 and CI
skips with a notice.

```bash
git tag -a vX.Y.Z -m "..." && git push origin vX.Y.Z   # latest released: v0.8.1
```
