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
//   ofxprobe --edit name=value     like --set, but delivered as a user edit with
//                                  kOfxActionInstanceChanged, so param-driven
//                                  behaviour (presets) actually runs (repeatable)
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

/// "0.5" or "1,0.72,0.2" — multi-component params take a comma list.
std::vector< double > parseValues( const char* text )
{
	std::vector< double > values;
	const char* p = text;
	char* end     = nullptr;
	for( ;; )
	{
		values.push_back( strtod( p, &end ) );
		if( end == nullptr || *end != ',' )
			break;
		p = end + 1;
	}
	return values;
}

void usage()
{
	printf( "usage: ofxprobe [--dir PATH]... [--json] [--manifest IDENTIFIER] [--render IDENTIFIER]\n"
			"                [--set name=value]... [--set-string name=value]...\n                [--edit name=value]... [--size WxH] [--out FILE.bmp] [--quiet]\n" );
}

void put32( std::vector< uint8_t >& v, uint32_t x )
{
	v.push_back( (uint8_t)( x ) );
	v.push_back( (uint8_t)( x >> 8 ) );
	v.push_back( (uint8_t)( x >> 16 ) );
	v.push_back( (uint8_t)( x >> 24 ) );
}

/// Write the input and output frames side by side as a 24-bit BMP, input on
/// the left. BMP because it needs no libraries and macOS `sips` converts it to
/// PNG. Frames are RGBA with row 0 at the bottom (the OFX convention), which
/// is also BMP's row order, so rows copy straight across.
bool writeComparisonBmp( const std::string& path,
						 const ofxbridge::Frame& in,
						 const ofxbridge::Frame& out,
						 int width, int height )
{
	const int gap    = 8;
	const int outW   = width * 2 + gap;
	const int stride = ( outW * 3 + 3 ) & ~3;

	std::vector< uint8_t > pixels( (size_t)stride * height, 24 );

	for( int y = 0; y < height; ++y )
	{
		uint8_t* row = pixels.data() + (size_t)y * stride;
		for( int x = 0; x < width; ++x )
		{
			const uint8_t* l = in.data.data() + (size_t)y * in.rowBytes + (size_t)x * 4;
			uint8_t* d       = row + (size_t)x * 3;
			d[ 0 ] = l[ 2 ];
			d[ 1 ] = l[ 1 ];
			d[ 2 ] = l[ 0 ];

			const uint8_t* r = out.data.data() + (size_t)y * out.rowBytes + (size_t)x * 4;
			uint8_t* d2      = row + (size_t)( x + width + gap ) * 3;
			d2[ 0 ] = r[ 2 ];
			d2[ 1 ] = r[ 1 ];
			d2[ 2 ] = r[ 0 ];
		}
	}

	std::vector< uint8_t > header;
	header.push_back( 'B' );
	header.push_back( 'M' );
	put32( header, (uint32_t)( 14 + 40 + pixels.size() ) );
	put32( header, 0 );
	put32( header, 14 + 40 );
	put32( header, 40 );
	put32( header, (uint32_t)outW );
	put32( header, (uint32_t)height );
	header.push_back( 1 );
	header.push_back( 0 );// planes
	header.push_back( 24 );
	header.push_back( 0 );// bpp
	put32( header, 0 );   // BI_RGB
	put32( header, (uint32_t)pixels.size() );
	put32( header, 2835 );
	put32( header, 2835 );
	put32( header, 0 );
	put32( header, 0 );

	FILE* f = fopen( path.c_str(), "wb" );
	if( f == nullptr )
		return false;
	fwrite( header.data(), 1, header.size(), f );
	fwrite( pixels.data(), 1, pixels.size(), f );
	fclose( f );
	return true;
}

