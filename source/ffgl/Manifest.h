#pragma once
//
// The manifest a generated wrapper reads at load time, and the mapping from OFX
// parameters onto FFGL parameter slots.
//
// One OFX parameter can become several FFGL ones: OFX has vector and colour
// types, FFGL has only scalars (with red/green/blue/alpha tagged so hosts can
// reassemble a colour picker). The FfglParam list is therefore flatter than the
// OFX list, and each entry remembers which OFX parameter and component it drives.
//

#include <string>
#include <vector>

namespace ofxffgl {

/// One parameter as described by ofxprobe.
struct ManifestParam
{
	std::string name;
	std::string label;
	std::string type;// kOfxParamType*
	std::string parent;
	std::string hint;
	bool secret    = false;
	int dimension  = 1;

	std::vector< double > defaults;
	std::vector< double > displayMin;
	std::vector< double > displayMax;
	std::vector< double > hardMin;
	std::vector< double > hardMax;
	std::vector< std::string > choices;
	bool boolDefault = false;
	std::string stringDefault;
};

struct Manifest
{
	int manifestVersion = 0;
	std::string identifier;
	std::string label;
	std::string grouping;
	int versionMajor = 0;
	int versionMinor = 0;
	std::string bundlePath;
	int indexInBundle = 0;
	std::vector< ManifestParam > params;

	static bool load( const std::string& path, Manifest& out, std::string& error );
};

/// One FFGL parameter slot.
struct FfglParam
{
	/// Stable identifier given to FFGL. Resolume serialises by this, so it must
	/// never change for a given plugin between versions.
	std::string name;
	/// What the user sees. Free to change.
	std::string displayName;
	/// Group heading, from the OFX parent group's label.
	std::string group;

	unsigned int ffglType = 0;// FF_TYPE_*
	float defaultValue    = 0.0f;
	float rangeMin        = 0.0f;
	float rangeMax        = 1.0f;
	bool visible          = true;

	/// Options for FF_TYPE_OPTION.
	std::vector< std::string > elements;

	/// Which OFX parameter this drives, and which component of it.
	std::string ofxName;
	std::string ofxType;
	int component = 0;

	/// True when the OFX parameter is a string, driven via SetTextParameter.
	bool isText = false;

	std::string textDefault;
};

/// Flatten a manifest into FFGL parameter slots, in declaration order.
///
/// Group and Page params produce no slot of their own; groups instead become the
/// `group` field of their children, which is how FFGL expresses the same idea.
std::vector< FfglParam > buildParamTable( const Manifest& manifest );

/// A deterministic 4-character FFGL plugin ID derived from an OFX identifier.
///
/// FFGL requires exactly 4 characters and uses them to tell plugins apart, so it
/// must be stable across machines and runs — a hash, never a counter.
std::string makePluginId( const std::string& ofxIdentifier );

} // namespace ofxffgl
