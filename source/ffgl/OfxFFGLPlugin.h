#pragma once
//
// The generic FFGL plugin that hosts one OFX effect.
//
// A single build of this class is copied per OFX plugin; which effect it loads
// is decided at runtime by the manifest found next to the binary. See
// SelfPath.h for how it locates that.
//

#include "Manifest.h"
#include "MetalBridge.h"
#include "OpenCLBridge.h"

#include "FFGLSDK.h"

#include "../StoatworksAboutParams.h"

#include <memory>
#include <string>
#include <vector>

namespace ofxbridge {
class Host;
class Effect;
struct Frame;
} // namespace ofxbridge

namespace ofxffgl {

/// Process-wide state established from the manifest before Resolume asks the
/// plugin anything. Loading is attempted once and the outcome cached, because
/// FFGL gives us no way to report a load failure other than behaving inertly.
struct PluginContext
{
	bool loaded = false;
	std::string error;
	Manifest manifest;
	std::vector< FfglParam > params;
	std::string pluginId;
	std::string pluginName;
	std::string description;

	static const PluginContext& get();
};

class OfxFFGLPlugin : public CFFGLPlugin
{
public:
	OfxFFGLPlugin();
	~OfxFFGLPlugin() override;

	FFResult InitGL( const FFGLViewportStruct* vp ) override;
	FFResult DeInitGL() override;
	FFResult ProcessOpenGL( ProcessOpenGLStruct* pGL ) override;

	FFResult SetFloatParameter( unsigned int index, float value ) override;
	float GetFloatParameter( unsigned int index ) override;
	FFResult SetTextParameter( unsigned int index, const char* value ) override;
	char* GetTextParameter( unsigned int index ) override;

private:
	/// Create the OFX instance. Deferred until we have a GL context and a frame
	/// size, because clip preferences depend on the image we will actually pass.
	bool ensureEffect( int width, int height );
	void destroyEffect();

	/// Push the FFGL parameter values Resolume changed into the OFX instance,
	/// then deliver kOfxActionInstanceChanged for them the way a real host
	/// would. Called before each render. Only *changed* values cross: pushing
	/// everything every frame would overwrite values the plugin set on itself
	/// (a preset choice filling in the sliders).
	void applyParams();

	/// The plugin changed one of its own params. Refresh our copy so the
	/// per-frame push does not undo it, and raise FF_EVENT_FLAG_VALUE so
	/// Resolume re-reads the control.
	void syncParamFromOfx( const std::string& ofxName );

	/// True when both the plugin and we can take the GL texture path, so no pixel
	/// crosses to the CPU.
	bool useGLPath() const;

	/// True when the plugin renders with Metal. Preferred over the GL path when
	/// a plugin somehow offers both, because it is what Resolve-targeted plugins
	/// actually implement.
	bool useMetalPath() const;

	/// Render through Metal: GL texture -> shared IOSurface -> MTLBuffer ->
	/// plugin -> back, with no CPU round trip.
	bool renderViaMetal( ProcessOpenGLStruct* pGL, const FFGLTextureStruct& in, int width, int height );

	/// True when the plugin renders with OpenCL.
	bool useOpenCLPath() const;

	/// Render through OpenCL: GL texture -> shared PBO -> cl_mem -> plugin ->
	/// back, with no CPU round trip.
	bool renderViaOpenCL( ProcessOpenGLStruct* pGL, const FFGLTextureStruct& in, int width, int height );

	/// Render straight from `in` into our blit texture, with no CPU round trip.
	bool renderViaGL( ProcessOpenGLStruct* pGL, const FFGLTextureStruct& in, int width, int height );

	/// Make sure `_blitTex` exists and is `width` x `height`.
	void ensureBlitTexture( int width, int height );

	/// Pull `texture` into `_input`.
	bool readbackTexture( const FFGLTextureStruct& texture );
	/// Push `_output` to the currently bound draw framebuffer.
	bool uploadAndBlit( ProcessOpenGLStruct* pGL, int width, int height );

	void releaseGLResources();

	std::unique_ptr< ofxbridge::Host > _host;
	std::unique_ptr< ofxbridge::Effect > _effect;
	std::unique_ptr< ofxbridge::Frame > _input;
	std::unique_ptr< ofxbridge::Frame > _output;

	/// Current FFGL parameter values, indexed as the param table.
	/// Where the About block starts. The OFX plugin's own parameters come
	/// first and there is no compile-time count of them, so this is the one
	/// FFGL param base in the fleet that is worked out at construction.
	unsigned int _aboutBase = 0;
	/// GetTextParameter hands the host a bare pointer, so this outlives the call.
	std::string _aboutText;

	std::vector< float > _values;
	std::vector< std::string > _textValues;
	/// Which of _values changed since the last applyParams().
	std::vector< bool > _dirty;
	/// The first push after (re)instantiation is setup, not a user edit, so it
	/// gets no instanceChanged action.
	bool _pushedOnce = false;

	GLuint _readFbo   = 0;
	GLuint _blitFbo   = 0;
	GLuint _blitTex   = 0;
	int _texWidth     = 0;
	int _texHeight    = 0;

	int _effectWidth  = 0;
	int _effectHeight = 0;
	bool _effectFailed = false;
	bool _glContextAttached = false;

	MetalBridge _metal;
	bool _metalReady  = false;
	bool _metalFailed = false;

	OpenCLBridge _opencl;
	bool _openclReady  = false;
	bool _openclFailed = false;

	double _time = 0.0;
};

} // namespace ofxffgl
