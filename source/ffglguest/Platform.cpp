#include "Platform.h"

#include <filesystem>

#if defined( __APPLE__ )
	#include <OpenGL/OpenGL.h>
	#include <OpenGL/gl3.h>
	#include <dlfcn.h>
#elif defined( _WIN32 )
	#include <windows.h>
	#include <GL/glew.h>
	#include <GL/wglew.h>
#else
	#include <EGL/egl.h>
	#include <GL/glew.h>
	#include <dlfcn.h>
#endif

namespace fs = std::filesystem;

namespace ffglguest {
namespace platform {

std::string binaryInside( const std::string& pluginPath )
{
	std::error_code ec;
	fs::path p = pluginPath;
	if( !fs::is_directory( p, ec ) )
		return p.string();

	// A macOS bundle. Windows and Linux FFGL plugins are plain .dll/.so files,
	// so this branch simply never fires there.
	const fs::path macos = p / "Contents" / "MacOS";
	for( const auto& e : fs::directory_iterator( macos, ec ) )
		return e.path().string();
	return std::string();
}

// ---------------------------------------------------------------------------
// Module
// ---------------------------------------------------------------------------

bool Module::load( const std::string& path, std::string& error )
{
	const std::string binary = binaryInside( path );
	if( binary.empty() )
	{
		error = "no loadable binary in " + path;
		return false;
	}

#if defined( _WIN32 )
	handle = (void*)LoadLibraryA( binary.c_str() );
	if( handle == nullptr )
	{
		error = "LoadLibrary failed (" + std::to_string( (unsigned long)GetLastError() ) + ")";
		return false;
	}
#else
	handle = dlopen( binary.c_str(), RTLD_NOW | RTLD_LOCAL );
	if( handle == nullptr )
	{
		error = std::string( "dlopen: " ) + dlerror();
		return false;
	}
#endif
	return true;
}

void* Module::symbol( const char* name ) const
{
	if( handle == nullptr )
		return nullptr;
#if defined( _WIN32 )
	return (void*)GetProcAddress( (HMODULE)handle, name );
#else
	return dlsym( handle, name );
#endif
}

// ---------------------------------------------------------------------------
// GlContext
// ---------------------------------------------------------------------------

#if defined( __APPLE__ )

bool GlContext::create( std::string& error, bool legacy )
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
		error = "no suitable GL pixel format";
		return false;
	}

	CGLContextObj ctx  = nullptr;
	const CGLError err = CGLCreateContext( pix, nullptr, &ctx );
	CGLDestroyPixelFormat( pix );
	if( err != kCGLNoError || ctx == nullptr )
	{
		error = "CGLCreateContext failed (" + std::to_string( err ) + ")";
		return false;
	}
	handle = ctx;
	return true;
}

void GlContext::makeCurrent()  { CGLSetCurrentContext( (CGLContextObj)handle ); }
void GlContext::clearCurrent() { CGLSetCurrentContext( nullptr ); }

void GlContext::destroy()
{
	if( handle == nullptr )
		return;
	CGLSetCurrentContext( nullptr );
	CGLDestroyContext( (CGLContextObj)handle );
	handle = nullptr;
}

#elif defined( _WIN32 )

namespace {
struct WinSurface
{
	HWND window = nullptr;
	HDC dc      = nullptr;
};
} // namespace

namespace {
/// Whether this process can put a window on a desktop at all.
///
/// WGL has no offscreen path: a context needs a window, a window needs a
/// desktop, and a process in session 0 -- a service, a scheduled task with no
/// logged-in user, anything launched by a remote-exec agent -- has neither.
/// Some drivers return an error there; the Parallels one loads and takes the
/// process down with no output at all, which reads as "the GL code is broken"
/// when nothing was ever wrong with it.
///
/// Checked first so that failure has a sentence attached to it.
bool haveInteractiveWindowStation()
{
	const HWINSTA station = GetProcessWindowStation();
	if( station == nullptr )
		return false;

	USEROBJECTFLAGS flags = {};
	DWORD needed          = 0;
	if( !GetUserObjectInformationA( station, UOI_FLAGS, &flags, sizeof( flags ), &needed ) )
		return false;

	return ( flags.dwFlags & WSF_VISIBLE ) != 0;
}
} // namespace

