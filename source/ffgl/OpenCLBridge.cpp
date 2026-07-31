//
// GL <-> OpenCL interop.
//
// Same problem as MetalBridge, different API. OFX's OpenCL *buffer* mode passes
// images as `cl_mem` buffers, so we need a `cl_mem` that shares memory with an
// OpenGL object.
//
// The shared object here is a **pixel buffer object**. Unlike Metal — where an
// IOSurface can back a GL texture directly — OpenCL's buffer interop
// (clCreateFromGLBuffer) takes a GL *buffer*, not a texture. So:
//
//   host GL texture --glReadPixels--> PBO (= cl_mem) --kernel--> PBO --> texture
//
// Both glReadPixels-into-a-PBO and glTexSubImage2D-from-a-PBO stay on the GPU,
// so despite the extra hop no pixel crosses to the CPU.
//
// On macOS the CL context is created from the current CGL share group, which is
// what makes the GL objects visible to OpenCL at all.
//
// OpenCL is deprecated on macOS but still functional, and this is the only
// platform where the path can currently be tested. It matters in production on
// Windows and Linux, where Resolve uses OpenCL on AMD hardware.
//

#include "OpenCLBridge.h"

#ifdef __APPLE__
#include <OpenCL/opencl.h>
#include <OpenCL/cl_gl_ext.h>
#include <OpenGL/CGLCurrent.h>
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#else
#include <CL/cl.h>
#include <CL/cl_gl.h>
#endif

namespace ofxffgl {

struct OpenCLBridge::Impl
{
	cl_context context     = nullptr;
	cl_device_id device    = nullptr;
	cl_command_queue queue = nullptr;

	GLuint sourcePbo = 0;
	GLuint outputPbo = 0;
	cl_mem sourceMem = nullptr;
	cl_mem outputMem = nullptr;

	int width    = 0;
	int height   = 0;
	int rowBytes = 0;

