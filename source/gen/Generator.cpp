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
