#include "MetalBridge.h"

#import <Metal/Metal.h>
#import <IOSurface/IOSurface.h>

#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <OpenGL/CGLIOSurface.h>

namespace ofxffgl {

namespace {

/// One IOSurface, viewed simultaneously as a GL texture and a Metal buffer.
struct SharedSurface
{
	IOSurfaceRef surface   = nullptr;
	GLuint texture         = 0;
	id< MTLBuffer > buffer = nil;
	int width              = 0;
	int height             = 0;
	int rowBytes           = 0;

	void release()
	{
		buffer = nil;
		if( texture != 0 )
		{
			glDeleteTextures( 1, &texture );
			texture = 0;
		}
		if( surface != nullptr )
		{
			CFRelease( surface );
			surface = nullptr;
		}
		width = height = rowBytes = 0;
	}

	bool create( id< MTLDevice > device, int w, int h, std::string& error );
};

bool SharedSurface::create( id< MTLDevice > device, int w, int h, std::string& error )
{
	release();

	// 'BGRA' is the format both GL and Metal handle natively on macOS. See the
	// channel-order note in create()'s caller.
	NSDictionary* props = @{
		(__bridge NSString*)kIOSurfaceWidth : @( w ),
		(__bridge NSString*)kIOSurfaceHeight : @( h ),
		(__bridge NSString*)kIOSurfaceBytesPerElement : @( 4 ),
		(__bridge NSString*)kIOSurfacePixelFormat : @( (unsigned int)'BGRA' ),
	};

	surface = IOSurfaceCreate( (__bridge CFDictionaryRef)props );
	if( surface == nullptr )
	{
		error = "IOSurfaceCreate failed";
		return false;
	}

	width    = w;
	height   = h;
	rowBytes = (int)IOSurfaceGetBytesPerRow( surface );

	// --- GL view -----------------------------------------------------------
	// CGLTexImageIOSurface2D accepts only GL_TEXTURE_RECTANGLE. That is a macOS
	// constraint, not a choice.
	CGLContextObj cgl = CGLGetCurrentContext();
	if( cgl == nullptr )
	{
		error = "no current CGL context when creating the shared surface";
		release();
		return false;
	}

	glGenTextures( 1, &texture );
	glBindTexture( GL_TEXTURE_RECTANGLE, texture );
	const CGLError cglErr = CGLTexImageIOSurface2D( cgl, GL_TEXTURE_RECTANGLE, GL_RGBA, w, h, GL_BGRA,
													GL_UNSIGNED_INT_8_8_8_8_REV, surface, 0 );
	glTexParameteri( GL_TEXTURE_RECTANGLE, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_RECTANGLE, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	glBindTexture( GL_TEXTURE_RECTANGLE, 0 );

	if( cglErr != kCGLNoError )
	{
		error = "CGLTexImageIOSurface2D failed";
		release();
		return false;
	}

	// --- Metal view --------------------------------------------------------
	// No copy: the buffer wraps the surface's own pages. IOSurface base
	// addresses are page-aligned, which newBufferWithBytesNoCopy requires.
	void* base           = IOSurfaceGetBaseAddress( surface );
	const size_t allocSz = IOSurfaceGetAllocSize( surface );
	buffer               = [ device newBufferWithBytesNoCopy:base
													  length:allocSz
													 options:MTLResourceStorageModeShared
												 deallocator:nil ];
	if( buffer == nil )
	{
		error = "newBufferWithBytesNoCopy over the IOSurface failed";
		release();
		return false;
	}

	return true;
}

} // namespace

struct MetalBridge::Impl
{
	id< MTLDevice > device       = nil;
	id< MTLCommandQueue > queue  = nil;
	SharedSurface source;
	SharedSurface output;
};

MetalBridge::MetalBridge() : _impl( new Impl )
{
}

MetalBridge::~MetalBridge()
{
	shutdown();
	delete _impl;
}

bool MetalBridge::init( std::string& error )
{
	if( _impl->device != nil )
		return true;

	_impl->device = MTLCreateSystemDefaultDevice();
	if( _impl->device == nil )
	{
		error = "no Metal device available";
		return false;
	}

	_impl->queue = [ _impl->device newCommandQueue ];
	if( _impl->queue == nil )
	{
		error = "could not create a Metal command queue";
		_impl->device = nil;
		return false;
	}
	return true;
}

void MetalBridge::shutdown()
{
	_impl->source.release();
	_impl->output.release();
	_impl->queue  = nil;
	_impl->device = nil;
}

bool MetalBridge::isReady() const
{
	return _impl->device != nil && _impl->source.buffer != nil && _impl->output.buffer != nil;
}

bool MetalBridge::resize( int width, int height, std::string& error )
{
	if( _impl->device == nil )
	{
		error = "Metal bridge not initialised";
		return false;
	}
	if( _impl->source.width == width && _impl->source.height == height && _impl->source.buffer != nil )
		return true;

	if( !_impl->source.create( _impl->device, width, height, error ) )
		return false;
	if( !_impl->output.create( _impl->device, width, height, error ) )
		return false;
	return true;
}

void* MetalBridge::commandQueue() const
{
	return (__bridge void*)_impl->queue;
}

void* MetalBridge::sourceBuffer() const
{
	return (__bridge void*)_impl->source.buffer;
}

void* MetalBridge::outputBuffer() const
{
	return (__bridge void*)_impl->output.buffer;
}

int MetalBridge::rowBytes() const
{
	return _impl->source.rowBytes;
}

unsigned int MetalBridge::sourceTexture() const
{
	return _impl->source.texture;
}

unsigned int MetalBridge::outputTexture() const
{
	return _impl->output.texture;
}

unsigned int MetalBridge::textureTarget() const
{
	return GL_TEXTURE_RECTANGLE;
}

void MetalBridge::flushGLWrites()
{
	// IOSurface gives coherency, but not ordering: without this the Metal kernel
	// can read the surface before the GL blit that filled it has actually run.
	glFlush();
}

void MetalBridge::waitForMetal()
{
	// The OFX Metal contract explicitly allows a plugin to return before its
	// work completes, so the host must be the one to wait before GL reads the
	// output surface. An empty command buffer scheduled after the plugin's work
	// completes only once that work has.
	id< MTLCommandBuffer > barrier = [ _impl->queue commandBuffer ];
	[ barrier commit ];
	[ barrier waitUntilCompleted ];
}

} // namespace ofxffgl