	void releaseBuffers()
	{
		if( sourceMem )
		{
			clReleaseMemObject( sourceMem );
			sourceMem = nullptr;
		}
		if( outputMem )
		{
			clReleaseMemObject( outputMem );
			outputMem = nullptr;
		}
		if( sourcePbo )
		{
			glDeleteBuffers( 1, &sourcePbo );
			sourcePbo = 0;
		}
		if( outputPbo )
		{
			glDeleteBuffers( 1, &outputPbo );
			outputPbo = 0;
		}
		width = height = rowBytes = 0;
	}
};

OpenCLBridge::OpenCLBridge() : _impl( new Impl )
{
}

OpenCLBridge::~OpenCLBridge()
{
	shutdown();
	delete _impl;
}

bool OpenCLBridge::init( std::string& error )
{
	if( _impl->context != nullptr )
		return true;

#ifdef __APPLE__
	CGLContextObj cgl = CGLGetCurrentContext();
	if( cgl == nullptr )
	{
		error = "no current CGL context; OpenCL/GL sharing needs one";
		return false;
	}
	CGLShareGroupObj shareGroup = CGLGetShareGroup( cgl );

	// The share group is what makes our GL buffers visible to OpenCL. Without
	// it clCreateFromGLBuffer fails with CL_INVALID_CONTEXT.
	cl_context_properties props[] = {
		CL_CONTEXT_PROPERTY_USE_CGL_SHAREGROUP_APPLE, (cl_context_properties)shareGroup, 0
	};

	cl_int err     = CL_SUCCESS;
	_impl->context = clCreateContext( props, 0, nullptr, nullptr, nullptr, &err );
	if( err != CL_SUCCESS || _impl->context == nullptr )
	{
		error = "clCreateContext from the CGL share group failed";
		return false;
	}

	size_t deviceBytes = 0;
	clGetContextInfo( _impl->context, CL_CONTEXT_DEVICES, 0, nullptr, &deviceBytes );
	if( deviceBytes < sizeof( cl_device_id ) )
	{
		error = "the shared OpenCL context has no devices";
		shutdown();
		return false;
	}
	clGetContextInfo( _impl->context, CL_CONTEXT_DEVICES, sizeof( cl_device_id ), &_impl->device, nullptr );
#else
	error = "OpenCL/GL sharing is only wired up for macOS in this build";
	return false;
#endif

	cl_int qerr  = CL_SUCCESS;
	_impl->queue = clCreateCommandQueue( _impl->context, _impl->device, 0, &qerr );
	if( qerr != CL_SUCCESS || _impl->queue == nullptr )
	{
		error = "clCreateCommandQueue failed";
		shutdown();
		return false;
	}
	return true;
}

void OpenCLBridge::shutdown()
{
	_impl->releaseBuffers();
	if( _impl->queue )
	{
		clReleaseCommandQueue( _impl->queue );
		_impl->queue = nullptr;
	}
	if( _impl->context )
	{
		clReleaseContext( _impl->context );
		_impl->context = nullptr;
	}
	_impl->device = nullptr;
}

bool OpenCLBridge::isReady() const
{
	return _impl->context != nullptr && _impl->sourceMem != nullptr && _impl->outputMem != nullptr;
}

bool OpenCLBridge::resize( int width, int height, std::string& error )
{
	if( _impl->context == nullptr )
	{
		error = "OpenCL bridge not initialised";
		return false;
	}
	if( _impl->width == width && _impl->height == height && _impl->sourceMem != nullptr )
		return true;

	_impl->releaseBuffers();

	// Tightly packed; nothing forces padding on a PBO the way IOSurface pads.
	const int rowBytes  = width * 4;
	const size_t bytes  = (size_t)rowBytes * (size_t)height;

	GLuint pbos[ 2 ] = { 0, 0 };
	glGenBuffers( 2, pbos );
	for( int i = 0; i < 2; ++i )
	{
		glBindBuffer( GL_PIXEL_PACK_BUFFER, pbos[ i ] );
		glBufferData( GL_PIXEL_PACK_BUFFER, (GLsizeiptr)bytes, nullptr, GL_DYNAMIC_COPY );
	}
	glBindBuffer( GL_PIXEL_PACK_BUFFER, 0 );

	cl_int err       = CL_SUCCESS;
	cl_mem srcMem    = clCreateFromGLBuffer( _impl->context, CL_MEM_READ_WRITE, pbos[ 0 ], &err );
	cl_int err2      = CL_SUCCESS;
	cl_mem dstMem    = clCreateFromGLBuffer( _impl->context, CL_MEM_READ_WRITE, pbos[ 1 ], &err2 );

	if( err != CL_SUCCESS || err2 != CL_SUCCESS || srcMem == nullptr || dstMem == nullptr )
	{
		if( srcMem )
			clReleaseMemObject( srcMem );
		if( dstMem )
			clReleaseMemObject( dstMem );
		glDeleteBuffers( 2, pbos );
		error = "clCreateFromGLBuffer failed";
		return false;
	}

	_impl->sourcePbo = pbos[ 0 ];
	_impl->outputPbo = pbos[ 1 ];
	_impl->sourceMem = srcMem;
	_impl->outputMem = dstMem;
	_impl->width     = width;
	_impl->height    = height;
	_impl->rowBytes  = rowBytes;
	return true;
}

void* OpenCLBridge::commandQueue() const
{
	return (void*)_impl->queue;
}

void* OpenCLBridge::sourceMem() const
{
	return (void*)_impl->sourceMem;
}

void* OpenCLBridge::outputMem() const
{
	return (void*)_impl->outputMem;
}

int OpenCLBridge::rowBytes() const
{
	return _impl->rowBytes;
}

unsigned int OpenCLBridge::sourcePbo() const
{
	return _impl->sourcePbo;
}

unsigned int OpenCLBridge::outputPbo() const
{
	return _impl->outputPbo;
}

bool OpenCLBridge::acquireFromGL()
{
	// OpenCL may not touch a shared object until the GL commands that wrote it
	// have completed and ownership has transferred.
	glFlush();
	cl_mem objects[ 2 ] = { _impl->sourceMem, _impl->outputMem };
	return clEnqueueAcquireGLObjects( _impl->queue, 2, objects, 0, nullptr, nullptr ) == CL_SUCCESS;
}

bool OpenCLBridge::releaseToGL()
{
	cl_mem objects[ 2 ] = { _impl->sourceMem, _impl->outputMem };
	if( clEnqueueReleaseGLObjects( _impl->queue, 2, objects, 0, nullptr, nullptr ) != CL_SUCCESS )
		return false;
	// The plugin was entitled to return before its kernel finished, so the host
	// waits here before GL reads the output buffer.
	return clFinish( _impl->queue ) == CL_SUCCESS;
}

} // namespace ofxffgl
