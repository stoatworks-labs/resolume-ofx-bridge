#include "OfxFFGLPlugin.h"
#include "SelfPath.h"

#include "Host.h"
#include "Catalog.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>

namespace ofxffgl {

// ---------------------------------------------------------------------------
// PluginContext
// ---------------------------------------------------------------------------

const PluginContext& PluginContext::get()
{
	// Function-local static: initialised on first use, which is before Resolume
	// queries anything but after the dynamic loader has finished with us — the
	// only window in which dladdr can resolve our own path.
	static PluginContext ctx = [] {
		PluginContext c;

		const std::string path = selfManifestPath();
		if( path.empty() )
		{
			c.error = "could not determine own binary path";
			return c;
		}

		std::string error;
		if( !Manifest::load( path, c.manifest, error ) )
		{
			c.error = "manifest (" + path + "): " + error;
			return c;
		}

		c.params      = buildParamTable( c.manifest );
		c.pluginId    = makePluginId( c.manifest.identifier );
		c.pluginName  = c.manifest.label.empty() ? c.manifest.identifier : c.manifest.label;
		c.description = c.manifest.grouping.empty()
							? ( "OFX: " + c.manifest.identifier )
							: ( "OFX " + c.manifest.grouping + ": " + c.manifest.identifier );
		c.loaded      = true;
		return c;
	}();
	return ctx;
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

OfxFFGLPlugin::OfxFFGLPlugin()
{
	SetMinInputs( 1 );
	SetMaxInputs( 1 );

	const PluginContext& ctx = PluginContext::get();

	_values.resize( ctx.params.size(), 0.0f );
	_textValues.resize( ctx.params.size() );
	_dirty.resize( ctx.params.size(), true );

	for( size_t i = 0; i < ctx.params.size(); ++i )
	{
		const FfglParam& p    = ctx.params[ i ];
		const unsigned int id = (unsigned int)i;

		if( p.ffglType == FF_TYPE_TEXT )
		{
			SetParamInfo( id, p.name.c_str(), FF_TYPE_TEXT, p.textDefault.c_str() );
			_textValues[ i ] = p.textDefault;
		}
		else if( p.ffglType == FF_TYPE_OPTION )
		{
			SetOptionParamInfo( id, p.name.c_str(), (unsigned int)p.elements.size(), p.defaultValue );
			for( size_t e = 0; e < p.elements.size(); ++e )
				SetParamElementInfo( id, (unsigned int)e, p.elements[ e ].c_str(), (float)e );
			// SetOptionParamInfo does not set a range, so without this a choice
			// param reports 0..1 however many options it has.
			SetParamRange( id, p.rangeMin, p.rangeMax );
			_values[ i ] = p.defaultValue;
		}
		else if( p.ffglType == FF_TYPE_BOOLEAN || p.ffglType == FF_TYPE_EVENT )
		{
			SetParamInfo( id, p.name.c_str(), p.ffglType, p.defaultValue != 0.0f );
			_values[ i ] = p.defaultValue;
		}
		else
		{
			SetParamInfo( id, p.name.c_str(), p.ffglType, p.defaultValue );
			SetParamRange( id, p.rangeMin, p.rangeMax );
			_values[ i ] = p.defaultValue;
		}

		// The display name is what the user reads; the name above stays fixed so
		// Resolume's serialisation is stable even if a label changes.
		if( !p.displayName.empty() && p.displayName != p.name )
			SetParamDisplayName( id, p.displayName, false );
		if( !p.group.empty() )
			SetParamGroup( id, p.group );
		if( !p.visible )
			SetParamVisibility( id, false, false );
	}

	// The About block, after the OFX plugin's own parameters. FFGL has no
	// window, so the name, the version, the maker and the links are parameters
	// the host draws. See ../StoatworksAboutParams.h.
	//
	// The credit is the declared default as well as the answer to
	// GetTextParameter: nothing in FFGL says which of the two a host reads, and
	// supplying one but not the other shows a blank line in whichever host
	// picked the other.
	_aboutBase = (unsigned int)ctx.params.size();
	SetParamInfo( _aboutBase, "About", FF_TYPE_TEXT, stoatworks::about::defaultText() );
	{
		unsigned int aboutId = _aboutBase + 1;
		for( const auto& b : stoatworks::about::buttons() )
			SetParamInfo( aboutId++, b.label, FF_TYPE_EVENT, false );
	}
}

OfxFFGLPlugin::~OfxFFGLPlugin()
{
	destroyEffect();
}

// ---------------------------------------------------------------------------
// GL lifecycle
// ---------------------------------------------------------------------------

FFResult OfxFFGLPlugin::InitGL( const FFGLViewportStruct* vp )
{
	if( !PluginContext::get().loaded )
	{
		// Say why. A swallowed context error looks identical to every other
		// way FF_INSTANTIATE_GL can fail, and cost real time to attribute.
		fprintf( stderr, "ofxbridge: plugin context not loaded: %s\n",
				 PluginContext::get().error.c_str() );
		return FF_FAIL;
	}

	glGenFramebuffers( 1, &_readFbo );
	glGenFramebuffers( 1, &_blitFbo );
	glGenTextures( 1, &_blitTex );

	// The base implementation dereferences vp unconditionally, so it must be
	// forwarded rather than passed as null.
	return vp != nullptr ? CFFGLPlugin::InitGL( vp ) : FF_SUCCESS;
}

FFResult OfxFFGLPlugin::DeInitGL()
{
	releaseGLResources();
	destroyEffect();
	_metal.shutdown();
	_metalReady = false;
	_opencl.shutdown();
	_openclReady = false;
	return FF_SUCCESS;
}

void OfxFFGLPlugin::releaseGLResources()
{
	if( _readFbo )
		glDeleteFramebuffers( 1, &_readFbo );
	if( _blitFbo )
		glDeleteFramebuffers( 1, &_blitFbo );
	if( _blitTex )
		glDeleteTextures( 1, &_blitTex );
	_readFbo = _blitFbo = _blitTex = 0;
	_texWidth = _texHeight = 0;
}

// ---------------------------------------------------------------------------
// OFX instance lifecycle
// ---------------------------------------------------------------------------

bool OfxFFGLPlugin::ensureEffect( int width, int height )
{
	if( _effectFailed )
		return false;
	if( _effect && _effectWidth == width && _effectHeight == height )
		return true;

	// A resize changes clip preferences, so the instance is rebuilt rather than
	// resized in place.
	destroyEffect();

	const PluginContext& ctx = PluginContext::get();

	_host = std::make_unique< ofxbridge::Host >();

	// Note: this ends up loading every OFX bundle in the same directory, not just
	// ours -- see the limitation noted in Catalog.cpp::createEffect.
	std::string error;
	_effect = ofxbridge::createEffect( *_host, ctx.manifest.bundlePath, ctx.manifest.identifier, error );
	if( !_effect )
	{
		// The one line that says why. This was once swallowed, and the symptom
		// -- FF_INSTANTIATE_GL failing with no message anywhere -- cost real
		// time to attribute.
		fprintf( stderr, "ofxbridge: createEffect failed: %s\n", error.c_str() );
		_effectFailed = true;
		return false;
	}

	_effect->setFrameSize( width, height );
	if( !_effect->init( error ) )
	{
		fprintf( stderr, "ofxbridge: effect init failed: %s\n", error.c_str() );
		_effect.reset();
		_effectFailed = true;
		return false;
	}

	// A fresh instance holds only defaults: everything must be pushed once,
	// silently (setup is not a user edit). And when the plugin drives its own
	// params — a preset choice filling in the sliders — our copy follows.
	std::fill( _dirty.begin(), _dirty.end(), true );
	_pushedOnce                     = false;
	_effect->onParamChangedByPlugin = [ this ]( const std::string& name ) { syncParamFromOfx( name ); };

	// Bring Metal up before deciding the path: useMetalPath() depends on it.
	if( PluginContext::get().manifest.supportsMetalRender && !_metalReady && !_metalFailed )
	{
		std::string metalError;
		if( _metal.init( metalError ) && _metal.resize( width, height, metalError ) )
			_metalReady = true;
		else
			_metalFailed = true;
	}
	else if( _metalReady )
	{
		std::string metalError;
		if( !_metal.resize( width, height, metalError ) )
		{
			_metalReady  = false;
			_metalFailed = true;
		}
	}

	if( PluginContext::get().manifest.supportsOpenCLRender && !_openclReady && !_openclFailed )
	{
		std::string clError;
		if( _opencl.init( clError ) && _opencl.resize( width, height, clError ) )
			_openclReady = true;
		else
			_openclFailed = true;
	}
	else if( _openclReady )
	{
		std::string clError;
		if( !_opencl.resize( width, height, clError ) )
		{
			_openclReady  = false;
			_openclFailed = true;
		}
	}

	if( useMetalPath() || useOpenCLPath() )
	{
		// Nothing to allocate: the plugin reads and writes GPU buffers that share
		// memory with GL objects.
	}
	else if( useGLPath() )
	{
		// The plugin reads and writes textures, so the CPU frames would never be
		// touched -- at 4K that is 66MB of allocation saved per instance.
		if( !_effect->attachGLContext( error ) )
		{
			_effect.reset();
			_effectFailed = true;
			return false;
		}
		_glContextAttached = true;
	}
	else
	{
		_input  = std::make_unique< ofxbridge::Frame >();
		_output = std::make_unique< ofxbridge::Frame >();
		_input->allocate( width, height, /*asFloat*/ false );
		_output->allocate( width, height, /*asFloat*/ false );
	}

	_effectWidth  = width;
	_effectHeight = height;
	return true;
}

bool OfxFFGLPlugin::useMetalPath() const
{
	return PluginContext::get().manifest.supportsMetalRender && _metalReady;
}

bool OfxFFGLPlugin::useOpenCLPath() const
{
	// Metal wins on macOS when a plugin offers both: it is native, whereas
	// OpenCL here is a deprecated compatibility layer.
	if( PluginContext::get().manifest.supportsMetalRender && _metalReady )
		return false;
	return PluginContext::get().manifest.supportsOpenCLRender && _openclReady;
}

bool OfxFFGLPlugin::useGLPath() const
{
	// The GPU buffer paths win when a plugin offers both.
	if( PluginContext::get().manifest.supportsMetalRender && _metalReady )
		return false;
	if( PluginContext::get().manifest.supportsOpenCLRender && _openclReady )
		return false;

	// Both sides must agree. The host always offers it; the plugin usually does
	// not, since Resolve -- which most OFX plugins target -- uses Metal or CUDA
	// rather than the OFX OpenGL render path.
	return PluginContext::get().manifest.supportsOpenGLRender;
}

void OfxFFGLPlugin::destroyEffect()
{
	if( _effect && _glContextAttached )
		_effect->detachGLContext();
	_glContextAttached = false;
	_effect.reset();
	_host.reset();
	_input.reset();
	_output.reset();
	_effectWidth = _effectHeight = 0;
}

// ---------------------------------------------------------------------------
// Parameters
// ---------------------------------------------------------------------------

FFResult OfxFFGLPlugin::SetFloatParameter( unsigned int index, float value )
{
	// An About button is a press, not a value to keep: it opens a browser and
	// nothing about the effect changes.
	if( index >= _aboutBase )
		return stoatworks::about::handleParam( index - _aboutBase, value ) ? FF_SUCCESS : FF_FAIL;

	if( index >= _values.size() )
		return FF_FAIL;
	if( _values[ index ] != value )
	{
		_values[ index ] = value;
		_dirty[ index ]  = true;
	}
	return FF_SUCCESS;
}

float OfxFFGLPlugin::GetFloatParameter( unsigned int index )
{
	return index < _values.size() ? _values[ index ] : 0.0f;
}

FFResult OfxFFGLPlugin::SetTextParameter( unsigned int index, const char* value )
{
	// The About text is generated, never stored — but it must ACCEPT a set.
	// The SDK's instantiateGL pushes every parameter's default into a fresh
	// instance and destroys it on the first FF_FAIL, so rejecting this set
	// made every generated plugin fail to instantiate, with no message,
	// anywhere. _textValues covers only the OFX plugin's own parameters.
	if( index >= _aboutBase )
		return FF_SUCCESS;
	if( index >= _textValues.size() )
		return FF_FAIL;
	_textValues[ index ] = value ? value : "";
	return FF_SUCCESS;
}

char* OfxFFGLPlugin::GetTextParameter( unsigned int index )
{
	if( index == _aboutBase )
	{
		_aboutText = stoatworks::about::textParam( 0 );
		return const_cast< char* >( _aboutText.c_str() );
	}

	if( index >= _textValues.size() )
		return nullptr;
	return const_cast< char* >( _textValues[ index ].c_str() );
}

void OfxFFGLPlugin::applyParams()
{
	if( !_effect )
		return;

	const PluginContext& ctx = PluginContext::get();

	// Multi-component OFX params are spread across several FFGL slots, so gather
	// each one's components before pushing it across. Only params with a dirty
	// slot are pushed: unconditionally pushing every value each frame would
	// overwrite values the plugin set on itself (a preset filling in sliders).
	// On the first push everything crosses, but only values that already
	// differ from their declared default count as *edits*: the defaults
	// arriving is instance setup, while a preset picked before the first
	// frame rendered is an edit that must still reach changedParam.
	const bool firstPush = !_pushedOnce;

	std::string pending;
	std::vector< double > components;
	bool pendingDirty  = false;
	bool pendingEdited = false;
	std::vector< std::string > changed;

	auto flush = [ & ]() {
		if( !pending.empty() && !components.empty() && pendingDirty )
		{
			if( _effect->setParamValue( pending, components ) && pendingEdited )
				changed.push_back( pending );
		}
		pending.clear();
		components.clear();
		pendingDirty  = false;
		pendingEdited = false;
	};

	for( size_t i = 0; i < ctx.params.size() && i < _values.size(); ++i )
	{
		const FfglParam& p = ctx.params[ i ];

		if( p.isText )
		{
			flush();
			_effect->setParamString( p.ofxName, _textValues[ i ] );
			continue;
		}
		if( p.ffglType == FF_TYPE_EVENT )
		{
			flush();
			continue;// push buttons carry no persistent value
		}

		if( p.ofxName != pending )
			flush();

		pending = p.ofxName;
		if( (int)components.size() <= p.component )
			components.resize( p.component + 1, 0.0 );
		components[ p.component ] = (double)_values[ i ];
		if( _dirty[ i ] )
		{
			pendingDirty = true;
			_dirty[ i ]  = false;
			if( !firstPush || _values[ i ] != p.defaultValue )
				pendingEdited = true;
		}
	}
	flush();

	// A real host follows a user edit with kOfxActionInstanceChanged, and that
	// is where param-driven behaviour lives — a preset dropdown does its work
	// in changedParam, not in the value store.
	_pushedOnce = true;
	if( changed.empty() )
		return;

	OfxPointD renderScale = { 1.0, 1.0 };
	_effect->beginInstanceChangedAction( kOfxChangeUserEdited );
	for( const std::string& name : changed )
		_effect->paramInstanceChangedAction( name, kOfxChangeUserEdited, _time, renderScale );
	_effect->endInstanceChangedAction( kOfxChangeUserEdited );
}

void OfxFFGLPlugin::syncParamFromOfx( const std::string& ofxName )
{
	if( !_effect )
		return;

	std::vector< double > values;
	if( !_effect->getParamValue( ofxName, values ) )
		return;

	const PluginContext& ctx = PluginContext::get();
	for( size_t i = 0; i < ctx.params.size() && i < _values.size(); ++i )
	{
		const FfglParam& p = ctx.params[ i ];
		if( p.ofxName != ofxName || p.isText || p.ffglType == FF_TYPE_EVENT )
			continue;
		if( p.component < 0 || p.component >= (int)values.size() )
			continue;

		const float v = (float)values[ p.component ];
		if( _values[ i ] != v )
		{
			_values[ i ] = v;
			// Resolume re-reads the control on this event; a host that ignores
			// it still renders correctly and merely shows a stale knob.
			RaiseParamEvent( (unsigned int)i, FF_EVENT_FLAG_VALUE );
		}
		// Whatever the OFX side holds is now the truth; don't push ours back.
		_dirty[ i ] = false;
	}
}

// ---------------------------------------------------------------------------
// Pixel bridge
// ---------------------------------------------------------------------------

bool OfxFFGLPlugin::readbackTexture( const FFGLTextureStruct& texture )
{
	glBindFramebuffer( GL_READ_FRAMEBUFFER, _readFbo );
	glFramebufferTexture2D( GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture.Handle, 0 );

	if( glCheckFramebufferStatus( GL_READ_FRAMEBUFFER ) != GL_FRAMEBUFFER_COMPLETE )
	{
		glBindFramebuffer( GL_READ_FRAMEBUFFER, 0 );
		return false;
	}

	// The texture may be padded to a larger hardware size; the image occupies the
	// bottom-left Width x Height, which matches OFX's bottom-up convention.
	glPixelStorei( GL_PACK_ALIGNMENT, 1 );
	glReadPixels( 0, 0, texture.Width, texture.Height, GL_RGBA, GL_UNSIGNED_BYTE, _input->data.data() );

	glBindFramebuffer( GL_READ_FRAMEBUFFER, 0 );
	return true;
}

void OfxFFGLPlugin::ensureBlitTexture( int width, int height )
{
	if( _texWidth == width && _texHeight == height )
		return;

	glBindTexture( GL_TEXTURE_2D, _blitTex );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	glBindTexture( GL_TEXTURE_2D, 0 );
	_texWidth  = width;
	_texHeight = height;
}

bool OfxFFGLPlugin::renderViaMetal( ProcessOpenGLStruct* pGL, const FFGLTextureStruct& in, int width,
									int height )
{
	// 1. Copy the host's texture into the shared source surface. On-GPU; the
	//    Metal buffer views the same memory, so this is the whole "upload".
	glBindFramebuffer( GL_READ_FRAMEBUFFER, _readFbo );
	glFramebufferTexture2D( GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, in.Handle, 0 );
	if( glCheckFramebufferStatus( GL_READ_FRAMEBUFFER ) != GL_FRAMEBUFFER_COMPLETE )
	{
		glBindFramebuffer( GL_FRAMEBUFFER, pGL->HostFBO );
		return false;
	}

	glBindFramebuffer( GL_DRAW_FRAMEBUFFER, _blitFbo );
	glFramebufferTexture2D( GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, _metal.textureTarget(),
							_metal.sourceTexture(), 0 );
	if( glCheckFramebufferStatus( GL_DRAW_FRAMEBUFFER ) != GL_FRAMEBUFFER_COMPLETE )
	{
		glBindFramebuffer( GL_FRAMEBUFFER, pGL->HostFBO );
		return false;
	}
	glBlitFramebuffer( 0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST );

	// 2. Order the GL writes before Metal reads them.
	_metal.flushGLWrites();

	// 3. The plugin renders buffer -> buffer.
	std::string error;
	const bool ok = _effect->renderMetal( _metal.sourceBuffer(), _metal.outputBuffer(), _metal.rowBytes(),
										  _metal.commandQueue(), width, height, _time, error );
	if( !ok )
	{
		glBindFramebuffer( GL_FRAMEBUFFER, pGL->HostFBO );
		return false;
	}

	// 4. The plugin was entitled to return before its work finished.
	_metal.waitForMetal();

	// 5. Hand the output surface back to the host.
	glBindFramebuffer( GL_READ_FRAMEBUFFER, _blitFbo );
	glFramebufferTexture2D( GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, _metal.textureTarget(),
							_metal.outputTexture(), 0 );
	glBindFramebuffer( GL_DRAW_FRAMEBUFFER, pGL->HostFBO );
	glBlitFramebuffer( 0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST );

	glBindFramebuffer( GL_READ_FRAMEBUFFER, 0 );
	glBindFramebuffer( GL_FRAMEBUFFER, pGL->HostFBO );
	return true;
}

bool OfxFFGLPlugin::renderViaOpenCL( ProcessOpenGLStruct* pGL, const FFGLTextureStruct& in, int width,
									 int height )
{
	const int rowBytes = _opencl.rowBytes();

	// 1. Host texture -> source PBO. glReadPixels into a bound pixel-pack buffer
	//    is a GPU-side copy: nothing reaches the CPU.
	glBindFramebuffer( GL_READ_FRAMEBUFFER, _readFbo );
	glFramebufferTexture2D( GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, in.Handle, 0 );
	if( glCheckFramebufferStatus( GL_READ_FRAMEBUFFER ) != GL_FRAMEBUFFER_COMPLETE )
	{
		glBindFramebuffer( GL_FRAMEBUFFER, pGL->HostFBO );
		return false;
	}
	glBindBuffer( GL_PIXEL_PACK_BUFFER, _opencl.sourcePbo() );
	glPixelStorei( GL_PACK_ALIGNMENT, 1 );
	glReadPixels( 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, nullptr );
	glBindBuffer( GL_PIXEL_PACK_BUFFER, 0 );
	glBindFramebuffer( GL_READ_FRAMEBUFFER, 0 );

	// 2. Hand the shared buffers to OpenCL.
	if( !_opencl.acquireFromGL() )
	{
		glBindFramebuffer( GL_FRAMEBUFFER, pGL->HostFBO );
		return false;
	}

	std::string error;
	const bool ok = _effect->renderOpenCL( _opencl.sourceMem(), _opencl.outputMem(), rowBytes,
										   _opencl.commandQueue(), width, height, _time, error );

	// 3. Give them back, and wait for the plugin's queued work.
	_opencl.releaseToGL();

	if( !ok )
	{
		glBindFramebuffer( GL_FRAMEBUFFER, pGL->HostFBO );
		return false;
	}

	// 4. Output PBO -> our texture -> host framebuffer.
	ensureBlitTexture( width, height );
	glBindBuffer( GL_PIXEL_UNPACK_BUFFER, _opencl.outputPbo() );
	glBindTexture( GL_TEXTURE_2D, _blitTex );
	glPixelStorei( GL_UNPACK_ALIGNMENT, 1 );
	glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, nullptr );
	glBindTexture( GL_TEXTURE_2D, 0 );
	glBindBuffer( GL_PIXEL_UNPACK_BUFFER, 0 );

	glBindFramebuffer( GL_READ_FRAMEBUFFER, _blitFbo );
	glFramebufferTexture2D( GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, _blitTex, 0 );
	glBindFramebuffer( GL_DRAW_FRAMEBUFFER, pGL->HostFBO );
	glBlitFramebuffer( 0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST );
	glBindFramebuffer( GL_READ_FRAMEBUFFER, 0 );
	glBindFramebuffer( GL_FRAMEBUFFER, pGL->HostFBO );
	return true;
}

bool OfxFFGLPlugin::renderViaGL( ProcessOpenGLStruct* pGL, const FFGLTextureStruct& in, int width, int height )
{
	ensureBlitTexture( width, height );

	// The OFX OpenGL contract is that the plugin draws into whatever framebuffer
	// is currently bound; the output texture it fetches is for reference. So bind
	// our own target and hand the plugin both texture names.
	glBindFramebuffer( GL_FRAMEBUFFER, _blitFbo );
	glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, _blitTex, 0 );
	if( glCheckFramebufferStatus( GL_FRAMEBUFFER ) != GL_FRAMEBUFFER_COMPLETE )
	{
		glBindFramebuffer( GL_FRAMEBUFFER, pGL->HostFBO );
		return false;
	}
	glViewport( 0, 0, width, height );