bool GlContext::create( std::string& error, bool legacy )
{
	if( !haveInteractiveWindowStation() )
	{
		error = "no interactive window station, so WGL cannot create a context. "
				"Run this from a logged-in desktop session rather than a service "
				"or a headless remote-exec agent.";
		return false;
	}

	// WGL's chicken and egg: wglCreateContextAttribsARB — the only way to ask
	// for a core profile — is itself a WGL extension, so a legacy context has
	// to exist first just to look it up. Both live on a hidden window; there
	// is no offscreen path in plain WGL.
	static bool classRegistered = false;
	if( !classRegistered )
	{
		WNDCLASSA wc  = {};
		wc.lpfnWndProc   = DefWindowProcA;
		wc.hInstance     = GetModuleHandleA( nullptr );
		wc.lpszClassName = "StoatworksFfglGuest";
		if( RegisterClassA( &wc ) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS )
		{
			error = "RegisterClass failed";
			return false;
		}
		classRegistered = true;
	}

	auto* surface = new WinSurface();
	surface->window = CreateWindowExA( 0, "StoatworksFfglGuest", "", WS_OVERLAPPEDWINDOW,
									   0, 0, 16, 16, nullptr, nullptr,
									   GetModuleHandleA( nullptr ), nullptr );
	if( surface->window == nullptr )
	{
		delete surface;
		error = "CreateWindow failed";
		return false;
	}
	surface->dc = GetDC( surface->window );

	PIXELFORMATDESCRIPTOR pfd = {};
	pfd.nSize      = sizeof( pfd );
	pfd.nVersion   = 1;
	pfd.dwFlags    = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL;
	pfd.iPixelType = PFD_TYPE_RGBA;
	pfd.cColorBits = 24;
	pfd.cAlphaBits = 8;

	const int format = ChoosePixelFormat( surface->dc, &pfd );
	if( format == 0 || !SetPixelFormat( surface->dc, format, &pfd ) )
	{
		ReleaseDC( surface->window, surface->dc );
		DestroyWindow( surface->window );
		delete surface;
		error = "no suitable pixel format";
		return false;
	}

	HGLRC legacyCtx = wglCreateContext( surface->dc );
	if( legacyCtx == nullptr )
	{
		ReleaseDC( surface->window, surface->dc );
		DestroyWindow( surface->window );
		delete surface;
		error = "wglCreateContext failed";
		return false;
	}
	wglMakeCurrent( surface->dc, legacyCtx );

	if( glewInit() != GLEW_OK )
	{
		wglMakeCurrent( nullptr, nullptr );
		wglDeleteContext( legacyCtx );
		ReleaseDC( surface->window, surface->dc );
		DestroyWindow( surface->window );
		delete surface;
		error = "glewInit failed";
		return false;
	}

	if( legacy )
	{
		// The context that already exists is the compatibility one, so this is
		// simply not throwing it away. glewInit has run against it.
		wglMakeCurrent( nullptr, nullptr );
		handle = legacyCtx;
		extra  = surface;
		return true;
	}

	HGLRC core = nullptr;
	if( wglewIsSupported( "WGL_ARB_create_context" ) && wglCreateContextAttribsARB != nullptr )
	{
		const int attribs[] = {
			WGL_CONTEXT_MAJOR_VERSION_ARB, 4,
			WGL_CONTEXT_MINOR_VERSION_ARB, 1,
			WGL_CONTEXT_PROFILE_MASK_ARB,  WGL_CONTEXT_CORE_PROFILE_BIT_ARB,
			0
		};
		core = wglCreateContextAttribsARB( surface->dc, nullptr, attribs );
	}

	wglMakeCurrent( nullptr, nullptr );
	if( core != nullptr )
	{
		wglDeleteContext( legacyCtx );
		handle = core;
	}
	else
	{
		// No core profile available. FFGL 2.x shaders are `#version 410 core`
		// and will not compile here, so say so rather than fail later inside
		// somebody's shader.
		wglDeleteContext( legacyCtx );
		ReleaseDC( surface->window, surface->dc );
		DestroyWindow( surface->window );
		delete surface;
		error = "no OpenGL 4.1 core profile available on this display driver";
		return false;
	}

	extra = surface;
	return true;
}

