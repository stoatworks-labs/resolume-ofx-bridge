#pragma once
//
// Locating our own binary.
//
// A generated wrapper is a *copy* of one prebuilt binary, so it cannot know at
// compile time which OFX plugin it represents. It finds out by reading a
// manifest stored alongside itself, which first requires knowing where "itself"
// is — not the host executable, which is what argv[0] and the working directory
// would give.
//

#include <string>

namespace ofxffgl {

/// Absolute path of the shared library this code is compiled into.
/// Empty on failure.
std::string selfBinaryPath();

/// Where this binary's manifest should be.
///
/// Inside a macOS .bundle that is Contents/Resources/manifest.json; elsewhere
/// (and for a loose dylib during development) it is a sidecar file next to the
/// binary with a .manifest.json extension.
std::string selfManifestPath();

} // namespace ofxffgl
