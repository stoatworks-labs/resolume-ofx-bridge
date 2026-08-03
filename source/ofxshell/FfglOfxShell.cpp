//
// The generic OpenFX shell: one prebuilt .ofx binary that carries an FFGL
// plugin inside it. The FFGL→OFX cell of the any-to-any matrix.
//
// Same architecture as the FFGL wrapper going the other way (see AGENTS.md):
// the generator copies this binary into a bundle per wrapped plugin, drops the
// original FFGL bundle in Contents/Guest/, and writes Contents/manifest.json.
// Everything here configures itself from that manifest at load time — the
// generator is a file-copier, and users never need a compiler.
//
// What crossing this boundary costs, stated rather than hidden:
//
//   - FFGL is an 8-bit world: frames cross as RGBA8 whatever the OFX host's
//     depth. Premultiplied, matching Resolume's convention.
//   - FFGL is sequential: time is forwarded, but a guest that integrates its
//     own clock behaves under scrubbing like a live source, not a file.
//   - The guest renders through a private offscreen GL context, serialised by
//     a global mutex — correctness first; a busy timeline renders one wrapped
//     effect at a time.
//

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "ofxsImageEffect.h"
#include "ofxsProcessing.h"

#include "../ffgl/Json.h"
#include "../ffgl/SelfPath.h"
#include "../ffglguest/FfglGuest.h"

namespace
{

/// The FFGL param types, as stored in the manifest. Mirrors FFGL.h values.
enum FfglType : int
{
	kBoolean  = 0,
	kEvent    = 1,
	kRed      = 2,
	kGreen    = 3,
	kBlue     = 4,
	kXPos     = 5,
	kYPos     = 6,
	kStandard = 10,
	kOption   = 11,
	kBuffer   = 12,
	kInteger  = 13,
	kFile     = 14,
	kText     = 100,
	kHue        = 200,
	kSaturation = 201,
	kBrightness = 202,
	kAlpha      = 203,
};

struct ManifestParam
{
	int index = 0;
	std::string name;
	int type       = kStandard;
	double def     = 0.0;
	double min     = 0.0;
	double max     = 1.0;
	std::string textDefault;
	std::string group;
	std::vector< std::string > elements;
	std::vector< double > elementValues;
};

struct Manifest
{
	std::string identifier;
	std::string label;
	std::string grouping = "FFGL";
	std::string guestBundle;//!< relative to Contents/
	bool isSource = false;
	int versionMajor = 1;
	int versionMinor = 0;
	std::vector< ManifestParam > params;

