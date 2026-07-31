//
// A minimal OFX plugin that renders with Metal.
//
// It exists because no such plugin is publicly available to test against: every
// OpenFX example is CPU or OpenGL, and the commercial plugins that do implement
// Metal render are exactly the ones that refuse to load in an unrecognised host.
// Without this, the bridge's Metal path could not be developed or verified at
// all.
//
// Deliberately GPU-only: it declares no CPU render path and fails if the host
// does not enable Metal. That makes it a faithful stand-in for the
// Resolve-targeted plugins this path is meant to unlock, and means a host that
// silently falls back to CPU is caught rather than flattered.
//
// Written against the bare OFX C API — no Support library — to keep the
// dependency surface small and the contract explicit.
//

#import <Metal/Metal.h>

#include "ofxCore.h"
#include "ofxImageEffect.h"
#include "ofxGPURender.h"
#include "ofxParam.h"
#include "ofxProperty.h"

#include <cstdio>
#include <cstring>

#define EXPORT __attribute__( ( visibility( "default" ) ) )

namespace {

OfxHost* gHost                        = nullptr;
OfxImageEffectSuiteV1* gEffectSuite   = nullptr;
OfxPropertySuiteV1* gPropSuite        = nullptr;
OfxParameterSuiteV1* gParamSuite      = nullptr;

id< MTLDevice > gDevice                       = nil;
id< MTLComputePipelineState > gPipeline        = nil;

/// Must match the struct in the kernel below.
struct KernelParams
{
	unsigned int width;
	unsigned int height;
	unsigned int rowBytes;
	float gain;
};

const char* kKernelSource = R"(
#include <metal_stdlib>
using namespace metal;

struct KernelParams {
    uint  width;
    uint  height;
    uint  rowBytes;
    float gain;
};

kernel void gainKernel(device const uchar   *src [[buffer(0)]],
                       device       uchar   *dst [[buffer(1)]],
                       constant KernelParams &p  [[buffer(2)]],
                       uint2 gid [[thread_position_in_grid]])
{
    if (gid.x >= p.width || gid.y >= p.height)
        return;

    // rowBytes is honoured rather than assuming width*4: the host is free to
    // pad rows, and silently ignoring that produces a sheared image.
    uint off = gid.y * p.rowBytes + gid.x * 4;

    for (uint c = 0; c < 4; ++c) {
        float v = float(src[off + c]) * p.gain;
        dst[off + c] = uchar(clamp(v, 0.0f, 255.0f));
    }
}
)";

bool buildPipeline()
{
	if( gPipeline != nil )
		return true;

	gDevice = MTLCreateSystemDefaultDevice();
	if( gDevice == nil )
		return false;

	NSError* error = nil;
	// Compiled from source at load rather than shipping a .metallib, so the
	// plugin stays a single self-contained binary.
	id< MTLLibrary > library =
		[ gDevice newLibraryWithSource:[ NSString stringWithUTF8String:kKernelSource ]
							   options:nil
								 error:&error ];
	if( library == nil )
	{
		fprintf( stderr, "[metalgain] shader compile failed: %s\n",
				 error ? [ [ error localizedDescription ] UTF8String ] : "unknown" );
		return false;
	}

	id< MTLFunction > fn = [ library newFunctionWithName:@"gainKernel" ];
	if( fn == nil )
		return false;

	gPipeline = [ gDevice newComputePipelineStateWithFunction:fn error:&error ];
	if( gPipeline == nil )
	{
		fprintf( stderr, "[metalgain] pipeline failed: %s\n",
				 error ? [ [ error localizedDescription ] UTF8String ] : "unknown" );
		return false;
	}
	return true;
}

// ---------------------------------------------------------------------------
// OFX actions
// ---------------------------------------------------------------------------

OfxStatus onLoad()
{
	gEffectSuite = (OfxImageEffectSuiteV1*)gHost->fetchSuite( gHost->host, kOfxImageEffectSuite, 1 );
	gPropSuite   = (OfxPropertySuiteV1*)gHost->fetchSuite( gHost->host, kOfxPropertySuite, 1 );
	gParamSuite  = (OfxParameterSuiteV1*)gHost->fetchSuite( gHost->host, kOfxParameterSuite, 1 );

	if( gEffectSuite == nullptr || gPropSuite == nullptr || gParamSuite == nullptr )
		return kOfxStatErrMissingHostFeature;

	return buildPipeline() ? kOfxStatOK : kOfxStatFailed;
}

