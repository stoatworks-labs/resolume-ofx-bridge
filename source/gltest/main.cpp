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
//   ffgltest <bundle> [param=value]... [--demo out.bmp]
//                     [--expect-centre R,G,B,A] [--require-gpu]
//
// --expect-centre asserts the centre pixel and exits 1 on mismatch, so a test
// cannot pass by rendering a black frame. --require-gpu exits 3 when only a
// software renderer is present, letting CI skip GPU paths honestly instead of
// running them and reporting a meaningless pass.
//
// --demo runs a larger colour test pattern through the effect and writes a
// side-by-side before/after image, so the documentation shows real plugin
// output rather than a mock-up.
//
// Unlike ofxgen, this links the FFGL SDK so the function codes and structs come
// from the headers rather than being duplicated.
//

#include "ffgl/FFGL.h"

#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>

#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

using PlugMainFn = FFMixed ( * )( FFUInt32, FFMixed, FFInstanceID );

// Small by default (fast, and a wrong stride is obvious); --demo uses a 16:9
// frame big enough to look at.
int gWidth  = 64;
int gHeight = 32;

/// Create a context.
///
/// 4.1 core is what Resolume uses on macOS, so that is the default. The legacy
/// 2.1 profile exists only for testing OFX OpenGL-render plugins that draw with
/// immediate mode (glBegin/glVertex), which is illegal in a core profile — see
/// docs/04-gpu-acceleration.md. macOS offers no compatibility profile above 2.1,
/// so this really is either/or.
CGLContextObj createContext( bool legacy )
{
	CGLPixelFormatAttribute attrs[] = {
		kCGLPFAOpenGLProfile,
		(CGLPixelFormatAttribute)( legacy ? kCGLOGLPVersion_Legacy : kCGLOGLPVersion_GL4_Core ),
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
	px.resize( (size_t)gWidth * gHeight * 4 );
	for( int y = 0; y < gHeight; ++y )
	{
		for( int x = 0; x < gWidth; ++x )
		{
			uint8_t* p = px.data() + ( (size_t)y * gWidth + x ) * 4;
			p[ 0 ]     = (uint8_t)( x * 4 );
			p[ 1 ]     = (uint8_t)( y * 8 );
			p[ 2 ]     = 128;
			p[ 3 ]     = 255;
		}
	}
}

/// A colour pattern for the demo image: saturated bars over a luminance ramp,
/// with a grey wedge along the bottom. Chosen so that gain, inversion and
/// per-channel effects are all visibly distinguishable.
void fillPattern( std::vector< uint8_t >& px )
{
	px.resize( (size_t)gWidth * gHeight * 4 );
	static const uint8_t bars[ 8 ][ 3 ] = {
		{ 255, 255, 255 }, { 255, 255, 0 }, { 0, 255, 255 }, { 0, 255, 0 },
		{ 255, 0, 255 },   { 255, 0, 0 },   { 0, 0, 255 },   { 32, 32, 32 }
	};

	for( int y = 0; y < gHeight; ++y )
	{
		for( int x = 0; x < gWidth; ++x )
		{
			uint8_t* p = px.data() + ( (size_t)y * gWidth + x ) * 4;

			if( y > gHeight * 3 / 4 )
			{
				// Grey wedge: makes a gain change obvious as a shift in where it
				// clips, not just an overall brightness change.
				const uint8_t v = (uint8_t)( 255 * x / ( gWidth - 1 ) );
				p[ 0 ] = p[ 1 ] = p[ 2 ] = v;
			}
			else
			{
				const int bar   = ( x * 8 ) / gWidth;
				const float fade = 0.35f + 0.65f * ( 1.0f - (float)y / ( gHeight * 3 / 4 ) );
				p[ 0 ] = (uint8_t)( bars[ bar ][ 0 ] * fade );
				p[ 1 ] = (uint8_t)( bars[ bar ][ 1 ] * fade );
				p[ 2 ] = (uint8_t)( bars[ bar ][ 2 ] * fade );
			}
			p[ 3 ] = 255;
		}
	}
}

void put32( std::vector< uint8_t >& v, uint32_t x )
{
	v.push_back( (uint8_t)( x & 0xFF ) );
	v.push_back( (uint8_t)( ( x >> 8 ) & 0xFF ) );
	v.push_back( (uint8_t)( ( x >> 16 ) & 0xFF ) );
	v.push_back( (uint8_t)( ( x >> 24 ) & 0xFF ) );
}

/// Write `left` and `right` side by side as a 24-bit BMP.
///
/// BMP because it needs no libraries and macOS `sips` converts it to PNG; both
/// buffers are RGBA top-down, and BMP rows are bottom-up BGR.
bool writeComparisonBmp( const std::string& path,
						 const std::vector< uint8_t >& left,
						 const std::vector< uint8_t >& right )
{
	const int gap   = 8;
	const int outW  = gWidth * 2 + gap;
	const int outH  = gHeight;
	const int stride = ( outW * 3 + 3 ) & ~3;

	std::vector< uint8_t > pixels( (size_t)stride * outH, 24 );

	for( int y = 0; y < outH; ++y )
	{
		uint8_t* row = pixels.data() + (size_t)( outH - 1 - y ) * stride;
		for( int x = 0; x < gWidth; ++x )
		{
			const uint8_t* l = left.data() + ( (size_t)y * gWidth + x ) * 4;
			uint8_t* d       = row + (size_t)x * 3;
			d[ 0 ] = l[ 2 ];
			d[ 1 ] = l[ 1 ];
			d[ 2 ] = l[ 0 ];

			const uint8_t* r = right.data() + ( (size_t)y * gWidth + x ) * 4;
			uint8_t* d2      = row + (size_t)( x + gWidth + gap ) * 3;
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
	put32( header, (uint32_t)outH );
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

} // namespace

int main( int argc, char** argv )
{
	if( argc < 2 )
	{
		fprintf( stderr, "usage: ffgltest <bundle> [paramIndex=value]...\n" );
		return 2;
	}

	std::string demoPath;
	int benchFrames = 0;
	bool legacyGL   = false;
	bool requireGpu = false;
	int expect[ 4 ]  = { -1, -1, -1, -1 };
	for( int i = 2; i < argc; ++i )
	{
		const std::string a = argv[ i ];
		if( a == "--demo" && i + 1 < argc )
			demoPath = argv[ i + 1 ];
		else if( a == "--legacy-gl" )
			legacyGL = true;
		else if( a == "--require-gpu" )
			requireGpu = true;
		else if( a == "--expect-centre" && i + 1 < argc )
		{
			if( sscanf( argv[ i + 1 ], "%d,%d,%d,%d", &expect[ 0 ], &expect[ 1 ], &expect[ 2 ],
						&expect[ 3 ] ) != 4 )
			{
				fprintf( stderr, "--expect-centre expects R,G,B,A\n" );
				return 2;
			}
		}
		else if( a == "--bench" && i + 1 < argc )
			benchFrames = atoi( argv[ i + 1 ] );
		else if( a == "--size" && i + 1 < argc )
		{
			if( sscanf( argv[ i + 1 ], "%dx%d", &gWidth, &gHeight ) != 2 )
			{
				fprintf( stderr, "--size expects WxH\n" );
				return 2;
			}
		}
	}

	if( !demoPath.empty() )
	{
		gWidth  = 480;
		gHeight = 270;
	}

	const std::string binary = findBinary( argv[ 1 ] );
	if( binary.empty() )
	{
		fprintf( stderr, "could not find an executable in %s\n", argv[ 1 ] );
		return 1;
	}

	CGLContextObj ctx = createContext( legacyGL );
	if( ctx == nullptr )
		return 1;

	const char* renderer = (const char*)glGetString( GL_RENDERER );
	printf( "GL renderer: %s\n", renderer );
	printf( "GL version : %s\n", glGetString( GL_VERSION ) );

	// A software renderer cannot do IOSurface-backed GL/Metal sharing, so the GPU
	// paths would "succeed" while producing nothing. Say so and skip, rather than
	// report a pass that means nothing.
	if( requireGpu && renderer != nullptr && strstr( renderer, "Software" ) != nullptr )
	{
		printf( "skipping: software renderer, no GPU available for this path\n" );
		return 3;
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
		fprintf( stderr, "plugMain not found\n" );
		return 1;
	}

	FFMixed zero;
	zero.UIntValue = 0;

	// Input texture with a known ramp.
	std::vector< uint8_t > inPixels;
	if( demoPath.empty() )
		fillRamp( inPixels );
	else
		fillPattern( inPixels );

	GLuint inTex = 0;
	glGenTextures( 1, &inTex );
	glBindTexture( GL_TEXTURE_2D, inTex );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, gWidth, gHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, inPixels.data() );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	glBindTexture( GL_TEXTURE_2D, 0 );

	// Output texture and FBO, standing in for Resolume's render target.
	GLuint outTex = 0;
	glGenTextures( 1, &outTex );
	glBindTexture( GL_TEXTURE_2D, outTex );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, gWidth, gHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr );
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
	FFGLViewportStruct viewport{ 0, 0, (GLuint)gWidth, (GLuint)gHeight };
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
			continue;// --demo and its argument
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
	inStruct.Width          = gWidth;
	inStruct.Height         = gHeight;
	inStruct.HardwareWidth  = gWidth;
	inStruct.HardwareHeight = gHeight;
	inStruct.Handle         = inTex;

	FFGLTextureStruct* textures[ 1 ] = { &inStruct };
	ProcessOpenGLStruct pgl{};
	pgl.numInputTextures = 1;
	pgl.inputTextures    = textures;
	pgl.HostFBO          = hostFbo;

	glBindFramebuffer( GL_FRAMEBUFFER, hostFbo );
	glViewport( 0, 0, gWidth, gHeight );

	FFMixed pglArg;
	pglArg.PointerValue = &pgl;
	const FFMixed rc    = plugMain( FF_PROCESS_OPENGL, pglArg, instance );
	if( rc.UIntValue == FF_FAIL )
	{
		fprintf( stderr, "FF_PROCESS_OPENGL returned FF_FAIL\n" );
		return 1;
	}
	printf( "ProcessOpenGL ok\n" );

	if( benchFrames > 0 )
	{
		// Warm up first: the initial frame builds the OFX instance and allocates
		// the buffers, which is not what we are trying to measure.
		using Clock = std::chrono::steady_clock;
		plugMain( FF_PROCESS_OPENGL, pglArg, instance );
		glFinish();

		const auto t0 = Clock::now();
		for( int i = 0; i < benchFrames; ++i )
			plugMain( FF_PROCESS_OPENGL, pglArg, instance );
		glFinish();
		const double totalMs =
			std::chrono::duration_cast< std::chrono::microseconds >( Clock::now() - t0 ).count() / 1000.0;

		const double perFrame = totalMs / benchFrames;
		printf( "\nbenchmark: %d frames at %dx%d\n", benchFrames, gWidth, gHeight );
		printf( "  %.2f ms/frame  (%.1f fps)\n", perFrame, 1000.0 / perFrame );
		printf( "  budget at 60fps is 16.67 ms -- this effect uses %.0f%% of it\n",
				perFrame / 16.67 * 100.0 );
	}

	// Read the result back out of the host FBO.
	std::vector< uint8_t > outPixels( (size_t)gWidth * gHeight * 4 );
	glBindFramebuffer( GL_READ_FRAMEBUFFER, hostFbo );
	glPixelStorei( GL_PACK_ALIGNMENT, 1 );
	glReadPixels( 0, 0, gWidth, gHeight, GL_RGBA, GL_UNSIGNED_BYTE, outPixels.data() );

	const GLenum glErr = glGetError();
	if( glErr != GL_NO_ERROR )
		fprintf( stderr, "GL error 0x%04x after readback\n", glErr );

	const size_t mid = ( (size_t)( gHeight / 2 ) * gWidth + ( gWidth / 2 ) ) * 4;
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

	if( !demoPath.empty() )
	{
		if( writeComparisonBmp( demoPath, inPixels, outPixels ) )
			printf( "wrote %s (%dx%d, before | after)\n", demoPath.c_str(), gWidth * 2 + 8, gHeight );
		else
			fprintf( stderr, "could not write %s\n", demoPath.c_str() );
	}

	if( expect[ 0 ] >= 0 )
	{
		const int got[ 4 ] = { outPixels[ mid ], outPixels[ mid + 1 ], outPixels[ mid + 2 ],
							   outPixels[ mid + 3 ] };
		if( got[ 0 ] != expect[ 0 ] || got[ 1 ] != expect[ 1 ] || got[ 2 ] != expect[ 2 ] ||
			got[ 3 ] != expect[ 3 ] )
		{
			fprintf( stderr,
					 "FAIL: centre pixel is %d,%d,%d,%d but %d,%d,%d,%d was expected\n",
					 got[ 0 ], got[ 1 ], got[ 2 ], got[ 3 ], expect[ 0 ], expect[ 1 ], expect[ 2 ],
					 expect[ 3 ] );
			return 1;
		}
		printf( "centre pixel matches %d,%d,%d,%d\n", expect[ 0 ], expect[ 1 ], expect[ 2 ], expect[ 3 ] );
	}

	plugMain( FF_DEINSTANTIATE_GL, zero, instance );
	CGLSetCurrentContext( nullptr );
	CGLDestroyContext( ctx );
	return 0;
}
