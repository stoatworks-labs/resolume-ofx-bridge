//
// ofxgen - turn discovered OFX plugins into FFGL bundles.
//
// Generation is a copy, not a compile. Each output bundle is the prebuilt
// ofxwrapper binary plus a manifest describing one OFX plugin; the wrapper reads
// that manifest when the host loads it. See source/ffgl/PluginMain.cpp.
//
//   ofxgen list    [--dir PATH]...
//   ofxgen generate --out DIR [--dir PATH]... [--template PATH] [--only ID]
//   ofxgen verify  BUNDLE
//
// `verify` loads a generated bundle exactly as a host would and prints what it
// advertises, so a bundle can be checked without launching Resolume.
//

#include "../ofxbridge/Catalog.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#if !defined( _WIN32 )
#include <dlfcn.h>
#endif

namespace fs = std::filesystem;

namespace {

// FFGL wire protocol, duplicated here so the generator does not have to link the
// FFGL SDK just to interrogate a bundle.
using FFUInt32 = uint32_t;
union FFMixed
{
	FFUInt32 UIntValue;
	void* PointerValue;
};
// FFInstanceID is void*, not a 32-bit id -- getting this wrong corrupts the
// call frame on 64-bit and crashes before plugMain even runs.
using FFInstanceID = void*;
using PlugMainFn   = FFMixed ( * )( FFUInt32, FFMixed, FFInstanceID );

constexpr FFUInt32 FF_GET_INFO              = 0;
constexpr FFUInt32 FF_GET_NUM_PARAMETERS    = 4;
constexpr FFUInt32 FF_GET_PARAMETER_NAME    = 5;
constexpr FFUInt32 FF_GET_PARAMETER_DEFAULT = 6;
constexpr FFUInt32 FF_GET_PARAMETER_TYPE    = 15;
constexpr FFUInt32 FF_GET_RANGE             = 41;
constexpr FFUInt32 FF_GET_PARAM_GROUP       = 50;
constexpr FFUInt32 FF_GET_NUM_PARAMETER_ELEMENTS = 31;
constexpr FFUInt32 FF_GET_PARAMETER_ELEMENT_NAME = 35;
constexpr FFUInt32 FF_FAIL                  = 0xFFFFFFFF;

struct GetParameterElementNameStruct
{
	FFUInt32 ParameterNumber;
	FFUInt32 ElementNumber;
};

/// FFGL returns results through a union, and signals "no value" by putting
/// FF_FAIL in the integer member. Reading that back as a pointer yields
/// 0xFFFFFFFF, which is non-null and faults on dereference -- so every pointer
/// result has to be filtered through this.
const char* asString( FFMixed v )
{
	if( v.UIntValue == FF_FAIL || v.PointerValue == nullptr )
		return nullptr;
	return (const char*)v.PointerValue;
}

struct PluginInfoStruct
{
	FFUInt32 APIMajorVersion;
	FFUInt32 APIMinorVersion;
	char PluginUniqueID[ 4 ];
	char PluginName[ 16 ];
	FFUInt32 PluginType;
};

struct RangeStruct
{
	float min;
	float max;
};
struct GetRangeStruct
{
	FFUInt32 parameterNumber;
	RangeStruct range;
};

// Group names are not returned as a pointer: the host supplies the buffer and
// the plugin copies into it, without writing a terminating nul.
struct StringBufferStruct
{
	char* address;
	FFUInt32 maxToWrite;
};
struct GetStringStruct
{
	FFUInt32 parameterNumber;
	StringBufferStruct stringBuffer;
};

const char* ffglTypeName( FFUInt32 t )
{
	switch( t )
	{
	case 0: return "boolean";
	case 1: return "event";
	case 2: return "red";
	case 3: return "green";
	case 4: return "blue";
	case 5: return "xpos";
	case 6: return "ypos";
	case 10: return "standard";
	case 11: return "option";
	case 12: return "buffer";
	case 13: return "integer";
	case 14: return "file";
	case 100: return "text";
	case 203: return "alpha";
	default: return "?";
	}
}

void usage()
{
	printf( "usage:\n"
			"  ofxgen list     [--dir PATH]...\n"
			"  ofxgen generate --out DIR [--dir PATH]... [--template PATH] [--only IDENTIFIER]\n"
			"  ofxgen verify   BUNDLE\n" );
}

/// Turn a plugin label into something safe for a filename.
std::string sanitise( const std::string& in )
{
	std::string out;
	for( char c : in )
	{
		if( ( c >= 'a' && c <= 'z' ) || ( c >= 'A' && c <= 'Z' ) || ( c >= '0' && c <= '9' ) || c == '-' || c == '_' )
			out += c;
		else if( c == ' ' || c == '.' || c == '/' || c == '\\' )
			out += '_';
	}
	if( out.empty() )
		out = "OfxPlugin";
	return out;
}

/// Locate the prebuilt wrapper. Looked up next to the executable first so an
/// installed copy works, then in the build tree for development.
std::string findTemplate( const char* argv0 )
{
	std::vector< fs::path > candidates;
	std::error_code ec;
	const fs::path exe = fs::absolute( fs::path( argv0 ), ec );
	const fs::path dir = exe.parent_path();

#if defined( __APPLE__ )
	const char* leaf = "ofxwrapper.bundle";
#elif defined( _WIN32 )
	const char* leaf = "ofxwrapper.dll";
#else
	const char* leaf = "libofxwrapper.so";
#endif

	candidates.push_back( dir / leaf );
	candidates.push_back( dir / ".." / leaf );
	candidates.push_back( fs::current_path( ec ) / "build" / leaf );

	for( const auto& c : candidates )
		if( fs::exists( c, ec ) )
			return fs::weakly_canonical( c, ec ).string();
	return std::string();
}

int doVerify( const std::string& bundlePath )
{
#if defined( _WIN32 )
	fprintf( stderr, "verify is not implemented on Windows yet\n" );
	(void)bundlePath;
	return 1;
#else
	fs::path binary = bundlePath;
	std::error_code ec;

	// Inside a macOS bundle the loadable object is Contents/MacOS/<name>.
	if( fs::is_directory( binary, ec ) )
	{
		const fs::path macos = binary / "Contents" / "MacOS";
		if( !fs::is_directory( macos, ec ) )
		{
			fprintf( stderr, "%s is not an FFGL bundle\n", bundlePath.c_str() );
			return 1;
		}
		bool found = false;
		for( const auto& e : fs::directory_iterator( macos, ec ) )
		{
			binary = e.path();
			found  = true;
			break;
		}
		if( !found )
		{
			fprintf( stderr, "%s has no executable\n", bundlePath.c_str() );
			return 1;
		}
	}

	void* handle = dlopen( binary.c_str(), RTLD_NOW | RTLD_LOCAL );
	if( handle == nullptr )
	{
		fprintf( stderr, "dlopen failed: %s\n", dlerror() );
		return 1;
	}

	auto plugMain = (PlugMainFn)dlsym( handle, "plugMain" );
	if( plugMain == nullptr )
	{
		fprintf( stderr, "plugMain not found: %s\n", dlerror() );
		dlclose( handle );
		return 1;
	}

	FFMixed zero;
	zero.UIntValue = 0;

	FFMixed info = plugMain( FF_GET_INFO, zero, nullptr );
	if( info.PointerValue == nullptr )
	{
		fprintf( stderr, "plugin returned no info\n" );
		dlclose( handle );
		return 1;
	}
	const PluginInfoStruct* pi = (const PluginInfoStruct*)info.PointerValue;

	printf( "bundle    : %s\n", bundlePath.c_str() );
	printf( "FFGL API  : %u.%u\n", pi->APIMajorVersion, pi->APIMinorVersion );
	printf( "unique ID : %.4s\n", pi->PluginUniqueID );
	printf( "name      : %.16s\n", pi->PluginName );
	printf( "type      : %s\n", pi->PluginType == 0 ? "effect" : ( pi->PluginType == 1 ? "source" : "mixer" ) );

	const FFUInt32 n = plugMain( FF_GET_NUM_PARAMETERS, zero, nullptr ).UIntValue;
	printf( "parameters: %u\n", n );

	for( FFUInt32 i = 0; i < n; ++i )
	{
		FFMixed arg;
		arg.UIntValue = i;

		const char* name = asString( plugMain( FF_GET_PARAMETER_NAME, arg, nullptr ) );
		const FFUInt32 type = plugMain( FF_GET_PARAMETER_TYPE, arg, nullptr ).UIntValue;
		char groupBuf[ 128 ] = {};
		GetStringStruct groupReq{};
		groupReq.parameterNumber       = i;
		groupReq.stringBuffer.address    = groupBuf;
		groupReq.stringBuffer.maxToWrite = (FFUInt32)( sizeof( groupBuf ) - 1 );
		FFMixed groupMixed;
		groupMixed.PointerValue = &groupReq;
		const bool haveGroup    = plugMain( FF_GET_PARAM_GROUP, groupMixed, nullptr ).UIntValue == 0;
		const char* group       = haveGroup ? groupBuf : nullptr;

		GetRangeStruct rangeArg{};
		rangeArg.parameterNumber = i;
		FFMixed rangeMixed;
		rangeMixed.PointerValue = &rangeArg;
		plugMain( FF_GET_RANGE, rangeMixed, nullptr );

		FFMixed def = plugMain( FF_GET_PARAMETER_DEFAULT, arg, nullptr );

		printf( "  %-28s %-9s", name ? name : "?", ffglTypeName( type ) );
		if( type == 100 )
			printf( " text" );
		else
			printf( " default=%-8g range=%g..%g",
					*(float*)&def.UIntValue,
					rangeArg.range.min, rangeArg.range.max );
		if( group && *group )
			printf( "  group=%s", group );
		printf( "\n" );

		// Option parameters carry their choices as elements; list them, since a
		// choice param with no elements would silently be an empty dropdown.
		if( type == 11 )
		{
			const FFUInt32 count = plugMain( FF_GET_NUM_PARAMETER_ELEMENTS, arg, nullptr ).UIntValue;
			for( FFUInt32 e = 0; e < count && count != FF_FAIL; ++e )
			{
				GetParameterElementNameStruct req{ i, e };
				FFMixed reqMixed;
				reqMixed.PointerValue = &req;
				const char* elementName = asString( plugMain( FF_GET_PARAMETER_ELEMENT_NAME, reqMixed, nullptr ) );
				printf( "      [%u] %s\n", e, elementName ? elementName : "?" );
			}
		}
	}

	// Deliberately not dlclose()d: the wrapper and this tool both link the OFX
	// host, so unloading the bundle would tear down one copy of its statics while
	// ours is still live. Nothing here outlives the process anyway.
	return 0;
#endif
}

int doGenerate( const std::vector< std::string >& dirs,
				const std::string& outDir,
				const std::string& templatePath,
				const std::string& only )
{
	if( templatePath.empty() || !fs::exists( templatePath ) )
	{
		fprintf( stderr, "wrapper template not found. Build it first (cmake --build build --target ofxwrapper)\n"
						 "or pass --template <path>.\n" );
		return 1;
	}

	std::string log;
	std::vector< ofxbridge::PluginDesc > plugins = ofxbridge::scanAndDescribe( dirs, log );

	std::error_code ec;
	fs::create_directories( outDir, ec );

	int generated = 0;
	int skipped   = 0;

	for( const auto& p : plugins )
	{
		if( !only.empty() && p.identifier != only )
			continue;
		if( !p.error.empty() )
		{
			printf( "  skip  %s: %s\n", p.identifier.c_str(), p.error.c_str() );
			++skipped;
			continue;
		}

		const std::string name = sanitise( p.label.empty() ? p.identifier : p.label );

#if defined( __APPLE__ )
		const fs::path bundle    = fs::path( outDir ) / ( name + ".bundle" );
		const fs::path contents  = bundle / "Contents";
		const fs::path macosDir  = contents / "MacOS";
		const fs::path resources = contents / "Resources";

		fs::remove_all( bundle, ec );
		fs::create_directories( macosDir, ec );
		fs::create_directories( resources, ec );

		// Copy the wrapper executable out of the template bundle, renaming it to
		// match this bundle so CFBundleExecutable stays consistent.
		fs::path templateExe;
		for( const auto& e : fs::directory_iterator( fs::path( templatePath ) / "Contents" / "MacOS", ec ) )
		{
			templateExe = e.path();
			break;
		}
		if( templateExe.empty() )
		{
			fprintf( stderr, "template bundle has no executable\n" );
			return 1;
		}

		fs::copy_file( templateExe, macosDir / name, fs::copy_options::overwrite_existing, ec );
		if( ec )
		{
			fprintf( stderr, "  fail  %s: %s\n", p.identifier.c_str(), ec.message().c_str() );
			return 1;
		}
		fs::permissions( macosDir / name,
						 fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec |
							 fs::perms::others_read | fs::perms::others_exec,
						 ec );

		{
			std::ofstream plist( contents / "Info.plist" );
			plist << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
				  << "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
					 "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
				  << "<plist version=\"1.0\">\n<dict>\n"
				  << "\t<key>CFBundleExecutable</key><string>" << name << "</string>\n"
				  << "\t<key>CFBundleIdentifier</key><string>com.stoatworks.ffgl.ofx." << name << "</string>\n"
				  << "\t<key>CFBundleName</key><string>" << name << "</string>\n"
				  << "\t<key>CFBundlePackageType</key><string>BNDL</string>\n"
				  << "\t<key>CFBundleShortVersionString</key><string>" << p.versionMajor << "." << p.versionMinor
				  << "</string>\n"
				  << "</dict>\n</plist>\n";
		}

		{
			std::ofstream manifest( resources / "manifest.json" );
			manifest << ofxbridge::toManifestJson( p );
		}
#else
		// Windows and Linux: a bare shared library with a sidecar manifest.
		const fs::path outFile = fs::path( outDir ) / ( name + fs::path( templatePath ).extension().string() );
		fs::copy_file( templatePath, outFile, fs::copy_options::overwrite_existing, ec );
		if( ec )
		{
			fprintf( stderr, "  fail  %s: %s\n", p.identifier.c_str(), ec.message().c_str() );
			return 1;
		}
		{
			std::ofstream manifest( fs::path( outDir ) / ( name + ".manifest.json" ) );
			manifest << ofxbridge::toManifestJson( p );
		}
		const fs::path bundle = outFile;
#endif

		printf( "  built %s  <- %s (%zu param(s))\n", bundle.filename().string().c_str(), p.identifier.c_str(),
				p.params.size() );
		++generated;
	}

	printf( "\n%d generated, %d skipped, into %s\n", generated, skipped, outDir.c_str() );
	return generated > 0 ? 0 : 1;
}

} // namespace

