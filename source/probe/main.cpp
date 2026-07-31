//
// ofxprobe - load OFX bundles and dump what they contain.
//
// This is the introspection half of the generator, exposed as a CLI so that a
// plugin can be inspected (and a crash attributed) without going anywhere near
// Resolume.
//
//   ofxprobe                       scan the conventional OFX directories
//   ofxprobe --dir <path>          also scan <path> (repeatable)
//   ofxprobe --json                emit the manifest JSON for every plugin
//   ofxprobe --manifest <id>       emit the manifest for one plugin identifier
//   ofxprobe --render <id>         instantiate and render one frame (CPU)
//   ofxprobe --set name=value      set an OFX parameter before rendering (repeatable)
//

#include "../ofxbridge/Catalog.h"
#include "../ofxbridge/Host.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <memory>
#include <utility>
#include <string>
#include <vector>

namespace {

void usage()
{
	printf( "usage: ofxprobe [--dir PATH]... [--json] [--manifest IDENTIFIER] [--render IDENTIFIER] [--quiet]\n" );
}

/// Instantiate a plugin and push one frame through it, entirely on the CPU.
///
/// This exercises the same host code the FFGL wrapper uses, minus OpenGL, so a
/// render failure can be attributed without involving Resolume or a GPU.
int renderTest( const std::vector< ofxbridge::PluginDesc >& plugins,
				const std::string& identifier,
				const std::vector< std::pair< std::string, double > >& overrides )
{
	const ofxbridge::PluginDesc* target = nullptr;
	for( const auto& p : plugins )
		if( p.identifier == identifier )
			target = &p;

	if( target == nullptr )
	{
		fprintf( stderr, "no plugin with identifier '%s'\n", identifier.c_str() );
		return 1;
	}
	if( !target->error.empty() )
	{
		fprintf( stderr, "plugin is not usable: %s\n", target->error.c_str() );
		return 1;
	}

	const int width  = 64;
	const int height = 32;

	ofxbridge::Host host;
	std::string error;
	std::unique_ptr< ofxbridge::Effect > effect =
		ofxbridge::createEffect( host, target->bundlePath, target->identifier, error );
	if( !effect )
	{
		fprintf( stderr, "createEffect failed: %s\n", error.c_str() );
		return 1;
	}

	effect->setFrameSize( width, height );
	if( !effect->init( error ) )
	{
		fprintf( stderr, "init failed: %s\n", error.c_str() );
		for( const auto& m : effect->messages() )
			fprintf( stderr, "  %s\n", m.c_str() );
		return 1;
	}

	// Setting parameters here proves the same path the FFGL wrapper uses:
	// Effect::setParamValue -> the concrete param instance -> what render sees.
	for( const auto& kv : overrides )
	{
		if( effect->setParamValue( kv.first, { kv.second } ) )
			printf( "  set %s = %g\n", kv.first.c_str(), kv.second );
		else
			fprintf( stderr, "  WARNING: no numeric parameter named '%s'\n", kv.first.c_str() );
	}

	ofxbridge::Frame in, out;
	in.allocate( width, height, false );
	out.allocate( width, height, false );

	// A horizontal ramp, so a wrong stride or row order is visible rather than
	// averaging out to something plausible.
	for( int y = 0; y < height; ++y )
	{
		for( int x = 0; x < width; ++x )
		{
			uint8_t* px = in.data.data() + (size_t)y * in.rowBytes + (size_t)x * 4;
			px[ 0 ] = (uint8_t)( x * 4 );
			px[ 1 ] = (uint8_t)( y * 8 );
			px[ 2 ] = 128;
			px[ 3 ] = 255;
		}
	}

	if( !effect->render( in, out, 0.0, error ) )
	{
		fprintf( stderr, "render failed: %s\n", error.c_str() );
		for( const auto& m : effect->messages() )
			fprintf( stderr, "  plugin said: %s\n", m.c_str() );
		return 1;
	}

	const uint8_t* i0 = in.data.data();
	const uint8_t* o0 = out.data.data();
	const size_t mid  = (size_t)( height / 2 ) * out.rowBytes + (size_t)( width / 2 ) * 4;

	printf( "rendered %dx%d through %s\n", width, height, identifier.c_str() );
	printf( "  in  [0,0]      RGBA %3u %3u %3u %3u\n", i0[ 0 ], i0[ 1 ], i0[ 2 ], i0[ 3 ] );
	printf( "  out [0,0]      RGBA %3u %3u %3u %3u\n", o0[ 0 ], o0[ 1 ], o0[ 2 ], o0[ 3 ] );
	printf( "  in  [centre]   RGBA %3u %3u %3u %3u\n", in.data[ mid ], in.data[ mid + 1 ], in.data[ mid + 2 ],
			in.data[ mid + 3 ] );
	printf( "  out [centre]   RGBA %3u %3u %3u %3u\n", out.data[ mid ], out.data[ mid + 1 ], out.data[ mid + 2 ],
			out.data[ mid + 3 ] );

	size_t changed = 0;
	for( size_t i = 0; i < out.data.size(); ++i )
		if( out.data[ i ] != in.data[ i ] )
			++changed;
	printf( "  %zu of %zu bytes differ from the input\n", changed, out.data.size() );

	for( const auto& m : effect->messages() )
		printf( "  plugin said: %s\n", m.c_str() );

	return 0;
}

} // namespace

