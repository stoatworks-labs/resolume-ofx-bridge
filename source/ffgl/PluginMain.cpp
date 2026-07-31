//
// FFGL plugin registration.
//
// This is where the "one binary, many plugins" trick actually happens.
//
// `CFFGLPluginInfo` is a global whose constructor arguments are evaluated during
// dynamic initialisation — after the loader has mapped us in, but before the
// host calls anything. That is late enough for us to locate our own file and
// read the manifest sitting next to it, and early enough that the host still
// sees a fully-formed plugin. So a *copy* of this binary reports whichever OFX
// plugin its manifest names, with no recompilation.
//

#include "OfxFFGLPlugin.h"

using namespace ofxffgl;

namespace {

/// FFGL truncates names at 16 characters; do it ourselves so the result is
/// predictable rather than whatever the SDK's memcpy leaves behind.
std::string shortName()
{
	const PluginContext& ctx = PluginContext::get();
	std::string name = ctx.loaded ? ctx.pluginName : std::string( "OFX (no manifest)" );
	if( name.size() > 16 )
		name.resize( 16 );
	return name;
}

/// A failed load still has to produce a registered plugin: FFGL offers no way to
/// report "I could not initialise" other than existing and doing nothing, and an
/// effect that silently fails is far easier to diagnose in Resolume than one
/// that never appears.
std::string aboutText()
{
	const PluginContext& ctx = PluginContext::get();
	if( !ctx.loaded )
		return "OFX bridge failed to load: " + ctx.error;
	return "OFX " + ctx.manifest.identifier + " v" + std::to_string( ctx.manifest.versionMajor ) + "." +
		   std::to_string( ctx.manifest.versionMinor ) + " via resolume-ofx-bridge";
}

const std::string gPluginId   = PluginContext::get().loaded ? PluginContext::get().pluginId : std::string( "OFX0" );
const std::string gPluginName = shortName();
const std::string gDescription = PluginContext::get().loaded ? PluginContext::get().description
															 : std::string( "OFX bridge (manifest missing)" );
const std::string gAbout = aboutText();

} // namespace

static CFFGLPluginInfo PluginInfo(
	PluginFactory< OfxFFGLPlugin >,// create method
	gPluginId.c_str(),             // unique ID, exactly 4 characters
	gPluginName.c_str(),           // plugin name, max 16 characters
	2,                             // API major version
	1,                             // API minor version
	1,                             // plugin major version
	0,                             // plugin minor version
	FF_EFFECT,                     // plugin type
	gDescription.c_str(),          // description
	gAbout.c_str()                 // about
);