	bool loaded = false;
	std::string error;
};

std::string contentsDir()
{
	// Contents/MacOS/<binary> -> Contents/
	const std::string bin = ofxffgl::selfBinaryPath();
	const size_t macos    = bin.rfind( "/MacOS/" );
	if( macos == std::string::npos )
		return {};
	return bin.substr( 0, macos );
}

const Manifest& manifest()
{
	static Manifest m = [] {
		Manifest out;
		const std::string dir = contentsDir();
		if( dir.empty() )
		{
			out.error = "could not locate own bundle";
			return out;
		}

		ofxffgl::Json root;
		if( !ofxffgl::Json::parseFile( dir + "/manifest.json", root, out.error ) )
			return out;

		auto str = []( const ofxffgl::Json& j, const char* key, const std::string& fallback = {} ) {
			const ofxffgl::Json* v = j.find( key );
			return v ? v->string( fallback ) : fallback;
		};
		auto num = []( const ofxffgl::Json& j, const char* key, double fallback ) {
			const ofxffgl::Json* v = j.find( key );
			return v ? v->number( fallback ) : fallback;
		};

		out.identifier   = str( root, "identifier" );
		out.label        = str( root, "label" );
		out.grouping     = str( root, "grouping", "FFGL" );
		out.guestBundle  = str( root, "guestBundle" );
		const ofxffgl::Json* isSource = root.find( "isSource" );
		out.isSource     = isSource != nullptr && isSource->boolean( false );
		out.versionMajor = (int)num( root, "versionMajor", 1 );
		out.versionMinor = (int)num( root, "versionMinor", 0 );

		const ofxffgl::Json* params = root.find( "params" );
		if( params != nullptr )
		{
			for( const ofxffgl::Json& p : params->arrayValue )
			{
				ManifestParam mp;
				mp.index       = (int)num( p, "index", 0 );
				mp.name        = str( p, "name" );
				mp.type        = (int)num( p, "type", kStandard );
				mp.def         = num( p, "default", 0.0 );
				mp.min         = num( p, "min", 0.0 );
				mp.max         = num( p, "max", 1.0 );
				mp.textDefault = str( p, "textDefault" );
				mp.group       = str( p, "group" );
				if( const ofxffgl::Json* elements = p.find( "elements" ) )
					mp.elements = elements->stringArray();
				if( const ofxffgl::Json* values = p.find( "elementValues" ) )
					mp.elementValues = values->numberArray();
				out.params.push_back( std::move( mp ) );
			}
		}

		if( out.identifier.empty() || out.guestBundle.empty() )
		{
			out.error = "manifest is missing identifier or guestBundle";
			return out;
		}

		out.loaded = true;
		return out;
	}();
	return m;
}

/// A stable OFX-side script name for an FFGL parameter. FFGL names are
/// display strings ("Lamp Size"); OFX wants an identifier-ish name that stays
/// put across versions, so this is the display name squashed — and the FFGL
/// index appended, because nothing stops two FFGL params sharing a name.
std::string scriptName( const ManifestParam& p )
{
	std::string out;
	for( char c : p.name )
	{
		if( isalnum( (unsigned char)c ) )
			out += (char)tolower( (unsigned char)c );
	}
	if( out.empty() )
		out = "param";
	return out + "_" + std::to_string( p.index );
}

bool isNumericType( int type )
{
	switch( type )
	{
	case kBoolean:
	case kEvent:
	case kOption:
	case kText:
	case kFile:
	case kBuffer:
		return false;
	default:
		return true;
	}
}

/// One GL render at a time, process-wide. Contexts are per-instance, but GL
/// on macOS is not reliably thread-safe across contexts, and an NLE will
/// happily render two instances concurrently.
std::mutex& renderMutex()
{
	static std::mutex m;
	return m;
}

class FfglOfxPlugin : public OFX::ImageEffect
{
public:
	explicit FfglOfxPlugin( OfxImageEffectHandle handle ) :
		OFX::ImageEffect( handle )
	{
		const Manifest& m = manifest();

		dstClip = fetchClip( kOfxImageEffectOutputClipName );
		if( !m.isSource )
			srcClip = fetchClip( kOfxImageEffectSimpleSourceClipName );

		for( const ManifestParam& p : m.params )
		{
			Bound b;
			b.spec = &p;
			switch( p.type )
			{
			case kBoolean:
				b.boolean = fetchBooleanParam( scriptName( p ) );
				break;
			case kEvent:
				b.push = fetchPushButtonParam( scriptName( p ) );
				break;
			case kOption:
				b.choice = fetchChoiceParam( scriptName( p ) );
				break;
			case kText:
			case kFile:
				b.text = fetchStringParam( scriptName( p ) );
				break;
			case kInteger:
				b.integer = fetchIntParam( scriptName( p ) );
				break;
			case kBuffer:
				break;// not representable; leave at the guest's default
			default:
				b.number = fetchDoubleParam( scriptName( p ) );
				break;
			}
			bound.push_back( b );
		}
	}

	void render( const OFX::RenderArguments& args ) override
	{
		const Manifest& m = manifest();

		std::unique_ptr< OFX::Image > dst( dstClip->fetchImage( args.time ) );
		std::unique_ptr< OFX::Image > src;
		if( srcClip != nullptr && srcClip->isConnected() )
			src = std::unique_ptr< OFX::Image >( srcClip->fetchImage( args.time ) );

		const OfxRectI bounds = dst->getBounds();
		const int width       = bounds.x2 - bounds.x1;
		const int height      = bounds.y2 - bounds.y1;

		const bool premultiplied =
			srcClip != nullptr && srcClip->getPreMultiplication() == OFX::eImagePreMultiplied;

		// --- everything GL happens under the lock -------------------------
		std::lock_guard< std::mutex > lock( renderMutex() );

		std::string error;
		if( !guestOpen )
		{
			const std::string path = contentsDir() + "/" + m.guestBundle;
			if( !guest.open( path, error ) )
				OFX::throwSuiteStatusException( kOfxStatFailed );
			guestOpen = true;
		}

		if( !guest.ensureInstance( width, height, error ) )
			OFX::throwSuiteStatusException( kOfxStatFailed );

		pushParams( args.time );

		double fps = dstClip->getFrameRate();
		if( !( fps > 0.0 ) )
			fps = 24.0;
		guest.setTime( args.time / fps );

		// --- frames across the boundary, RGBA8 premultiplied ---------------
		std::vector< uint8_t > inFrame;
		if( src )
		{
			inFrame.resize( (size_t)width * height * 4 );
			toRgba8Premult( *src, bounds, premultiplied, inFrame.data(), width, height );
		}

		std::vector< uint8_t > outFrame( (size_t)width * height * 4 );
		if( !guest.render( src ? inFrame.data() : nullptr, outFrame.data(), error ) )
			OFX::throwSuiteStatusException( kOfxStatFailed );

		fromRgba8Premult( outFrame.data(), *dst, bounds, premultiplied, args.renderWindow );
	}

