#include "FfglGuest.h"

#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>

#include <cstring>
#include <dlfcn.h>
#include <filesystem>

namespace fs = std::filesystem;

namespace ffglguest {
namespace {

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

/// FFGL hands strings back as pointers into the plugin's own memory, length
/// implied — FF_GET_INFO's name field is famously NOT NUL-terminated at 16
/// characters, and parameter names at exactly 16 likewise.
std::string boundedString( const char* s, size_t maxLen )
{
	if( s == nullptr )
		return {};
	size_t n = 0;
	while( n < maxLen && s[ n ] != '\0' )
		++n;
	return std::string( s, n );
}

CGLContextObj createCoreContext( std::string& error )
{
	// 4.1 core — what Resolume uses on macOS, and what every fleet plugin's
	// shaders declare. See ffgltest for the legacy story; a guest that needs
	// immediate-mode GL cannot work in a core profile and is not supported.
	CGLPixelFormatAttribute attrs[] = {
		kCGLPFAOpenGLProfile,
		(CGLPixelFormatAttribute)kCGLOGLPVersion_GL4_Core,
		kCGLPFAColorSize, (CGLPixelFormatAttribute)24,
		kCGLPFAAlphaSize, (CGLPixelFormatAttribute)8,
		(CGLPixelFormatAttribute)0
	};

	CGLPixelFormatObj pix = nullptr;
	GLint npix            = 0;
	if( CGLChoosePixelFormat( attrs, &pix, &npix ) != kCGLNoError || pix == nullptr )
	{
		error = "no suitable GL pixel format";
		return nullptr;
	}

	CGLContextObj ctx  = nullptr;
	const CGLError err = CGLCreateContext( pix, nullptr, &ctx );
	CGLDestroyPixelFormat( pix );
	if( err != kCGLNoError || ctx == nullptr )
	{
		error = "CGLCreateContext failed (" + std::to_string( err ) + ")";
		return nullptr;
	}
	return ctx;
}

using PlugMainFn = FFMixed ( * )( FFUInt32, FFMixed, FFInstanceID );

bool readInfo( PlugMainFn plugMain, GuestInfo& out, std::string& error )
{
	FFMixed zero;
	zero.UIntValue = 0;

	const FFMixed info = plugMain( FF_GET_INFO, zero, nullptr );
	const auto* pis    = static_cast< const PluginInfoStruct* >( info.PointerValue );
	if( pis == nullptr )
	{
		error = "FF_GET_INFO returned nothing";
		return false;
	}

	out.name       = boundedString( reinterpret_cast< const char* >( pis->PluginName ), 16 );
	out.uniqueId   = boundedString( reinterpret_cast< const char* >( pis->PluginUniqueID ), 4 );
	out.pluginType = pis->PluginType;

	FFMixed capArg;
	capArg.UIntValue = FF_CAP_MINIMUM_INPUT_FRAMES;
	out.minInputs    = (int)plugMain( FF_GET_PLUGIN_CAPS, capArg, nullptr ).UIntValue;
	capArg.UIntValue = FF_CAP_MAXIMUM_INPUT_FRAMES;
	out.maxInputs    = (int)plugMain( FF_GET_PLUGIN_CAPS, capArg, nullptr ).UIntValue;

	const FFUInt32 count = plugMain( FF_GET_NUM_PARAMETERS, zero, nullptr ).UIntValue;
	out.params.clear();
	out.params.reserve( count );

	for( FFUInt32 i = 0; i < count; ++i )
	{
		FFMixed arg;
		arg.UIntValue = i;

		GuestParam p;
		p.index = i;
		p.name  = boundedString(
			static_cast< const char* >( plugMain( FF_GET_PARAMETER_NAME, arg, nullptr ).PointerValue ), 16 );
		p.type = plugMain( FF_GET_PARAMETER_TYPE, arg, nullptr ).UIntValue;

		const FFMixed def = plugMain( FF_GET_PARAMETER_DEFAULT, arg, nullptr );
		if( p.type == FF_TYPE_TEXT || p.type == FF_TYPE_FILE )
			p.textDefault = def.PointerValue ? static_cast< const char* >( def.PointerValue ) : "";
		else
			std::memcpy( &p.defaultValue, &def.UIntValue, sizeof( float ) );

		// Range is FFGL 2.2; plugins predating it answer FF_FAIL and keep 0..1.
		RangeStruct range{};
		FFMixed rangeArg;
		GetRangeStruct getRange{ i, range };
		rangeArg.PointerValue = &getRange;
		if( plugMain( FF_GET_RANGE, rangeArg, nullptr ).UIntValue == FF_SUCCESS )
		{
			p.rangeMin = getRange.range.min;
			p.rangeMax = getRange.range.max;
		}

		// FF_GET_PARAM_GROUP is a fill-my-buffer call, not a give-me-a-pointer
		// one: the host supplies the storage and the plugin memcpys into it,
		// WITHOUT a terminating nul.
		{
			char groupBuffer[ 64 ] = {};
			GetStringStruct getGroup{};
			getGroup.parameterNumber        = i;
			getGroup.stringBuffer.address   = groupBuffer;
			getGroup.stringBuffer.maxToWrite = sizeof( groupBuffer ) - 1;
			FFMixed groupArg;
			groupArg.PointerValue = &getGroup;
			if( plugMain( FF_GET_PARAM_GROUP, groupArg, nullptr ).UIntValue == FF_SUCCESS )
				p.group = groupBuffer;
		}

		if( p.type == FF_TYPE_OPTION )
		{
			const FFUInt32 elements = plugMain( FF_GET_NUM_PARAMETER_ELEMENTS, arg, nullptr ).UIntValue;
			for( FFUInt32 e = 0; e < elements; ++e )
			{
				GetParameterElementNameStruct en{ i, e };
				FFMixed enArg;
				enArg.PointerValue = &en;
				const char* name =
					static_cast< const char* >( plugMain( FF_GET_PARAMETER_ELEMENT_NAME, enArg, nullptr ).PointerValue );
				p.elements.push_back( name ? name : ( "Option " + std::to_string( e ) ) );

				GetParameterElementValueStruct ev{ i, e };
				FFMixed evArg;
				evArg.PointerValue = &ev;
				const FFMixed v = plugMain( FF_GET_PARAMETER_ELEMENT_VALUE, evArg, nullptr );
				float value;
				std::memcpy( &value, &v.UIntValue, sizeof( float ) );
				p.elementValues.push_back( value );
			}
		}

		out.params.push_back( std::move( p ) );
	}

	return true;
}

} // namespace

bool describe( const std::string& bundlePath, GuestInfo& out, std::string& error )
{
	const std::string binary = findBinary( bundlePath );
	if( binary.empty() )
	{
		error = "no executable inside " + bundlePath;
		return false;
	}

	void* handle = dlopen( binary.c_str(), RTLD_NOW | RTLD_LOCAL );
	if( handle == nullptr )
	{
		error = std::string( "dlopen: " ) + dlerror();
		return false;
	}

	auto plugMain = reinterpret_cast< PlugMainFn >( dlsym( handle, "plugMain" ) );
	if( plugMain == nullptr )
	{
		error = "plugMain not exported — not an FFGL plugin";
		dlclose( handle );
		return false;
	}

	const bool ok = readInfo( plugMain, out, error );

	// Closing after metadata-only queries is safe for well-behaved plugins;
	// the exit-time-destructor trap only bites when the *process* exits with
	// the module gone, and describe() callers are tools, not shows.
	dlclose( handle );
	return ok;
}

FfglGuest::~FfglGuest()
{
	destroyInstance();

	// The dlopen handle is deliberately NOT closed. The fleet learnt this the
	// hard way twice over: a module with exit-time destructors that has been
	// dlclosed leaves dangling __cxa_atexit entries, and the crash lands at
	// process exit with a one-frame backtrace. The OS reclaims it all anyway.
}

bool FfglGuest::open( const std::string& bundlePath, std::string& error )
{
	const std::string binary = findBinary( bundlePath );
	if( binary.empty() )
	{
		error = "no executable inside " + bundlePath;
		return false;
	}

	dlHandle = dlopen( binary.c_str(), RTLD_NOW | RTLD_LOCAL );
	if( dlHandle == nullptr )
	{
		error = std::string( "dlopen: " ) + dlerror();
		return false;
	}

	plugMain = reinterpret_cast< PlugMainFn >( dlsym( dlHandle, "plugMain" ) );
	if( plugMain == nullptr )
	{
		error = "plugMain not exported — not an FFGL plugin";
		return false;
	}

	return readInfo( plugMain, guestInfo, error );
}

void FfglGuest::destroyInstance()
{
	if( cglContext == nullptr )
		return;

	CGLSetCurrentContext( (CGLContextObj)cglContext );

	if( instance != nullptr && plugMain != nullptr )
	{
		FFMixed zero;
		zero.UIntValue = 0;
		plugMain( FF_DEINSTANTIATE_GL, zero, instance );
		instance = nullptr;
	}

	if( hostFbo )
		glDeleteFramebuffers( 1, &hostFbo );
	if( inTexture )
		glDeleteTextures( 1, &inTexture );
	if( outTexture )
		glDeleteTextures( 1, &outTexture );
	hostFbo = inTexture = outTexture = 0;

	CGLSetCurrentContext( nullptr );
	CGLDestroyContext( (CGLContextObj)cglContext );
	cglContext = nullptr;
	width = height = 0;
}

bool FfglGuest::ensureInstance( int newWidth, int newHeight, std::string& error )
{
	if( cglContext != nullptr && width == newWidth && height == newHeight )
		return true;

	destroyInstance();

	CGLContextObj ctx = createCoreContext( error );
	if( ctx == nullptr )
		return false;
	cglContext = ctx;
	CGLSetCurrentContext( ctx );

	width  = newWidth;
	height = newHeight;

	glGenTextures( 1, &inTexture );
	glBindTexture( GL_TEXTURE_2D, inTexture );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );

	glGenTextures( 1, &outTexture );
	glBindTexture( GL_TEXTURE_2D, outTexture );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	glBindTexture( GL_TEXTURE_2D, 0 );

	glGenFramebuffers( 1, &hostFbo );
	glBindFramebuffer( GL_FRAMEBUFFER, hostFbo );
	glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, outTexture, 0 );
	if( glCheckFramebufferStatus( GL_FRAMEBUFFER ) != GL_FRAMEBUFFER_COMPLETE )
	{
		error = "host FBO incomplete";
		destroyInstance();
		return false;
	}

	FFGLViewportStruct viewport{ 0, 0, (GLuint)width, (GLuint)height };
	FFMixed instArg;
	instArg.PointerValue    = &viewport;
	const FFMixed instResult = plugMain( FF_INSTANTIATE_GL, instArg, nullptr );
	if( instResult.UIntValue == FF_FAIL || instResult.PointerValue == nullptr )
	{
		error = "FF_INSTANTIATE_GL failed";
		destroyInstance();
		return false;
	}
	instance = instResult.PointerValue;

	CGLSetCurrentContext( nullptr );
	return true;
}

void FfglGuest::setParam( FFUInt32 index, float value )
{
	if( instance == nullptr )
		return;
	SetParameterStruct sp{};
	sp.ParameterNumber = index;
	std::memcpy( &sp.NewParameterValue.UIntValue, &value, sizeof( float ) );
	FFMixed arg;
	arg.PointerValue = &sp;
	plugMain( FF_SET_PARAMETER, arg, instance );
}