int main( int argc, char** argv )
{
	if( argc < 2 )
	{
		usage();
		return 2;
	}

	const std::string command = argv[ 1 ];

	std::vector< std::string > dirs;
	std::string outDir;
	std::string templatePath;
	std::string only;
	std::string target;

	for( int i = 2; i < argc; ++i )
	{
		const std::string a = argv[ i ];
		if( a == "--dir" && i + 1 < argc )
			dirs.push_back( argv[ ++i ] );
		else if( a == "--out" && i + 1 < argc )
			outDir = argv[ ++i ];
		else if( a == "--template" && i + 1 < argc )
			templatePath = argv[ ++i ];
		else if( a == "--only" && i + 1 < argc )
			only = argv[ ++i ];
		else if( a[ 0 ] != '-' )
			target = a;
		else
		{
			fprintf( stderr, "unknown argument: %s\n", a.c_str() );
			usage();
			return 2;
		}
	}

	if( command == "verify" )
	{
		if( target.empty() )
		{
			fprintf( stderr, "verify needs a bundle path\n" );
			return 2;
		}
		return doVerify( target );
	}

	// list and generate both scan; only generate needs the conventional paths
	// suppressed when the user names directories explicitly.
	std::vector< std::string > searchPaths = dirs.empty() ? ofxbridge::defaultSearchPaths() : dirs;

	if( command == "list" )
	{
		std::string log;
		auto plugins = ofxbridge::scanAndDescribe( searchPaths, log );
		fputs( log.c_str(), stderr );
		for( const auto& p : plugins )
			printf( "%-48s %-28s %s\n", p.identifier.c_str(), p.label.c_str(),
					p.error.empty() ? "ok" : p.error.c_str() );
		return 0;
	}

	if( command == "generate" )
	{
		if( outDir.empty() )
		{
			fprintf( stderr, "generate needs --out DIR\n" );
			return 2;
		}
		if( templatePath.empty() )
			templatePath = findTemplate( argv[ 0 ] );
		return doGenerate( searchPaths, outDir, templatePath, only );
	}

	usage();
	return 2;
}