	void changedParam( const OFX::InstanceChangedArgs&, const std::string& paramName ) override
	{
		// A pushbutton is a press, not a value: forward it as the momentary
		// 1.0 an FFGL event parameter expects. Best-effort — before the first
		// render there is no instance, and a press then means nothing anyway.
		std::lock_guard< std::mutex > lock( renderMutex() );
		if( !guestOpen )
			return;
		for( const Bound& b : bound )
		{
			if( b.push != nullptr && scriptName( *b.spec ) == paramName )
			{
				guest.setParam( (FFUInt32)b.spec->index, 1.0f );
				guest.setParam( (FFUInt32)b.spec->index, 0.0f );
			}
		}
	}

private:
	struct Bound
	{
		const ManifestParam* spec     = nullptr;
		OFX::DoubleParam* number      = nullptr;
		OFX::IntParam* integer        = nullptr;
		OFX::BooleanParam* boolean    = nullptr;
		OFX::ChoiceParam* choice      = nullptr;
		OFX::StringParam* text        = nullptr;
		OFX::PushButtonParam* push    = nullptr;
	};

	void pushParams( double t )
	{
		for( const Bound& b : bound )
		{
			const FFUInt32 index = (FFUInt32)b.spec->index;
			if( b.number != nullptr )
				guest.setParam( index, (float)b.number->getValueAtTime( t ) );
			else if( b.integer != nullptr )
				guest.setParam( index, (float)b.integer->getValueAtTime( t ) );
			else if( b.boolean != nullptr )
				guest.setParam( index, b.boolean->getValueAtTime( t ) ? 1.0f : 0.0f );
			else if( b.choice != nullptr )
			{
				int option = 0;
				b.choice->getValueAtTime( t, option );
				// FFGL options carry their own element values; the index is
				// only the position in the dropdown.
				float value = (float)option;
				if( option >= 0 && option < (int)b.spec->elementValues.size() )
					value = (float)b.spec->elementValues[ (size_t)option ];
				guest.setParam( index, value );
			}
			else if( b.text != nullptr )
			{
				std::string value;
				b.text->getValueAtTime( t, value );
				guest.setTextParam( index, value );
			}
		}
	}

	static void toRgba8Premult( OFX::Image& img, const OfxRectI& dstBounds, bool premultiplied,
								uint8_t* out, int width, int height )
	{
		const OfxRectI b                    = img.getBounds();
		const OFX::BitDepthEnum depth       = img.getPixelDepth();
		const OFX::PixelComponentEnum comps = img.getPixelComponents();
		const int n                         = comps == OFX::ePixelComponentRGBA ? 4 : 3;

		for( int y = 0; y < height; ++y )
		{
			uint8_t* row = out + (size_t)y * width * 4;
			for( int x = 0; x < width; ++x )
			{
				const void* pix = img.getPixelAddress( dstBounds.x1 + x, dstBounds.y1 + y );
				float r = 0, g = 0, bl = 0, a = 0;
				if( pix != nullptr )
				{
					switch( depth )
					{
					case OFX::eBitDepthUByte:
					{
						const uint8_t* p = (const uint8_t*)pix;
						r = p[ 0 ] / 255.0f; g = p[ 1 ] / 255.0f; bl = p[ 2 ] / 255.0f;
						a = n == 4 ? p[ 3 ] / 255.0f : 1.0f;
						break;
					}
					case OFX::eBitDepthUShort:
					{
						const uint16_t* p = (const uint16_t*)pix;
						r = p[ 0 ] / 65535.0f; g = p[ 1 ] / 65535.0f; bl = p[ 2 ] / 65535.0f;
						a = n == 4 ? p[ 3 ] / 65535.0f : 1.0f;
						break;
					}
					default:
					{
						const float* p = (const float*)pix;
						r = p[ 0 ]; g = p[ 1 ]; bl = p[ 2 ];
						a = n == 4 ? p[ 3 ] : 1.0f;
						break;
					}
					}
					if( !premultiplied && n == 4 )
					{
						r *= a; g *= a; bl *= a;
					}
				}
				row[ x * 4 + 0 ] = (uint8_t)( std::clamp( r, 0.0f, 1.0f ) * 255.0f + 0.5f );
				row[ x * 4 + 1 ] = (uint8_t)( std::clamp( g, 0.0f, 1.0f ) * 255.0f + 0.5f );
				row[ x * 4 + 2 ] = (uint8_t)( std::clamp( bl, 0.0f, 1.0f ) * 255.0f + 0.5f );
				row[ x * 4 + 3 ] = (uint8_t)( std::clamp( a, 0.0f, 1.0f ) * 255.0f + 0.5f );
			}
		}
	}

