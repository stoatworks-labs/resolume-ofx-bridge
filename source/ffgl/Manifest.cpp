#include "Manifest.h"
#include "Json.h"

#include "ffgl/FFGL.h"

#include <algorithm>
#include <cstdint>
#include <map>

namespace ofxffgl {

namespace {

/// OFX param type strings. Declared locally so this file, which is compiled into
/// the FFGL bundle, does not have to pull in the OFX headers.
const char* const kDouble    = "OfxParamTypeDouble";
const char* const kDouble2D  = "OfxParamTypeDouble2D";
const char* const kDouble3D  = "OfxParamTypeDouble3D";
const char* const kInteger   = "OfxParamTypeInteger";
const char* const kInteger2D = "OfxParamTypeInteger2D";
const char* const kInteger3D = "OfxParamTypeInteger3D";
const char* const kBoolean   = "OfxParamTypeBoolean";
const char* const kChoice    = "OfxParamTypeChoice";
const char* const kRGB       = "OfxParamTypeRGB";
const char* const kRGBA      = "OfxParamTypeRGBA";
const char* const kString    = "OfxParamTypeString";
const char* const kCustom    = "OfxParamTypeCustom";
const char* const kGroup     = "OfxParamTypeGroup";
const char* const kPage      = "OfxParamTypePage";
const char* const kPush      = "OfxParamTypePushButton";

double at( const std::vector< double >& v, size_t i, double fallback )
{
	return i < v.size() ? v[ i ] : fallback;
}

/// Picks the range a slider should span.
///
/// OFX distinguishes a hard min/max (what the plugin will accept) from a display
/// min/max (what a UI should offer). Plugins routinely leave the hard range
/// unbounded — ±DBL_MAX — which is useless for a slider, so display range wins
/// when present and we fall back to a sane finite guess when it isn't.
void chooseRange( const ManifestParam& p, int component, float& outMin, float& outMax )
{
	const double dispMin = at( p.displayMin, component, 0.0 );
	const double dispMax = at( p.displayMax, component, 0.0 );
	if( !p.displayMin.empty() && !p.displayMax.empty() && dispMax > dispMin )
	{
		outMin = (float)dispMin;
		outMax = (float)dispMax;
		return;
	}

	const double hardMin = at( p.hardMin, component, 0.0 );
	const double hardMax = at( p.hardMax, component, 1.0 );
	const bool finite    = hardMax > hardMin && hardMax < 1e30 && hardMin > -1e30;
	if( finite )
	{
		outMin = (float)hardMin;
		outMax = (float)hardMax;
		return;
	}

	// Unbounded. Span the default symmetrically so it isn't pinned at an edge.
	const double def = at( p.defaults, component, 0.0 );
	const double mag = std::max( 1.0, std::abs( def ) * 2.0 );
	outMin = (float)std::max( hardMin, -mag );
	outMax = (float)std::min( hardMax, mag );
}

/// Component suffixes, so a flattened vector param still reads sensibly.
const char* suffixFor( const std::string& type, int component )
{
	if( type == kRGB || type == kRGBA )
		return ( const char*[] ){ " R", " G", " B", " A" }[ component ];
	if( type == kDouble2D || type == kInteger2D )
		return ( const char*[] ){ " X", " Y" }[ component ];
	if( type == kDouble3D || type == kInteger3D )
		return ( const char*[] ){ " X", " Y", " Z" }[ component ];
	return "";
}

/// FFGL tags colour components so hosts can present a colour picker.
unsigned int colourTypeFor( const std::string& type, int component )
{
	static const unsigned int rgba[] = { FF_TYPE_RED, FF_TYPE_GREEN, FF_TYPE_BLUE, FF_TYPE_ALPHA };
	if( ( type == kRGB && component < 3 ) || ( type == kRGBA && component < 4 ) )
		return rgba[ component ];
	return FF_TYPE_STANDARD;
}

} // namespace

bool Manifest::load( const std::string& path, Manifest& out, std::string& error )
{
	Json root;
	if( !Json::parseFile( path, root, error ) )
		return false;
	if( root.type != Json::Type::Object )
	{
		error = "manifest is not a JSON object";
		return false;
	}

	auto str = [ & ]( const char* key ) -> std::string {
		const Json* v = root.find( key );
		return v ? v->string() : std::string();
	};
	auto num = [ & ]( const char* key, double d ) -> double {
		const Json* v = root.find( key );
		return v ? v->number( d ) : d;
	};

	out.manifestVersion = (int)num( "manifestVersion", 0 );
	if( out.manifestVersion != 1 )
	{
		error = "unsupported manifest version " + std::to_string( out.manifestVersion );
		return false;
	}

	out.identifier    = str( "identifier" );
	out.label         = str( "label" );
	out.grouping      = str( "grouping" );
	out.bundlePath    = str( "bundlePath" );
	out.versionMajor  = (int)num( "versionMajor", 0 );
	out.versionMinor  = (int)num( "versionMinor", 0 );
	out.indexInBundle = (int)num( "indexInBundle", 0 );

	if( const Json* v = root.find( "supportsOpenGLRender" ) )
		out.supportsOpenGLRender = v->boolean();
	if( const Json* v = root.find( "supportsMetalRender" ) )
		out.supportsMetalRender = v->boolean();

	if( out.identifier.empty() || out.bundlePath.empty() )
	{
		error = "manifest is missing identifier or bundlePath";
		return false;
	}

	const Json* params = root.find( "params" );
	if( params != nullptr && params->type == Json::Type::Array )
	{
		for( const Json& p : params->arrayValue )
		{
			ManifestParam mp;
			auto ps = [ & ]( const char* key ) -> std::string {
				const Json* v = p.find( key );
				return v ? v->string() : std::string();
			};
			auto pa = [ & ]( const char* key ) -> std::vector< double > {
				const Json* v = p.find( key );
				return v ? v->numberArray() : std::vector< double >();
			};

			mp.name   = ps( "name" );
			mp.label  = ps( "label" );
			mp.type   = ps( "type" );
			mp.parent = ps( "parent" );
			mp.hint   = ps( "hint" );

			if( const Json* v = p.find( "secret" ) )
				mp.secret = v->boolean();
			if( const Json* v = p.find( "dimension" ) )
				mp.dimension = (int)v->number( 1 );
			if( const Json* v = p.find( "boolDefault" ) )
				mp.boolDefault = v->boolean();

			mp.stringDefault = ps( "stringDefault" );
			mp.defaults      = pa( "defaults" );
			mp.displayMin    = pa( "displayMin" );
			mp.displayMax    = pa( "displayMax" );
			mp.hardMin       = pa( "hardMin" );
			mp.hardMax       = pa( "hardMax" );

			if( const Json* v = p.find( "choices" ) )
				mp.choices = v->stringArray();

			if( mp.name.empty() || mp.type.empty() )
			{
				error = "manifest contains a parameter with no name or type";
				return false;
			}
			out.params.push_back( std::move( mp ) );
		}
	}
	return true;
}

std::vector< FfglParam > buildParamTable( const Manifest& manifest )
{
	// Group labels, so a child can name its heading rather than repeat the
	// group's internal id.
	std::map< std::string, std::string > groupLabels;
	for( const ManifestParam& p : manifest.params )
		if( p.type == kGroup )
			groupLabels[ p.name ] = p.label.empty() ? p.name : p.label;

	std::vector< FfglParam > table;

	for( const ManifestParam& p : manifest.params )
	{
		// Structural params carry no value of their own.
		if( p.type == kGroup || p.type == kPage )
			continue;

		std::string group;
		if( !p.parent.empty() )
		{
			auto it = groupLabels.find( p.parent );
			group   = it != groupLabels.end() ? it->second : p.parent;
		}

		const std::string label = p.label.empty() ? p.name : p.label;

		if( p.type == kString || p.type == kCustom )
		{
			FfglParam f;
			f.name        = p.name;
			f.displayName = label;
			f.group       = group;
			f.ffglType    = FF_TYPE_TEXT;
			f.visible     = !p.secret;
			f.ofxName     = p.name;
			f.ofxType     = p.type;
			f.isText      = true;
			f.textDefault = p.stringDefault;
			table.push_back( std::move( f ) );
			continue;
		}

		if( p.type == kPush )
		{
			FfglParam f;
			f.name        = p.name;
			f.displayName = label;
			f.group       = group;
			f.ffglType    = FF_TYPE_EVENT;
			f.visible     = !p.secret;
			f.ofxName     = p.name;
			f.ofxType     = p.type;
			table.push_back( std::move( f ) );
			continue;
		}

		if( p.type == kBoolean )
		{
			FfglParam f;
			f.name         = p.name;
			f.displayName  = label;
			f.group        = group;
			f.ffglType     = FF_TYPE_BOOLEAN;
			f.defaultValue = p.boolDefault ? 1.0f : 0.0f;
			f.visible      = !p.secret;
			f.ofxName      = p.name;
			f.ofxType      = p.type;
			table.push_back( std::move( f ) );
			continue;
		}

		if( p.type == kChoice )
		{
			FfglParam f;
			f.name         = p.name;
			f.displayName  = label;
			f.group        = group;
			f.ffglType     = FF_TYPE_OPTION;
			f.elements     = p.choices;
			f.defaultValue = (float)at( p.defaults, 0, 0.0 );
			f.rangeMin     = 0.0f;
			f.rangeMax     = p.choices.empty() ? 1.0f : (float)( p.choices.size() - 1 );
			f.visible      = !p.secret;
			f.ofxName      = p.name;
			f.ofxType      = p.type;
			table.push_back( std::move( f ) );
			continue;
		}

		// Numeric, possibly multi-component.
		const bool isInt = ( p.type == kInteger || p.type == kInteger2D || p.type == kInteger3D );
		const int dims   = std::max( 1, p.dimension );

		for( int c = 0; c < dims; ++c )
		{
			FfglParam f;
			// Suffix the *identifier* too: FFGL param names must be unique, and
			// this keeps serialisation stable per component.
			f.name        = dims > 1 ? p.name + "." + std::to_string( c ) : p.name;
			f.displayName = label + suffixFor( p.type, c );
			f.group       = group;
			f.visible     = !p.secret;
			f.ofxName     = p.name;
			f.ofxType     = p.type;
			f.component   = c;

			if( p.type == kRGB || p.type == kRGBA )
			{
				f.ffglType = colourTypeFor( p.type, c );
				f.rangeMin = 0.0f;
				f.rangeMax = 1.0f;
			}
			else
			{
				f.ffglType = isInt ? FF_TYPE_INTEGER : FF_TYPE_STANDARD;
				chooseRange( p, c, f.rangeMin, f.rangeMax );
			}

			f.defaultValue = (float)at( p.defaults, c, 0.0 );
			table.push_back( std::move( f ) );
		}
	}

	return table;
}

std::string makePluginId( const std::string& ofxIdentifier )
{
	// FNV-1a, then base-62. Any stable hash would do; what matters is that the
	// same OFX plugin always yields the same FFGL id on every machine.
	uint64_t hash = 1469598103934665603ull;
	for( unsigned char c : ofxIdentifier )
	{
		hash ^= c;
		hash *= 1099511628211ull;
	}

	static const char alphabet[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
	std::string id( 4, '0' );
	for( int i = 0; i < 4; ++i )
	{
		id[ i ] = alphabet[ hash % 62 ];
		hash /= 62;
	}
	return id;
}

} // namespace ofxffgl
