#include "OfxFFGLPlugin.h"
#include "SelfPath.h"

#include "Host.h"
#include "Catalog.h"

#include <algorithm>
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
}

OfxFFGLPlugin::~OfxFFGLPlugin()
{
	destroyEffect();
}

// ---------------------------------------------------------------------------
// GL lifecycle
// ---------------------------------------------------------------------------

FFResult OfxFFGLPlugin::InitGL( const FFGLViewportStruct* )
{
	if( !PluginContext::get().loaded )
		return FF_FAIL;

	glGenFramebuffers( 1, &_readFbo );
	glGenFramebuffers( 1, &_blitFbo );
	glGenTextures( 1, &_blitTex );
	return CFFGLPlugin::InitGL( nullptr );
}

FFResult OfxFFGLPlugin::DeInitGL()
{
	releaseGLResources();
	destroyEffect();
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

	std::string error;
	_effect = ofxbridge::createEffect( *_host, ctx.manifest.bundlePath, ctx.manifest.identifier, error );
	if( !_effect )
	{
		_effectFailed = true;
		return false;
	}

	_effect->setFrameSize( width, height );
	if( !_effect->init( error ) )
	{
		_effect.reset();
		_effectFailed = true;
		return false;
	}

	_input  = std::make_unique< ofxbridge::Frame >();
	_output = std::make_unique< ofxbridge::Frame >();
	_input->allocate( width, height, /*asFloat*/ false );
	_output->allocate( width, height, /*asFloat*/ false );

	_effectWidth  = width;
	_effectHeight = height;
	return true;
}

void OfxFFGLPlugin::destroyEffect()
{
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
	if( index >= _values.size() )
		return FF_FAIL;
	_values[ index ] = value;
	return FF_SUCCESS;
}

float OfxFFGLPlugin::GetFloatParameter( unsigned int index )
{
	return index < _values.size() ? _values[ index ] : 0.0f;
}

FFResult OfxFFGLPlugin::SetTextParameter( unsigned int index, const char* value )
{
	if( index >= _textValues.size() )
		return FF_FAIL;
	_textValues[ index ] = value ? value : "";
	return FF_SUCCESS;
}

char* OfxFFGLPlugin::GetTextParameter( unsigned int index )
{
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
	// each one's components before pushing it across.
	std::string pending;
	std::vector< double > components;

	auto flush = [ & ]() {
		if( !pending.empty() && !components.empty() )
			_effect->setParamValue( pending, components );
		pending.clear();
		components.clear();
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
	}
	flush();
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

bool OfxFFGLPlugin::uploadAndBlit( ProcessOpenGLStruct* pGL, int width, int height )
{
	if( _texWidth != width || _texHeight != height )
	{
		glBindTexture( GL_TEXTURE_2D, _blitTex );
		glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
		glBindTexture( GL_TEXTURE_2D, 0 );
		_texWidth  = width;
		_texHeight = height;
	}

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

	if( !readbackTexture( in ) )
		return FF_FAIL;

	applyParams();

	std::string error;
	if( !_effect->render( *_input, *_output, _time, error ) )
		return FF_FAIL;

	if( !uploadAndBlit( pGL, width, height ) )
		return FF_FAIL;

	return FF_SUCCESS;
}

} // namespace ofxffgl