OfxStatus describe( OfxImageEffectHandle effect )
{
	OfxPropertySetHandle props;
	gEffectSuite->getPropertySet( effect, &props );

	gPropSuite->propSetString( props, kOfxPropLabel, 0, "Metal Gain Example" );
	gPropSuite->propSetString( props, kOfxImageEffectPluginPropGrouping, 0, "Stoatworks Test" );
	gPropSuite->propSetString( props, kOfxImageEffectPropSupportedContexts, 0, kOfxImageEffectContextFilter );
	gPropSuite->propSetString( props, kOfxImageEffectPropSupportedPixelDepths, 0, kOfxBitDepthByte );

	// The whole point of this plugin.
	gPropSuite->propSetString( props, kOfxImageEffectPropMetalRenderSupported, 0, "true" );

	gPropSuite->propSetInt( props, kOfxImageEffectPluginPropSingleInstance, 0, 0 );
	gPropSuite->propSetInt( props, kOfxImageEffectPropSupportsTiles, 0, 0 );
	gPropSuite->propSetInt( props, kOfxImageEffectPropSupportsMultiResolution, 0, 0 );
	return kOfxStatOK;
}

OfxStatus describeInContext( OfxImageEffectHandle effect )
{
	OfxPropertySetHandle clip;

	gEffectSuite->clipDefine( effect, kOfxImageEffectSimpleSourceClipName, &clip );
	gPropSuite->propSetString( clip, kOfxImageEffectPropSupportedComponents, 0, kOfxImageComponentRGBA );

	gEffectSuite->clipDefine( effect, kOfxImageEffectOutputClipName, &clip );
	gPropSuite->propSetString( clip, kOfxImageEffectPropSupportedComponents, 0, kOfxImageComponentRGBA );

	OfxParamSetHandle params;
	gEffectSuite->getParamSet( effect, &params );

	OfxPropertySetHandle param;
	gParamSuite->paramDefine( params, kOfxParamTypeDouble, "gain", &param );
	gPropSuite->propSetString( param, kOfxPropLabel, 0, "Gain" );
	gPropSuite->propSetString( param, kOfxParamPropHint, 0, "Multiplies every channel, including alpha" );
	gPropSuite->propSetDouble( param, kOfxParamPropDefault, 0, 1.0 );
	gPropSuite->propSetDouble( param, kOfxParamPropMin, 0, 0.0 );
	gPropSuite->propSetDouble( param, kOfxParamPropDisplayMin, 0, 0.0 );
	gPropSuite->propSetDouble( param, kOfxParamPropDisplayMax, 0, 4.0 );

	return kOfxStatOK;
}

