#pragma once
//
// A minimal OpenFX image-effect host, built on the OpenFX HostSupport library.
//
// This is the shared core used by both `ofxprobe` (which only runs the describe
// actions to introspect a plugin) and the generic FFGL wrapper (which also
// instantiates and renders). Keeping one host implementation means the params a
// generated wrapper exposes are by construction the params the renderer sees.
//
// Everything here is CPU-side. The GL<->CPU bridging lives in the FFGL layer;
// this file deliberately knows nothing about OpenGL so that `ofxprobe` can run
// headless.
//

#include "ofxhBinary.h"
#include "ofxhPluginCache.h"
#include "ofxhPluginAPICache.h"
#include "ofxhImageEffect.h"
#include "ofxhImageEffectAPI.h"
#include "ofxhClip.h"
#include "ofxhMemory.h"
#include "ofxhInteract.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ofxbridge {

/// A CPU pixel buffer handed to (or taken from) an OFX plugin.
///
/// OFX images are bottom-up by convention and addressed by a rect in canonical
/// coordinates; we always use a full-frame rect anchored at the origin, which
/// keeps the RoD/RoI negotiation trivial for the filter context.
struct Frame
{
	int width    = 0;
	int height   = 0;
	int rowBytes = 0;
	/// kOfxBitDepthByte or kOfxBitDepthFloat.
	std::string bitDepth = "OfxBitDepthByte";
	/// kOfxImageComponentRGBA (the only thing Resolume can give us).
	std::string components = "OfxImageComponentRGBA";
	std::vector< uint8_t > data;

	void allocate( int w, int h, bool isFloat );
	bool isFloat() const;
	size_t bytesPerPixel() const;
};

class Host;
class Effect;

/// The property set an OFX plugin expects from clipGetImage.
///
/// `data` is a CPU pointer on the normal path and an `id<MTLBuffer>` on the
/// Metal path — the OFX contract is that kOfxImagePropData means whichever the
/// host said it enabled, so one class serves both.
class Image : public OFX::Host::ImageEffect::Image
{
public:
	Image( void* data, int rowBytes, int width, int height, const std::string& bitDepth,
		   const std::string& components, const std::string& premult );
	~Image() override = default;
};

/// One clip (input "Source" or "Output") on an effect instance.
class Clip : public OFX::Host::ImageEffect::ClipInstance
{
public:
	Clip( Effect* effect, OFX::Host::ImageEffect::ClipDescriptor& desc, bool isOutput );

	// -- ClipInstance interface -------------------------------------------------
	const std::string& getUnmappedBitDepth() const override;
	const std::string& getUnmappedComponents() const override;
	const std::string& getPremult() const override;
	double getAspectRatio() const override;
	double getFrameRate() const override;
	void getFrameRange( double& startFrame, double& endFrame ) const override;
	const std::string& getFieldOrder() const override;
	bool getConnected() const override;
	double getUnmappedFrameRate() const override;
	void getUnmappedFrameRange( double& start, double& end ) const override;
	bool getContinuousSamples() const override;

	OFX::Host::ImageEffect::Image* getImage( OfxTime time, const OfxRectD* optionalBounds ) override;
	OFX::Host::ImageEffect::Texture* loadTexture( OfxTime time, const char* format,
												  const OfxRectD* optionalBounds ) override;
	OfxRectD getRegionOfDefinition( OfxTime time ) const override;

	/// Point this clip at a caller-owned frame for the duration of one render.
	void setFrame( Frame* frame )
	{
		_frame = frame;
	}

	/// Point this clip at an `id<MTLBuffer>` for the duration of one render.
	/// Taken as void* so this header stays free of Objective-C.
	void setMetalBuffer( void* buffer, int rowBytes, int width, int height )
	{
		_metalBuffer   = buffer;
		_metalRowBytes = rowBytes;
		_metalWidth    = width;
		_metalHeight   = height;
	}
	void clearMetalBuffer()
	{
		_metalBuffer = nullptr;
	}

	/// Point this clip at an OpenGL texture for the duration of one render.
	///
	/// Taken as a plain unsigned so this header stays free of GL headers —
	/// ofxprobe links the host and runs with no GL context at all.
	void setTexture( unsigned int name, unsigned int target, int width, int height )
	{
		_textureName   = name;
		_textureTarget = target;
		_textureWidth  = width;
		_textureHeight = height;
	}
	void clearTexture()
	{
		_textureName = 0;
	}

private:
	Effect* _effect;
	Frame* _frame = nullptr;
	bool _isOutput;

