#pragma once
//
// Discovery + introspection: find OFX bundles, run the describe actions, and
// flatten each plugin's parameters into a form the generator can turn into an
// FFGL parameter table.
//
// Describing a plugin means loading and running its code, so this is also where
// we tolerate plugins that crash or refuse to load: a failure is recorded on the
// PluginDesc rather than propagated.
//

#include <string>
#include <vector>

namespace ofxbridge {

/// One OFX parameter, normalised across the OFX param types.
struct ParamDesc
{
	/// OFX param name. Stable identifier; used for serialisation on both sides.
	std::string name;
	/// Human label as the plugin wants it shown.
	std::string label;
	/// One of the kOfxParamType* strings.
	std::string type;
	/// Name of the enclosing group param, or empty.
	std::string parent;
	std::string hint;

	bool secret  = false;
	bool enabled = true;

	/// Number of components: 1 for Double/Int, 2 for Double2D, 3 for RGB, 4 for RGBA.
	int dimension = 1;

	/// Per-component range and defaults. Ranges are the *display* range where the
	/// plugin gave one, because that is what a UI slider should span; the hard
	/// min/max is kept separately for clamping.
	bool hasDisplayRange = false;
	std::vector< double > displayMin;
	std::vector< double > displayMax;
	std::vector< double > hardMin;
	std::vector< double > hardMax;
	std::vector< double > defaults;

	/// Choice param options, in order.
	std::vector< std::string > choices;
	/// Default for string params.
	std::string stringDefault;
	bool boolDefault = false;
};

/// One plugin inside a bundle (a bundle may export several).
struct PluginDesc
{
	std::string identifier;
	std::string label;
	std::string grouping;
	int versionMajor = 0;
	int versionMinor = 0;

	std::string bundlePath;
	/// Index of this plugin within its bundle's OfxGetPlugin() enumeration.
	int indexInBundle = 0;

	std::vector< std::string > contexts;
	bool supportsFilter = false;

	std::vector< ParamDesc > params;

	/// Non-empty if describe failed; the plugin is then unusable.
	std::string error;
};

/// The conventional OFX plugin directories for this platform, plus anything on
/// the OFX_PLUGIN_PATH environment variable.
std::vector< std::string > defaultSearchPaths();

/// Scan `searchPaths`, load every bundle found, and describe every plugin.
/// `log` accumulates human-readable progress and any load failures.
std::vector< PluginDesc > scanAndDescribe( const std::vector< std::string >& searchPaths, std::string& log );

/// Serialise a described plugin to the JSON manifest a generated wrapper reads
/// at load time.
std::string toManifestJson( const PluginDesc& plugin );

} // namespace ofxbridge
