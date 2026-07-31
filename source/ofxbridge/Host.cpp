#include "Host.h"
#include "Params.h"

#include "ofxImageEffect.h"
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

Image::Image( Frame& frame, double renderScaleX, double renderScaleY, const std::string& premult ) :
	OFX::Host::ImageEffect::Image(), _frame( frame )
{
	OfxRectI bounds = { 0, 0, frame.width, frame.height };

	setStringProperty( kOfxPropType, kOfxTypeImage );
	setStringProperty( kOfxImageEffectPropPixelDepth, frame.bitDepth );
	setStringProperty( kOfxImageEffectPropComponents, frame.components );
	setStringProperty( kOfxImageEffectPropPreMultiplication, premult );
	setStringProperty( kOfxImagePropField, kOfxImageFieldNone );
	setStringProperty( kOfxImagePropUniqueIdentifier, "ofxbridge-frame" );

	setDoubleProperty( kOfxImageEffectPropRenderScale, renderScaleX, 0 );
	setDoubleProperty( kOfxImageEffectPropRenderScale, renderScaleY, 1 );
	setDoubleProperty( kOfxImagePropPixelAspectRatio, 1.0 );

	setPointerProperty( kOfxImagePropData, frame.data.data() );
	setIntProperty( kOfxImagePropRowBytes, frame.rowBytes );

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

OfxRectI Image::getBounds() const
{
	OfxRectI r = { 0, 0, _frame.width, _frame.height };
	return r;
}

// ---------------------------------------------------------------------------
// Clip
// ---------------------------------------------------------------------------

Clip::Clip( Effect* effect, OFX::Host::ImageEffect::ClipDescriptor& desc, bool isOutput ) :
	OFX::Host::ImageEffect::ClipInstance( effect, desc ), _effect( effect ), _isOutput( isOutput )
{
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
	return 60.0;
}

void Clip::getFrameRange( double& startFrame, double& endFrame ) const
{
	startFrame = 0.0;
	endFrame   = 0.0;
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
	return _isOutput ? true : ( _frame != nullptr );
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

OFX::Host::ImageEffect::Image* Clip::getImage( OfxTime /*time*/, const OfxRectD* /*optionalBounds*/ )
{
	if( _frame == nullptr )
		return nullptr;

	// HostSupport expects a freshly retained image; the plugin releases it via
	// clipReleaseImage, which drops the refcount and deletes it.
	return new Image( *_frame, 1.0, 1.0, getPremult() );
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
	// A plugin may drive one param from another (a preset choice setting sliders,
	// say). We have nothing to sync eagerly: the FFGL layer re-reads values it
	// cares about, and Resolume owns the UI copy.
	(void)param;
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
	return 1.0;
}

double Effect::getFrameRate() const
{
	return 60.0;
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
	t1 = 0.0;
	t2 = 0.0;
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
	_properties.setIntProperty( kOfxImageEffectPropTemporalClipAccess, 0 );

	// RGBA only: that is all an FFGL texture can carry.
	_properties.setStringProperty( kOfxImageEffectPropSupportedComponents, kOfxImageComponentRGBA, 0 );

	// Filter is the only context that maps onto an FFGL effect slot. Generator
	// and General are deliberately excluded; see docs/01-architecture.md.
	_properties.setStringProperty( kOfxImageEffectPropSupportedContexts, kOfxImageEffectContextFilter, 0 );

	_properties.setStringProperty( kOfxImageEffectPropSupportedPixelDepths, kOfxBitDepthFloat, 0 );
	_properties.setStringProperty( kOfxImageEffectPropSupportedPixelDepths, kOfxBitDepthByte, 1 );

	// Advertise the OFX OpenGL render path. A plugin that also advertises it can
	// be handed our GL texture directly, skipping the CPU round trip entirely.
	// Plugins that don't (which is most Resolve-targeted ones, since Resolve uses
	// Metal/CUDA rather than this) simply fall back to the CPU path.
	_properties.setStringProperty( kOfxImageEffectPropOpenGLRenderSupported, "true" );

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
