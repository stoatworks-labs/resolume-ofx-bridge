cmake_minimum_required( VERSION 3.20 )

# Must be set before project() or a "universal" macOS build silently produces a
# single-architecture binary.
if( APPLE AND NOT DEFINED CMAKE_OSX_ARCHITECTURES )
	set( CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "macOS architectures" )
endif()

project( resolume-ofx-bridge VERSION 0.5.0 LANGUAGES CXX )

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

# Enables HostSupport's OFX OpenGL render path: the kOfxOpenGLRenderSuite, the
# Texture class, and the contextAttached/Detached actions. A plugin that
# implements OpenGL render can then be handed our GL texture directly instead of
# going through a CPU round trip.
#
# PUBLIC because it changes which virtuals our own Host/Clip must implement.
target_compile_definitions( ofxhostsupport PUBLIC OFX_SUPPORTS_OPENGLRENDER )

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

# ---------------------------------------------------------------------------
# ofxwrapper - the generic FFGL plugin
#
# One build of this binary is *copied* per OFX plugin by the generator; each
# copy reads a manifest stored beside it to learn which effect it hosts. That is
# why nothing here is specialised to a particular plugin.
# ---------------------------------------------------------------------------
if( NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/ffgl/CMakeLists.txt" )
	message( FATAL_ERROR "external/ffgl is missing. Run: git submodule update --init --recursive" )
endif()

set( FFGL_BUILD_EXAMPLE_PLUGINS OFF CACHE BOOL "" FORCE )
add_subdirectory( external/ffgl EXCLUDE_FROM_ALL )

add_library( ofxwrapper MODULE
	source/ffgl/PluginMain.cpp
	source/ffgl/OfxFFGLPlugin.cpp
	source/ffgl/Manifest.cpp
	source/ffgl/Json.cpp
	source/ffgl/SelfPath.cpp
)

if( APPLE )
	# Objective-C++: the GL <-> Metal interop needs Metal and IOSurface.
	# ARC, so the Metal objects held in MetalBridge are actually retained.
	target_sources( ofxwrapper PRIVATE source/ffgl/MetalBridge.mm )
	set_source_files_properties( source/ffgl/MetalBridge.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc" )
	target_sources( ofxwrapper PRIVATE source/ffgl/OpenCLBridge.cpp )
	target_link_libraries( ofxwrapper PRIVATE
		"-framework Metal" "-framework IOSurface" "-framework Foundation"
		"-framework OpenCL" )
	# OpenCL is deprecated on macOS but still functional, and this is the only
	# platform where the OpenCL path can currently be tested.
	target_compile_definitions( ofxwrapper PRIVATE CL_SILENCE_DEPRECATION GL_SILENCE_DEPRECATION )
endif()
target_include_directories( ofxwrapper PRIVATE source/ffgl )
target_link_libraries( ofxwrapper PRIVATE ffgl::sdk ofxbridge )

if( NOT APPLE )
	find_package( GLEW REQUIRED )
	target_link_libraries( ofxwrapper PRIVATE GLEW::GLEW )
endif()

if( APPLE )
	set_target_properties( ofxwrapper PROPERTIES
		BUNDLE TRUE
		BUNDLE_EXTENSION "bundle"
		MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Info.plist.in"
		MACOSX_BUNDLE_BUNDLE_NAME "OFX Wrapper"
		MACOSX_BUNDLE_GUI_IDENTIFIER "com.stoatworks.ffgl.ofxwrapper"
		MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
	)
	# FFGL hosts resolve plugMain by symbol name, so keep default visibility.
	target_link_libraries( ofxwrapper PRIVATE "-framework OpenGL" )
endif()

# ---------------------------------------------------------------------------
# ofxgenlib - scan + copy, with no UI and no argv
#
# Shared by the CLI and the app so that "what a generated bundle contains" has
# exactly one implementation.
# ---------------------------------------------------------------------------
add_library( ofxgenlib STATIC source/gen/Generator.cpp )
target_include_directories( ofxgenlib PUBLIC source/gen )
target_link_libraries( ofxgenlib PUBLIC ofxbridge )

# ---------------------------------------------------------------------------
# ofxgen - the generator CLI
# ---------------------------------------------------------------------------
add_executable( ofxgen source/gen/main.cpp )
target_link_libraries( ofxgen PRIVATE ofxgenlib )
add_dependencies( ofxgen ofxwrapper )

# ---------------------------------------------------------------------------
# OFX Bridge - the app
#
# A window around ofxgenlib: pick plugins, pick a destination, press Start. It
# carries its own copy of the wrapper template in Resources, so a downloaded
# .app generates bundles with nothing else installed.
# ---------------------------------------------------------------------------
if( APPLE )
	add_executable( ofxgenui MACOSX_BUNDLE
		source/ui/main.mm
		source/ui/BridgeWindow.mm
	)
	set_target_properties( ofxgenui PROPERTIES
		OUTPUT_NAME "OFX Bridge"
		MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/cmake/AppInfo.plist.in"
		MACOSX_BUNDLE_BUNDLE_NAME "OFX Bridge"
		MACOSX_BUNDLE_GUI_IDENTIFIER "com.stoatworks.ofxbridge.ui"
		MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
		MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
	)
	# ARC, so the AppKit objects the window controller holds are retained.
	target_compile_options( ofxgenui PRIVATE -fobjc-arc )
	target_link_libraries( ofxgenui PRIVATE ofxgenlib "-framework Cocoa" )

	# Ship the wrapper template inside the app. Without this the app can still
	# find a build-tree copy, but a distributed .app could not generate anything.
	add_dependencies( ofxgenui ofxwrapper )
	add_custom_command( TARGET ofxgenui POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E make_directory
			"$<TARGET_BUNDLE_CONTENT_DIR:ofxgenui>/Resources"
		COMMAND ${CMAKE_COMMAND} -E rm -rf
			"$<TARGET_BUNDLE_CONTENT_DIR:ofxgenui>/Resources/ofxwrapper.bundle"
		COMMAND ${CMAKE_COMMAND} -E copy_directory
			"$<TARGET_BUNDLE_DIR:ofxwrapper>"
			"$<TARGET_BUNDLE_CONTENT_DIR:ofxgenui>/Resources/ofxwrapper.bundle"
		COMMENT "Bundling the wrapper template into OFX Bridge.app"
		VERBATIM
	)
endif()

# ---------------------------------------------------------------------------
# ffgltest - drive a generated bundle through a real GL context
#
# The only way to exercise ProcessOpenGL without Resolume. macOS-only for now:
# it creates the context with CGL directly.
# ---------------------------------------------------------------------------
if( APPLE )
	add_executable( ffgltest source/gltest/main.cpp )
	target_link_libraries( ffgltest PRIVATE ffgl::sdk "-framework OpenGL" )
	add_dependencies( ffgltest ofxwrapper )
endif()
