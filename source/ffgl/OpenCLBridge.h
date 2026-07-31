#pragma once
//
// GL <-> OpenCL interop for the OFX OpenCL buffer render path.
//
// See OpenCLBridge.cpp for why a pixel buffer object is the shared object here,
// where Metal uses an IOSurface.
//
// Plain C++; OpenCL handles cross as void* so the rest of the wrapper needs no
// OpenCL headers.
//

#include <string>

namespace ofxffgl {

class OpenCLBridge
{
public:
	OpenCLBridge();
	~OpenCLBridge();

	OpenCLBridge( const OpenCLBridge& )            = delete;
	OpenCLBridge& operator=( const OpenCLBridge& ) = delete;

	/// Create a CL context sharing with the current GL context.
	bool init( std::string& error );
	void shutdown();
	bool isReady() const;

	bool resize( int width, int height, std::string& error );

	/// `cl_command_queue` for the plugin to enqueue onto.
	void* commandQueue() const;
	/// `cl_mem` buffers the plugin reads and writes.
	void* sourceMem() const;
	void* outputMem() const;
	int rowBytes() const;

	/// The GL pixel buffers backing those cl_mem objects.
	unsigned int sourcePbo() const;
	unsigned int outputPbo() const;

	/// Transfer ownership of the shared buffers to OpenCL, and back afterwards.
	/// releaseToGL() also waits for the plugin's queued work.
	bool acquireFromGL();
	bool releaseToGL();

private:
	struct Impl;
	Impl* _impl;
};

} // namespace ofxffgl
