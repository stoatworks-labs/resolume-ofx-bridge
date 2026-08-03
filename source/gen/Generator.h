#pragma once
//
// The generation step, factored out of the CLI so the GUI drives exactly the
// same code path. Nothing here knows about argv or about AppKit.
//
// Generation is a copy, not a compile: each output bundle is the prebuilt
// ofxwrapper binary plus a manifest describing one OFX plugin.
//

#include <functional>
#include <string>
#include <vector>

namespace ofxgen {

struct Options
{
	/// Directories to scan. Empty means ofxbridge::defaultSearchPaths().
	std::vector< std::string > searchPaths;

	/// Where the FFGL bundles are written. Created if missing.
	std::string outDir;

	/// The prebuilt ofxwrapper bundle/library that every output is a copy of.
	/// Empty means findTemplate().
	std::string templatePath;

	/// Generate only the plugin with this OFX identifier, if set.
	std::string onlyIdentifier;

	/// Generate only plugins that came out of this OFX bundle, if set.
	///
	/// This exists because HostSupport's search path is directory-scoped (see
	/// AGENTS.md): pointing the scanner at one .ofx.bundle finds nothing, so
	/// selecting a single plugin file means scanning its parent directory and
	/// filtering the results afterwards.
	std::string onlyBundlePath;

	/// Strip com.apple.quarantine from what we write. A quarantined plugin makes
	/// Resolume skip it silently rather than prompt, and a wrapper copied out of
	/// a downloaded zip inherits the flag. macOS only; ignored elsewhere.
	bool clearQuarantine = true;
};

struct Result
{
	int generated = 0;
	int skipped   = 0;
	/// Set when generation could not start at all (no template, unwritable
	/// output). Per-plugin failures are counted in `skipped` instead.
	std::string error;
	/// Absolute paths of the bundles written.
	std::vector< std::string > bundles;

	bool cancelled = false;
};

/// One line of human-readable progress, without a trailing newline.
using LogFn = std::function< void( const std::string& line ) >;

/// `done` of `total` plugins handled. `total` is 0 while still scanning, which
/// a UI should show as an indeterminate phase: scanning loads and describes
/// every plugin found, so its cost is not known in advance.
using ProgressFn = std::function< void( int done, int total, const std::string& label ) >;

/// Polled between plugins. Returning true stops as soon as the current plugin
/// finishes; bundles already written are left in place.
using CancelFn = std::function< bool() >;

/// Locate the prebuilt wrapper, given the running executable's path. Looks next
/// to the executable, one level up, inside an enclosing .app's Resources, and
/// finally in ./build for a development tree.
std::string findTemplate( const std::string& executablePath );

/// Turn a plugin label into something safe for a filename.
std::string sanitise( const std::string& in );

/// Scan, describe and write bundles. Blocking; safe to call off the main thread
/// as long as the callbacks are.
Result generate( const Options& options, const LogFn& log, const ProgressFn& progress, const CancelFn& cancelled );

// ---------------------------------------------------------------------------
// The other direction: FFGL plugins wrapped as OFX bundles (macOS only).
// ---------------------------------------------------------------------------

struct WrapFfglOptions
{
	/// FFGL bundles to wrap.
	std::vector< std::string > bundles;

	/// Where the .ofx.bundle outputs are written. Created if missing.
	std::string outDir;

	/// The prebuilt ffglofxshell.ofx binary. Empty means findOfxShell().
	std::string shellPath;
};

/// Locate the prebuilt OFX shell binary, next to the running executable, one
/// level up, or in ./build for a development tree.
std::string findOfxShell( const std::string& executablePath );

/// Describe each FFGL bundle and write a self-contained OFX bundle around it:
/// the shell binary, a manifest, and a copy of the guest inside Contents/Guest.
Result wrapFfgl( const WrapFfglOptions& options, const LogFn& log );

} // namespace ofxgen
