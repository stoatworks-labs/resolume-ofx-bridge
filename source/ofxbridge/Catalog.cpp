#include "Catalog.h"
#include "Host.h"

#include "ofxParam.h"
#include "ofxImageEffect.h"
#include "ofxGPURender.h"

#include <cstdlib>
#include <map>
#include <memory>
#include <sstream>

namespace ofxbridge {

namespace {

/// Read up to `n` components of a double property, tolerating absent props.
std::vector< double > readDoubles( const OFX::Host::Property::Set& props, const char* name, int n )
{
	std::vector< double > out;
	for( int i = 0; i < n; ++i )
	{
		if( props.fetchProperty( name ) == nullptr )
			break;
		out.push_back( props.getDoubleProperty( name, i ) );
	}
	return out;
}

std::vector< double > readInts( const OFX::Host::Property::Set& props, const char* name, int n )
{
	std::vector< double > out;
	for( int i = 0; i < n; ++i )
	{
		if( props.fetchProperty( name ) == nullptr )
			break;
		out.push_back( (double)props.getIntProperty( name, i ) );
	}
	return out;
}

/// How many components a given OFX param type carries.
int dimensionOf( const std::string& type )
{
	if( type == kOfxParamTypeDouble2D || type == kOfxParamTypeInteger2D )
		return 2;
	if( type == kOfxParamTypeDouble3D || type == kOfxParamTypeInteger3D || type == kOfxParamTypeRGB )
		return 3;
	if( type == kOfxParamTypeRGBA )
		return 4;
	return 1;
}

bool isDoubleFlavoured( const std::string& type )
{
	return type == kOfxParamTypeDouble || type == kOfxParamTypeDouble2D || type == kOfxParamTypeDouble3D ||
		   type == kOfxParamTypeRGB || type == kOfxParamTypeRGBA;
}

bool isIntFlavoured( const std::string& type )
{
	return type == kOfxParamTypeInteger || type == kOfxParamTypeInteger2D || type == kOfxParamTypeInteger3D;
}

std::string jsonEscape( const std::string& s )
{
	std::string out;
	out.reserve( s.size() + 8 );
	for( char c : s )
	{
		switch( c )
		{
		case '"': out += "\\\""; break;
		case '\\': out += "\\\\"; break;
		case '\n': out += "\\n"; break;
		case '\r': out += "\\r"; break;
		case '\t': out += "\\t"; break;
		default:
			if( (unsigned char)c < 0x20 )
			{
				char buf[ 8 ];
				snprintf( buf, sizeof( buf ), "\\u%04x", c );
				out += buf;
			}
			else
			{
				out += c;
			}
		}
	}
	return out;
}

void appendDoubleArray( std::ostringstream& o, const char* key, const std::vector< double >& v )
{
	o << "\"" << key << "\":[";
	for( size_t i = 0; i < v.size(); ++i )
	{
		if( i )
			o << ",";
		o << v[ i ];
	}
	o << "]";
}

} // namespace

std::vector< std::string > defaultSearchPaths()
{
	std::vector< std::string > paths;

#if defined( __APPLE__ )
	paths.push_back( "/Library/OFX/Plugins" );
	if( const char* home = std::getenv( "HOME" ) )
		paths.push_back( std::string( home ) + "/Library/OFX/Plugins" );
#elif defined( _WIN32 )
	paths.push_back( "C:\\Program Files\\Common Files\\OFX\\Plugins" );
#else
	paths.push_back( "/usr/OFX/Plugins" );
	paths.push_back( "/usr/local/OFX/Plugins" );
#endif

	// OFX_PLUGIN_PATH is the spec-defined override and takes priority in hosts
	// that honour it, so we append it too.
	if( const char* env = std::getenv( "OFX_PLUGIN_PATH" ) )
	{
#if defined( _WIN32 )
		const char sep = ';';
#else
		const char sep = ':';
#endif
		std::stringstream ss( env );
		std::string item;
		while( std::getline( ss, item, sep ) )
			if( !item.empty() )
				paths.push_back( item );
	}
	return paths;
}

std::vector< PluginDesc > scanAndDescribe( const std::vector< std::string >& searchPaths, std::string& log )
{
	std::vector< PluginDesc > results;
	std::ostringstream logs;

	static Host host;
	static OFX::Host::ImageEffect::PluginCache imageEffectCache( host );

	OFX::Host::PluginCache cache;
	cache.registerAPICache( kOfxImageEffectPluginApi, 1, 1, &imageEffectCache );
	for( const auto& p : searchPaths )
	{
		logs << "search path: " << p << "\n";
		cache.addFileToPath( p );
	}
	cache.scanPluginFiles();

	const auto& plugins = imageEffectCache.getPlugins();
	logs << "found " << plugins.size() << " image-effect plugin(s)\n";

	for( auto* plugin : plugins )
	{
		PluginDesc desc;
		desc.identifier   = plugin->getIdentifier();
		desc.versionMajor = plugin->getVersionMajor();
		desc.versionMinor = plugin->getVersionMinor();
		desc.indexInBundle = plugin->getIndex();
		if( plugin->getBinary() )
			desc.bundlePath = plugin->getBinary()->getBundlePath();

		// Describing runs plugin code and can fail or throw.
		OFX::Host::ImageEffect::Descriptor* rootDesc = nullptr;
		try
		{
			rootDesc = &plugin->getDescriptor();
		}
		catch( const std::exception& e )
		{
			desc.error = std::string( "describe threw: " ) + e.what();
		}
		catch( ... )
		{
			desc.error = "describe threw an unknown exception";
		}

		if( rootDesc == nullptr && desc.error.empty() )
			desc.error = "plugin returned no descriptor (it may have refused to load in this host)";

		if( !desc.error.empty() )
		{
			logs << "  FAIL " << desc.identifier << ": " << desc.error << "\n";
			results.push_back( desc );
			continue;
		}

		const auto& rootProps = rootDesc->getProps();
		desc.label    = rootDesc->getLabel();
		desc.grouping = rootProps.getStringProperty( kOfxImageEffectPluginPropGrouping );
		desc.supportsOpenGLRender =
			rootProps.getStringProperty( kOfxImageEffectPropOpenGLRenderSupported ) == "true";
		desc.supportsMetalRender =
			rootProps.getStringProperty( kOfxImageEffectPropMetalRenderSupported ) == "true";

		// Which contexts does it offer? We can only host Filter.
		{
			const OFX::Host::Property::Property* p = rootProps.fetchProperty( kOfxImageEffectPropSupportedContexts );
			const int n = p ? p->getDimension() : 0;
			for( int i = 0; i < n; ++i )
			{
				std::string ctx = rootProps.getStringProperty( kOfxImageEffectPropSupportedContexts, i );
				desc.contexts.push_back( ctx );
				if( ctx == kOfxImageEffectContextFilter )
					desc.supportsFilter = true;
			}
		}

		if( !desc.supportsFilter )
		{
			desc.error = "plugin does not support the Filter context";
			logs << "  SKIP " << desc.identifier << ": " << desc.error << "\n";
			results.push_back( desc );
			continue;
		}

		// Describe-in-context is what actually populates the parameter set.
		OFX::Host::ImageEffect::Descriptor* ctxDesc = nullptr;
		try
		{
			ctxDesc = plugin->getContext( kOfxImageEffectContextFilter );
		}
		catch( const std::exception& e )
		{
			desc.error = std::string( "describeInContext threw: " ) + e.what();
		}
		catch( ... )
		{
			desc.error = "describeInContext threw an unknown exception";
		}

		if( ctxDesc == nullptr )
		{
			if( desc.error.empty() )
				desc.error = "describeInContext produced no descriptor";
			logs << "  FAIL " << desc.identifier << ": " << desc.error << "\n";
			results.push_back( desc );
			continue;
		}

		for( auto* p : ctxDesc->getParamList() )
		{
			if( p == nullptr )
				continue;

			ParamDesc pd;
			pd.type  = p->getType();
			pd.name  = p->getName();
			pd.label = p->getLabel();

			const auto& props = p->getProperties();
			pd.hint    = props.getStringProperty( kOfxParamPropHint );
			pd.parent  = props.getStringProperty( kOfxParamPropParent );
			pd.secret  = props.getIntProperty( kOfxParamPropSecret ) != 0;
			pd.enabled = props.getIntProperty( kOfxParamPropEnabled ) != 0;
			pd.dimension = dimensionOf( pd.type );

			if( isDoubleFlavoured( pd.type ) )
			{
				pd.defaults   = readDoubles( props, kOfxParamPropDefault, pd.dimension );
				pd.hardMin    = readDoubles( props, kOfxParamPropMin, pd.dimension );
				pd.hardMax    = readDoubles( props, kOfxParamPropMax, pd.dimension );
				pd.displayMin = readDoubles( props, kOfxParamPropDisplayMin, pd.dimension );
				pd.displayMax = readDoubles( props, kOfxParamPropDisplayMax, pd.dimension );
			}
			else if( isIntFlavoured( pd.type ) )
			{
				pd.defaults   = readInts( props, kOfxParamPropDefault, pd.dimension );
				pd.hardMin    = readInts( props, kOfxParamPropMin, pd.dimension );
				pd.hardMax    = readInts( props, kOfxParamPropMax, pd.dimension );
				pd.displayMin = readInts( props, kOfxParamPropDisplayMin, pd.dimension );
				pd.displayMax = readInts( props, kOfxParamPropDisplayMax, pd.dimension );
			}
			else if( pd.type == kOfxParamTypeBoolean )
			{
				pd.boolDefault = props.getIntProperty( kOfxParamPropDefault ) != 0;
			}
			else if( pd.type == kOfxParamTypeChoice )
			{
				const OFX::Host::Property::Property* opts = props.fetchProperty( kOfxParamPropChoiceOption );
				const int n = opts ? opts->getDimension() : 0;
				for( int i = 0; i < n; ++i )
					pd.choices.push_back( props.getStringProperty( kOfxParamPropChoiceOption, i ) );
				pd.defaults.push_back( (double)props.getIntProperty( kOfxParamPropDefault ) );
			}
			else if( pd.type == kOfxParamTypeString )
			{
				pd.stringDefault = props.getStringProperty( kOfxParamPropDefault );
			}

			pd.hasDisplayRange = !pd.displayMin.empty() && !pd.displayMax.empty();
			desc.params.push_back( pd );
		}

		logs << "  OK   " << desc.identifier << " (" << desc.label << ") "
			 << desc.params.size() << " param(s)\n";
		results.push_back( desc );
	}

	log = logs.str();
	return results;
}

std::string toManifestJson( const PluginDesc& p )
{
	std::ostringstream o;
	o << "{\n";
	o << "  \"manifestVersion\": 1,\n";
	o << "  \"identifier\": \"" << jsonEscape( p.identifier ) << "\",\n";
	o << "  \"label\": \"" << jsonEscape( p.label ) << "\",\n";
	o << "  \"grouping\": \"" << jsonEscape( p.grouping ) << "\",\n";
	o << "  \"versionMajor\": " << p.versionMajor << ",\n";
	o << "  \"versionMinor\": " << p.versionMinor << ",\n";
	o << "  \"bundlePath\": \"" << jsonEscape( p.bundlePath ) << "\",\n";
	o << "  \"indexInBundle\": " << p.indexInBundle << ",\n";
	o << "  \"supportsOpenGLRender\": " << ( p.supportsOpenGLRender ? "true" : "false" ) << ",\n";
	o << "  \"supportsMetalRender\": " << ( p.supportsMetalRender ? "true" : "false" ) << ",\n";
	o << "  \"params\": [\n";

	for( size_t i = 0; i < p.params.size(); ++i )
	{
		const ParamDesc& pd = p.params[ i ];
		o << "    {";
		o << "\"name\":\"" << jsonEscape( pd.name ) << "\",";
		o << "\"label\":\"" << jsonEscape( pd.label ) << "\",";
		o << "\"type\":\"" << jsonEscape( pd.type ) << "\",";
		o << "\"parent\":\"" << jsonEscape( pd.parent ) << "\",";
		o << "\"hint\":\"" << jsonEscape( pd.hint ) << "\",";
		o << "\"secret\":" << ( pd.secret ? "true" : "false" ) << ",";
		o << "\"dimension\":" << pd.dimension << ",";
		appendDoubleArray( o, "defaults", pd.defaults );
		o << ",";
		appendDoubleArray( o, "displayMin", pd.displayMin );
		o << ",";
		appendDoubleArray( o, "displayMax", pd.displayMax );
		o << ",";
		appendDoubleArray( o, "hardMin", pd.hardMin );
		o << ",";
		appendDoubleArray( o, "hardMax", pd.hardMax );
		o << ",";
		o << "\"boolDefault\":" << ( pd.boolDefault ? "true" : "false" ) << ",";
		o << "\"stringDefault\":\"" << jsonEscape( pd.stringDefault ) << "\",";
		o << "\"choices\":[";
		for( size_t c = 0; c < pd.choices.size(); ++c )
		{
			if( c )
				o << ",";
			o << "\"" << jsonEscape( pd.choices[ c ] ) << "\"";
		}
		o << "]";
		o << "}";
		if( i + 1 < p.params.size() )
			o << ",";
		o << "\n";
	}

	o << "  ]\n}\n";
	return o.str();
}

std::unique_ptr< Effect > createEffect( Host& host,
										const std::string& bundlePath,
										const std::string& identifier,
										std::string& error )
{
	// The plugin cache owns the loaded binary and every descriptor hanging off
	// it, so it must outlive the instance. One cache per bundle path is kept for
	// the life of the process; a wrapper only ever needs one.
	//
	// KNOWN LIMITATION: addFileToPath only takes directories, so this loads and
	// describes *every* bundle sitting beside the one we want, inside a live video
	// process. That is wasteful and means a broken neighbour can misbehave during
	// a show. Fixing it properly needs a bundle-scoped load path in HostSupport.
	static std::map< std::string, std::unique_ptr< OFX::Host::ImageEffect::PluginCache > > caches;
	static std::map< std::string, std::unique_ptr< OFX::Host::PluginCache > > binaryCaches;

	auto it = caches.find( bundlePath );
	if( it == caches.end() )
	{
		auto ieCache = std::make_unique< OFX::Host::ImageEffect::PluginCache >( host );
		auto cache   = std::make_unique< OFX::Host::PluginCache >();
		cache->registerAPICache( kOfxImageEffectPluginApi, 1, 1, ieCache.get() );
		// addFileToPath takes a directory to scan, not a bundle, so point it at the
		// bundle's parent and rely on the identifier match below to pick ours out.
		const size_t slash = bundlePath.find_last_of( "/\\" );
		cache->addFileToPath( slash == std::string::npos ? bundlePath : bundlePath.substr( 0, slash ) );
		cache->scanPluginFiles();

		binaryCaches[ bundlePath ] = std::move( cache );
		it = caches.emplace( bundlePath, std::move( ieCache ) ).first;
	}

	OFX::Host::ImageEffect::ImageEffectPlugin* found = nullptr;
	for( auto* p : it->second->getPlugins() )
	{
		if( p->getIdentifier() == identifier )
		{
			found = p;
			break;
		}
	}

	if( found == nullptr )
	{
		error = "bundle " + bundlePath + " does not contain plugin " + identifier;
		return nullptr;
	}

	OFX::Host::ImageEffect::Instance* instance = nullptr;
	try
	{
		if( found->getContext( kOfxImageEffectContextFilter ) == nullptr )
		{
			error = identifier + " does not support the Filter context";
			return nullptr;
		}
		instance = found->createInstance( kOfxImageEffectContextFilter, nullptr );
	}
	catch( const std::exception& e )
	{
		error = std::string( "creating instance threw: " ) + e.what();
		return nullptr;
	}
	catch( ... )
	{
		error = "creating instance threw an unknown exception";
		return nullptr;
	}

	Effect* effect = dynamic_cast< Effect* >( instance );
	if( effect == nullptr )
	{
		delete instance;
		error = "host produced an unexpected instance type";
		return nullptr;
	}
	return std::unique_ptr< Effect >( effect );
}

} // namespace ofxbridge