	void* _metalBuffer = nullptr;
	int _metalRowBytes = 0;
	int _metalWidth    = 0;
	int _metalHeight   = 0;

	unsigned int _textureName   = 0;
	unsigned int _textureTarget = 0;
	int _textureWidth           = 0;
	int _textureHeight          = 0;
};

/// A live instance of an OFX effect, in the "Filter" context.
class Effect : public OFX::Host::ImageEffect::Instance
{
public:
	Effect( OFX::Host::ImageEffect::ImageEffectPlugin* plugin,
			OFX::Host::ImageEffect::Descriptor& desc,
			const std::string& context,
			Host* host );

	/// Runs kOfxActionCreateInstance and the initial clip-preferences pass.
	/// Returns false (with `error` populated) rather than throwing, because the
	/// FFGL layer has no way to surface an exception to Resolume.
	bool init( std::string& error );

	/// Render `in` into `out`. Both frames must already be allocated and the
	/// same size. Returns false on a plugin-reported failure.
	bool render( Frame& in, Frame& out, double time, std::string& error );

	/// True if this plugin advertises OFX OpenGL render, so it can read and write
	/// GL textures directly and no pixel need cross to the CPU.
	bool supportsOpenGLRender() const;

	/// Render via OFX OpenGL render.
	///
	/// The caller must have a current GL context, and must have bound a
	/// framebuffer whose colour attachment is `outputTexture` — the OFX GL
	/// contract is that the plugin draws into whatever is currently bound, and
	/// merely *reads* the output texture id for reference.
	bool renderGL( unsigned int inputTexture, unsigned int outputTexture, unsigned int target,
				   int width, int height, double time, std::string& error );

	/// Tell the plugin a GL context has become available (or gone away). Must be
	/// called with the context current.
	bool attachGLContext( std::string& error );
	void detachGLContext();

	/// True if this plugin advertises OFX Metal render.
	bool supportsMetalRender() const;

	/// Render via OFX Metal render. The buffers are `id<MTLBuffer>` and
	/// `commandQueue` an `id<MTLCommandQueue>`, passed as void* to keep this
	/// header free of Objective-C.
	///
	/// The plugin is not required to wait for completion — the host owns the
	/// queue and decides when to synchronise.
	bool renderMetal( void* sourceBuffer, void* outputBuffer, int rowBytes, void* commandQueue,
					  int width, int height, double time, std::string& error );

	/// True if this plugin advertises OFX OpenCL buffer render.
	bool supportsOpenCLRender() const;

	/// Render via OFX OpenCL render. Buffers are `cl_mem`, the queue a
	/// `cl_command_queue`, both as void* to keep this header OpenCL-free.
	bool renderOpenCL( void* sourceMem, void* outputMem, int rowBytes, void* commandQueue, int width,
					   int height, double time, std::string& error );

	/// True if this plugin advertises OFX CUDA render.
	bool supportsCudaRender() const;

	/// Render via OFX CUDA render. The buffers are CUDA device pointers and
	/// `stream` an optional cudaStream_t, both void* here.
	///
	/// **UNVERIFIED.** This has never been compiled against the CUDA toolkit nor
	/// executed: CUDA needs an NVIDIA GPU, which macOS has not supported since
	/// 10.13 and Apple Silicon has never had. It is written from the OFX
	/// specification alone. Treat it as a starting point, not working code.
	bool renderCuda( void* sourceBuffer, void* outputBuffer, int rowBytes, void* stream, int width,
					 int height, double time, std::string& error );

	// -- Instance interface -----------------------------------------------------
	OFX::Host::ImageEffect::ClipInstance* newClipInstance( OFX::Host::ImageEffect::Instance* effect,
														   OFX::Host::ImageEffect::ClipDescriptor* descriptor,
														   int index ) override;

	// Param::SetInstance interface.
	OFX::Host::Param::Instance* newParam( const std::string& name,
										  OFX::Host::Param::Descriptor& descriptor ) override;
	OfxStatus editBegin( const std::string& name ) override;
	OfxStatus editEnd() override;
	void paramChangedByPlugin( OFX::Host::Param::Instance* param ) override;