	std::string error;
	const bool ok =
		_effect->renderGL( in.Handle, _blitTex, GL_TEXTURE_2D, width, height, _time, error );

	if( !ok )
	{
		glBindFramebuffer( GL_FRAMEBUFFER, pGL->HostFBO );
		return false;
	}

	// Hand the result to the host exactly as the CPU path does.
	glBindFramebuffer( GL_READ_FRAMEBUFFER, _blitFbo );
	glBindFramebuffer( GL_DRAW_FRAMEBUFFER, pGL->HostFBO );
	glBlitFramebuffer( 0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST );
	glBindFramebuffer( GL_READ_FRAMEBUFFER, 0 );
	glBindFramebuffer( GL_FRAMEBUFFER, pGL->HostFBO );
	return true;
}

bool OfxFFGLPlugin::uploadAndBlit( ProcessOpenGLStruct* pGL, int width, int height )
{
	ensureBlitTexture( width, height );

	glBindTexture( GL_TEXTURE_2D, _blitTex );
	glPixelStorei( GL_UNPACK_ALIGNMENT, 1 );
	glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, _output->data.data() );
	glBindTexture( GL_TEXTURE_2D, 0 );

	glBindFramebuffer( GL_READ_FRAMEBUFFER, _blitFbo );
	glFramebufferTexture2D( GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, _blitTex, 0 );
	if( glCheckFramebufferStatus( GL_READ_FRAMEBUFFER ) != GL_FRAMEBUFFER_COMPLETE )
	{
		glBindFramebuffer( GL_READ_FRAMEBUFFER, 0 );
		return false;
	}

	glBindFramebuffer( GL_DRAW_FRAMEBUFFER, pGL->HostFBO );
	glBlitFramebuffer( 0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST );

	glBindFramebuffer( GL_READ_FRAMEBUFFER, 0 );
	glBindFramebuffer( GL_FRAMEBUFFER, pGL->HostFBO );
	return true;
}

