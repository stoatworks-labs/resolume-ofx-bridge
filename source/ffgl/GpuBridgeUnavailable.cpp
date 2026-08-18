//
// MetalBridge and OpenCLBridge for platforms where neither interop is wired up.
//
// The wrapper calls both bridges unconditionally and decides what to do from
// whether init() succeeded, so the alternative to this file is an #if around
// every call site in OfxFFGLPlugin — and around the members themselves, since
// the pimpl is held by value. Failing honestly in init() keeps the render path
// one shape on every platform: the bridge declines, `_metalFailed` latches, and
// the frame goes down the CPU or GL route exactly as it does for a plugin that
// never offered a GPU path at all.
//
// Metal has no meaning off macOS and never will. OpenCL does: OpenCLBridge.cpp
// already carries a non-Apple branch that returns an error where the CGL share
// group would be obtained, and CL/cl_gl.h is the only reason it is not compiled
// here. Wiring Windows OpenCL up is replacing this file's OpenCLBridge half
// with that translation unit plus a WGL context-property path — not writing
// anything new from scratch.
//
// The host declines these plugins up front rather than mid-render, because
// kOfxImageEffectPropMetalRenderSupported and its OpenCL twin are advertised
// only where the bridge exists. See Host.cpp, which does the same for CUDA.
//

#include "MetalBridge.h"
#include "OpenCLBridge.h"

namespace ofxffgl {

namespace {
const char* const kNoMetal  = "Metal is a macOS API; this build has no Metal render path";
const char* const kNoOpenCL = "OpenCL/GL sharing is not wired up in this build";
} // namespace

// ---------------------------------------------------------------------------
// MetalBridge
// ---------------------------------------------------------------------------

struct MetalBridge::Impl
{
};

MetalBridge::MetalBridge() :
	_impl( nullptr )
{
}

MetalBridge::~MetalBridge()
{
}

bool MetalBridge::init( std::string& error )
{
	error = kNoMetal;
	return false;
}

void MetalBridge::shutdown()
{
}

bool MetalBridge::isReady() const
{
	return false;
}

bool MetalBridge::resize( int /*width*/, int /*height*/, std::string& error )
{
	error = kNoMetal;
	return false;
}

void* MetalBridge::commandQueue() const
{
	return nullptr;
}

void* MetalBridge::sourceBuffer() const
{
	return nullptr;
}

void* MetalBridge::outputBuffer() const
{
	return nullptr;
}

int MetalBridge::rowBytes() const
{
	return 0;
}

unsigned int MetalBridge::sourceTexture() const
{
	return 0;
}

unsigned int MetalBridge::outputTexture() const
{
	return 0;
}

unsigned int MetalBridge::textureTarget() const
{
	return 0;
}

void MetalBridge::flushGLWrites()
{
}

void MetalBridge::waitForMetal()
{
}

// ---------------------------------------------------------------------------
// OpenCLBridge
// ---------------------------------------------------------------------------

struct OpenCLBridge::Impl
{
};

OpenCLBridge::OpenCLBridge() :
	_impl( nullptr )
{
}

OpenCLBridge::~OpenCLBridge()
{
}

bool OpenCLBridge::init( std::string& error )
{
	error = kNoOpenCL;
	return false;
}

void OpenCLBridge::shutdown()
{
}

bool OpenCLBridge::isReady() const
{
	return false;
}

bool OpenCLBridge::resize( int /*width*/, int /*height*/, std::string& error )
{
	error = kNoOpenCL;
	return false;
}

void* OpenCLBridge::commandQueue() const
{
	return nullptr;
}

void* OpenCLBridge::sourceMem() const
{
	return nullptr;
}

void* OpenCLBridge::outputMem() const
{
	return nullptr;
}

int OpenCLBridge::rowBytes() const
{
	return 0;
}

unsigned int OpenCLBridge::sourcePbo() const
{
	return 0;
}

unsigned int OpenCLBridge::outputPbo() const
{
	return 0;
}

bool OpenCLBridge::acquireFromGL()
{
	return false;
}

bool OpenCLBridge::releaseToGL()
{
	return false;
}

} // namespace ofxffgl