void GlContext::makeCurrent()
{
	auto* s = (WinSurface*)extra;
	if( s != nullptr )
		wglMakeCurrent( s->dc, (HGLRC)handle );
}

void GlContext::clearCurrent() { wglMakeCurrent( nullptr, nullptr ); }

void GlContext::destroy()
{
	if( handle == nullptr )
		return;
	wglMakeCurrent( nullptr, nullptr );
	wglDeleteContext( (HGLRC)handle );
	handle = nullptr;

	auto* s = (WinSurface*)extra;
	if( s != nullptr )
	{
		ReleaseDC( s->window, s->dc );
		DestroyWindow( s->window );
		delete s;
		extra = nullptr;
	}
}

#else // Linux

bool GlContext::create( std::string& error, bool legacy )
{
	// Surfaceless EGL: no X display, no window, nothing to fail on a headless
	// render node. The FBO the guest draws into is all the surface needed.
	EGLDisplay display = eglGetDisplay( EGL_DEFAULT_DISPLAY );
	if( display == EGL_NO_DISPLAY )
	{
		error = "eglGetDisplay failed";
		return false;
	}
	if( !eglInitialize( display, nullptr, nullptr ) )
	{
		error = "eglInitialize failed";
		return false;
	}
	if( !eglBindAPI( EGL_OPENGL_API ) )
	{
		error = "eglBindAPI(OpenGL) failed — this driver is GLES only";
		return false;
	}

	const EGLint configAttribs[] = {
		EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
		EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
		EGL_NONE
	};
	EGLConfig config = nullptr;
	EGLint numConfigs = 0;
	if( !eglChooseConfig( display, configAttribs, &config, 1, &numConfigs ) || numConfigs == 0 )
	{
		error = "no suitable EGL config";
		return false;
	}

	const EGLint coreAttribs[] = {
		EGL_CONTEXT_MAJOR_VERSION, 4,
		EGL_CONTEXT_MINOR_VERSION, 1,
		EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
		EGL_NONE
	};
	// Ask for nothing in particular and the driver gives its default, which is
	// the compatibility profile wherever one exists.
	const EGLint legacyAttribs[] = { EGL_NONE };

	const EGLint* contextAttribs = legacy ? legacyAttribs : coreAttribs;
	EGLContext ctx = eglCreateContext( display, config, EGL_NO_CONTEXT, contextAttribs );
	if( ctx == EGL_NO_CONTEXT )
	{
		error = "eglCreateContext failed — no OpenGL 4.1 core profile";
		return false;
	}

	handle = ctx;
	extra  = display;

	eglMakeCurrent( display, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx );
	if( glewInit() != GLEW_OK )
	{
		error = "glewInit failed";
		eglMakeCurrent( display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT );
		return false;
	}
	eglMakeCurrent( display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT );
	return true;
}

void GlContext::makeCurrent()
{
	eglMakeCurrent( (EGLDisplay)extra, EGL_NO_SURFACE, EGL_NO_SURFACE, (EGLContext)handle );
}

void GlContext::clearCurrent()
{
	eglMakeCurrent( (EGLDisplay)extra, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT );
}

void GlContext::destroy()
{
	if( handle == nullptr )
		return;
	eglMakeCurrent( (EGLDisplay)extra, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT );
	eglDestroyContext( (EGLDisplay)extra, (EGLContext)handle );
	handle = nullptr;
	extra  = nullptr;
}

#endif

} // namespace platform
} // namespace ffglguest