/// Instantiate a plugin and push one frame through it, entirely on the CPU.
///
/// This exercises the same host code the FFGL wrapper uses, minus OpenGL, so a
/// render failure can be attributed without involving Resolume or a GPU.
int renderTest( const std::vector< ofxbridge::PluginDesc >& plugins,
				const std::string& identifier,
				const std::vector< std::pair< std::string, std::vector< double > > >& overrides,
				const std::vector< std::pair< std::string, std::vector< double > > >& edits,
				const std::vector< std::pair< std::string, std::string > >& stringOverrides,
				int width, int height,
				const std::string& outPath )
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

	auto printValues = []( const std::vector< double >& v ) {
		for( size_t i = 0; i < v.size(); ++i )
			printf( "%s%g", i ? "," : "", v[ i ] );
	};

	// String parameters first: a plugin that loads a file off one of these
	// generally needs it before anything numeric means very much, and a sheet
	// or a font that has not been chosen makes every other setting look dead.
	for( const auto& kv : stringOverrides )
	{
		if( effect->setParamString( kv.first, kv.second ) )
			printf( "  set %s = \"%s\"\n", kv.first.c_str(), kv.second.c_str() );
		else
			fprintf( stderr, "  WARNING: no string parameter named '%s'\n", kv.first.c_str() );
	}

	// Setting parameters here proves the same path the FFGL wrapper uses:
	// Effect::setParamValue -> the concrete param instance -> what render sees.
	for( const auto& kv : overrides )
	{
		if( effect->setParamValue( kv.first, kv.second ) )
		{
			printf( "  set %s = ", kv.first.c_str() );
			printValues( kv.second );
			printf( "\n" );
		}
		else
			fprintf( stderr, "  WARNING: no numeric parameter named '%s'\n", kv.first.c_str() );
	}

	// Edits go through kOfxActionInstanceChanged as well, the way a host UI
	// would deliver them, so behaviour a plugin hangs off changedParam — a
	// preset choice writing the other params — actually runs. Anything the
	// plugin writes back is reported, because that is usually the point.
	effect->onParamChangedByPlugin = [ & ]( const std::string& name ) {
		std::vector< double > v;
		if( effect->getParamValue( name, v ) && !v.empty() )
			printf( "  plugin set %s = %g\n", name.c_str(), v[ 0 ] );
		else
			printf( "  plugin set %s\n", name.c_str() );
	};
	for( const auto& kv : edits )
	{
		if( effect->editParamValue( kv.first, kv.second ) )
		{
			printf( "  edit %s = ", kv.first.c_str() );
			printValues( kv.second );
			printf( "\n" );
		}
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

	if( !outPath.empty() )
	{
		if( writeComparisonBmp( outPath, in, out, width, height ) )
			printf( "  wrote %s (input | output)\n", outPath.c_str() );
		else
		{
			fprintf( stderr, "could not write %s\n", outPath.c_str() );
			return 1;
		}
	}

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
	std::vector< std::pair< std::string, std::vector< double > > > overrides;
	std::vector< std::pair< std::string, std::vector< double > > > edits;
	std::vector< std::pair< std::string, std::string > > stringOverrides;
	int renderWidth  = 64;
	int renderHeight = 32;
	std::string outPath;

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
			overrides.emplace_back( kv.substr( 0, eq ), parseValues( kv.c_str() + eq + 1 ) );
		}
		else if( a == "--set-string" && i + 1 < argc )
		{
			const std::string kv = argv[ ++i ];
			const size_t eq      = kv.find( '=' );
			if( eq == std::string::npos )
			{
				fprintf( stderr, "--set-string expects name=value, got '%s'\n", kv.c_str() );
				return 2;
			}
			stringOverrides.emplace_back( kv.substr( 0, eq ), kv.substr( eq + 1 ) );
		}
		else if( a == "--edit" && i + 1 < argc )
		{
			const std::string kv = argv[ ++i ];
			const size_t eq      = kv.find( '=' );
			if( eq == std::string::npos )
			{
				fprintf( stderr, "--edit expects name=value, got '%s'\n", kv.c_str() );
				return 2;
			}
			edits.emplace_back( kv.substr( 0, eq ), parseValues( kv.c_str() + eq + 1 ) );
		}
		else if( a == "--size" && i + 1 < argc )
		{
			const std::string wh = argv[ ++i ];
			const size_t x       = wh.find( 'x' );
			if( x == std::string::npos || sscanf( wh.c_str(), "%dx%d", &renderWidth, &renderHeight ) != 2
				|| renderWidth < 1 || renderHeight < 1 )
			{
				fprintf( stderr, "--size expects WxH, got '%s'\n", wh.c_str() );
				return 2;
			}
		}
		else if( a == "--out" && i + 1 < argc )
			outPath = argv[ ++i ];
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
		return renderTest( plugins, wantRenderFor, overrides, edits, stringOverrides, renderWidth, renderHeight, outPath );

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
		printf( "  opencl     : %s\n", p.supportsOpenCLRender ? "yes" : "no" );
		printf( "  cuda       : %s%s\n", p.supportsCudaRender ? "yes" : "no",
				p.supportsCudaRender ? "  (bridge UNVERIFIED -- see docs/04)" : "" );
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
