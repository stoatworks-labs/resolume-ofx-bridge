cmake_minimum_required( VERSION 3.20 )

# Must be set before project() or a "universal" macOS build silently produces a
# single-architecture binary.
if( APPLE AND NOT DEFINED CMAKE_OSX_ARCHITECTURES )
	set( CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "macOS architectures" )
endif()

project( resolume-ofx-bridge VERSION 0.8.1 LANGUAGES CXX )

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

# Enables HostSupport's OFX multi-thread suite, which our Host implements over
# std::thread and std::recursive_mutex -- nothing platform-specific, so this is
# not conditional. It used to be inside an if(APPLE) purely because macOS was
# the only platform anyone had compiled; MSVC found that out immediately, since
# without the define the suite's methods exist to override nothing.
target_compile_definitions( ofxhostsupport PUBLIC OFX_SUPPORTS_MULTITHREAD )

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

	# Which GPU render paths the host may advertise. These say "the interop for
	# this exists in this build", so they belong wherever the bridges are
	# compiled -- see the ofxwrapper block below and Host.cpp's capability
	# properties. PUBLIC because ofxprobe reports what the host claims.
	target_compile_definitions( ofxbridge PUBLIC OFXBRIDGE_HAS_METAL OFXBRIDGE_HAS_OPENCL )
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
else()
	# Neither interop is wired up off macOS. Both bridges still exist as objects
	# the wrapper calls; they decline in init() and the frame takes the CPU or
	# GL route. See source/ffgl/GpuBridgeUnavailable.cpp.
	target_sources( ofxwrapper PRIVATE source/ffgl/GpuBridgeUnavailable.cpp )
endif()
target_include_directories( ofxwrapper PRIVATE source/ffgl )
target_link_libraries( ofxwrapper PRIVATE ffgl::sdk ofxbridge )

if( NOT APPLE )
	find_package( GLEW REQUIRED )
	target_link_libraries( ofxwrapper PRIVATE GLEW::GLEW )
	find_package( OpenGL REQUIRED )
	target_link_libraries( ofxwrapper PRIVATE OpenGL::GL )
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
# ---------------------------------------------------------------------------
# ffglguest - host an FFGL plugin as a guest (the FFGL->OFX cell of the
# any-to-any matrix). Its Platform module owns the two OS dependencies -- module
# loading and an offscreen GL context -- for all three platforms, which is why
# ffgltest links it rather than making a context of its own.
# ---------------------------------------------------------------------------
if( APPLE )
	add_library( ffglguest STATIC
		source/ffglguest/FfglGuest.cpp
		source/ffglguest/Platform.cpp
	)
	target_include_directories( ffglguest PUBLIC
		source/ffglguest
		external/ffgl/source/lib/ffgl
	)
	# Apple deprecated all of OpenGL in 10.14; FFGL is still OpenGL, and CI
	# promotes the warning to an error.
	target_compile_definitions( ffglguest PUBLIC GL_SILENCE_DEPRECATION )
	target_link_libraries( ffglguest PUBLIC "-framework OpenGL" )
else()
	add_library( ffglguest STATIC
		source/ffglguest/FfglGuest.cpp
		source/ffglguest/Platform.cpp
	)
	target_include_directories( ffglguest PUBLIC
		source/ffglguest
		external/ffgl/source/lib/ffgl
	)
	# GLEW for the entry points a core profile does not expose statically; on
	# Linux, EGL for a context with no display attached.
	find_package( GLEW REQUIRED )
	target_link_libraries( ffglguest PUBLIC GLEW::GLEW )
	if( WIN32 )
		target_link_libraries( ffglguest PUBLIC opengl32 )
	else()
		find_package( OpenGL REQUIRED )
		target_link_libraries( ffglguest PUBLIC OpenGL::GL EGL )
	endif()
endif()

# ---------------------------------------------------------------------------
# aeguest - the After Effects guest host: a Rust staticlib (exact PF struct
# layouts from the community -sys bindings) behind a narrow C ABI, plus the
# C++ face in source/aeguest/. Universal via two cargo targets + lipo.
# ---------------------------------------------------------------------------
add_library( bridgejson STATIC source/ffgl/Json.cpp )
target_include_directories( bridgejson PUBLIC source/ffgl )

