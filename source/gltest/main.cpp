//
// ffgltest - drive a generated FFGL bundle through a real OpenGL context.
//
// Everything else in this repo can be tested headlessly, but ProcessOpenGL
// cannot: it reads an input texture back to the CPU, runs the OFX effect, and
// blits the result into the host's framebuffer. None of that can run without a
// live GL context, and driving Resolume by hand is neither repeatable nor
// something CI can do.
//
// So this creates an offscreen context, hands the plugin a texture with known
// contents, and checks what comes back — the same sequence Resolume performs,
// minus Resolume.
//
//   ffgltest <bundle> [param=value]...
//
// Unlike ofxgen, this links the FFGL SDK so the function codes and structs come
// from the headers rather than being duplicated.
//

#include "ffgl/FFGL.h"

#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

using PlugMainFn = FFMixed ( * )( FFUInt32, FFMixed, FFInstanceID );

constexpr int kWidth  = 64;
constexpr int kHeight = 32;

CGLContextObj createContext()
{
	// 4.1 core is what Resolume uses on macOS, so match it rather than asking for
	// the oldest thing that would work.
	CGLPixelFormatAttribute attrs[] = {
		kCGLPFAOpenGLProfile, (CGLPixelFormatAttribute)kCGLOGLPVersion_GL4_Core,
		kCGLPFAColorSize, (CGLPixelFormatAttribute)24,
		kCGLPFAAlphaSize, (CGLPixelFormatAttribute)8,
		(CGLPixelFormatAttribute)0
	};

	CGLPixelFormatObj pix = nullptr;
	GLint npix            = 0;
	if( CGLChoosePixelFormat( attrs, &pix, &npix ) != kCGLNoError || pix == nullptr )
	{
		fprintf( stderr, "no suitable GL pixel format\n" );
		return nullptr;
	}

	CGLContextObj ctx = nullptr;
	const CGLError err = CGLCreateContext( pix, nullptr, &ctx );
	CGLDestroyPixelFormat( pix );
	if( err != kCGLNoError || ctx == nullptr )
	{
		fprintf( stderr, "CGLCreateContext failed (%d)\n", err );
		return nullptr;
	}

	CGLSetCurrentContext( ctx );
	return ctx;
}

std::string findBinary( const std::string& bundlePath )
{
	std::error_code ec;
	fs::path p = bundlePath;
	if( !fs::is_directory( p, ec ) )
		return p.string();

	const fs::path macos = p / "Contents" / "MacOS";
	for( const auto& e : fs::directory_iterator( macos, ec ) )
		return e.path().string();
	return std::string();
}

void fillRamp( std::vector< uint8_t >& px )
{
	px.resize( (size_t)kWidth * kHeight * 4 );
	for( int y = 0; y < kHeight; ++y )
	{
		for( int x = 0; x < kWidth; ++x )
		{
			uint8_t* p = px.data() + ( (size_t)y * kWidth + x ) * 4;
			p[ 0 ]     = (uint8_t)( x * 4 );
			p[ 1 ]     = (uint8_t)( y * 8 );
			p[ 2 ]     = 128;
			p[ 3 ]     = 255;
		}
	}
}

} // namespace