namespace {

/// Per-stage timing, enabled with OFXBRIDGE_TIMING=1.
///
/// Exists to answer one question honestly: how much of a frame is the CPU round
/// trip, and how much is the plugin's own work? Optimising the bridge is only
/// worth it if the readback and upload dominate.
struct FrameTimer
{
	using Clock = std::chrono::steady_clock;

	bool enabled = false;
	Clock::time_point mark;
	double readbackUs = 0, renderUs = 0, uploadUs = 0;

	FrameTimer()
	{
		const char* e = getenv( "OFXBRIDGE_TIMING" );
		enabled       = e != nullptr && *e != '0';
		mark          = Clock::now();
	}

	double lap()
	{
		const auto now = Clock::now();
		const double us =
			std::chrono::duration_cast< std::chrono::nanoseconds >( now - mark ).count() / 1000.0;
		mark = now;
		return us;
	}

	void report( int width, int height ) const
	{
		if( !enabled )
			return;
		fprintf( stderr, "[timing] %dx%d  readback %7.1fus  render %8.1fus  upload+blit %7.1fus  total %8.1fus\n",
				 width, height, readbackUs, renderUs, uploadUs, readbackUs + renderUs + uploadUs );
	}
};

} // namespace

FFResult OfxFFGLPlugin::ProcessOpenGL( ProcessOpenGLStruct* pGL )
{
	if( pGL == nullptr || pGL->numInputTextures < 1 || pGL->inputTextures[ 0 ] == nullptr )
		return FF_FAIL;
	if( !PluginContext::get().loaded )
		return FF_FAIL;

	const FFGLTextureStruct& in = *pGL->inputTextures[ 0 ];
	const int width  = in.Width;
	const int height = in.Height;
	if( width <= 0 || height <= 0 )
		return FF_FAIL;

	if( !ensureEffect( width, height ) )
		return FF_FAIL;

	FrameTimer timer;

	if( useMetalPath() )
	{
		applyParams();
		if( !renderViaMetal( pGL, in, width, height ) )
			return FF_FAIL;
		if( timer.enabled )
		{
			timer.renderUs = timer.lap();
			fprintf( stderr, "[timing] %dx%d  Metal render %8.1fus (no CPU round trip)\n", width, height,
					 timer.renderUs );
		}
		return FF_SUCCESS;
	}

	if( useOpenCLPath() )
	{
		applyParams();
		if( !renderViaOpenCL( pGL, in, width, height ) )
			return FF_FAIL;
		if( timer.enabled )
		{
			timer.renderUs = timer.lap();
			fprintf( stderr, "[timing] %dx%d  OpenCL render %8.1fus (no CPU round trip)\n", width, height,
					 timer.renderUs );
		}
		return FF_SUCCESS;
	}

	if( useGLPath() )
	{
		applyParams();
		if( !renderViaGL( pGL, in, width, height ) )
			return FF_FAIL;
		if( timer.enabled )
		{
			glFinish();
			timer.renderUs = timer.lap();
			fprintf( stderr, "[timing] %dx%d  GL render %8.1fus (no CPU round trip)\n",
					 width, height, timer.renderUs );
		}
		return FF_SUCCESS;
	}

	if( !readbackTexture( in ) )
		return FF_FAIL;
	if( timer.enabled )
		timer.readbackUs = timer.lap();

	applyParams();

	std::string error;
	if( !_effect->render( *_input, *_output, _time, error ) )
		return FF_FAIL;
	if( timer.enabled )
		timer.renderUs = timer.lap();

	if( !uploadAndBlit( pGL, width, height ) )
		return FF_FAIL;
	if( timer.enabled )
	{
		// The blit is queued, not finished; without this the upload cost lands on
		// whatever call next forces a flush.
		glFinish();
		timer.uploadUs = timer.lap();
	}

	timer.report( width, height );
	return FF_SUCCESS;
}

} // namespace ofxffgl
