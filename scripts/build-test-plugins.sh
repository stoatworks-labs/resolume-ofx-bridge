#!/usr/bin/env bash
#
# Build a corpus of real OFX plugins to develop and test against.
#
# There is deliberately no OFX plugin installed on a clean machine: DaVinci
# Resolve compiles its own ResolveFX into the application rather than shipping
# loadable bundles, so `/Library/OFX/Plugins` is usually empty. Without this
# script there is nothing to point the bridge at.
#
# The plugins come from the OpenFX repo's own examples. They are built here
# rather than via the upstream CMake project because that project requires
# Conan-only packages (cimg, spdlog, opengl_system) that only its *other*
# examples need.
#
# Output: build/test-plugins/<name>.ofx.bundle
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OFX="$ROOT/external/openfx"
OUT="$ROOT/build/test-plugins"
SUPPORT_BUILD="$ROOT/build/ofx-support"

if [ ! -f "$OFX/include/ofxCore.h" ]; then
	echo "external/openfx is missing. Run: git submodule update --init --recursive" >&2
	exit 1
fi

ARCH="${ARCH:-$(uname -m)}"

# The OpenFX C++ plugin Support library, which the examples are written against.
#
# Compiled directly rather than through the upstream CMake project: that project
# resolves EXPAT via Conan's `expat::expat` target, which the system FindEXPAT
# module does not define, so its generate step fails on a plain checkout. We only
# need the Support sources, which have no such dependency.
echo "==> building OpenFX Support library ($ARCH)"
mkdir -p "$SUPPORT_BUILD"
LIB="$SUPPORT_BUILD/libOfxSupport.a"

objs=()
for src in "$OFX"/Support/Library/*.cpp; do
	obj="$SUPPORT_BUILD/$(basename "$src" .cpp).o"
	clang++ -std=c++17 -O2 -arch "$ARCH" -fPIC -Wno-deprecated-declarations -c \
		-I "$OFX/include" -I "$OFX/Support/include" \
		"$src" -o "$obj"
	objs+=( "$obj" )
done
ar rcs "$LIB" "${objs[@]}"

[ -f "$LIB" ] || { echo "libOfxSupport.a was not produced" >&2; exit 1; }

mkdir -p "$OUT"

# Invert       - the minimal filter: no parameters at all
# Basic        - doubles with display ranges, a boolean, a group and a page
# ChoiceParams - choice parameters with named options
# Custom       - General context only; exercises our "cannot host this" path
# OpenGL       - implements OFX OpenGL render; the only test subject for the
#                GPU path, since none of the others can avoid the CPU round trip
for ex in Invert Basic ChoiceParams Custom OpenGL; do
	src="$(ls "$OFX/Examples/$ex/"*.cpp 2>/dev/null | head -1 || true)"
	[ -n "$src" ] || { echo "  skip $ex (no source)"; continue; }

	name="$(basename "$src" .cpp)"
	bdl="$OUT/$name.ofx.bundle/Contents/MacOS"
	mkdir -p "$bdl"

	clang++ -std=c++17 -O2 -arch "$ARCH" -dynamiclib -fvisibility=hidden -Wno-deprecated-declarations \
		-I "$OFX/include" -I "$OFX/Support/include" -I "$OFX/Examples/include" \
		"$src" "$LIB" \
		-framework OpenGL -framework CoreFoundation \
		-o "$bdl/$name.ofx"

	echo "  built $name.ofx.bundle"
done

echo
echo "test plugins in: $OUT"
echo "try: ./build/ofxprobe --dir $OUT"