# After Effects runs on macOS and Windows and nowhere else, and the community
# -sys crate ships bindings for exactly those two. So the AE guest is not built
# on Linux — not as a limitation, but because there is no such thing as a Linux
# After Effects plugin to carry.
if( APPLE OR WIN32 )
	set( BRIDGE_HAS_AE_GUEST ON )
	set( AEGUEST_TARGET_DIR "${CMAKE_BINARY_DIR}/aeguest-target" )

	if( APPLE )
		# Two slices and lipo, because a plugin has to load in both flavours of
		# host. `rustup target add` because CI runners ship only one target;
		# harmless when they are already present.
		set( AEGUEST_A "${CMAKE_BINARY_DIR}/libaeguest_universal.a" )
		add_custom_command(
			OUTPUT "${AEGUEST_A}"
			COMMAND rustup target add aarch64-apple-darwin x86_64-apple-darwin
			COMMAND cargo build --release --target aarch64-apple-darwin --target-dir "${AEGUEST_TARGET_DIR}"
			COMMAND cargo build --release --target x86_64-apple-darwin --target-dir "${AEGUEST_TARGET_DIR}"
			COMMAND lipo "${AEGUEST_TARGET_DIR}/aarch64-apple-darwin/release/libaeguest.a"
						 "${AEGUEST_TARGET_DIR}/x86_64-apple-darwin/release/libaeguest.a"
						 -create -output "${AEGUEST_A}"
			WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/aeguest"
			DEPENDS aeguest/src/lib.rs aeguest/Cargo.toml
			COMMENT "cargo: aeguest (universal)"
			VERBATIM
		)
	elseif( WIN32 )
		# One slice, but named explicitly rather than left to cargo's host
		# default. MSVC cross-compiles happily -- an ARM64 Windows machine
		# building -A x64 is the normal way to produce x64 binaries here -- and
		# cargo would answer that with a host-arch .lib. The link error names
		# only the symbols, so the arch mismatch is the last thing you suspect.
		#
		# `rustup target add` because a CI runner ships one target; harmless
		# when it is already installed, and the Apple branch does the same.
		if( CMAKE_GENERATOR_PLATFORM )
			set( AEGUEST_ARCH "${CMAKE_GENERATOR_PLATFORM}" )
		else()
			set( AEGUEST_ARCH "${CMAKE_SYSTEM_PROCESSOR}" )
		endif()
		string( TOLOWER "${AEGUEST_ARCH}" AEGUEST_ARCH )

		if( AEGUEST_ARCH STREQUAL "arm64" OR AEGUEST_ARCH STREQUAL "aarch64" )
			set( AEGUEST_RUST_TARGET "aarch64-pc-windows-msvc" )
		elseif( AEGUEST_ARCH STREQUAL "win32" OR AEGUEST_ARCH STREQUAL "x86" )
			set( AEGUEST_RUST_TARGET "i686-pc-windows-msvc" )
		else()
			set( AEGUEST_RUST_TARGET "x86_64-pc-windows-msvc" )
		endif()

		set( AEGUEST_A "${AEGUEST_TARGET_DIR}/${AEGUEST_RUST_TARGET}/release/aeguest.lib" )
		add_custom_command(
			OUTPUT "${AEGUEST_A}"
			COMMAND rustup target add ${AEGUEST_RUST_TARGET}
			COMMAND cargo build --release --target ${AEGUEST_RUST_TARGET} --target-dir "${AEGUEST_TARGET_DIR}"
			WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/aeguest"
			DEPENDS aeguest/src/lib.rs aeguest/Cargo.toml
			COMMENT "cargo: aeguest (${AEGUEST_RUST_TARGET})"
			VERBATIM
		)
	else()
		# One native slice, and no lipo — which does not exist off macOS and
		# whose absence surfaces only as a bare "Error 127".
		set( AEGUEST_A "${AEGUEST_TARGET_DIR}/release/libaeguest.a" )
		add_custom_command(
			OUTPUT "${AEGUEST_A}"
			COMMAND cargo build --release --target-dir "${AEGUEST_TARGET_DIR}"
			WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/aeguest"
			DEPENDS aeguest/src/lib.rs aeguest/Cargo.toml
			COMMENT "cargo: aeguest (native)"
			VERBATIM
		)
	endif()
	add_custom_target( aeguest_rust DEPENDS "${AEGUEST_A}" )

	add_library( aeguest_cpp STATIC source/aeguest/AeGuest.cpp )
	target_include_directories( aeguest_cpp PUBLIC source/aeguest )
	target_link_libraries( aeguest_cpp PUBLIC bridgejson "${AEGUEST_A}" )
	if( WIN32 )
		# What the Rust standard library needs from Windows when linked into a
		# C++ target rather than built by cargo's own linker invocation.
		target_link_libraries( aeguest_cpp PUBLIC ntdll userenv ws2_32 bcrypt advapi32 )
	elseif( NOT APPLE )
		target_link_libraries( aeguest_cpp PUBLIC dl pthread )
	endif()
	add_dependencies( aeguest_cpp aeguest_rust )