void FfglGuest::setTextParam( FFUInt32 index, const std::string& value )
{
	if( instance == nullptr )
		return;
	SetParameterStruct sp{};
	sp.ParameterNumber              = index;
	sp.NewParameterValue.PointerValue = const_cast< char* >( value.c_str() );
	FFMixed arg;
	arg.PointerValue = &sp;
	plugMain( FF_SET_PARAMETER, arg, instance );
}

void FfglGuest::setTime( double seconds )
{
	pendingTime = seconds;
}

bool FfglGuest::render( const uint8_t* rgbaIn, uint8_t* rgbaOut, std::string& error )
{
	if( instance == nullptr || cglContext == nullptr )
	{
		error = "no instance";
		return false;
	}

	CGLSetCurrentContext( (CGLContextObj)cglContext );

	if( pendingTime >= 0.0 )
	{
		FFMixed timeArg;
		timeArg.PointerValue = &pendingTime;
		plugMain( FF_SET_TIME, timeArg, instance );
		pendingTime = -1.0;
	}

	FFGLTextureStruct inStruct{};
	FFGLTextureStruct* textures[ 1 ] = { &inStruct };

	ProcessOpenGLStruct pgl{};
	pgl.HostFBO = hostFbo;

	if( rgbaIn != nullptr )
	{
		glBindTexture( GL_TEXTURE_2D, inTexture );
		glPixelStorei( GL_UNPACK_ALIGNMENT, 1 );
		glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgbaIn );
		glBindTexture( GL_TEXTURE_2D, 0 );

		inStruct.Width          = width;
		inStruct.Height         = height;
		inStruct.HardwareWidth  = width;
		inStruct.HardwareHeight = height;
		inStruct.Handle         = inTexture;

		pgl.numInputTextures = 1;
		pgl.inputTextures    = textures;
	}

	glBindFramebuffer( GL_FRAMEBUFFER, hostFbo );
	glViewport( 0, 0, width, height );
	glClearColor( 0, 0, 0, 0 );
	glClear( GL_COLOR_BUFFER_BIT );

	FFMixed pglArg;
	pglArg.PointerValue = &pgl;
	if( plugMain( FF_PROCESS_OPENGL, pglArg, instance ).UIntValue == FF_FAIL )
	{
		error = "FF_PROCESS_OPENGL returned FF_FAIL";
		CGLSetCurrentContext( nullptr );
		return false;
	}

	glBindFramebuffer( GL_READ_FRAMEBUFFER, hostFbo );
	glPixelStorei( GL_PACK_ALIGNMENT, 1 );
	glReadPixels( 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgbaOut );

	const GLenum glErr = glGetError();
	CGLSetCurrentContext( nullptr );

	if( glErr != GL_NO_ERROR )
	{
		error = "GL error 0x" + std::to_string( glErr ) + " after readback";
		return false;
	}
	return true;
}

} // namespace ffglguest