	/// Set a numeric parameter by OFX name. Silently ignores unknown names so a
	/// stale manifest can't take the effect down mid-show.
	bool setParamValue( const std::string& name, const std::vector< double >& values );
	bool setParamString( const std::string& name, const std::string& value );

	const std::string& getDefaultOutputFielding() const override;

	OfxStatus vmessage( const char* type, const char* id, const char* format, va_list args ) override;
	OfxStatus setPersistentMessage( const char* type, const char* id, const char* format, va_list args ) override;
	OfxStatus clearPersistentMessage() override;

	void getProjectSize( double& xSize, double& ySize ) const override;
	void getProjectOffset( double& xOffset, double& yOffset ) const override;
	void getProjectExtent( double& xSize, double& ySize ) const override;
	double getProjectPixelAspectRatio() const override;
	double getEffectDuration() const override;
	double getFrameRate() const override;
	double getFrameRecursive() const override;
	void getRenderScaleRecursive( double& x, double& y ) const override;

	OfxStatus getViewCount( int* nViews ) const;

	// Progress + timeline suites: no-ops, but the plugin may call them.
	void progressStart( const std::string& message, const std::string& messageid ) override;
	void progressEnd() override;
	bool progressUpdate( double progress ) override;
	double timeLineGetTime() override;
	void timeLineGotoTime( double t ) override;
	void timeLineGetBounds( double& t1, double& t2 ) override;

	/// The current frame size, used to answer RoD queries.
	void setFrameSize( int w, int h )
	{
		_width  = w;
		_height = h;
	}
	int frameWidth() const
	{
		return _width;
	}
	int frameHeight() const
	{
		return _height;
	}

	/// Messages the plugin emitted, for surfacing in the generator UI / log.
	const std::vector< std::string >& messages() const
	{
		return _messages;
	}

private:
	Host* _host;
	int _width  = 1920;
	int _height = 1080;
	double _time = 0.0;
	std::vector< std::string > _messages;
};

/// The host object itself. Owns the plugin cache.
class Host : public OFX::Host::ImageEffect::Host
{
public:
	Host();

	OFX::Host::ImageEffect::Instance* newInstance( void* clientData,
												   OFX::Host::ImageEffect::ImageEffectPlugin* plugin,
												   OFX::Host::ImageEffect::Descriptor& desc,
												   const std::string& context ) override;

	OFX::Host::ImageEffect::Descriptor* makeDescriptor( OFX::Host::ImageEffect::ImageEffectPlugin* plugin ) override;
	OFX::Host::ImageEffect::Descriptor* makeDescriptor( const OFX::Host::ImageEffect::Descriptor& rootContext,
														OFX::Host::ImageEffect::ImageEffectPlugin* plug ) override;
	OFX::Host::ImageEffect::Descriptor* makeDescriptor( const std::string& bundlePath,
														OFX::Host::ImageEffect::ImageEffectPlugin* plug ) override;

	OfxStatus vmessage( const char* type, const char* id, const char* format, va_list args ) override;
	OfxStatus setPersistentMessage( const char* type, const char* id, const char* format, va_list args ) override;
	OfxStatus clearPersistentMessage() override;

	// Multi-thread suite. We render on Resolume's GL thread and farm out to a
	// small pool; see Host.cpp for why the pool is bounded.
	OfxStatus multiThread( OfxThreadFunctionV1 func, unsigned int nThreads, void* customArg ) override;
	OfxStatus multiThreadNumCPUS( unsigned int* nCPUs ) const override;
	OfxStatus multiThreadIndex( unsigned int* threadIndex ) const override;
	int multiThreadIsSpawnedThread() const override;
	OfxStatus mutexCreate( OfxMutexHandle* mutex, int lockCount ) override;
	OfxStatus mutexDestroy( const OfxMutexHandle mutex ) override;
	OfxStatus mutexLock( const OfxMutexHandle mutex ) override;
	OfxStatus mutexUnLock( const OfxMutexHandle mutex ) override;
	OfxStatus mutexTryLock( const OfxMutexHandle mutex ) override;

	OfxStatus flushOpenGLResources() const override;
};

} // namespace ofxbridge
