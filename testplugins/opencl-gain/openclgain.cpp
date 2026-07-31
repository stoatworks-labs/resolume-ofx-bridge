//
// A minimal OFX plugin that renders with OpenCL.
//
// Companion to testplugins/metal-gain, and it exists for the same reason: no
// OpenCL OFX plugin is publicly available to test against. The OpenFX examples
// are CPU or OpenGL, and commercial OpenCL plugins refuse to load in an
// unrecognised host.
//
// Also deliberately GPU-only — it declines to render if the host has not enabled
// OpenCL — so a host that silently falls back to CPU is caught rather than
// flattered.
//
// OFX defines two OpenCL modes: *buffers* (kOfxImageEffectPropOpenCLRenderSupported,
// where kOfxImagePropData is a cl_mem buffer) and *images*
// (kOfxImageEffectPropOpenCLSupported, with kOfxImageEffectPropOpenCLImage). This
// implements the buffer variant, which is what Resolve uses.
//

#ifdef __APPLE__
#include <OpenCL/opencl.h>
#else
#include <CL/cl.h>
#endif

#include "ofxCore.h"
#include "ofxImageEffect.h"
#include "ofxGPURender.h"
#include "ofxParam.h"
#include "ofxProperty.h"

#include <cstdio>
#include <cstring>
#include <map>

#define EXPORT __attribute__( ( visibility( "default" ) ) )

namespace {

OfxHost* gHost                      = nullptr;
OfxImageEffectSuiteV1* gEffectSuite = nullptr;
OfxPropertySuiteV1* gPropSuite      = nullptr;
OfxParameterSuiteV1* gParamSuite    = nullptr;

/// Kernels are cached per context: the host owns the queue, and building a
/// program on every frame would swamp the render it is meant to accelerate.
std::map< cl_context, cl_kernel > gKernels;

const char* kKernelSource = R"CLC(
__kernel void gainKernel(__global const uchar *src,
                         __global       uchar *dst,
                         const uint  width,
                         const uint  height,
                         const uint  rowBytes,
                         const float gain)
{
    uint x = get_global_id(0);
    uint y = get_global_id(1);
    if (x >= width || y >= height)
        return;

    // rowBytes is honoured rather than assuming width*4 — the host is free to
    // pad rows, and ignoring that shears the image.
    uint off = y * rowBytes + x * 4;

    for (uint c = 0; c < 4; ++c) {
        float v = convert_float(src[off + c]) * gain;
        dst[off + c] = convert_uchar_sat(v);
    }
}
)CLC";

/// Build (or fetch) the kernel for the context that owns `queue`.
cl_kernel kernelFor( cl_command_queue queue )
{
	cl_context context = nullptr;
	if( clGetCommandQueueInfo( queue, CL_QUEUE_CONTEXT, sizeof( context ), &context, nullptr ) != CL_SUCCESS )
		return nullptr;

	auto it = gKernels.find( context );
	if( it != gKernels.end() )
		return it->second;

	cl_int err        = CL_SUCCESS;
	cl_program program = clCreateProgramWithSource( context, 1, &kKernelSource, nullptr, &err );
	if( err != CL_SUCCESS || program == nullptr )
	{
		fprintf( stderr, "[openclgain] clCreateProgramWithSource failed (%d)\n", err );
		return nullptr;
	}

	cl_device_id device = nullptr;
	clGetCommandQueueInfo( queue, CL_QUEUE_DEVICE, sizeof( device ), &device, nullptr );

	err = clBuildProgram( program, 1, &device, nullptr, nullptr, nullptr );
	if( err != CL_SUCCESS )
	{
		char log[ 4096 ] = {};
		clGetProgramBuildInfo( program, device, CL_PROGRAM_BUILD_LOG, sizeof( log ) - 1, log, nullptr );
		fprintf( stderr, "[openclgain] build failed: %s\n", log );
		clReleaseProgram( program );
		return nullptr;
	}

	cl_kernel kernel = clCreateKernel( program, "gainKernel", &err );
	clReleaseProgram( program );
	if( err != CL_SUCCESS || kernel == nullptr )
	{
		fprintf( stderr, "[openclgain] clCreateKernel failed (%d)\n", err );
		return nullptr;
	}

	gKernels[ context ] = kernel;
	return kernel;
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
	return kOfxStatOK;
}

