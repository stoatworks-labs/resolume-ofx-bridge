#include "Generator.h"

#include "../ofxbridge/Catalog.h"

#include <filesystem>
#include <fstream>
#include <sstream>

#if defined( __APPLE__ )
#include <sys/xattr.h>
#endif

namespace fs = std::filesystem;

namespace ofxgen {

namespace {

void emit( const LogFn& log, const std::string& line )
{
	if( log )
		log( line );
}

/// scanAndDescribe accumulates its progress into one string; the callers here
/// want it a line at a time.
void emitBlock( const LogFn& log, const std::string& block )
{
	if( !log )
		return;
	std::istringstream in( block );
	std::string line;
	while( std::getline( in, line ) )
		if( !line.empty() )
			log( line );
}

#if defined( __APPLE__ )
/// A quarantined plugin is skipped by Resolume silently, rather than prompting,
/// so it looks like the bundle simply did not work.
///
/// The flag arrives by inheritance: files written by a quarantined process get
/// it, and an app downloaded from a release page is quarantined until the user
/// clears it. That case cannot be reproduced from a local build, so this runs
/// unconditionally rather than only when the attribute is found.
void clearQuarantineAt( const fs::path& path )
{
	std::error_code ec;
	::removexattr( path.c_str(), "com.apple.quarantine", XATTR_NOFOLLOW );
	if( !fs::is_directory( path, ec ) )
		return;
	for( fs::recursive_directory_iterator it( path, ec ), end; it != end; it.increment( ec ) )
	{
		if( ec )
			break;
		::removexattr( it->path().c_str(), "com.apple.quarantine", XATTR_NOFOLLOW );
	}
}
#endif

} // namespace

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

std::string findTemplate( const std::string& executablePath )
{
	std::error_code ec;
	const fs::path exe = fs::absolute( fs::path( executablePath ), ec );
	const fs::path dir = exe.parent_path();

#if defined( __APPLE__ )
	const char* leaf = "ofxwrapper.bundle";
#elif defined( _WIN32 )
	const char* leaf = "ofxwrapper.dll";
#else
	const char* leaf = "libofxwrapper.so";
#endif

	std::vector< fs::path > candidates;
	candidates.push_back( dir / leaf );
	candidates.push_back( dir / ".." / leaf );
	// Inside a .app the executable sits in Contents/MacOS, and the template we
	// ship with the GUI is a bundled resource.
	candidates.push_back( dir / ".." / "Resources" / leaf );
	candidates.push_back( fs::current_path( ec ) / "build" / leaf );

	for( const auto& c : candidates )
		if( fs::exists( c, ec ) )
			return fs::weakly_canonical( c, ec ).string();
	return std::string();
}

Result generate( const Options& options, const LogFn& log, const ProgressFn& progress, const CancelFn& cancelled )
{
	Result result;

	const std::string templatePath = options.templatePath;
	std::error_code ec;
	if( templatePath.empty() || !fs::exists( templatePath, ec ) )
	{
		result.error = "wrapper template not found. Build it first "
					   "(cmake --build build --target ofxwrapper), or pass one explicitly.";
		return result;
	}

	if( options.outDir.empty() )
	{
		result.error = "no output directory";
		return result;
	}

	std::vector< std::string > searchPaths = options.searchPaths;
	if( !options.onlyBundlePath.empty() )
	{
		// addFileToPath() takes a directory: a bundle path finds nothing, with no
		// error. Scan the parent and filter below.
		const fs::path parent = fs::path( options.onlyBundlePath ).parent_path();
		if( !parent.empty() )
			searchPaths = { parent.string() };
	}
	if( searchPaths.empty() )
		searchPaths = ofxbridge::defaultSearchPaths();

	if( progress )
		progress( 0, 0, "Scanning for OFX plugins" );

	std::string scanLog;
	std::vector< ofxbridge::PluginDesc > plugins = ofxbridge::scanAndDescribe( searchPaths, scanLog );
	emitBlock( log, scanLog );

	const fs::path wanted =
		options.onlyBundlePath.empty() ? fs::path() : fs::weakly_canonical( options.onlyBundlePath, ec );

	std::vector< const ofxbridge::PluginDesc* > selected;
	for( const auto& p : plugins )
	{
		if( !options.onlyIdentifier.empty() && p.identifier != options.onlyIdentifier )
			continue;
		if( !wanted.empty() && fs::weakly_canonical( p.bundlePath, ec ) != wanted )
			continue;
		selected.push_back( &p );
	}

	fs::create_directories( options.outDir, ec );
	if( ec )
	{
		result.error = "cannot create " + options.outDir + ": " + ec.message();
		return result;
	}

	const int total = (int)selected.size();
	int done        = 0;

	for( const ofxbridge::PluginDesc* pp : selected )
	{
		if( cancelled && cancelled() )
		{
			result.cancelled = true;
			emit( log, "cancelled" );
			break;
		}

		const ofxbridge::PluginDesc& p = *pp;
		if( progress )
			progress( done, total, p.label.empty() ? p.identifier : p.label );
		++done;

		if( !p.error.empty() )
		{
			emit( log, "  skip  " + p.identifier + ": " + p.error );
			++result.skipped;
			continue;
		}

		const std::string name = sanitise( p.label.empty() ? p.identifier : p.label );

#if defined( __APPLE__ )
		const fs::path bundle    = fs::path( options.outDir ) / ( name + ".bundle" );
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
			result.error = "template bundle has no executable";
			return result;
		}

		fs::copy_file( templateExe, macosDir / name, fs::copy_options::overwrite_existing, ec );
		if( ec )
		{
			emit( log, "  fail  " + p.identifier + ": " + ec.message() );
			++result.skipped;
			ec.clear();
			continue;
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

		if( options.clearQuarantine )
			clearQuarantineAt( bundle );
#else
		// Windows and Linux: a bare shared library with a sidecar manifest.
		const fs::path outFile =
			fs::path( options.outDir ) / ( name + fs::path( templatePath ).extension().string() );
		fs::copy_file( templatePath, outFile, fs::copy_options::overwrite_existing, ec );
		if( ec )
		{
			emit( log, "  fail  " + p.identifier + ": " + ec.message() );
			++result.skipped;
			ec.clear();
			continue;
		}
		{
			std::ofstream manifest( fs::path( options.outDir ) / ( name + ".manifest.json" ) );
			manifest << ofxbridge::toManifestJson( p );
		}
		const fs::path bundle = outFile;
#endif

		emit( log, "  built " + bundle.filename().string() + "  <- " + p.identifier + " (" +
						std::to_string( p.params.size() ) + " param(s))" );
		result.bundles.push_back( bundle.string() );
		++result.generated;
	}

	if( progress )
		progress( done, total, std::string() );

	return result;
}

} // namespace ofxgen

