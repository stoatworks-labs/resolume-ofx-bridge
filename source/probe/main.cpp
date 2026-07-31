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
//

#include "../ofxbridge/Catalog.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

void usage()
{
	printf( "usage: ofxprobe [--dir PATH]... [--json] [--manifest IDENTIFIER] [--quiet]\n" );
}

} // namespace

int main( int argc, char** argv )
{
	std::vector< std::string > extraDirs;
	bool wantJson  = false;
	bool quiet     = false;
	std::string wantManifestFor;

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

	if( !quiet && wantManifestFor.empty() && !wantJson )
		fputs( log.c_str(), stderr );

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
