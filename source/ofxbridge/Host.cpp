#include "Host.h"
#include "Params.h"

#include "ofxImageEffect.h"
#include "ofxParam.h"
#include "ofxGPURender.h"
#include "ofxProgress.h"
#include "ofxTimeLine.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

namespace ofxbridge {

namespace {

// Drops a property from a set entirely, so the plugin's get or dimension query
// answers kOfxStatErrUnknown, as from a host that never provided it.
// Property::Set has no remove and external/openfx stays unpatched, but its map
// is protected, and a pointer to a protected member formed through a derived
// class may be used on any Set.
struct PropertySetAccess : OFX::Host::Property::Set
{
	static OFX::Host::Property::PropertyMap OFX::Host::Property::Set::* map()
	{
		return &PropertySetAccess::_props;
	}
};

void removeProperty( OFX::Host::Property::Set& set, const std::string& name )
{
	OFX::Host::Property::PropertyMap& props = set.*PropertySetAccess::map();
	const auto found = props.find( name );
	if( found != props.end() )
	{
		delete found->second;// the Set owns its properties
		props.erase( found );
	}
}

} // namespace

HostOptions& hostOptions()
{
	static HostOptions options;
	return options;
}

// ---------------------------------------------------------------------------
// Frame
// ---------------------------------------------------------------------------

size_t Frame::bytesPerPixel() const
{
	return isFloat() ? 16u : 4u;// RGBA only
}

bool Frame::isFloat() const
{
	return bitDepth == kOfxBitDepthFloat;
}

void Frame::allocate( int w, int h, bool asFloat )
{
	width      = w;
	height     = h;
	bitDepth   = asFloat ? kOfxBitDepthFloat : kOfxBitDepthByte;
	components = kOfxImageComponentRGBA;
	rowBytes   = w * (int)bytesPerPixel();
	data.assign( (size_t)rowBytes * (size_t)h, 0 );
}

// ---------------------------------------------------------------------------
// Image
// ---------------------------------------------------------------------------

Image::Image( void* data, int rowBytes, int width, int height, const std::string& bitDepth,
			  const std::string& components, const std::string& premult ) :
	OFX::Host::ImageEffect::Image()
{
	OfxRectI bounds = { 0, 0, width, height };

	setStringProperty( kOfxPropType, kOfxTypeImage );
	setStringProperty( kOfxImageEffectPropPixelDepth, bitDepth );
	setStringProperty( kOfxImageEffectPropComponents, components );
	setStringProperty( kOfxImageEffectPropPreMultiplication, premult );
	setStringProperty( kOfxImagePropField, kOfxImageFieldNone );
	setStringProperty( kOfxImagePropUniqueIdentifier, "ofxbridge-image" );

	setDoubleProperty( kOfxImageEffectPropRenderScale, 1.0, 0 );
	setDoubleProperty( kOfxImageEffectPropRenderScale, 1.0, 1 );
	setDoubleProperty( kOfxImagePropPixelAspectRatio, 1.0 );

	setPointerProperty( kOfxImagePropData, data );
	setIntProperty( kOfxImagePropRowBytes, rowBytes );

	// Bounds and RoD are identical for us: we never render a partial region.
	setIntProperty( kOfxImagePropBounds, bounds.x1, 0 );
	setIntProperty( kOfxImagePropBounds, bounds.y1, 1 );
	setIntProperty( kOfxImagePropBounds, bounds.x2, 2 );
	setIntProperty( kOfxImagePropBounds, bounds.y2, 3 );
	setIntProperty( kOfxImagePropRegionOfDefinition, bounds.x1, 0 );
	setIntProperty( kOfxImagePropRegionOfDefinition, bounds.y1, 1 );
	setIntProperty( kOfxImagePropRegionOfDefinition, bounds.x2, 2 );
	setIntProperty( kOfxImagePropRegionOfDefinition, bounds.y2, 3 );
}

// ---------------------------------------------------------------------------
// Clip
// ---------------------------------------------------------------------------

Clip::Clip( Effect* effect, OFX::Host::ImageEffect::ClipDescriptor& desc, bool isOutput ) :
	OFX::Host::ImageEffect::ClipInstance( effect, desc ), _effect( effect ), _isOutput( isOutput )
{
	// --quirks fusion: a host at least as hostile as Resolve 21.1's Fusion page,
	// which gives no clip a frame rate (measured 2026-10-04). Every clip here
	// has no frame rate (get -> kOfxStatErrUnknown), a frame range of [0, 0],
	// and the unmapped rate/range present with dimension 0. Only the
	// plugin-facing property set changes; the host's own C++ view of the clip
	// (getFrameRate() etc.) is untouched.
	if( hostOptions().fusionQuirks )
	{
		using OFX::Host::Property::PropSpec;
		removeProperty( _properties, kOfxImageEffectPropFrameRate );

		removeProperty( _properties, kOfxImageEffectPropFrameRange );
		const PropSpec range = { kOfxImageEffectPropFrameRange, OFX::Host::Property::eDouble, 2, true, "0" };
		_properties.createProperty( range );

		for( const char* name : { kOfxImageEffectPropUnmappedFrameRate, kOfxImageEffectPropUnmappedFrameRange } )
		{
			removeProperty( _properties, name );
			const PropSpec empty = { name, OFX::Host::Property::eDouble, 0, true, "" };
			_properties.createProperty( empty );
		}
	}
}

const std::string& Clip::getUnmappedBitDepth() const
{
	// We only ever hand the plugin 8-bit RGBA or float RGBA; the choice is made
	// once per instance and reported consistently here and in clip preferences.
	static const std::string sByte  = kOfxBitDepthByte;
	static const std::string sFloat = kOfxBitDepthFloat;
	return _pixelDepth == kOfxBitDepthFloat ? sFloat : sByte;
}

const std::string& Clip::getUnmappedComponents() const
{
	static const std::string s = kOfxImageComponentRGBA;
	return s;
}

const std::string& Clip::getPremult() const
{
	// Resolume hands FFGL plugins premultiplied RGBA.
	static const std::string s = kOfxImagePreMultiplied;
	return s;
}

double Clip::getAspectRatio() const
{
	return 1.0;
}

double Clip::getFrameRate() const
{
	// One rate for the whole effect: 60 in the bridge, --frame-rate in ofxprobe.
	return _effect->getFrameRate();
}

void Clip::getFrameRange( double& startFrame, double& endFrame ) const
{
	// An input fed from a source reports that source's range (a sequence's
	// first..last frame). Everything else reports the effect's timeline, which
	// is [0, 0] unless the test host sets one -- what the bridge always said.
	if( _source != nullptr && !_isOutput )
	{
		_source->frameRange( startFrame, endFrame );
		return;
	}
	_effect->timeline( startFrame, endFrame );
}

const std::string& Clip::getFieldOrder() const
{
	static const std::string s = kOfxImageFieldNone;
	return s;
}

bool Clip::getConnected() const
{
	// The output clip is always connected; an input clip is connected only when
	// the FFGL layer has actually bound a frame to it this pass.
	return _isOutput ? true : ( _frame != nullptr || _source != nullptr );
}

double Clip::getUnmappedFrameRate() const
{
	return getFrameRate();
}

void Clip::getUnmappedFrameRange( double& start, double& end ) const
{
	getFrameRange( start, end );
}

bool Clip::getContinuousSamples() const
{
	return false;
}

OfxRectD Clip::getRegionOfDefinition( OfxTime /*time*/ ) const
{
	OfxRectD r = { 0.0, 0.0, (double)_effect->frameWidth(), (double)_effect->frameHeight() };
	return r;
}

OFX::Host::ImageEffect::Texture* Clip::loadTexture( OfxTime /*time*/, const char* /*format*/,
													const OfxRectD* /*optionalBounds*/ )
{
	// Only meaningful while the FFGL layer has bound a texture for this pass.
	// ofxprobe links this host with no GL context at all, so a null here is the
	// normal headless answer rather than an error.
	if( _textureName == 0 )
		return nullptr;

	auto* tex = new OFX::Host::ImageEffect::Texture( *this );

	tex->setStringProperty( kOfxPropType, kOfxTypeImage );
	tex->setStringProperty( kOfxImageEffectPropPixelDepth, kOfxBitDepthByte );
	tex->setStringProperty( kOfxImageEffectPropComponents, kOfxImageComponentRGBA );
	tex->setStringProperty( kOfxImageEffectPropPreMultiplication, getPremult() );
	tex->setStringProperty( kOfxImagePropField, kOfxImageFieldNone );
	tex->setStringProperty( kOfxImagePropUniqueIdentifier, "ofxbridge-texture" );

	// The whole point of this path: the plugin gets the GL texture name, and no
	// pixel ever leaves the GPU.
	tex->setIntProperty( kOfxImageEffectPropOpenGLTextureIndex, (int)_textureName );
	tex->setIntProperty( kOfxImageEffectPropOpenGLTextureTarget, (int)_textureTarget );

	tex->setDoubleProperty( kOfxImageEffectPropRenderScale, 1.0, 0 );
	tex->setDoubleProperty( kOfxImageEffectPropRenderScale, 1.0, 1 );
	tex->setDoubleProperty( kOfxImagePropPixelAspectRatio, 1.0 );

	// No CPU mapping exists for a texture.
	tex->setPointerProperty( kOfxImagePropData, nullptr );
	tex->setIntProperty( kOfxImagePropRowBytes, 0 );

	const OfxRectI bounds = { 0, 0, _textureWidth, _textureHeight };
	for( int i = 0; i < 4; ++i )
	{
		const int v = ( &bounds.x1 )[ i ];
		tex->setIntProperty( kOfxImagePropBounds, v, i );
		tex->setIntProperty( kOfxImagePropRegionOfDefinition, v, i );
	}
	return tex;
}

OFX::Host::ImageEffect::Image* Clip::getImage( OfxTime time, const OfxRectD* /*optionalBounds*/ )
{
	// HostSupport expects a freshly retained image; the plugin releases it via
	// clipReleaseImage, which drops the refcount and deletes it.

	// On the Metal path kOfxImagePropData carries an id<MTLBuffer> rather than a
	// CPU pointer, which is exactly what the OFX Metal contract specifies once
	// the host has set kOfxImageEffectPropMetalEnabled.
	if( _metalBuffer != nullptr )
		return new Image( _metalBuffer, _metalRowBytes, _metalWidth, _metalHeight, kOfxBitDepthByte,
						  kOfxImageComponentRGBA, getPremult() );

	// A time-addressed source answers for any time the plugin asks
	// about; "no frame there" is a null image, which the OFX Support library
	// turns into a null fetchImage (kOfxStatFailed) -- what a host does past a
	// clip's ends.
	if( _source != nullptr && !_isOutput )
	{
		Frame* f = _source->frameAt( time );
		if( f == nullptr )
			return nullptr;
		auto* img = new Image( f->data.data(), f->rowBytes, f->width, f->height, f->bitDepth, f->components,
							   getPremult() );
		img->setStringProperty( kOfxImagePropUniqueIdentifier, getName() + "@" + _source->frameId( time ) );
		return img;
	}

	if( _frame == nullptr )
		return nullptr;

	return new Image( _frame->data.data(), _frame->rowBytes, _frame->width, _frame->height,
					  _frame->bitDepth, _frame->components, getPremult() );
}

// ---------------------------------------------------------------------------
// Effect
// ---------------------------------------------------------------------------

Effect::Effect( OFX::Host::ImageEffect::ImageEffectPlugin* plugin,
				OFX::Host::ImageEffect::Descriptor& desc,
				const std::string& context,
				Host* host ) :
	OFX::Host::ImageEffect::Instance( plugin, desc, context, false ), _host( host )
{
	// --quirks fusion: the effect instance has no frame rate either. Stricter
	// than Fusion, which does give the effect one, so a plugin that survives
	// here survives there.
	if( hostOptions().fusionQuirks )
		removeProperty( _properties, kOfxImageEffectPropFrameRate );
}

bool Effect::init( std::string& error )
{
	// Note: populate() is NOT called here. ImageEffectPlugin::createInstance
	// already does it, and calling it twice fails on duplicate parameter names.
	OfxStatus st = createInstanceAction();
	if( st != kOfxStatOK && st != kOfxStatReplyDefault )
	{
		error = "kOfxActionCreateInstance failed";
		return false;
	}

	// Note: unlike the other actions, getClipPreferences returns a bool, not an
	// OfxStatus. Comparing it against kOfxStatOK reads success (true == 1) as an
	// error code.
	if( !getClipPreferences() )
	{
		error = "kOfxImageEffectActionGetClipPreferences failed";
		return false;
	}
	return true;
}

bool Effect::render( Frame& in, Frame& out, double time, std::string& error )
{
	_time = time;
	setFrameSize( out.width, out.height );

	Clip* source = dynamic_cast< Clip* >( getClip( kOfxImageEffectSimpleSourceClipName ) );
	Clip* output = dynamic_cast< Clip* >( getClip( kOfxImageEffectOutputClipName ) );
	if( output == nullptr )
	{
		error = "effect has no output clip";
		return false;
	}

	if( source )
		source->setFrame( &in );
	output->setFrame( &out );

	OfxRectI window = { 0, 0, out.width, out.height };
	OfxPointD scale = { 1.0, 1.0 };

	OfxStatus st = beginRenderAction( time, time, 1.0, /*interactive*/ false, scale,
									  /*sequentialRender*/ false, /*interactiveRender*/ false );
	if( st != kOfxStatOK && st != kOfxStatReplyDefault )
	{
		error = "begin render failed";
		return false;
	}

	st = renderAction( time, kOfxImageFieldNone, window, scale,
					   /*sequentialRender*/ false, /*interactiveRender*/ false, /*draftRender*/ false );

	endRenderAction( time, time, 1.0, false, scale, false, false );

	if( source )
		source->setFrame( nullptr );
	output->setFrame( nullptr );

	if( st != kOfxStatOK && st != kOfxStatReplyDefault )
	{
		error = "kOfxImageEffectActionRender failed";
		return false;
	}
	return true;
}

bool Effect::supportsMetalRender() const
{
	const OFX::Host::Property::Set& props = getDescriptor().getProps();
	return props.getStringProperty( kOfxImageEffectPropMetalRenderSupported ) == "true";
}

bool Effect::renderMetal( void* sourceBuffer, void* outputBuffer, int rowBytes, void* commandQueue,
						  int width, int height, double time, std::string& error )
{
	_time = time;
	setFrameSize( width, height );

	Clip* source = dynamic_cast< Clip* >( getClip( kOfxImageEffectSimpleSourceClipName ) );
	Clip* output = dynamic_cast< Clip* >( getClip( kOfxImageEffectOutputClipName ) );
	if( output == nullptr )
	{
		error = "effect has no output clip";
		return false;
	}

	if( source )
		source->setMetalBuffer( sourceBuffer, rowBytes, width, height );
	output->setMetalBuffer( outputBuffer, rowBytes, width, height );

	OfxRectI window = { 0, 0, width, height };
	OfxPointD scale = { 1.0, 1.0 };

	OfxStatus st = beginRenderAction( time, time, 1.0, false, scale, false, false );
	if( st != kOfxStatOK && st != kOfxStatReplyDefault )
	{
		error = "begin render failed";
		if( source )
			source->clearMetalBuffer();
		output->clearMetalBuffer();
		return false;
	}

	// As with the OpenGL path, HostSupport's renderAction cannot carry the extra
	// properties, so the action is issued here. Metal needs two: the enable flag,
	// and the command queue the plugin must encode onto.
	static const OFX::Host::Property::PropSpec inStuff[] = {
		{ kOfxPropTime, OFX::Host::Property::eDouble, 1, true, "0" },
		{ kOfxImageEffectPropFieldToRender, OFX::Host::Property::eString, 1, true, "" },
		{ kOfxImageEffectPropRenderWindow, OFX::Host::Property::eInt, 4, true, "0" },
		{ kOfxImageEffectPropRenderScale, OFX::Host::Property::eDouble, 2, true, "0" },
		{ kOfxImageEffectPropSequentialRenderStatus, OFX::Host::Property::eInt, 1, true, "0" },
		{ kOfxImageEffectPropInteractiveRenderStatus, OFX::Host::Property::eInt, 1, true, "0" },
		{ kOfxImageEffectPropRenderQualityDraft, OFX::Host::Property::eInt, 1, true, "0" },
		{ kOfxImageEffectPropMetalEnabled, OFX::Host::Property::eInt, 1, true, "0" },
		{ kOfxImageEffectPropMetalCommandQueue, OFX::Host::Property::ePointer, 1, true, "" },
		OFX::Host::Property::propSpecEnd
	};

	OFX::Host::Property::Set inArgs( inStuff );
	inArgs.setStringProperty( kOfxImageEffectPropFieldToRender, kOfxImageFieldNone );
	inArgs.setDoubleProperty( kOfxPropTime, time );
	inArgs.setIntPropertyN( kOfxImageEffectPropRenderWindow, &window.x1, 4 );
	inArgs.setDoublePropertyN( kOfxImageEffectPropRenderScale, &scale.x, 2 );
	inArgs.setIntProperty( kOfxImageEffectPropSequentialRenderStatus, 0 );
	inArgs.setIntProperty( kOfxImageEffectPropInteractiveRenderStatus, 0 );
	inArgs.setIntProperty( kOfxImageEffectPropRenderQualityDraft, 0 );
	inArgs.setIntProperty( kOfxImageEffectPropMetalEnabled, 1 );
	inArgs.setPointerProperty( kOfxImageEffectPropMetalCommandQueue, commandQueue );

	st = mainEntry( kOfxImageEffectActionRender, this->getHandle(), &inArgs, 0 );

	endRenderAction( time, time, 1.0, false, scale, false, false );

	if( source )
		source->clearMetalBuffer();
	output->clearMetalBuffer();

	if( st != kOfxStatOK && st != kOfxStatReplyDefault )
	{
		error = "kOfxImageEffectActionRender (Metal) failed";
		return false;
	}
	return true;
}

bool Effect::supportsCudaRender() const
{
	const OFX::Host::Property::Set& props = getDescriptor().getProps();
	return props.getStringProperty( kOfxImageEffectPropCudaRenderSupported ) == "true";
}

bool Effect::renderCuda( void* sourceBuffer, void* outputBuffer, int rowBytes, void* stream, int width,
						 int height, double time, std::string& error )
{
	// ---------------------------------------------------------------------
	// UNVERIFIED. Never compiled against the CUDA toolkit, never executed.
	// Written from the OFX specification; there is no NVIDIA hardware on which
	// to run it. See docs/04-gpu-acceleration.md.
	// ---------------------------------------------------------------------
	_time = time;
	setFrameSize( width, height );

	Clip* source = dynamic_cast< Clip* >( getClip( kOfxImageEffectSimpleSourceClipName ) );
	Clip* output = dynamic_cast< Clip* >( getClip( kOfxImageEffectOutputClipName ) );
	if( output == nullptr )
	{
		error = "effect has no output clip";
		return false;
	}

	if( source )
		source->setMetalBuffer( sourceBuffer, rowBytes, width, height );
	output->setMetalBuffer( outputBuffer, rowBytes, width, height );

	OfxRectI window = { 0, 0, width, height };
	OfxPointD scale = { 1.0, 1.0 };

	OfxStatus st = beginRenderAction( time, time, 1.0, false, scale, false, false );
	if( st != kOfxStatOK && st != kOfxStatReplyDefault )
	{
		error = "begin render failed";
		if( source )
			source->clearMetalBuffer();
		output->clearMetalBuffer();
		return false;
	}

	static const OFX::Host::Property::PropSpec inStuff[] = {
		{ kOfxPropTime, OFX::Host::Property::eDouble, 1, true, "0" },
		{ kOfxImageEffectPropFieldToRender, OFX::Host::Property::eString, 1, true, "" },
		{ kOfxImageEffectPropRenderWindow, OFX::Host::Property::eInt, 4, true, "0" },
		{ kOfxImageEffectPropRenderScale, OFX::Host::Property::eDouble, 2, true, "0" },
		{ kOfxImageEffectPropSequentialRenderStatus, OFX::Host::Property::eInt, 1, true, "0" },
		{ kOfxImageEffectPropInteractiveRenderStatus, OFX::Host::Property::eInt, 1, true, "0" },
		{ kOfxImageEffectPropRenderQualityDraft, OFX::Host::Property::eInt, 1, true, "0" },
		{ kOfxImageEffectPropCudaEnabled, OFX::Host::Property::eInt, 1, true, "0" },
		{ kOfxImageEffectPropCudaStream, OFX::Host::Property::ePointer, 1, true, "" },
		OFX::Host::Property::propSpecEnd
	};

	OFX::Host::Property::Set inArgs( inStuff );
	inArgs.setStringProperty( kOfxImageEffectPropFieldToRender, kOfxImageFieldNone );
	inArgs.setDoubleProperty( kOfxPropTime, time );
	inArgs.setIntPropertyN( kOfxImageEffectPropRenderWindow, &window.x1, 4 );
	inArgs.setDoublePropertyN( kOfxImageEffectPropRenderScale, &scale.x, 2 );
	inArgs.setIntProperty( kOfxImageEffectPropSequentialRenderStatus, 0 );
	inArgs.setIntProperty( kOfxImageEffectPropInteractiveRenderStatus, 0 );
	inArgs.setIntProperty( kOfxImageEffectPropRenderQualityDraft, 0 );
	inArgs.setIntProperty( kOfxImageEffectPropCudaEnabled, 1 );
	inArgs.setPointerProperty( kOfxImageEffectPropCudaStream, stream );

	st = mainEntry( kOfxImageEffectActionRender, this->getHandle(), &inArgs, 0 );

	endRenderAction( time, time, 1.0, false, scale, false, false );

	if( source )
		source->clearMetalBuffer();
	output->clearMetalBuffer();

	if( st != kOfxStatOK && st != kOfxStatReplyDefault )
	{
		error = "kOfxImageEffectActionRender (CUDA) failed";
		return false;
	}
	return true;
}

bool Effect::supportsOpenCLRender() const
{
	const OFX::Host::Property::Set& props = getDescriptor().getProps();
	return props.getStringProperty( kOfxImageEffectPropOpenCLRenderSupported ) == "true";
}

bool Effect::renderOpenCL( void* sourceMem, void* outputMem, int rowBytes, void* commandQueue, int width,
						   int height, double time, std::string& error )
{
	_time = time;
	setFrameSize( width, height );

	Clip* source = dynamic_cast< Clip* >( getClip( kOfxImageEffectSimpleSourceClipName ) );
	Clip* output = dynamic_cast< Clip* >( getClip( kOfxImageEffectOutputClipName ) );
	if( output == nullptr )
	{
		error = "effect has no output clip";
		return false;
	}

	// Same mechanism as Metal: kOfxImagePropData carries whichever handle the
	// host said it enabled, so the existing buffer plumbing serves both.
	if( source )
		source->setMetalBuffer( sourceMem, rowBytes, width, height );
	output->setMetalBuffer( outputMem, rowBytes, width, height );

	OfxRectI window = { 0, 0, width, height };
	OfxPointD scale = { 1.0, 1.0 };

	OfxStatus st = beginRenderAction( time, time, 1.0, false, scale, false, false );
	if( st != kOfxStatOK && st != kOfxStatReplyDefault )
	{
		error = "begin render failed";
		if( source )
			source->clearMetalBuffer();
		output->clearMetalBuffer();
		return false;
	}

	static const OFX::Host::Property::PropSpec inStuff[] = {
		{ kOfxPropTime, OFX::Host::Property::eDouble, 1, true, "0" },
		{ kOfxImageEffectPropFieldToRender, OFX::Host::Property::eString, 1, true, "" },
		{ kOfxImageEffectPropRenderWindow, OFX::Host::Property::eInt, 4, true, "0" },
		{ kOfxImageEffectPropRenderScale, OFX::Host::Property::eDouble, 2, true, "0" },
		{ kOfxImageEffectPropSequentialRenderStatus, OFX::Host::Property::eInt, 1, true, "0" },
		{ kOfxImageEffectPropInteractiveRenderStatus, OFX::Host::Property::eInt, 1, true, "0" },
		{ kOfxImageEffectPropRenderQualityDraft, OFX::Host::Property::eInt, 1, true, "0" },
		{ kOfxImageEffectPropOpenCLEnabled, OFX::Host::Property::eInt, 1, true, "0" },
		{ kOfxImageEffectPropOpenCLCommandQueue, OFX::Host::Property::ePointer, 1, true, "" },
		OFX::Host::Property::propSpecEnd
	};

	OFX::Host::Property::Set inArgs( inStuff );
	inArgs.setStringProperty( kOfxImageEffectPropFieldToRender, kOfxImageFieldNone );
	inArgs.setDoubleProperty( kOfxPropTime, time );
	inArgs.setIntPropertyN( kOfxImageEffectPropRenderWindow, &window.x1, 4 );
	inArgs.setDoublePropertyN( kOfxImageEffectPropRenderScale, &scale.x, 2 );
	inArgs.setIntProperty( kOfxImageEffectPropSequentialRenderStatus, 0 );
	inArgs.setIntProperty( kOfxImageEffectPropInteractiveRenderStatus, 0 );
	inArgs.setIntProperty( kOfxImageEffectPropRenderQualityDraft, 0 );
	inArgs.setIntProperty( kOfxImageEffectPropOpenCLEnabled, 1 );
	inArgs.setPointerProperty( kOfxImageEffectPropOpenCLCommandQueue, commandQueue );

	st = mainEntry( kOfxImageEffectActionRender, this->getHandle(), &inArgs, 0 );

	endRenderAction( time, time, 1.0, false, scale, false, false );

	if( source )
		source->clearMetalBuffer();
	output->clearMetalBuffer();

	if( st != kOfxStatOK && st != kOfxStatReplyDefault )
	{
		error = "kOfxImageEffectActionRender (OpenCL) failed";
		return false;
	}
	return true;
}

bool Effect::supportsOpenGLRender() const
{
	// The descriptor answers for the plugin; our own host property says only what
	// we are willing to do.
	const OFX::Host::Property::Set& props = getDescriptor().getProps();
	return props.getStringProperty( kOfxImageEffectPropOpenGLRenderSupported ) == "true";
}

bool Effect::attachGLContext( std::string& error )
{
	const OfxStatus st = contextAttachedAction();
	if( st != kOfxStatOK && st != kOfxStatReplyDefault )
	{
		error = "kOfxActionOpenGLContextAttached failed";
		return false;
	}
	return true;
}

void Effect::detachGLContext()
{
	contextDetachedAction();
}

bool Effect::renderGL( unsigned int inputTexture, unsigned int outputTexture, unsigned int target,
					   int width, int height, double time, std::string& error )
{
	_time = time;
	setFrameSize( width, height );

	Clip* source = dynamic_cast< Clip* >( getClip( kOfxImageEffectSimpleSourceClipName ) );
	Clip* output = dynamic_cast< Clip* >( getClip( kOfxImageEffectOutputClipName ) );
	if( output == nullptr )
	{
		error = "effect has no output clip";
		return false;
	}

	if( source )
		source->setTexture( inputTexture, target, width, height );
	output->setTexture( outputTexture, target, width, height );

	OfxRectI window = { 0, 0, width, height };
	OfxPointD scale = { 1.0, 1.0 };

	OfxStatus st = beginRenderAction( time, time, 1.0, false, scale, false, false );
	if( st != kOfxStatOK && st != kOfxStatReplyDefault )
	{
		error = "begin render failed";
		if( source )
			source->clearTexture();
		output->clearTexture();
		return false;
	}

	// HostSupport's renderAction has no way to set kOfxImageEffectPropOpenGLEnabled,
	// so the action is issued here instead. This mirrors
	// Instance::renderAction (ofxhImageEffect.cpp:918) with that one property added
	// -- without it a GL-capable plugin takes its CPU branch, or refuses outright
	// as the OpenFX example does.
	static const OFX::Host::Property::PropSpec inStuff[] = {
		{ kOfxPropTime, OFX::Host::Property::eDouble, 1, true, "0" },
		{ kOfxImageEffectPropFieldToRender, OFX::Host::Property::eString, 1, true, "" },
		{ kOfxImageEffectPropRenderWindow, OFX::Host::Property::eInt, 4, true, "0" },
		{ kOfxImageEffectPropRenderScale, OFX::Host::Property::eDouble, 2, true, "0" },
		{ kOfxImageEffectPropSequentialRenderStatus, OFX::Host::Property::eInt, 1, true, "0" },
		{ kOfxImageEffectPropInteractiveRenderStatus, OFX::Host::Property::eInt, 1, true, "0" },
		{ kOfxImageEffectPropRenderQualityDraft, OFX::Host::Property::eInt, 1, true, "0" },
		{ kOfxImageEffectPropOpenGLEnabled, OFX::Host::Property::eInt, 1, true, "0" },
		OFX::Host::Property::propSpecEnd
	};

	OFX::Host::Property::Set inArgs( inStuff );
	inArgs.setStringProperty( kOfxImageEffectPropFieldToRender, kOfxImageFieldNone );
	inArgs.setDoubleProperty( kOfxPropTime, time );
	inArgs.setIntPropertyN( kOfxImageEffectPropRenderWindow, &window.x1, 4 );
	inArgs.setDoublePropertyN( kOfxImageEffectPropRenderScale, &scale.x, 2 );
	inArgs.setIntProperty( kOfxImageEffectPropSequentialRenderStatus, 0 );
	inArgs.setIntProperty( kOfxImageEffectPropInteractiveRenderStatus, 0 );
	inArgs.setIntProperty( kOfxImageEffectPropRenderQualityDraft, 0 );
	inArgs.setIntProperty( kOfxImageEffectPropOpenGLEnabled, 1 );

	st = mainEntry( kOfxImageEffectActionRender, this->getHandle(), &inArgs, 0 );

	endRenderAction( time, time, 1.0, false, scale, false, false );

	if( source )
		source->clearTexture();
	output->clearTexture();

	if( st != kOfxStatOK && st != kOfxStatReplyDefault )
	{
		error = "kOfxImageEffectActionRender (OpenGL) failed";
		return false;
	}
	return true;
}

OFX::Host::ImageEffect::ClipInstance* Effect::newClipInstance( OFX::Host::ImageEffect::Instance* /*effect*/,
															   OFX::Host::ImageEffect::ClipDescriptor* descriptor,
															   int /*index*/ )
{
	const bool isOutput = descriptor->getName() == kOfxImageEffectOutputClipName;
	return new Clip( this, *descriptor, isOutput );
}

OFX::Host::Param::Instance* Effect::newParam( const std::string& name, OFX::Host::Param::Descriptor& descriptor )
{
	OFX::Host::Param::Instance* p = makeParamInstance( name, descriptor, this );
	if( getenv( "OFXBRIDGE_DEBUG" ) )
		fprintf( stderr, "[newParam] %-20s %-24s -> %s\n", name.c_str(), descriptor.getType().c_str(),
				 p ? "ok" : "NULL" );
	if( p == nullptr )
		_messages.push_back( "unsupported param type '" + descriptor.getType() + "' for param '" + name + "'" );
	return p;
}

OfxStatus Effect::editBegin( const std::string& )
{
	// We have no undo stack to open; the host UI owns undo.
	return kOfxStatReplyDefault;
}

OfxStatus Effect::editEnd()
{
	return kOfxStatReplyDefault;
}

void Effect::paramChangedByPlugin( OFX::Host::Param::Instance* param )
{
	// A plugin may drive one param from another (a preset choice setting the
	// sliders, say). The value is already stored by the time HostSupport calls
	// this; what remains is telling whoever fronts this effect that their copy
	// of it is now stale.
	if( onParamChangedByPlugin && param != nullptr )
		onParamChangedByPlugin( param->getName() );
}

bool Effect::setParamValue( const std::string& name, const std::vector< double >& values )
{
	OFX::Host::Param::Instance* p = getParam( name );
	if( p == nullptr )
		return false;
	ValueAccess* v = dynamic_cast< ValueAccess* >( p );
	if( v == nullptr || v->componentCount() == 0 )
		return false;
	v->setValues( values );
	return true;
}

bool Effect::getParamValue( const std::string& name, std::vector< double >& values )
{
	OFX::Host::Param::Instance* p = getParam( name );
	if( p == nullptr )
		return false;
	ValueAccess* v = dynamic_cast< ValueAccess* >( p );
	if( v == nullptr || v->componentCount() == 0 )
		return false;
	v->getValues( values );
	return true;
}

bool Effect::editParamValue( const std::string& name, const std::vector< double >& values )
{
	if( !setParamValue( name, values ) )
		return false;

	OfxPointD renderScale = { 1.0, 1.0 };
	beginInstanceChangedAction( kOfxChangeUserEdited );
	// At the current time: 0 unless the test host moved it (--time).
	paramInstanceChangedAction( name, kOfxChangeUserEdited, _time, renderScale );
	endInstanceChangedAction( kOfxChangeUserEdited );
	return true;
}

bool Effect::setParamString( const std::string& name, const std::string& value )
{
	OFX::Host::Param::Instance* p = getParam( name );
	if( p == nullptr )
		return false;
	ValueAccess* v = dynamic_cast< ValueAccess* >( p );
	if( v == nullptr || !v->isString() )
		return false;
	v->setString( value );
	return true;
}

const std::string& Effect::getDefaultOutputFielding() const
{
	static const std::string s = kOfxImageFieldNone;
	return s;
}

OfxStatus Effect::vmessage( const char* type, const char* /*id*/, const char* format, va_list args )
{
	char buf[ 1024 ];
	vsnprintf( buf, sizeof( buf ), format, args );
	_messages.push_back( std::string( type ? type : "message" ) + ": " + buf );
	return kOfxStatOK;
}

OfxStatus Effect::setPersistentMessage( const char* type, const char* id, const char* format, va_list args )
{
	return vmessage( type, id, format, args );
}

OfxStatus Effect::clearPersistentMessage()
{
	_messages.clear();
	return kOfxStatOK;
}

void Effect::getProjectSize( double& xSize, double& ySize ) const
{
	xSize = (double)_width;
	ySize = (double)_height;
}

void Effect::getProjectOffset( double& xOffset, double& yOffset ) const
{
	xOffset = 0.0;
	yOffset = 0.0;
}

void Effect::getProjectExtent( double& xSize, double& ySize ) const
{
	getProjectSize( xSize, ySize );
}

double Effect::getProjectPixelAspectRatio() const
{
	return 1.0;
}

double Effect::getEffectDuration() const
{
	// [0, 0] -> 1, as the bridge always reported.
	return _rangeLast - _rangeFirst + 1.0;
}

double Effect::getFrameRate() const
{
	return _frameRate;
}

double Effect::getFrameRecursive() const
{
	return _time;
}

void Effect::getRenderScaleRecursive( double& x, double& y ) const
{
	x = 1.0;
	y = 1.0;
}

OfxStatus Effect::getViewCount( int* nViews ) const
{
	*nViews = 1;
	return kOfxStatOK;
}

void Effect::progressStart( const std::string&, const std::string& )
{
}

void Effect::progressEnd()
{
}

bool Effect::progressUpdate( double )
{
	return true;// never cancel
}

double Effect::timeLineGetTime()
{
	return _time;
}

void Effect::timeLineGotoTime( double )
{
}

void Effect::timeLineGetBounds( double& t1, double& t2 )
{
	t1 = _rangeFirst;
	t2 = _rangeLast;
}

// ---------------------------------------------------------------------------
// test-host additions
// ---------------------------------------------------------------------------

bool Effect::getClipPreferences()
{
	if( !OFX::Host::ImageEffect::Instance::getClipPreferences() )
		return false;
	if( _forcedDepth.empty() )
		return true;
	for( auto& kv : _clips )
	{
		OFX::Host::ImageEffect::ClipInstance* clip = kv.second;
		if( clip != nullptr && isChromaticComponent( clip->getComponents() ) )
			clip->setPixelDepth( _forcedDepth );
	}
	return true;
}

bool Effect::bindSource( const std::string& clipName, FrameSource* source )
{
	Clip* clip = dynamic_cast< Clip* >( getClip( clipName ) );
	if( clip == nullptr )
		return false;
	clip->setSource( source );
	return true;
}

bool Effect::renderBound( Frame& out, double time, std::string& error )
{
	_time = time;
	setFrameSize( out.width, out.height );

	Clip* output = dynamic_cast< Clip* >( getClip( kOfxImageEffectOutputClipName ) );
	if( output == nullptr )
	{
		error = "effect has no output clip";
		return false;
	}
	runGetClipPrefsConditionally();
	output->setFrame( &out );

	OfxRectI window = { 0, 0, out.width, out.height };
	OfxPointD scale = { 1.0, 1.0 };

	const bool fusion = hostOptions().fusionQuirks;
	OfxStatus st      = fusion ? fusionSequenceAction( kOfxImageEffectActionBeginSequenceRender, time )
							   : beginRenderAction( time, time, 1.0, /*interactive*/ false, scale,
													/*sequentialRender*/ false, /*interactiveRender*/ false );
	if( st != kOfxStatOK && st != kOfxStatReplyDefault )
	{
		output->setFrame( nullptr );
		error = "begin render failed";
		return false;
	}

	st = fusion ? fusionRenderAction( time, window )
				: renderAction( time, kOfxImageFieldNone, window, scale,
								/*sequentialRender*/ false, /*interactiveRender*/ false, /*draftRender*/ false );

	if( fusion )
		fusionSequenceAction( kOfxImageEffectActionEndSequenceRender, time );
	else
		endRenderAction( time, time, 1.0, false, scale, false, false );
	output->setFrame( nullptr );

	if( st != kOfxStatOK && st != kOfxStatReplyDefault )
	{
		static const char* names[] = { "kOfxStatOK", "kOfxStatFailed", "kOfxStatErrFatal", "kOfxStatErrUnknown",
									   "kOfxStatErrMissingHostFeature", "kOfxStatErrUnsupported", "kOfxStatErrExists",
									   "kOfxStatErrFormat", "kOfxStatErrMemory", "kOfxStatErrBadHandle",
									   "kOfxStatErrBadIndex", "kOfxStatErrValue", "kOfxStatReplyYes", "kOfxStatReplyNo",
									   "kOfxStatReplyDefault", "kOfxStatUnlicensed" };
		char buf[ 128 ];
		snprintf( buf, sizeof( buf ), "kOfxImageEffectActionRender failed (status %d%s%s)", (int)st,
				  st >= 0 && st < (int)( sizeof( names ) / sizeof( names[ 0 ] ) ) ? " " : "",
				  st >= 0 && st < (int)( sizeof( names ) / sizeof( names[ 0 ] ) ) ? names[ st ] : "" );
		error = buf;
		return false;
	}
	return true;
}

// HostSupport's begin/render/end always carry the two render-status ints;
// Fusion's do not. These are the same actions with exactly those two left out.
OfxStatus Effect::fusionSequenceAction( const char* action, double time )
{
	static const OFX::Host::Property::PropSpec inStuff[] = {
		{ kOfxImageEffectPropFrameRange, OFX::Host::Property::eDouble, 2, true, "0" },
		{ kOfxImageEffectPropFrameStep, OFX::Host::Property::eDouble, 1, true, "0" },
		{ kOfxPropIsInteractive, OFX::Host::Property::eInt, 1, true, "0" },
		{ kOfxImageEffectPropRenderScale, OFX::Host::Property::eDouble, 2, true, "0" },
		OFX::Host::Property::propSpecEnd
	};
	OFX::Host::Property::Set inArgs( inStuff );
	inArgs.setDoubleProperty( kOfxImageEffectPropFrameRange, time, 0 );
	inArgs.setDoubleProperty( kOfxImageEffectPropFrameRange, time, 1 );
	inArgs.setDoubleProperty( kOfxImageEffectPropFrameStep, 1.0 );
	inArgs.setIntProperty( kOfxPropIsInteractive, 0 );
	const OfxPointD scale = { 1.0, 1.0 };
	inArgs.setDoublePropertyN( kOfxImageEffectPropRenderScale, &scale.x, 2 );
	return mainEntry( action, this->getHandle(), &inArgs, 0 );
}

OfxStatus Effect::fusionRenderAction( double time, const OfxRectI& window )
{
	static const OFX::Host::Property::PropSpec inStuff[] = {
		{ kOfxPropTime, OFX::Host::Property::eDouble, 1, true, "0" },
		{ kOfxImageEffectPropFieldToRender, OFX::Host::Property::eString, 1, true, "" },
		{ kOfxImageEffectPropRenderWindow, OFX::Host::Property::eInt, 4, true, "0" },
		{ kOfxImageEffectPropRenderScale, OFX::Host::Property::eDouble, 2, true, "0" },
		{ kOfxImageEffectPropRenderQualityDraft, OFX::Host::Property::eInt, 1, true, "0" },
		OFX::Host::Property::propSpecEnd
	};
	OFX::Host::Property::Set inArgs( inStuff );
	inArgs.setStringProperty( kOfxImageEffectPropFieldToRender, kOfxImageFieldNone );
	inArgs.setDoubleProperty( kOfxPropTime, time );
	inArgs.setIntPropertyN( kOfxImageEffectPropRenderWindow, &window.x1, 4 );
	const OfxPointD scale = { 1.0, 1.0 };
	inArgs.setDoublePropertyN( kOfxImageEffectPropRenderScale, &scale.x, 2 );
	inArgs.setIntProperty( kOfxImageEffectPropRenderQualityDraft, 0 );
	return mainEntry( kOfxImageEffectActionRender, this->getHandle(), &inArgs, 0 );
}

bool Effect::pressParam( const std::string& name, OfxStatus& status, std::string& error )
{
	OFX::Host::Param::Instance* p = getParam( name );
	if( p == nullptr )
	{
		error = "no parameter named '" + name + "'";
		return false;
	}
	OfxPointD renderScale = { 1.0, 1.0 };
	beginInstanceChangedAction( kOfxChangeUserEdited );
	status = paramInstanceChangedAction( name, kOfxChangeUserEdited, _time, renderScale );
	endInstanceChangedAction( kOfxChangeUserEdited );
	return true;
}

bool Effect::setParamKeys( const std::string& name, const std::vector< ParamKey >& keys, std::string& error )
{
	OFX::Host::Param::Instance* p = getParam( name );
	if( p == nullptr )
	{
		error = "no parameter named '" + name + "'";
		return false;
	}
	ValueAccess* v = dynamic_cast< ValueAccess* >( p );
	if( v == nullptr || v->componentCount() == 0 || !v->setKeys( keys ) )
	{
		error = "parameter '" + name + "' (" + p->getType() + ") cannot be keyed";
		return false;
	}
	p->getProperties().setIntProperty( kOfxParamPropIsAnimating, keys.empty() ? 0 : 1 );
	return true;
}

bool Effect::getParamValueAt( const std::string& name, double time, std::vector< double >& values )
{
	OFX::Host::Param::Instance* p = getParam( name );
	if( p == nullptr )
		return false;
	ValueAccess* v = dynamic_cast< ValueAccess* >( p );
	if( v == nullptr || v->componentCount() == 0 )
		return false;
	v->getValuesAt( time, values );
	return true;
}

bool Effect::queryIdentity( double time, int width, int height, std::string& clip, double& identityTime )
{
	OfxTime t        = time;
	OfxRectI window  = { 0, 0, width, height };
	OfxPointD scale  = { 1.0, 1.0 };
	std::string name;
	const OfxStatus st = isIdentityAction( t, kOfxImageFieldNone, window, scale, name );
	if( st != kOfxStatOK || name.empty() )
		return false;
	clip         = name;
	identityTime = t;
	return true;
}

std::map< std::string, std::vector< OfxRangeD > > Effect::queryFramesNeeded( double time, bool* answered )
{
	std::map< std::string, std::vector< OfxRangeD > > out;
	OFX::Host::ImageEffect::RangeMap ranges;
	const OfxStatus st = getFrameNeededAction( time, ranges );
	if( answered != nullptr )
		*answered = st == kOfxStatOK;
	for( auto& kv : ranges )
		if( kv.first != nullptr )
			out[ kv.first->getName() ] = kv.second;
	return out;
}

// ---------------------------------------------------------------------------
// Host
// ---------------------------------------------------------------------------

Host::Host()
{
	// Identify ourselves honestly. Some commercial plugins gate their licence on
	// the host name; we do not impersonate another host to get around that.
	_properties.setStringProperty( kOfxPropName, "com.stoatworks.ofxbridge" );
	_properties.setStringProperty( kOfxPropLabel, "Resolume OFX Bridge" );
	_properties.setStringProperty( kOfxPropVersionLabel, "0.1.0" );

	_properties.setIntProperty( kOfxImageEffectHostPropIsBackground, 0 );
	_properties.setIntProperty( kOfxImageEffectPropSupportsOverlays, 0 );
	_properties.setIntProperty( kOfxImageEffectPropSupportsMultiResolution, 0 );
	_properties.setIntProperty( kOfxImageEffectPropSupportsTiles, 0 );
	// 0 in the bridge (Resolume hands over one frame); the test host may say 1.
	_properties.setIntProperty( kOfxImageEffectPropTemporalClipAccess, hostOptions().temporalAccess );

	// RGBA only: that is all an FFGL texture can carry.
	_properties.setStringProperty( kOfxImageEffectPropSupportedComponents, kOfxImageComponentRGBA, 0 );

	// Filter is the only context that maps onto an FFGL effect slot. Generator
	// and General are deliberately excluded; see docs/01-architecture.md.
	//
	// The test host may add Transition/Generator/General; the default
	// list is Filter alone, exactly as before.
	{
		const auto& contexts = hostOptions().contexts;
		for( size_t i = 0; i < contexts.size(); ++i )
			_properties.setStringProperty( kOfxImageEffectPropSupportedContexts, contexts[ i ], (int)i );
	}

	_properties.setStringProperty( kOfxImageEffectPropSupportedPixelDepths, kOfxBitDepthFloat, 0 );
	_properties.setStringProperty( kOfxImageEffectPropSupportedPixelDepths, kOfxBitDepthByte, 1 );

	// Advertise the OFX OpenGL render path. A plugin that also advertises it can
	// be handed our GL texture directly, skipping the CPU round trip entirely.
	// Plugins that don't (which is most Resolve-targeted ones, since Resolve uses
	// Metal/CUDA rather than this) simply fall back to the CPU path.
	_properties.setStringProperty( kOfxImageEffectPropOpenGLRenderSupported, "true" );

	// Metal render. Many Resolve-targeted plugins are GPU-only, so without this
	// they do not render slowly -- they refuse to render at all.
	//
	// Advertised only where the GL<->Metal interop is actually compiled. A build
	// without it that claimed Metal anyway would be handed an id<MTLBuffer> it
	// has no way to produce, which fails mid-render instead of declining up
	// front -- the same reasoning as CUDA below.
#ifdef OFXBRIDGE_HAS_METAL
	_properties.setStringProperty( kOfxImageEffectPropMetalRenderSupported, "true" );
#endif

	// OpenCL buffer render. Matters on Windows and Linux, where Resolve uses it
	// on AMD hardware; on macOS it is deprecated but still functional, which is
	// what makes the path testable here at all. Same gate, and the same reason:
	// on the platforms where it matters most the interop is not written yet, so
	// claiming it there would be the loudest possible lie.
#ifdef OFXBRIDGE_HAS_OPENCL
	_properties.setStringProperty( kOfxImageEffectPropOpenCLRenderSupported, "true" );
#endif

	// CUDA is deliberately NOT advertised. The render action exists but its
	// GL interop has never been compiled or run, and claiming support we cannot
	// honour would make a CUDA plugin fail confusingly mid-render rather than
	// be declined cleanly up front. Build with OFXBRIDGE_ENABLE_CUDA to opt in
	// on hardware where it can actually be tested.
#ifdef OFXBRIDGE_ENABLE_CUDA
	_properties.setStringProperty( kOfxImageEffectPropCudaRenderSupported, "true" );
#endif

	_properties.setIntProperty( kOfxImageEffectPropSupportsMultipleClipDepths, 0 );
	_properties.setIntProperty( kOfxImageEffectPropSupportsMultipleClipPARs, 0 );
	_properties.setIntProperty( kOfxImageEffectPropSetableFrameRate, 0 );
	_properties.setIntProperty( kOfxImageEffectPropSetableFielding, 0 );

	_properties.setIntProperty( kOfxParamHostPropSupportsCustomInteract, 0 );
	_properties.setIntProperty( kOfxParamHostPropSupportsStringAnimation, 0 );
	_properties.setIntProperty( kOfxParamHostPropSupportsChoiceAnimation, 0 );
	_properties.setIntProperty( kOfxParamHostPropSupportsBooleanAnimation, 0 );
	_properties.setIntProperty( kOfxParamHostPropSupportsCustomAnimation, 0 );
	_properties.setIntProperty( kOfxParamHostPropMaxParameters, -1 );
	_properties.setIntProperty( kOfxParamHostPropMaxPages, 0 );
	_properties.setIntProperty( kOfxParamHostPropPageRowColumnCount, 0, 0 );
	_properties.setIntProperty( kOfxParamHostPropPageRowColumnCount, 0, 1 );
}

OFX::Host::ImageEffect::Instance* Host::newInstance( void* /*clientData*/,
													 OFX::Host::ImageEffect::ImageEffectPlugin* plugin,
													 OFX::Host::ImageEffect::Descriptor& desc,
													 const std::string& context )
{
	return new Effect( plugin, desc, context, this );
}

OFX::Host::ImageEffect::Descriptor* Host::makeDescriptor( OFX::Host::ImageEffect::ImageEffectPlugin* plugin )
{
	return new OFX::Host::ImageEffect::Descriptor( plugin );
}

OFX::Host::ImageEffect::Descriptor* Host::makeDescriptor( const OFX::Host::ImageEffect::Descriptor& rootContext,
														  OFX::Host::ImageEffect::ImageEffectPlugin* plug )
{
	return new OFX::Host::ImageEffect::Descriptor( rootContext, plug );
}

OFX::Host::ImageEffect::Descriptor* Host::makeDescriptor( const std::string& bundlePath,
														  OFX::Host::ImageEffect::ImageEffectPlugin* plug )
{
	return new OFX::Host::ImageEffect::Descriptor( bundlePath, plug );
}

OfxStatus Host::vmessage( const char* type, const char* /*id*/, const char* format, va_list args )
{
	char buf[ 1024 ];
	vsnprintf( buf, sizeof( buf ), format, args );
	fprintf( stderr, "[ofx %s] %s\n", type ? type : "message", buf );
	return kOfxStatOK;
}

OfxStatus Host::setPersistentMessage( const char* type, const char* id, const char* format, va_list args )
{
	return vmessage( type, id, format, args );
}

OfxStatus Host::clearPersistentMessage()
{
	return kOfxStatOK;
}

// -- multi-thread suite ------------------------------------------------------
//
// Plugins call multiThread() from inside render, which for us happens on
// Resolume's GL thread. We spawn real threads rather than running serially
// because several Resolve-targeted plugins are written assuming genuine
// parallelism, but we bound the pool: oversubscribing a live video thread costs
// more than it saves.

namespace {
thread_local int tlThreadIndex = -1;

unsigned int suggestedThreadCount()
{
	unsigned int hw = std::thread::hardware_concurrency();
	if( hw == 0 )
		hw = 4;
	return std::min( hw, 8u );
}
} // namespace

OfxStatus Host::multiThread( OfxThreadFunctionV1 func, unsigned int nThreads, void* customArg )
{
	if( func == nullptr )
		return kOfxStatFailed;

	if( nThreads <= 1 )
	{
		const int saved = tlThreadIndex;
		tlThreadIndex   = 0;
		func( 0, 1, customArg );
		tlThreadIndex = saved;
		return kOfxStatOK;
	}

	std::vector< std::thread > pool;
	pool.reserve( nThreads - 1 );
	for( unsigned int i = 1; i < nThreads; ++i )
	{
		pool.emplace_back( [ = ]() {
			tlThreadIndex = (int)i;
			func( i, nThreads, customArg );
			tlThreadIndex = -1;
		} );
	}

	// Run slice 0 on the calling thread so we don't idle it.
	const int saved = tlThreadIndex;
	tlThreadIndex   = 0;
	func( 0, nThreads, customArg );
	tlThreadIndex = saved;

	for( auto& t : pool )
		t.join();
	return kOfxStatOK;
}

OfxStatus Host::multiThreadNumCPUS( unsigned int* nCPUs ) const
{
	*nCPUs = suggestedThreadCount();
	return kOfxStatOK;
}

OfxStatus Host::multiThreadIndex( unsigned int* threadIndex ) const
{
	if( tlThreadIndex < 0 )
		return kOfxStatFailed;
	*threadIndex = (unsigned int)tlThreadIndex;
	return kOfxStatOK;
}

int Host::multiThreadIsSpawnedThread() const
{
	return tlThreadIndex > 0 ? 1 : 0;
}

OfxStatus Host::mutexCreate( OfxMutexHandle* mutex, int /*lockCount*/ )
{
	*mutex = (OfxMutexHandle) new std::recursive_mutex();
	return kOfxStatOK;
}

OfxStatus Host::mutexDestroy( const OfxMutexHandle mutex )
{
	delete (std::recursive_mutex*)mutex;
	return kOfxStatOK;
}

OfxStatus Host::mutexLock( const OfxMutexHandle mutex )
{
	( (std::recursive_mutex*)mutex )->lock();
	return kOfxStatOK;
}

OfxStatus Host::mutexUnLock( const OfxMutexHandle mutex )
{
	( (std::recursive_mutex*)mutex )->unlock();
	return kOfxStatOK;
}

OfxStatus Host::mutexTryLock( const OfxMutexHandle mutex )
{
	return ( (std::recursive_mutex*)mutex )->try_lock() ? kOfxStatOK : kOfxStatFailed;
}

OfxStatus Host::flushOpenGLResources() const
{
	// We allocate no GL resources on the plugin's behalf; the FFGL layer owns
	// everything and frees it in DeInitGL.
	return kOfxStatOK;
}

} // namespace ofxbridge