	void fromRgba8Premult( const uint8_t* in, OFX::Image& img, const OfxRectI& dstBounds,
						   bool premultiplied, const OfxRectI& window )
	{
		const OFX::BitDepthEnum depth       = img.getPixelDepth();
		const OFX::PixelComponentEnum comps = img.getPixelComponents();
		const int n                         = comps == OFX::ePixelComponentRGBA ? 4 : 3;
		const int width                     = dstBounds.x2 - dstBounds.x1;

		for( int y = window.y1; y < window.y2; ++y )
		{
			for( int x = window.x1; x < window.x2; ++x )
			{
				const uint8_t* p =
					in + ( (size_t)( y - dstBounds.y1 ) * width + ( x - dstBounds.x1 ) ) * 4;
				float r = p[ 0 ] / 255.0f, g = p[ 1 ] / 255.0f, bl = p[ 2 ] / 255.0f,
					  a = p[ 3 ] / 255.0f;

				if( !premultiplied && n == 4 && a > 0.0f )
				{
					r /= a; g /= a; bl /= a;
				}

				void* pix = img.getPixelAddress( x, y );
				if( pix == nullptr )
					continue;
				switch( depth )
				{
				case OFX::eBitDepthUByte:
				{
					uint8_t* d = (uint8_t*)pix;
					d[ 0 ] = (uint8_t)( r * 255.0f + 0.5f );
					d[ 1 ] = (uint8_t)( g * 255.0f + 0.5f );
					d[ 2 ] = (uint8_t)( bl * 255.0f + 0.5f );
					if( n == 4 )
						d[ 3 ] = (uint8_t)( a * 255.0f + 0.5f );
					break;
				}
				case OFX::eBitDepthUShort:
				{
					uint16_t* d = (uint16_t*)pix;
					d[ 0 ] = (uint16_t)( r * 65535.0f + 0.5f );
					d[ 1 ] = (uint16_t)( g * 65535.0f + 0.5f );
					d[ 2 ] = (uint16_t)( bl * 65535.0f + 0.5f );
					if( n == 4 )
						d[ 3 ] = (uint16_t)( a * 65535.0f + 0.5f );
					break;
				}
				default:
				{
					float* d = (float*)pix;
					d[ 0 ] = r;
					d[ 1 ] = g;
					d[ 2 ] = bl;
					if( n == 4 )
						d[ 3 ] = a;
					break;
				}
				}
			}
		}
	}

	OFX::Clip* dstClip = nullptr;
	OFX::Clip* srcClip = nullptr;
	std::vector< Bound > bound;

	ffglguest::FfglGuest guest;
	bool guestOpen = false;
};

} // namespace

mDeclarePluginFactory( FfglOfxShellFactory, {}, {} );

void FfglOfxShellFactory::describe( OFX::ImageEffectDescriptor& desc )
{
	const Manifest& m = manifest();

	const std::string label = m.loaded ? m.label : "FFGL (broken manifest)";
	desc.setLabels( label.c_str(), label.c_str(), label.c_str() );
	desc.setPluginGrouping( m.grouping.c_str() );
	desc.setPluginDescription(
		( "An FFGL plugin, carried into this host by the Stoatworks bridge. "
		  "The effect renders through its own OpenGL context at 8 bits per "
		  "channel, exactly as it would inside Resolume.\n\n"
		  "https://stoatworks-labs.com"
		  + ( m.loaded ? std::string() : "\n\nMANIFEST ERROR: " + m.error ) )
			.c_str() );

	if( m.isSource )
		desc.addSupportedContext( OFX::eContextGenerator );
	else
		desc.addSupportedContext( OFX::eContextFilter );
	desc.addSupportedContext( OFX::eContextGeneral );

	desc.addSupportedBitDepth( OFX::eBitDepthUByte );
	desc.addSupportedBitDepth( OFX::eBitDepthUShort );
	desc.addSupportedBitDepth( OFX::eBitDepthFloat );

	// The guest is a whole-frame GL effect with its own state: no tiles, and
	// one render at a time per instance.
	desc.setSupportsTiles( false );
	desc.setTemporalClipAccess( false );
	desc.setRenderThreadSafety( OFX::eRenderInstanceSafe );
	desc.setSupportsMultiResolution( true );
}

