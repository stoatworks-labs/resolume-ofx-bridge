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
./build/ofxprobe --dir build/test-plugins                         # what's there
./build/ofxprobe --dir build/test-plugins --render <id> --set k=v # CPU render
./build/ofxgen generate --dir build/test-plugins --out build/generated
./build/ofxgen verify build/generated/<Name>.bundle               # load as a host
./build/ffgltest build/generated/<Name>.bundle 0=0.5              # real GL
./build/ffgltest build/generated/<Name>.bundle --demo out.bmp     # demo image
```

`OFXBRIDGE_DEBUG=1` makes the host log each parameter instance it creates.

## Release

Tag-triggered; `.github/workflows/release.yml` builds macOS and Windows.

```bash
git tag -a v0.1.0 -m "..." && git push origin v0.1.0
```