endif()

# ---------------------------------------------------------------------------
# ffglofxshell - the generic OFX plugin that carries an FFGL guest. Built once;
# the generator copies it per wrapped plugin, exactly like ofxwrapper going the
# other way. Compiles the OFX Support (plugin-side) library from the vendored
# SDK, the same eight files every fleet OFX plugin builds.
# ---------------------------------------------------------------------------
# Built on every platform: this binary is what the browser wrapper serves, and
# the Windows and Linux ones are compiled-but-never-run (see the release
# workflow's note and docs/03-verification.md).
if( TRUE )
	add_library( ffglofxshell MODULE
		source/ofxshell/FfglOfxShell.cpp
		source/ffgl/SelfPath.cpp
		"${OFX_ROOT}/Support/Library/ofxsCore.cpp"
		"${OFX_ROOT}/Support/Library/ofxsImageEffect.cpp"
		"${OFX_ROOT}/Support/Library/ofxsInteract.cpp"
		"${OFX_ROOT}/Support/Library/ofxsLog.cpp"
		"${OFX_ROOT}/Support/Library/ofxsMultiThread.cpp"
		"${OFX_ROOT}/Support/Library/ofxsParams.cpp"
		"${OFX_ROOT}/Support/Library/ofxsProperty.cpp"
		"${OFX_ROOT}/Support/Library/ofxsPropertyValidation.cpp"
	)
	target_include_directories( ffglofxshell PRIVATE
		source/ffgl
		"${OFX_ROOT}/include"
		"${OFX_ROOT}/Support/include"
	)
	set_target_properties( ffglofxshell PROPERTIES
		PREFIX ""
		SUFFIX ".ofx"
		OUTPUT_NAME "ffglofxshell"
	)
	target_link_libraries( ffglofxshell PRIVATE ffglguest )
	if( BRIDGE_HAS_AE_GUEST )
		target_link_libraries( ffglofxshell PRIVATE aeguest_cpp )
		target_compile_definitions( ffglofxshell PRIVATE BRIDGE_HAS_AE_GUEST=1 )
	else()
		# Linux still needs the JSON reader the manifest path uses.
		target_link_libraries( ffglofxshell PRIVATE bridgejson )
	endif()
	if( WIN32 )
		# Keep MSVC's .exp/.lib away from the other targets' output directory.
		set_target_properties( ffglofxshell PROPERTIES
			ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/importlib"
			ARCHIVE_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/importlib"
			LIBRARY_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}"
			RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}" )
	endif()
endif()

add_library( ofxgenlib STATIC source/gen/Generator.cpp )
target_include_directories( ofxgenlib PUBLIC source/gen )
target_link_libraries( ofxgenlib PUBLIC ofxbridge )
target_link_libraries( ofxgenlib PUBLIC ffglguest )
target_compile_definitions( ofxgenlib PUBLIC OFXGEN_HAS_FFGL_GUEST=1 )
if( BRIDGE_HAS_AE_GUEST )
	target_link_libraries( ofxgenlib PUBLIC aeguest_cpp )
	target_compile_definitions( ofxgenlib PUBLIC OFXGEN_HAS_AE_GUEST=1 )
endif()

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
# The only way to exercise ProcessOpenGL without Resolume, and therefore the
# only evidence that the per-frame path a host actually drives works at all. It
# gets its context and its module loader from ffglguest, which has both for
# every platform, so this builds wherever the wrapper does.
# ---------------------------------------------------------------------------
add_executable( ffgltest source/gltest/main.cpp )
target_link_libraries( ffgltest PRIVATE ffgl::sdk ffglguest )
target_include_directories( ffgltest PRIVATE source )
add_dependencies( ffgltest ofxwrapper )
if( APPLE )
	target_link_libraries( ffgltest PRIVATE "-framework OpenGL" )
endif()