OfxStatus render( OfxImageEffectHandle effect, OfxPropertySetHandle inArgs )
{
	OfxTime time;
	gPropSuite->propGetDouble( inArgs, kOfxPropTime, 0, &time );

	OfxRectI window;
	gPropSuite->propGetIntN( inArgs, kOfxImageEffectPropRenderWindow, 4, &window.x1 );

	// Refuse anything but Metal. A host that quietly hands over CPU pointers
	// should fail loudly here rather than produce a plausible-looking frame.
	int metalEnabled = 0;
	gPropSuite->propGetInt( inArgs, kOfxImageEffectPropMetalEnabled, 0, &metalEnabled );
	if( !metalEnabled )
	{
		fprintf( stderr, "[metalgain] host did not enable Metal render; refusing\n" );
		return kOfxStatErrImageFormat;
	}

	void* queuePtr = nullptr;
	gPropSuite->propGetPointer( inArgs, kOfxImageEffectPropMetalCommandQueue, 0, &queuePtr );
	if( queuePtr == nullptr )
	{
		fprintf( stderr, "[metalgain] host enabled Metal but supplied no command queue\n" );
		return kOfxStatFailed;
	}
	id< MTLCommandQueue > queue = (__bridge id< MTLCommandQueue >)queuePtr;

	OfxImageClipHandle sourceClip, outputClip;
	gEffectSuite->clipGetHandle( effect, kOfxImageEffectSimpleSourceClipName, &sourceClip, nullptr );
	gEffectSuite->clipGetHandle( effect, kOfxImageEffectOutputClipName, &outputClip, nullptr );

	OfxPropertySetHandle sourceImg = nullptr, outputImg = nullptr;
	if( gEffectSuite->clipGetImage( sourceClip, time, nullptr, &sourceImg ) != kOfxStatOK ||
		gEffectSuite->clipGetImage( outputClip, time, nullptr, &outputImg ) != kOfxStatOK )
	{
		if( sourceImg )
			gEffectSuite->clipReleaseImage( sourceImg );
		if( outputImg )
			gEffectSuite->clipReleaseImage( outputImg );
		return kOfxStatFailed;
	}

	void* srcData = nullptr;
	void* dstData = nullptr;
	int srcRowBytes = 0, dstRowBytes = 0;
	gPropSuite->propGetPointer( sourceImg, kOfxImagePropData, 0, &srcData );
	gPropSuite->propGetPointer( outputImg, kOfxImagePropData, 0, &dstData );
	gPropSuite->propGetInt( sourceImg, kOfxImagePropRowBytes, 0, &srcRowBytes );
	gPropSuite->propGetInt( outputImg, kOfxImagePropRowBytes, 0, &dstRowBytes );

	// With Metal enabled these are id<MTLBuffer>, not CPU pointers.
	id< MTLBuffer > srcBuffer = (__bridge id< MTLBuffer >)srcData;
	id< MTLBuffer > dstBuffer = (__bridge id< MTLBuffer >)dstData;

	OfxStatus status = kOfxStatOK;

	if( srcBuffer == nil || dstBuffer == nil || gPipeline == nil )
	{
		status = kOfxStatFailed;
	}
	else
	{
		double gain = 1.0;
		OfxParamSetHandle params;
		gEffectSuite->getParamSet( effect, &params );
		OfxParamHandle gainParam;
		gParamSuite->paramGetHandle( params, "gain", &gainParam, nullptr );
		gParamSuite->paramGetValueAtTime( gainParam, time, &gain );

		KernelParams kp;
		kp.width    = (unsigned int)( window.x2 - window.x1 );
		kp.height   = (unsigned int)( window.y2 - window.y1 );
		kp.rowBytes = (unsigned int)srcRowBytes;
		kp.gain     = (float)gain;

		id< MTLCommandBuffer > cmd      = [ queue commandBuffer ];
		id< MTLComputeCommandEncoder > enc = [ cmd computeCommandEncoder ];

		[ enc setComputePipelineState:gPipeline ];
		[ enc setBuffer:srcBuffer offset:0 atIndex:0 ];
		[ enc setBuffer:dstBuffer offset:0 atIndex:1 ];
		[ enc setBytes:&kp length:sizeof( kp ) atIndex:2 ];

		const MTLSize threads = MTLSizeMake( kp.width, kp.height, 1 );
		NSUInteger tgw        = gPipeline.threadExecutionWidth;
		NSUInteger tgh        = gPipeline.maxTotalThreadsPerThreadgroup / tgw;
		[ enc dispatchThreads:threads threadsPerThreadgroup:MTLSizeMake( tgw, tgh, 1 ) ];
		[ enc endEncoding ];

		// The OFX Metal contract says the plugin SHOULD NOT block on completion;
		// the host owns the queue and decides when to synchronise.
		[ cmd commit ];
	}

	gEffectSuite->clipReleaseImage( sourceImg );
	gEffectSuite->clipReleaseImage( outputImg );
	return status;
}

OfxStatus pluginMain( const char* action, const void* handle, OfxPropertySetHandle inArgs,
					  OfxPropertySetHandle /*outArgs*/ )
{
	OfxImageEffectHandle effect = (OfxImageEffectHandle)handle;

	if( strcmp( action, kOfxActionLoad ) == 0 )
		return onLoad();
	if( strcmp( action, kOfxActionDescribe ) == 0 )
		return describe( effect );
	if( strcmp( action, kOfxImageEffectActionDescribeInContext ) == 0 )
		return describeInContext( effect );
	if( strcmp( action, kOfxImageEffectActionRender ) == 0 )
		return render( effect, inArgs );

	return kOfxStatReplyDefault;
}

void setHostFunc( OfxHost* host )
{
	gHost = host;
}

OfxPlugin gPlugin = {
	kOfxImageEffectPluginApi,
	1,
	"com.stoatworks.MetalGainExample",
	1,
	0,
	setHostFunc,
	pluginMain
};

} // namespace

extern "C" {

EXPORT int OfxGetNumberOfPlugins( void )
{
	return 1;
}

EXPORT OfxPlugin* OfxGetPlugin( int nth )
{
	return nth == 0 ? &gPlugin : nullptr;
}

} // extern "C"