int main( int argc, char** argv )
{
	if( argc < 2 )
	{
		fprintf( stderr, "usage: ffgltest <bundle> [paramIndex=value]...\n" );
		return 2;
	}

	const std::string binary = findBinary( argv[ 1 ] );
	if( binary.empty() )
	{
		fprintf( stderr, "could not find an executable in %s\n", argv[ 1 ] );
		return 1;
	}

	CGLContextObj ctx = createContext();
	if( ctx == nullptr )
		return 1;

	printf( "GL renderer: %s\n", glGetString( GL_RENDERER ) );
	printf( "GL version : %s\n", glGetString( GL_VERSION ) );

	void* handle = dlopen( binary.c_str(), RTLD_NOW | RTLD_LOCAL );
	if( handle == nullptr )
	{
		fprintf( stderr, "dlopen failed: %s\n", dlerror() );
		return 1;
	}
	auto plugMain = (PlugMainFn)dlsym( handle, "plugMain" );
	if( plugMain == nullptr )
	{
		fprintf( stderr, "plugMain not found\n" );
		return 1;
	}

	FFMixed zero;
	zero.UIntValue = 0;

	// Input texture with a known ramp.
	std::vector< uint8_t > inPixels;
	fillRamp( inPixels );

	GLuint inTex = 0;
	glGenTextures( 1, &inTex );
	glBindTexture( GL_TEXTURE_2D, inTex );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, kWidth, kHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, inPixels.data() );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	glBindTexture( GL_TEXTURE_2D, 0 );

	// Output texture and FBO, standing in for Resolume's render target.
	GLuint outTex = 0;
	glGenTextures( 1, &outTex );
	glBindTexture( GL_TEXTURE_2D, outTex );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, kWidth, kHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	glBindTexture( GL_TEXTURE_2D, 0 );

	GLuint hostFbo = 0;
	glGenFramebuffers( 1, &hostFbo );
	glBindFramebuffer( GL_FRAMEBUFFER, hostFbo );
	glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, outTex, 0 );
	if( glCheckFramebufferStatus( GL_FRAMEBUFFER ) != GL_FRAMEBUFFER_COMPLETE )
	{
		fprintf( stderr, "host FBO incomplete\n" );
		return 1;
	}

	// Instantiate.
	FFGLViewportStruct viewport{ 0, 0, kWidth, kHeight };
	FFMixed instArg;
	instArg.PointerValue = &viewport;
	FFMixed instResult   = plugMain( FF_INSTANTIATE_GL, instArg, nullptr );
	if( instResult.UIntValue == FF_FAIL || instResult.PointerValue == nullptr )
	{
		fprintf( stderr, "FF_INSTANTIATE_GL failed -- the plugin could not create its OFX effect\n" );
		return 1;
	}
	FFInstanceID instance = instResult.PointerValue;
	printf( "instantiated ok\n" );

	// Any param overrides, by FFGL index.
	for( int i = 2; i < argc; ++i )
	{
		const std::string kv = argv[ i ];
		const size_t eq      = kv.find( '=' );
		if( eq == std::string::npos )
			continue;
		SetParameterStruct sp{};
		sp.ParameterNumber = (FFUInt32)atoi( kv.substr( 0, eq ).c_str() );
		const float value  = (float)atof( kv.c_str() + eq + 1 );
		memcpy( &sp.NewParameterValue.UIntValue, &value, sizeof( float ) );
		FFMixed spArg;
		spArg.PointerValue = &sp;
		plugMain( FF_SET_PARAMETER, spArg, instance );
		printf( "set param %u = %g\n", sp.ParameterNumber, value );
	}

	// Render.
	FFGLTextureStruct inStruct{};
	inStruct.Width          = kWidth;
	inStruct.Height         = kHeight;
	inStruct.HardwareWidth  = kWidth;
	inStruct.HardwareHeight = kHeight;
	inStruct.Handle         = inTex;

	FFGLTextureStruct* textures[ 1 ] = { &inStruct };
	ProcessOpenGLStruct pgl{};
	pgl.numInputTextures = 1;
	pgl.inputTextures    = textures;
	pgl.HostFBO          = hostFbo;

	glBindFramebuffer( GL_FRAMEBUFFER, hostFbo );
	glViewport( 0, 0, kWidth, kHeight );

	FFMixed pglArg;
	pglArg.PointerValue = &pgl;
	const FFMixed rc    = plugMain( FF_PROCESS_OPENGL, pglArg, instance );
	if( rc.UIntValue == FF_FAIL )
	{
		fprintf( stderr, "FF_PROCESS_OPENGL returned FF_FAIL\n" );
		return 1;
	}
	printf( "ProcessOpenGL ok\n" );

	// Read the result back out of the host FBO.
	std::vector< uint8_t > outPixels( (size_t)kWidth * kHeight * 4 );
	glBindFramebuffer( GL_READ_FRAMEBUFFER, hostFbo );
	glPixelStorei( GL_PACK_ALIGNMENT, 1 );
	glReadPixels( 0, 0, kWidth, kHeight, GL_RGBA, GL_UNSIGNED_BYTE, outPixels.data() );

	const GLenum glErr = glGetError();
	if( glErr != GL_NO_ERROR )
		fprintf( stderr, "GL error 0x%04x after readback\n", glErr );

	const size_t mid = ( (size_t)( kHeight / 2 ) * kWidth + ( kWidth / 2 ) ) * 4;
	printf( "  in  [0,0]    RGBA %3u %3u %3u %3u\n", inPixels[ 0 ], inPixels[ 1 ], inPixels[ 2 ], inPixels[ 3 ] );
	printf( "  out [0,0]    RGBA %3u %3u %3u %3u\n", outPixels[ 0 ], outPixels[ 1 ], outPixels[ 2 ], outPixels[ 3 ] );
	printf( "  in  [centre] RGBA %3u %3u %3u %3u\n", inPixels[ mid ], inPixels[ mid + 1 ], inPixels[ mid + 2 ],
			inPixels[ mid + 3 ] );
	printf( "  out [centre] RGBA %3u %3u %3u %3u\n", outPixels[ mid ], outPixels[ mid + 1 ], outPixels[ mid + 2 ],
			outPixels[ mid + 3 ] );

	size_t changed = 0;
	for( size_t i = 0; i < outPixels.size(); ++i )
		if( outPixels[ i ] != inPixels[ i ] )
			++changed;
	printf( "  %zu of %zu bytes differ from the input\n", changed, outPixels.size() );

	plugMain( FF_DEINSTANTIATE_GL, zero, instance );
	CGLSetCurrentContext( nullptr );
	CGLDestroyContext( ctx );
	return 0;
}