void FfglOfxShellFactory::describeInContext( OFX::ImageEffectDescriptor& desc, OFX::ContextEnum context )
{
	const Manifest& m = manifest();

	if( !m.isSource || context == OFX::eContextGeneral )
	{
		OFX::ClipDescriptor* srcClip = desc.defineClip( kOfxImageEffectSimpleSourceClipName );
		srcClip->addSupportedComponent( OFX::ePixelComponentRGBA );
		srcClip->addSupportedComponent( OFX::ePixelComponentRGB );
		srcClip->setSupportsTiles( false );
		if( m.isSource )
			srcClip->setOptional( true );
	}

	OFX::ClipDescriptor* dstClip = desc.defineClip( kOfxImageEffectOutputClipName );
	dstClip->addSupportedComponent( OFX::ePixelComponentRGBA );
	dstClip->addSupportedComponent( OFX::ePixelComponentRGB );
	dstClip->setSupportsTiles( false );

	OFX::PageParamDescriptor* page = desc.definePageParam( "Controls" );

	std::string currentGroup;
	OFX::GroupParamDescriptor* group = nullptr;

	for( const ManifestParam& p : m.params )
	{
		if( p.type == kBuffer )
			continue;

		if( p.group != currentGroup )
		{
			currentGroup = p.group;
			group        = nullptr;
			if( !currentGroup.empty() )
			{
				group = desc.defineGroupParam( ( "group_" + currentGroup ).c_str() );
				group->setLabels( currentGroup.c_str(), currentGroup.c_str(), currentGroup.c_str() );
			}
		}

		const std::string name = scriptName( p );

		OFX::ParamDescriptor* made = nullptr;
		switch( p.type )
		{
		case kBoolean:
		{
			OFX::BooleanParamDescriptor* b = desc.defineBooleanParam( name );
			b->setDefault( p.def >= 0.5 );
			made = b;
			break;
		}
		case kEvent:
		{
			made = desc.definePushButtonParam( name );
			break;
		}
		case kOption:
		{
			OFX::ChoiceParamDescriptor* c = desc.defineChoiceParam( name );
			for( const std::string& e : p.elements )
				c->appendOption( e );
			// The FFGL default is an element *value*; find its position.
			int def = 0;
			for( size_t i = 0; i < p.elementValues.size(); ++i )
				if( std::abs( p.elementValues[ i ] - p.def ) < 1e-6 )
					def = (int)i;
			c->setDefault( def );
			made = c;
			break;
		}
		case kText:
		case kFile:
		{
			OFX::StringParamDescriptor* s = desc.defineStringParam( name );
			s->setDefault( p.textDefault.c_str() );
			if( p.type == kFile )
				s->setStringType( OFX::eStringTypeFilePath );
			made = s;
			break;
		}
		case kInteger:
		{
			OFX::IntParamDescriptor* i = desc.defineIntParam( name );
			i->setRange( (int)p.min, (int)p.max );
			i->setDisplayRange( (int)p.min, (int)p.max );
			i->setDefault( (int)p.def );
			made = i;
			break;
		}
		default:
		{
			OFX::DoubleParamDescriptor* dp = desc.defineDoubleParam( name );
			const double lo = std::min( p.min, p.max );
			const double hi = std::max( p.min, p.max );
			dp->setRange( lo, hi );
			dp->setDisplayRange( lo, hi );
			dp->setDefault( std::clamp( p.def, lo, hi ) );
			made = dp;
			break;
		}
		}

		if( made != nullptr )
		{
			made->setLabels( p.name.c_str(), p.name.c_str(), p.name.c_str() );
			if( group != nullptr )
				made->setParent( *group );
			page->addChild( *made );
		}
	}
}

OFX::ImageEffect* FfglOfxShellFactory::createInstance( OfxImageEffectHandle handle, OFX::ContextEnum )
{
	return new FfglOfxPlugin( handle );
}

void OFX::Plugin::getPluginIDs( OFX::PluginFactoryArray& ids )
{
	const Manifest& m = manifest();

	// Deliberately leaked — see the fleet's exit-teardown trap. The identifier
	// comes from the manifest, which is what makes one prebuilt binary
	// register as a different plugin per generated bundle.
	static FfglOfxShellFactory* factory = new FfglOfxShellFactory(
		m.loaded ? m.identifier : "com.stoatworks.ffglshell.unconfigured",
		(unsigned)std::max( m.versionMajor, 1 ), (unsigned)std::max( m.versionMinor, 0 ) );
	ids.push_back( factory );
}
