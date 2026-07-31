cmake_minimum_required( VERSION 3.20 )

# Must be set before project() or a "universal" macOS build silently produces a
# single-architecture binary.
if( APPLE AND NOT DEFINED CMAKE_OSX_ARCHITECTURES )
	set( CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "macOS architectures" )
endif()

project( resolume-ofx-bridge VERSION 0.1.0 LANGUAGES CXX )

set( CMAKE_CXX_STANDARD 17 )
set( CMAKE_CXX_STANDARD_REQUIRED ON )
set( CMAKE_POSITION_INDEPENDENT_CODE ON )

if( NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES )
	set( CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE )
endif()

set( OFX_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/external/openfx" )

if( NOT EXISTS "${OFX_ROOT}/include/ofxCore.h" )
	message( FATAL_ERROR "external/openfx is missing. Run: git submodule update --init --recursive" )
endif()

# ---------------------------------------------------------------------------
# OpenFX HostSupport
#
# Vendored as a source build rather than via the upstream CMake project, because
# that project pulls in Conan-only packages (cimg, spdlog, opengl_system) that
# only its example plugins need.
# ---------------------------------------------------------------------------
file( GLOB OFXHOST_SOURCES "${OFX_ROOT}/HostSupport/src/*.cpp" )

add_library( ofxhostsupport STATIC ${OFXHOST_SOURCES} )
target_include_directories( ofxhostsupport PUBLIC
	"${OFX_ROOT}/include"
	"${OFX_ROOT}/HostSupport/include"
)

find_package( EXPAT REQUIRED )
target_link_libraries( ofxhostsupport PUBLIC EXPAT::EXPAT )

if( APPLE )
	target_compile_definitions( ofxhostsupport PUBLIC OFX_SUPPORTS_MULTITHREAD )
endif()

# HostSupport is third-party; don't let its warnings drown ours.
target_compile_options( ofxhostsupport PRIVATE -w )

# ---------------------------------------------------------------------------
# ofxbridge - our host implementation and plugin catalogue
# ---------------------------------------------------------------------------
add_library( ofxbridge STATIC
	source/ofxbridge/Host.cpp
	source/ofxbridge/Catalog.cpp
	source/ofxbridge/Params.cpp
)
target_include_directories( ofxbridge PUBLIC source/ofxbridge )
target_link_libraries( ofxbridge PUBLIC ofxhostsupport )

if( APPLE )
	target_link_libraries( ofxbridge PUBLIC "-framework CoreFoundation" )
endif()

# ---------------------------------------------------------------------------
# ofxprobe - introspection CLI
# ---------------------------------------------------------------------------
add_executable( ofxprobe source/probe/main.cpp )
target_link_libraries( ofxprobe PRIVATE ofxbridge )