// ---------------------------------------------------------------------------
// FFGL -> OFX. The same philosophy as the other direction: a copy, not a
// compile. The output bundle is the prebuilt shell binary, a manifest the
// shell configures itself from at load time, and the untouched guest bundle
// carried inside Contents/Guest so the output is self-contained.
// ---------------------------------------------------------------------------

#if OFXGEN_HAS_FFGL_GUEST

#include "FfglGuest.h"

namespace ofxgen {

namespace {

std::string jsonEscape( const std::string& in )
{
	std::string out;
	out.reserve( in.size() + 8 );
	for( char c : in )
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
				out += c;
		}
	}
	return out;
}

std::string ffglManifestJson( const ffglguest::GuestInfo& info, const std::string& identifier,
							  const std::string& guestLeaf )
{
	std::string o;
	o += "{\n";
	o += "  \"manifestVersion\": 2,\n";
	o += "  \"guestType\": \"ffgl\",\n";
	o += "  \"identifier\": \"" + jsonEscape( identifier ) + "\",\n";
	o += "  \"label\": \"" + jsonEscape( info.name + " (FFGL)" ) + "\",\n";
	o += "  \"grouping\": \"FFGL\",\n";
	o += "  \"guestBundle\": \"Guest/" + jsonEscape( guestLeaf ) + "\",\n";
	o += std::string( "  \"isSource\": " ) + ( info.pluginType == FF_SOURCE ? "true" : "false" ) + ",\n";
	o += "  \"versionMajor\": 1,\n";
	o += "  \"versionMinor\": 0,\n";
	o += "  \"params\": [\n";
	for( size_t i = 0; i < info.params.size(); ++i )
	{
		const ffglguest::GuestParam& p = info.params[ i ];
		o += "    {\"index\": " + std::to_string( p.index );
		o += ", \"name\": \"" + jsonEscape( p.name ) + "\"";
		o += ", \"type\": " + std::to_string( p.type );
		o += ", \"default\": " + std::to_string( p.defaultValue );
		o += ", \"min\": " + std::to_string( p.rangeMin );
		o += ", \"max\": " + std::to_string( p.rangeMax );
		if( !p.textDefault.empty() )
			o += ", \"textDefault\": \"" + jsonEscape( p.textDefault ) + "\"";
		if( !p.group.empty() )
			o += ", \"group\": \"" + jsonEscape( p.group ) + "\"";
		if( !p.elements.empty() )
		{
			o += ", \"elements\": [";
			for( size_t e = 0; e < p.elements.size(); ++e )
				o += ( e ? "," : "" ) + std::string( "\"" ) + jsonEscape( p.elements[ e ] ) + "\"";
			o += "], \"elementValues\": [";
			for( size_t e = 0; e < p.elementValues.size(); ++e )
				o += ( e ? "," : "" ) + std::to_string( p.elementValues[ e ] );
			o += "]";
		}
		o += "}";
		o += ( i + 1 < info.params.size() ) ? ",\n" : "\n";
	}
	o += "  ]\n";
	o += "}\n";
	return o;
}

} // namespace

