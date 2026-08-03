#pragma once
//
// The two things hosting an FFGL guest needs from the operating system: a way
// to load a shared library, and an offscreen OpenGL context to render it in.
//
// Both are different on all three platforms and neither is interesting, so
// they live here and FfglGuest.cpp stays about FFGL.
//
//   macOS    CGL, and a 4.1 core profile — the profile Resolume itself uses.
//   Windows  WGL, which cannot make a core context without first making a
//            legacy one on a real window to find wglCreateContextAttribsARB.
//   Linux    EGL with the surfaceless platform, so no X display is needed.
//
// The Windows and Linux paths are **compiled but have never been run**; the
// author has no Windows or Linux machine with a GPU to test them on. Treat
// them accordingly, and see docs/05-any-to-any.md.
//

#include <string>

namespace ffglguest {
namespace platform {

/// A loaded shared library. Deliberately never unloaded: a guest with
/// exit-time destructors that has been unloaded leaves dangling atexit
/// entries, and the crash lands at process exit with a one-frame backtrace.
struct Module
{
	void* handle = nullptr;

	/// Load `path`. Accepts a bundle directory on macOS (the binary inside is
	/// found) or a plain library file anywhere.
	bool load( const std::string& path, std::string& error );

	/// Resolve an exported symbol, or nullptr.
	void* symbol( const char* name ) const;
};

/// An offscreen OpenGL context, current on the calling thread while `bind` is
/// in effect. One per guest instance.
struct GlContext
{
	void* handle = nullptr;//!< CGLContextObj / HGLRC / EGLContext
	void* extra  = nullptr;//!< HWND+HDC pair on Windows, EGLDisplay on Linux

	bool create( std::string& error );
	void makeCurrent();
	void clearCurrent();
	void destroy();

	bool valid() const { return handle != nullptr; }
};

/// The path of the loadable binary inside a plugin, given the path the caller
/// was handed. A macOS bundle contains it; elsewhere the path IS the binary.
std::string binaryInside( const std::string& pluginPath );

} // namespace platform
} // namespace ffglguest
