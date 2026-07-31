#pragma once
//
// GL <-> Metal interop, so an OFX plugin can render with Metal on pixels that
// arrived as an OpenGL texture and must leave as one.
//
// The OFX Metal contract passes images as `id<MTLBuffer>` — linear memory, not a
// texture — so something has to bridge Resolume's GL texture to a Metal buffer
// and back. Doing that through the CPU would defeat the point entirely.
//
// The trick is IOSurface. A single IOSurface can back:
//
//   * a GL texture, via CGLTexImageIOSurface2D
//   * an MTLBuffer, via newBufferWithBytesNoCopy over its base address
//
// Both views address the same physical memory, so on Apple Silicon's unified
// memory there is no copy at all: a GL blit into the surface is immediately
// visible to Metal, and vice versa. The per-frame cost is two on-GPU blits.
//
// The interface is plain C++ and hands Metal objects out as void*, so the rest
// of the wrapper stays free of Objective-C.
//

#include <string>

namespace ofxffgl {

class MetalBridge
{
public:
	MetalBridge();
	~MetalBridge();

	MetalBridge( const MetalBridge& )            = delete;
	MetalBridge& operator=( const MetalBridge& ) = delete;

	/// Create the Metal device and command queue. Returns false with `error`
	/// populated if Metal is unavailable.
	bool init( std::string& error );
	void shutdown();

	bool isReady() const;

	/// Allocate (or reallocate) the paired surfaces at this size.
	bool resize( int width, int height, std::string& error );

	/// `id<MTLCommandQueue>` for the plugin to encode onto.
	void* commandQueue() const;

	/// `id<MTLBuffer>` views of the source and output surfaces.
	void* sourceBuffer() const;
	void* outputBuffer() const;

	/// Bytes per row of the surfaces. IOSurface pads rows, so this is generally
	/// **not** width * 4 — a plugin that assumes otherwise will shear the image.
	int rowBytes() const;

	/// GL texture names backing the same memory. Both are
	/// GL_TEXTURE_RECTANGLE: CGLTexImageIOSurface2D accepts no other target.
	unsigned int sourceTexture() const;
	unsigned int outputTexture() const;
	unsigned int textureTarget() const;

	/// Make GL writes to the source surface visible to Metal.
	void flushGLWrites();

	/// Wait for the plugin's queued Metal work before GL reads the output.
	///
	/// The OFX contract lets a plugin return without waiting, so the host has to
	/// be the one that synchronises.
	void waitForMetal();

private:
	struct Impl;
	Impl* _impl;
};

} // namespace ofxffgl