std::string findOfxShell( const std::string& executablePath )
{
	std::error_code ec;
	const fs::path exe = fs::absolute( fs::path( executablePath ), ec );
	const fs::path dir = exe.parent_path();

	const char* leaf = "ffglofxshell.ofx";

	std::vector< fs::path > candidates;
	candidates.push_back( dir / leaf );
	candidates.push_back( dir / ".." / leaf );
	candidates.push_back( dir / ".." / "Resources" / leaf );
	candidates.push_back( fs::current_path( ec ) / "build" / leaf );

	for( const auto& c : candidates )
		if( fs::exists( c, ec ) )
			return fs::weakly_canonical( c, ec ).string();
	return std::string();
}

Result wrapFfgl( const WrapFfglOptions& options, const LogFn& log )
{
	Result result;
	std::error_code ec;

	if( options.shellPath.empty() || !fs::exists( options.shellPath, ec ) )
	{
		result.error = "OFX shell not found. Build it first "
					   "(cmake --build build --target ffglofxshell), or pass one explicitly.";
		return result;
	}
	if( options.outDir.empty() )
	{
		result.error = "no output directory";
		return result;
	}
	fs::create_directories( options.outDir, ec );

	for( const std::string& bundlePath : options.bundles )
	{
		std::string error;
		ffglguest::GuestInfo info;
		if( !ffglguest::describe( bundlePath, info, error ) )
		{
			if( log )
				log( "  SKIP " + bundlePath + ": " + error );
			++result.skipped;
			continue;
		}

		const std::string safe       = sanitise( info.name );
		const std::string identifier = "com.stoatworks.ffglwrap." + [ & ] {
			std::string s;
			for( char c : safe )
				s += (char)tolower( (unsigned char)c );
			return s;
		}();

		// "_FFGL" in the bundle name so a wrapped Tinsel and a native OFX
		// Tinsel can share /Library/OFX/Plugins without colliding.
		const fs::path out = fs::path( options.outDir ) / ( safe + "_FFGL.ofx.bundle" );
		fs::remove_all( out, ec );
		fs::create_directories( out / "Contents" / "MacOS", ec );
		fs::create_directories( out / "Contents" / "Guest", ec );

		const std::string binaryName = safe + "_FFGL.ofx";
		fs::copy_file( options.shellPath, out / "Contents" / "MacOS" / binaryName,
					   fs::copy_options::overwrite_existing, ec );
		if( ec )
		{
			if( log )
				log( "  SKIP " + info.name + ": could not copy shell (" + ec.message() + ")" );
			++result.skipped;
			continue;
		}

		const fs::path guestSrc = fs::path( bundlePath );
		const std::string leaf  = guestSrc.filename().string();
		fs::copy( guestSrc, out / "Contents" / "Guest" / leaf,
				  fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec );
		if( ec )
		{
			if( log )
				log( "  SKIP " + info.name + ": could not copy guest (" + ec.message() + ")" );
			++result.skipped;
			continue;
		}

		{
			std::ofstream plist( out / "Contents" / "Info.plist" );
			plist << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
				  << "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
					 "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
				  << "<plist version=\"1.0\">\n<dict>\n"
				  << "\t<key>CFBundleExecutable</key>\n\t<string>" << binaryName << "</string>\n"
				  << "\t<key>CFBundleIdentifier</key>\n\t<string>" << identifier << "</string>\n"
				  << "\t<key>CFBundlePackageType</key>\n\t<string>BNDL</string>\n"
				  << "</dict>\n</plist>\n";
		}
		{
			std::ofstream manifest( out / "Contents" / "manifest.json" );
			manifest << ffglManifestJson( info, identifier, leaf );
		}

		if( log )
			log( "  wrote " + out.filename().string() + " (" + std::to_string( info.params.size() )
				 + " params)" );
		++result.generated;
		result.bundles.push_back( fs::weakly_canonical( out, ec ).string() );
	}

	return result;
}

} // namespace ofxgen

#endif // OFXGEN_HAS_FFGL_GUEST