int main( int argc, char** argv )
{
	std::vector< std::string > extraDirs;
	bool wantJson  = false;
	bool quiet     = false;
	std::string wantManifestFor;
	std::string wantRenderFor;
	std::vector< std::pair< std::string, double > > overrides;

	for( int i = 1; i < argc; ++i )
	{
		const std::string a = argv[ i ];
		if( a == "--dir" && i + 1 < argc )
			extraDirs.push_back( argv[ ++i ] );
		else if( a == "--json" )
			wantJson = true;
		else if( a == "--quiet" )
			quiet = true;
		else if( a == "--manifest" && i + 1 < argc )
			wantManifestFor = argv[ ++i ];
		else if( a == "--render" && i + 1 < argc )
			wantRenderFor = argv[ ++i ];
		else if( a == "--set" && i + 1 < argc )
		{
			const std::string kv = argv[ ++i ];
			const size_t eq      = kv.find( '=' );
			if( eq == std::string::npos )
			{
				fprintf( stderr, "--set expects name=value, got '%s'\n", kv.c_str() );
				return 2;
			}
			overrides.emplace_back( kv.substr( 0, eq ), atof( kv.c_str() + eq + 1 ) );
		}
		else if( a == "-h" || a == "--help" )
		{
			usage();
			return 0;
		}
		else
		{
			fprintf( stderr, "unknown argument: %s\n", a.c_str() );
			usage();
			return 2;
		}
	}

	std::vector< std::string > paths = ofxbridge::defaultSearchPaths();
	paths.insert( paths.end(), extraDirs.begin(), extraDirs.end() );

	std::string log;
	std::vector< ofxbridge::PluginDesc > plugins = ofxbridge::scanAndDescribe( paths, log );

	if( !quiet && wantManifestFor.empty() && wantRenderFor.empty() && !wantJson )
		fputs( log.c_str(), stderr );

	if( !wantRenderFor.empty() )
		return renderTest( plugins, wantRenderFor, overrides );

	if( !wantManifestFor.empty() )
	{
		for( const auto& p : plugins )
		{
			if( p.identifier == wantManifestFor )
			{
				if( !p.error.empty() )
				{
					fprintf( stderr, "plugin %s is not usable: %s\n", p.identifier.c_str(), p.error.c_str() );
					return 1;
				}
				fputs( ofxbridge::toManifestJson( p ).c_str(), stdout );
				return 0;
			}
		}
		fprintf( stderr, "no plugin with identifier '%s'\n", wantManifestFor.c_str() );
		return 1;
	}

	if( wantJson )
	{
		printf( "[\n" );
		bool first = true;
		for( const auto& p : plugins )
		{
			if( !p.error.empty() )
				continue;
			if( !first )
				printf( ",\n" );
			first = false;
			fputs( ofxbridge::toManifestJson( p ).c_str(), stdout );
		}
		printf( "]\n" );
		return 0;
	}

	// Human-readable summary.
	int usable = 0;
	for( const auto& p : plugins )
	{
		printf( "\n%s\n", p.identifier.c_str() );
		printf( "  label      : %s\n", p.label.c_str() );
		printf( "  grouping   : %s\n", p.grouping.c_str() );
		printf( "  version    : %d.%d\n", p.versionMajor, p.versionMinor );
		printf( "  bundle     : %s\n", p.bundlePath.c_str() );
		printf( "  gl render  : %s\n", p.supportsOpenGLRender ? "yes" : "no" );
		printf( "  metal      : %s\n", p.supportsMetalRender ? "yes" : "no" );
		printf( "  contexts   : " );
		for( const auto& c : p.contexts )
			printf( "%s ", c.c_str() );
		printf( "\n" );

		if( !p.error.empty() )
		{
			printf( "  UNUSABLE   : %s\n", p.error.c_str() );
			continue;
		}
		++usable;

		printf( "  parameters : %zu\n", p.params.size() );
		for( const auto& pd : p.params )
		{
			printf( "    %-24s %-22s %s", pd.name.c_str(), pd.type.c_str(), pd.label.c_str() );
			if( !pd.choices.empty() )
			{
				printf( "  [" );
				for( size_t i = 0; i < pd.choices.size(); ++i )
					printf( "%s%s", i ? "|" : "", pd.choices[ i ].c_str() );
				printf( "]" );
			}
			else if( pd.hasDisplayRange && !pd.displayMin.empty() )
			{
				printf( "  (%g..%g)", pd.displayMin[ 0 ], pd.displayMax[ 0 ] );
			}
			if( pd.secret )
				printf( "  [secret]" );
			printf( "\n" );
		}
	}

	printf( "\n%d of %zu plugin(s) usable as Resolume effects\n", usable, plugins.size() );
	return 0;
}
