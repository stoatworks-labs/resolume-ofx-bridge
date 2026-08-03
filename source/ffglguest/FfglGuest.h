#pragma once
//
// Host an FFGL plugin as a *guest*: load its bundle, read its parameter
// table, and push frames through ProcessOpenGL in an offscreen GL context.
//
// This is the second guest type in the bridge's any-to-any model. The first —
// OFX — lives in source/ofxbridge/ and has always been the point of the repo;
// this one turns the machinery of ffgltest (which drives generated FFGL
// bundles for testing) into a library, so that a *shell* speaking some other
// host's plugin API can carry an FFGL effect inside it.
//
// Deliberately macOS-first, like the rest of the repo's GL work: the offscreen
// context is CGL. The Windows path would be WGL with a hidden window and is
// not written.
//
// What a guest cannot promise its new host:
//
//   - FFGL is sequential and stateful. SetTime is forwarded before every
//     render, but a plugin that integrates its own clock (rather than being a
//     pure function of the time it is handed) will behave under scrubbing the
//     way a tape machine does, not the way a file does.
//   - FFGL is an 8-bit world. Frames cross the boundary as RGBA8, whatever
//     depth the outer host works in.
//

#include <FFGL.h>

#include <string>
#include <vector>

namespace ffglguest {

/// One parameter, as the plugin's own metadata describes it.
struct GuestParam
{
	FFUInt32 index = 0;
	std::string name;
	FFUInt32 type   = FF_TYPE_STANDARD;
	float defaultValue = 0.0f;
	float rangeMin     = 0.0f;
	float rangeMax     = 1.0f;
	std::string textDefault;
	std::string group;
	std::vector< std::string > elements;      //!< FF_TYPE_OPTION only
	std::vector< float > elementValues;
};

struct GuestInfo
{
	std::string name;
	std::string uniqueId;    //!< the 4-character FFGL plugin id
	FFUInt32 pluginType = 0; //!< FF_EFFECT or FF_SOURCE
	int minInputs = 1;
	int maxInputs = 1;
	std::vector< GuestParam > params;
};

/// Read a bundle's metadata without touching GL. Safe to call on any thread;
/// the bundle is opened, interrogated through plugMain, and closed again.
bool describe( const std::string& bundlePath, GuestInfo& out, std::string& error );

/// A live instance: its own dlopen handle, its own CGL context, its own
/// textures. One FfglGuest per outer-host instance; render() is not
/// re-entrant, and because GL contexts and drivers disagree about threads,
/// callers that render from a thread pool must serialise (the OFX shell
/// holds a global mutex).
class FfglGuest
{
public:
	~FfglGuest();

	FfglGuest()                              = default;
	FfglGuest( const FfglGuest& )            = delete;
	FfglGuest& operator=( const FfglGuest& ) = delete;

	/// dlopen the bundle and read its metadata. No GL yet.
	bool open( const std::string& bundlePath, std::string& error );

	const GuestInfo& info() const { return guestInfo; }

	/// (Re)create the GL context, the textures and the plugin instance at this
	/// frame size. Idempotent per size; a size change tears down and rebuilds,
	/// because FFGL plugins size their buffers from the viewport they were
	/// instantiated with.
	bool ensureInstance( int width, int height, std::string& error );

	void setParam( FFUInt32 index, float value );
	void setTextParam( FFUInt32 index, const std::string& value );

	/// Seconds. Forwarded before the next render.
	void setTime( double seconds );

	/// Push one frame through the plugin. `rgbaIn` may be null for a source
	/// plugin (FFGL sources take no input texture). Both buffers are
	/// width*height*4, row 0 at the *bottom* (the GL and OFX convention, so no
	/// flip happens at this boundary).
	bool render( const uint8_t* rgbaIn, uint8_t* rgbaOut, std::string& error );

private:
	void destroyInstance();

	using PlugMainFn = FFMixed ( * )( FFUInt32, FFMixed, FFInstanceID );

	void* dlHandle       = nullptr;
	PlugMainFn plugMain  = nullptr;
	GuestInfo guestInfo;

	void* cglContext     = nullptr; //!< CGLContextObj, void* to keep CGL out of this header
	FFInstanceID instance = nullptr;
	unsigned inTexture   = 0;
	unsigned outTexture  = 0;
	unsigned hostFbo     = 0;
	int width            = 0;
	int height           = 0;
	double pendingTime   = -1.0;
};

} // namespace ffglguest
