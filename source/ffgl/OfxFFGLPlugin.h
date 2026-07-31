#pragma once
//
// The generic FFGL plugin that hosts one OFX effect.
//
// A single build of this class is copied per OFX plugin; which effect it loads
// is decided at runtime by the manifest found next to the binary. See
// SelfPath.h for how it locates that.
//

#include "Manifest.h"

#include "FFGLSDK.h"

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

	/// Push every FFGL parameter value into the OFX instance. Called before each
	/// render; cheap compared with the pixel round trip and avoids having to
	/// track which values Resolume changed.
	void applyParams();

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
	std::vector< float > _values;
	std::vector< std::string > _textValues;

	GLuint _readFbo   = 0;
	GLuint _blitFbo   = 0;
	GLuint _blitTex   = 0;
	int _texWidth     = 0;
	int _texHeight    = 0;

	int _effectWidth  = 0;
	int _effectHeight = 0;
	bool _effectFailed = false;

	double _time = 0.0;
};

} // namespace ofxffgl
