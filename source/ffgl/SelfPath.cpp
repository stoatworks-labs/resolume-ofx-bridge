#include "SelfPath.h"

#include <string>
#include <vector>

#if defined( _WIN32 )
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace ofxffgl {

namespace {

/// Marker whose address is guaranteed to live in this binary, so dladdr resolves
/// to us and not to whichever host loaded us.
void selfMarker()
{
}

std::string dirName( const std::string& path )
{
	const size_t slash = path.find_last_of( "/\\" );
	return slash == std::string::npos ? std::string() : path.substr( 0, slash );
}

std::string baseName( const std::string& path )
{
	const size_t slash = path.find_last_of( "/\\" );
	return slash == std::string::npos ? path : path.substr( slash + 1 );
}

std::string stripExtension( const std::string& name )
{
	const size_t dot = name.find_last_of( '.' );
	return dot == std::string::npos ? name : name.substr( 0, dot );
}

} // namespace

std::string selfBinaryPath()
{
#if defined( _WIN32 )
	HMODULE module = nullptr;
	if( !GetModuleHandleExA( GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
							 (LPCSTR)&selfMarker, &module ) )
		return std::string();

	std::vector< char > buf( 1024 );
	for( ;; )
	{
		const DWORD n = GetModuleFileNameA( module, buf.data(), (DWORD)buf.size() );
		if( n == 0 )
			return std::string();
		if( n < buf.size() - 1 )
			return std::string( buf.data(), n );
		buf.resize( buf.size() * 2 );// path was truncated
	}
#else
	Dl_info info;
	if( dladdr( (const void*)&selfMarker, &info ) == 0 || info.dli_fname == nullptr )
		return std::string();
	return std::string( info.dli_fname );
#endif
}

std::string selfManifestPath()
{
	const std::string binary = selfBinaryPath();
	if( binary.empty() )
		return std::string();

#if defined( __APPLE__ )
	// FFGL plugins on macOS are bundles:
	//   Foo.bundle/Contents/MacOS/Foo   ->  Foo.bundle/Contents/Resources/manifest.json
	const std::string macosDir   = dirName( binary );  // .../Contents/MacOS
	const std::string contentsDir = dirName( macosDir );// .../Contents
	if( baseName( macosDir ) == "MacOS" && baseName( contentsDir ) == "Contents" )
		return contentsDir + "/Resources/manifest.json";

	// Loose dylib (during development): sidecar next to it.
	return dirName( binary ) + "/" + stripExtension( baseName( binary ) ) + ".manifest.json";
#else
	// Windows and Linux FFGL plugins are bare shared libraries, so the manifest
	// sits beside them under the same basename.
	return dirName( binary ) + "/" + stripExtension( baseName( binary ) ) + ".manifest.json";
#endif
}

} // namespace ofxffgl