OfxStatus describe( OfxImageEffectHandle effect )
{
	OfxPropertySetHandle props;
	gEffectSuite->getPropertySet( effect, &props );

	gPropSuite->propSetString( props, kOfxPropLabel, 0, "OpenCL Gain Example" );
	gPropSuite->propSetString( props, kOfxImageEffectPluginPropGrouping, 0, "Stoatworks Test" );
	gPropSuite->propSetString( props, kOfxImageEffectPropSupportedContexts, 0, kOfxImageEffectContextFilter );
	gPropSuite->propSetString( props, kOfxImageEffectPropSupportedPixelDepths, 0, kOfxBitDepthByte );

	// The buffer variant of OpenCL render.
	gPropSuite->propSetString( props, kOfxImageEffectPropOpenCLRenderSupported, 0, "true" );

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

	int enabled = 0;
	gPropSuite->propGetInt( inArgs, kOfxImageEffectPropOpenCLEnabled, 0, &enabled );
	if( !enabled )
	{
		fprintf( stderr, "[openclgain] host did not enable OpenCL render; refusing\n" );
		return kOfxStatErrImageFormat;
	}

	void* queuePtr = nullptr;
	gPropSuite->propGetPointer( inArgs, kOfxImageEffectPropOpenCLCommandQueue, 0, &queuePtr );
	if( queuePtr == nullptr )
	{
		fprintf( stderr, "[openclgain] host enabled OpenCL but supplied no command queue\n" );
		return kOfxStatFailed;
	}
	cl_command_queue queue = reinterpret_cast< cl_command_queue >( queuePtr );

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
	int rowBytes  = 0;
	gPropSuite->propGetPointer( sourceImg, kOfxImagePropData, 0, &srcData );
	gPropSuite->propGetPointer( outputImg, kOfxImagePropData, 0, &dstData );
	gPropSuite->propGetInt( sourceImg, kOfxImagePropRowBytes, 0, &rowBytes );

	// With OpenCL enabled these are cl_mem, not CPU pointers.
	cl_mem srcMem = reinterpret_cast< cl_mem >( srcData );
	cl_mem dstMem = reinterpret_cast< cl_mem >( dstData );

	OfxStatus status = kOfxStatOK;
	cl_kernel kernel = kernelFor( queue );

	if( srcMem == nullptr || dstMem == nullptr || kernel == nullptr )
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

		const cl_uint width  = (cl_uint)( window.x2 - window.x1 );
		const cl_uint height = (cl_uint)( window.y2 - window.y1 );
		const cl_uint rb     = (cl_uint)rowBytes;
		const cl_float g     = (cl_float)gain;

		clSetKernelArg( kernel, 0, sizeof( cl_mem ), &srcMem );
		clSetKernelArg( kernel, 1, sizeof( cl_mem ), &dstMem );
		clSetKernelArg( kernel, 2, sizeof( cl_uint ), &width );
		clSetKernelArg( kernel, 3, sizeof( cl_uint ), &height );
		clSetKernelArg( kernel, 4, sizeof( cl_uint ), &rb );
		clSetKernelArg( kernel, 5, sizeof( cl_float ), &g );

		const size_t global[ 2 ] = { width, height };
		const cl_int err = clEnqueueNDRangeKernel( queue, kernel, 2, nullptr, global, nullptr, 0, nullptr,
												   nullptr );
		if( err != CL_SUCCESS )
		{
			fprintf( stderr, "[openclgain] clEnqueueNDRangeKernel failed (%d)\n", err );
			status = kOfxStatFailed;
		}
		// Deliberately not finished here: the host owns the queue and decides
		// when to synchronise, exactly as on the Metal path.
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
	kOfxImageEffectPluginApi, 1, "com.stoatworks.OpenCLGainExample", 1, 0, setHostFunc, pluginMain
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
