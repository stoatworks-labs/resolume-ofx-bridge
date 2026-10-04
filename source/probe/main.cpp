//
// ofxprobe - load OFX bundles and dump what they contain.
//
// This is the introspection half of the generator, exposed as a CLI so that a
// plugin can be inspected (and a crash attributed) without going anywhere near
// Resolume.
//
//   ofxprobe                       scan the conventional OFX directories
//   ofxprobe --dir <path>          also scan <path> (repeatable)
//   ofxprobe --json                emit the manifest JSON for every plugin
//   ofxprobe --manifest <id>       emit the manifest for one plugin identifier
//   ofxprobe --render <id>         instantiate and render one frame (CPU)
//   ofxprobe --set name=value      set an OFX parameter before rendering (repeatable)
//   ofxprobe --edit name=value     like --set, but delivered as a user edit with
//                                  kOfxActionInstanceChanged, so param-driven
//                                  behaviour (presets) actually runs (repeatable)
//
// As a test host it adds, without changing what the flags above do:
// --no-system-dirs, --press, --in, --time, --seq, --seq-first, --frame-rate,
// --context, --from/--to, --seq-from/--seq-to, --transition,
// --transition-ramp, --key, --out-only, --depth, --temporal, --range,
// --identity, --frames-needed, --strict-frames, --batch and --quirks fusion.
// See docs/06-ofxprobe.md.
//

#include "../ofxbridge/Catalog.h"
#include "../ofxbridge/Host.h"

#include "ofxImageEffect.h"
#include "ofxParam.h"

#if defined( __APPLE__ )
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>
#define OFXHOST_HAS_IMAGEIO 1
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

/// "0.5" or "1,0.72,0.2" — multi-component params take a comma list.
std::vector< double > parseValues( const char* text )
{
	std::vector< double > values;
	const char* p = text;
	char* end     = nullptr;
	for( ;; )
	{
		values.push_back( strtod( p, &end ) );
		if( end == nullptr || *end != ',' )
			break;
		p = end + 1;
	}
	return values;
}

void usage()
{
	printf( "usage: ofxprobe [--dir PATH]... [--json] [--manifest IDENTIFIER] [--render IDENTIFIER]\n"
			"                [--set name=value]... [--set-string name=value]...\n                [--edit name=value]... [--size WxH] [--out FILE.bmp] [--quiet]\n" );
	printf( "  test host (see docs/06-ofxprobe.md):\n"
			"                [--no-system-dirs] [--context filter|transition|generator|general]\n"
			"                [--in FILE | --seq PATTERN [--seq-first N]] [--time T] [--frame-rate R]\n"
			"                [--from FILE|--seq-from PATTERN] [--to FILE|--seq-to PATTERN]\n"
			"                [--transition V | --transition-ramp FIRST:LAST] [--key name=t:v,t:v...]...\n"
			"                [--press NAME]... [--allow-link-press] [--out-only FILE.ppm|.bmp|.png]\n"
			"                [--depth byte|float] [--temporal 0|1] [--range FIRST:LAST]\n"
			"                [--identity] [--frames-needed] [--strict-frames] [--batch FILE]\n"
			"                [--quirks fusion]\n" );
}

void put32( std::vector< uint8_t >& v, uint32_t x )
{
	v.push_back( (uint8_t)( x ) );
	v.push_back( (uint8_t)( x >> 8 ) );
	v.push_back( (uint8_t)( x >> 16 ) );
	v.push_back( (uint8_t)( x >> 24 ) );
}

// ===========================================================================
// 8-bit RGBA images, rows bottom-up (the OFX convention), tightly packed.
// ===========================================================================

struct Rgba8
{
	int w = 0;
	int h = 0;
	std::vector< uint8_t > px;

	void allocate( int width, int height )
	{
		w = width;
		h = height;
		px.assign( (size_t)w * (size_t)h * 4u, 0 );
	}
	uint8_t* at( int x, int y )
	{
		return px.data() + ( (size_t)y * (size_t)w + (size_t)x ) * 4u;
	}
	const uint8_t* at( int x, int y ) const
	{
		return px.data() + ( (size_t)y * (size_t)w + (size_t)x ) * 4u;
	}
};

bool readFile( const std::string& path, std::vector< uint8_t >& bytes )
{
	FILE* f = fopen( path.c_str(), "rb" );
	if( f == nullptr )
		return false;
	fseek( f, 0, SEEK_END );
	const long n = ftell( f );
	fseek( f, 0, SEEK_SET );
	bytes.resize( n > 0 ? (size_t)n : 0u );
	const size_t got = bytes.empty() ? 0 : fread( bytes.data(), 1, bytes.size(), f );
	fclose( f );
	return got == bytes.size();
}

/// Binary PPM (P6) and PGM (P5), maxval up to 65535.
bool loadPnm( const std::vector< uint8_t >& b, Rgba8& img, std::string& err )
{
	size_t i         = 2;
	auto skipSpace = [ & ]() {
		for( ;; )
		{
			while( i < b.size() && isspace( b[ i ] ) )
				++i;
			if( i < b.size() && b[ i ] == '#' )
			{
				while( i < b.size() && b[ i ] != '\n' )
					++i;
				continue;
			}
			break;
		}
	};
	auto readInt = [ & ]( int& v ) {
		skipSpace();
		if( i >= b.size() || !isdigit( b[ i ] ) )
			return false;
		v = 0;
		while( i < b.size() && isdigit( b[ i ] ) )
			v = v * 10 + ( b[ i++ ] - '0' );
		return true;
	};
	const bool gray = b[ 1 ] == '5';
	int w = 0, h = 0, maxval = 0;
	if( !readInt( w ) || !readInt( h ) || !readInt( maxval ) || w < 1 || h < 1 || maxval < 1 || maxval > 65535 )
	{
		err = "bad PNM header";
		return false;
	}
	++i;// exactly one whitespace byte before the raster
	const int comps = gray ? 1 : 3;
	const int bps   = maxval > 255 ? 2 : 1;
	if( b.size() < i + (size_t)w * h * comps * bps )
	{
		err = "PNM raster is truncated";
		return false;
	}
	img.allocate( w, h );
	for( int y = 0; y < h; ++y )
	{
		const int dstY = h - 1 - y;// PNM is top-down
		for( int x = 0; x < w; ++x )
		{
			uint8_t c[ 3 ];
			for( int k = 0; k < comps; ++k )
			{
				const size_t o = i + ( ( (size_t)y * w + x ) * comps + k ) * bps;
				const int v    = bps == 2 ? ( b[ o ] << 8 ) | b[ o + 1 ] : b[ o ];
				c[ k ]         = (uint8_t)std::lround( v * 255.0 / maxval );
			}
			uint8_t* d = img.at( x, dstY );
			d[ 0 ]     = c[ 0 ];
			d[ 1 ]     = gray ? c[ 0 ] : c[ 1 ];
			d[ 2 ]     = gray ? c[ 0 ] : c[ 2 ];
			d[ 3 ]     = 255;
		}
	}
	return true;
}

uint32_t rd32( const std::vector< uint8_t >& b, size_t o )
{
	return (uint32_t)b[ o ] | ( (uint32_t)b[ o + 1 ] << 8 ) | ( (uint32_t)b[ o + 2 ] << 16 ) | ( (uint32_t)b[ o + 3 ] << 24 );
}
uint16_t rd16( const std::vector< uint8_t >& b, size_t o )
{
	return (uint16_t)( b[ o ] | ( b[ o + 1 ] << 8 ) );
}

int maskShift( uint32_t m )
{
	if( m == 0 )
		return 0;
	int s = 0;
	while( !( m & 1u ) )
	{
		m >>= 1;
		++s;
	}
	return s;
}
uint8_t maskValue( uint32_t px, uint32_t m )
{
	if( m == 0 )
		return 0;
	const int s        = maskShift( m );
	const uint32_t max = m >> s;
	return (uint8_t)( ( ( px & m ) >> s ) * 255u / max );
}

/// 24-bit and 32-bit uncompressed BMP (BI_RGB, BI_BITFIELDS), either row order.
bool loadBmp( const std::vector< uint8_t >& b, Rgba8& img, std::string& err )
{
	if( b.size() < 54 )
	{
		err = "BMP too short";
		return false;
	}
	const uint32_t offset  = rd32( b, 10 );
	const uint32_t hdr     = rd32( b, 14 );
	const int32_t w        = (int32_t)rd32( b, 18 );
	const int32_t hRaw     = (int32_t)rd32( b, 22 );
	const uint16_t bpp     = rd16( b, 28 );
	const uint32_t compr   = hdr >= 40 ? rd32( b, 30 ) : 0;
	const bool topDown     = hRaw < 0;
	const int h            = topDown ? -hRaw : hRaw;
	if( w < 1 || h < 1 || ( bpp != 24 && bpp != 32 ) || ( compr != 0 && compr != 3 && compr != 6 ) )
	{
		err = "unsupported BMP (need 24/32-bit uncompressed; got " + std::to_string( bpp ) + "-bit, compression " +
			  std::to_string( compr ) + ")";
		return false;
	}
	uint32_t rm = 0x00ff0000, gm = 0x0000ff00, bm = 0x000000ff, am = 0;
	if( bpp == 32 && ( compr == 3 || compr == 6 ) && b.size() >= 14 + 40 + 12 )
	{
		rm = rd32( b, 54 );
		gm = rd32( b, 58 );
		bm = rd32( b, 62 );
		am = ( compr == 6 || hdr >= 56 ) && b.size() >= 70 ? rd32( b, 66 ) : 0;
	}
	else if( bpp == 32 )
		am = 0xff000000;
	const size_t stride = ( (size_t)w * ( bpp / 8 ) + 3 ) & ~(size_t)3;
	if( b.size() < offset + stride * h )
	{
		err = "BMP raster is truncated";
		return false;
	}
	img.allocate( w, h );
	bool anyAlpha = false;
	for( int y = 0; y < h; ++y )
	{
		const int dstY   = topDown ? h - 1 - y : y;
		const uint8_t* r = b.data() + offset + stride * y;
		for( int x = 0; x < w; ++x )
		{
			uint8_t* d = img.at( x, dstY );
			if( bpp == 24 )
			{
				d[ 0 ] = r[ x * 3 + 2 ];
				d[ 1 ] = r[ x * 3 + 1 ];
				d[ 2 ] = r[ x * 3 + 0 ];
				d[ 3 ] = 255;
			}
			else
			{
				const uint32_t p = (uint32_t)r[ x * 4 ] | ( (uint32_t)r[ x * 4 + 1 ] << 8 ) |
								   ( (uint32_t)r[ x * 4 + 2 ] << 16 ) | ( (uint32_t)r[ x * 4 + 3 ] << 24 );
				d[ 0 ] = maskValue( p, rm );
				d[ 1 ] = maskValue( p, gm );
				d[ 2 ] = maskValue( p, bm );
				d[ 3 ] = am ? maskValue( p, am ) : 255;
				anyAlpha |= d[ 3 ] != 0;
			}
		}
	}
	// Plenty of writers leave a 32-bit BMP's fourth byte at zero; that means
	// "no alpha", not "transparent".
	if( bpp == 32 && !anyAlpha )
		for( size_t i = 3; i < img.px.size(); i += 4 )
			img.px[ i ] = 255;
	return true;
}

#if OFXHOST_HAS_IMAGEIO
CFURLRef fileUrl( const std::string& path )
{
	return CFURLCreateFromFileSystemRepresentation( nullptr, (const UInt8*)path.c_str(), (CFIndex)path.size(), false );
}

/// PNG/JPEG/TIFF/... through ImageIO. Drawn in the file's own RGB colour space
/// (no conversion); alpha comes back premultiplied, as CoreGraphics requires.
bool loadImageIO( const std::string& path, Rgba8& img, std::string& err )
{
	CFURLRef url = fileUrl( path );
	CGImageSourceRef src = url ? CGImageSourceCreateWithURL( url, nullptr ) : nullptr;
	if( url )
		CFRelease( url );
	CGImageRef cg = src ? CGImageSourceCreateImageAtIndex( src, 0, nullptr ) : nullptr;
	if( src )
		CFRelease( src );
	if( cg == nullptr )
	{
		err = "not a readable image (PPM P6/P5, BMP 24/32, or anything ImageIO opens)";
		return false;
	}
	const int w             = (int)CGImageGetWidth( cg );
	const int h             = (int)CGImageGetHeight( cg );
	CGColorSpaceRef own     = CGImageGetColorSpace( cg );
	const bool ownIsRgb     = own && CGColorSpaceGetModel( own ) == kCGColorSpaceModelRGB;
	CGColorSpaceRef cs      = ownIsRgb ? CGColorSpaceRetain( own ) : CGColorSpaceCreateWithName( kCGColorSpaceSRGB );
	std::vector< uint8_t > top( (size_t)w * h * 4, 0 );
	CGContextRef ctx = CGBitmapContextCreate( top.data(), (size_t)w, (size_t)h, 8, (size_t)w * 4, cs,
											  kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big );
	CGColorSpaceRelease( cs );
	if( ctx == nullptr )
	{
		CGImageRelease( cg );
		err = "could not create a bitmap context";
		return false;
	}
	CGContextSetBlendMode( ctx, kCGBlendModeCopy );
	CGContextDrawImage( ctx, CGRectMake( 0, 0, w, h ), cg );
	CGContextRelease( ctx );
	CGImageRelease( cg );
	img.allocate( w, h );
	for( int y = 0; y < h; ++y )
		memcpy( img.at( 0, h - 1 - y ), top.data() + (size_t)y * w * 4, (size_t)w * 4 );
	return true;
}

/// The RGBA bytes exactly as they are (premultiplied data written as straight
/// alpha -- byte-faithful rather than "correct" for a viewer).
bool writePng( const std::string& path, const Rgba8& img )
{
	std::vector< uint8_t > top( img.px.size() );
	for( int y = 0; y < img.h; ++y )
		memcpy( top.data() + (size_t)y * img.w * 4, img.at( 0, img.h - 1 - y ), (size_t)img.w * 4 );
	CGColorSpaceRef cs  = CGColorSpaceCreateWithName( kCGColorSpaceSRGB );
	CGDataProviderRef dp = CGDataProviderCreateWithData( nullptr, top.data(), top.size(), nullptr );
	CGImageRef cg = CGImageCreate( (size_t)img.w, (size_t)img.h, 8, 32, (size_t)img.w * 4, cs,
								   kCGImageAlphaLast | kCGBitmapByteOrder32Big, dp, nullptr, false,
								   kCGRenderingIntentDefault );
	CGDataProviderRelease( dp );
	CGColorSpaceRelease( cs );
	CFURLRef url = fileUrl( path );
	CGImageDestinationRef dst = url ? CGImageDestinationCreateWithURL( url, CFSTR( "public.png" ), 1, nullptr ) : nullptr;
	if( url )
		CFRelease( url );
	bool ok = false;
	if( dst && cg )
	{
		CGImageDestinationAddImage( dst, cg, nullptr );
		ok = CGImageDestinationFinalize( dst );
	}
	if( dst )
		CFRelease( dst );
	if( cg )
		CGImageRelease( cg );
	return ok;
}
#endif

bool loadImage( const std::string& path, Rgba8& img, std::string& err )
{
	std::vector< uint8_t > b;
	if( !readFile( path, b ) )
	{
		err = "cannot read " + path;
		return false;
	}
	bool ok = false;
	if( b.size() >= 2 && b[ 0 ] == 'P' && ( b[ 1 ] == '6' || b[ 1 ] == '5' ) )
		ok = loadPnm( b, img, err );
	else if( b.size() >= 2 && b[ 0 ] == 'B' && b[ 1 ] == 'M' )
		ok = loadBmp( b, img, err );
	else
	{
#if OFXHOST_HAS_IMAGEIO
		ok = loadImageIO( path, img, err );
#else
		err = "unsupported image format (need binary PPM P6/P5 or 24/32-bit BMP)";
#endif
	}
	if( !ok )
		err = path + ": " + err;
	return ok;
}

bool writePpm( const std::string& path, const Rgba8& img )
{
	FILE* f = fopen( path.c_str(), "wb" );
	if( f == nullptr )
		return false;
	fprintf( f, "P6\n%d %d\n255\n", img.w, img.h );
	std::vector< uint8_t > row( (size_t)img.w * 3 );
	for( int y = img.h - 1; y >= 0; --y )
	{
		for( int x = 0; x < img.w; ++x )
		{
			const uint8_t* s = img.at( x, y );
			row[ x * 3 + 0 ] = s[ 0 ];
			row[ x * 3 + 1 ] = s[ 1 ];
			row[ x * 3 + 2 ] = s[ 2 ];
		}
		fwrite( row.data(), 1, row.size(), f );
	}
	fclose( f );
	return true;
}

/// N images side by side as a 24-bit BMP, `gap` columns of grey 24 between.
/// With two panels this is byte-for-byte the original --out layout.
bool writeBmpPanels( const std::string& path, const std::vector< const Rgba8* >& panels, int width, int height )
{
	const int gap    = 8;
	const int n      = (int)panels.size();
	const int outW   = width * n + gap * ( n - 1 );
	const int stride = ( outW * 3 + 3 ) & ~3;

	std::vector< uint8_t > pixels( (size_t)stride * height, 24 );
	for( int y = 0; y < height; ++y )
	{
		uint8_t* row = pixels.data() + (size_t)y * stride;
		for( int p = 0; p < n; ++p )
		{
			const Rgba8* img = panels[ p ];
			if( img == nullptr || img->w != width || img->h != height )
				continue;// leave a missing panel grey
			for( int x = 0; x < width; ++x )
			{
				const uint8_t* s = img->at( x, y );
				uint8_t* d       = row + (size_t)( x + p * ( width + gap ) ) * 3;
				d[ 0 ]           = s[ 2 ];
				d[ 1 ]           = s[ 1 ];
				d[ 2 ]           = s[ 0 ];
			}
		}
	}

	std::vector< uint8_t > header;
	header.push_back( 'B' );
	header.push_back( 'M' );
	put32( header, (uint32_t)( 14 + 40 + pixels.size() ) );
	put32( header, 0 );
	put32( header, 14 + 40 );
	put32( header, 40 );
	put32( header, (uint32_t)outW );
	put32( header, (uint32_t)height );
	header.push_back( 1 );
	header.push_back( 0 );// planes
	header.push_back( 24 );
	header.push_back( 0 );// bpp
	put32( header, 0 );   // BI_RGB
	put32( header, (uint32_t)pixels.size() );
	put32( header, 2835 );
	put32( header, 2835 );
	put32( header, 0 );
	put32( header, 0 );

	FILE* f = fopen( path.c_str(), "wb" );
	if( f == nullptr )
		return false;
	fwrite( header.data(), 1, header.size(), f );
	fwrite( pixels.data(), 1, pixels.size(), f );
	fclose( f );
	return true;
}

std::string lowerExt( const std::string& path )
{
	const size_t dot   = path.find_last_of( '.' );
	const size_t slash = path.find_last_of( "/\\" );
	if( dot == std::string::npos || ( slash != std::string::npos && dot < slash ) )
		return "";
	std::string e = path.substr( dot + 1 );
	for( auto& c : e )
		c = (char)tolower( c );
	return e;
}

/// --out-only: PPM P6 by default, BMP for .bmp, PNG for .png (ImageIO).
bool writeImage( const std::string& path, const Rgba8& img, std::string& err )
{
	const std::string e = lowerExt( path );
	bool ok             = false;
	if( e == "bmp" )
		ok = writeBmpPanels( path, { &img }, img.w, img.h );
	else if( e == "png" )
	{
#if OFXHOST_HAS_IMAGEIO
		ok = writePng( path, img );
#else
		err = "PNG output needs ImageIO (macOS)";
		return false;
#endif
	}
	else
		ok = writePpm( path, img );
	if( !ok )
		err = "could not write " + path;
	return ok;
}

Rgba8 resizeNearest( const Rgba8& src, int w, int h )
{
	if( src.w == w && src.h == h )
		return src;
	Rgba8 dst;
	dst.allocate( w, h );
	for( int y = 0; y < h; ++y )
	{
		const int sy = std::min( src.h - 1, (int)( ( y + 0.5 ) * src.h / h ) );
		for( int x = 0; x < w; ++x )
		{
			const int sx = std::min( src.w - 1, (int)( ( x + 0.5 ) * src.w / w ) );
			memcpy( dst.at( x, y ), src.at( sx, sy ), 4 );
		}
	}
	return dst;
}

void toFrame( const Rgba8& img, ofxbridge::Frame& f, bool asFloat )
{
	f.allocate( img.w, img.h, asFloat );
	for( int y = 0; y < img.h; ++y )
	{
		uint8_t* row = f.data.data() + (size_t)y * f.rowBytes;
		if( !asFloat )
			memcpy( row, img.at( 0, y ), (size_t)img.w * 4 );
		else
		{
			float* d         = reinterpret_cast< float* >( row );
			const uint8_t* s = img.at( 0, y );
			for( int i = 0; i < img.w * 4; ++i )
				d[ i ] = s[ i ] / 255.0f;
		}
	}
}

Rgba8 fromFrame( const ofxbridge::Frame& f )
{
	Rgba8 img;
	img.allocate( f.width, f.height );
	for( int y = 0; y < f.height; ++y )
	{
		const uint8_t* row = f.data.data() + (size_t)y * f.rowBytes;
		if( !f.isFloat() )
			memcpy( img.at( 0, y ), row, (size_t)f.width * 4 );
		else
		{
			const float* s = reinterpret_cast< const float* >( row );
			uint8_t* d     = img.at( 0, y );
			for( int i = 0; i < f.width * 4; ++i )
			{
				const float v = std::isfinite( s[ i ] ) ? std::min( 1.0f, std::max( 0.0f, s[ i ] ) ) : 0.0f;
				d[ i ]        = (uint8_t)std::lround( v * 255.0f );
			}
		}
	}
	return img;
}

/// The original probe's horizontal ramp, so a wrong stride or row order is
/// visible rather than averaging out to something plausible.
Rgba8 makeRamp( int width, int height )
{
	Rgba8 img;
	img.allocate( width, height );
	for( int y = 0; y < height; ++y )
		for( int x = 0; x < width; ++x )
		{
			uint8_t* px = img.at( x, y );
			px[ 0 ]     = (uint8_t)( x * 4 );
			px[ 1 ]     = (uint8_t)( y * 8 );
			px[ 2 ]     = 128;
			px[ 3 ]     = 255;
		}
	return img;
}

/// Default SourceTo when none is given: an 8px checkerboard.
Rgba8 makeChecker( int width, int height )
{
	Rgba8 img;
	img.allocate( width, height );
	for( int y = 0; y < height; ++y )
		for( int x = 0; x < width; ++x )
		{
			uint8_t* px     = img.at( x, y );
			const uint8_t v = ( ( x / 8 ) + ( y / 8 ) ) % 2 ? 220 : 40;
			px[ 0 ]         = v;
			px[ 1 ]         = v;
			px[ 2 ]         = (uint8_t)( 255 - v );
			px[ 3 ]         = 255;
		}
	return img;
}

uint64_t fnv1a( const std::vector< uint8_t >& v )
{
	uint64_t h = 1469598103934665603ull;
	for( uint8_t b : v )
	{
		h ^= b;
		h *= 1099511628211ull;
	}
	return h;
}

bool fileExists( const std::string& p )
{
	FILE* f = fopen( p.c_str(), "rb" );
	if( f )
		fclose( f );
	return f != nullptr;
}

std::string formatPattern( const std::string& pattern, int n )
{
	char buf[ 4096 ];
	snprintf( buf, sizeof( buf ), pattern.c_str(), n );
	return buf;
}

// ===========================================================================
// Clip sources
// ===========================================================================

/// One image for every time. Its range is the effect's timeline.
class StillSource : public ofxbridge::FrameSource
{
public:
	StillSource( const Rgba8& img, bool asFloat, double first, double last, std::string label ) :
		_first( first ), _last( last ), _label( std::move( label ) )
	{
		toFrame( img, _frame, asFloat );
	}
	ofxbridge::Frame* frameAt( double ) override
	{
		return &_frame;
	}
	void frameRange( double& first, double& last ) const override
	{
		first = _first;
		last  = _last;
	}
	std::string frameId( double ) const override
	{
		return _label;
	}

private:
	ofxbridge::Frame _frame;
	double _first, _last;
	std::string _label;
};

/// A printf-pattern image sequence: frame time N shows file N. Times outside
/// [first, last] have no image. Non-integer times round to the nearest frame.
class SeqSource : public ofxbridge::FrameSource
{
public:
	SeqSource( std::string pattern, int first, int last, int w, int h, bool asFloat ) :
		_pattern( std::move( pattern ) ), _first( first ), _last( last ), _w( w ), _h( h ), _float( asFloat )
	{
	}

	ofxbridge::Frame* frameAt( double time ) override
	{
		const int n = (int)std::floor( time + 0.5 );
		if( n < _first || n > _last )
			return nullptr;
		std::lock_guard< std::mutex > lock( _mutex );
		auto it = _cache.find( n );
		if( it != _cache.end() )
			return it->second.get();
		Rgba8 img;
		std::string err;
		if( !loadImage( formatPattern( _pattern, n ), img, err ) )
		{
			fprintf( stderr, "  WARNING: sequence frame %d unreadable: %s\n", n, err.c_str() );
			return nullptr;
		}
		auto f = std::make_unique< ofxbridge::Frame >();
		toFrame( resizeNearest( img, _w, _h ), *f, _float );
		ofxbridge::Frame* raw = f.get();
		_cache.emplace( n, std::move( f ) );
		++_loads;
		return raw;
	}
	void frameRange( double& first, double& last ) const override
	{
		first = _first;
		last  = _last;
	}
	std::string frameId( double time ) const override
	{
		return "frame" + std::to_string( (int)std::floor( time + 0.5 ) );
	}
	int first() const
	{
		return _first;
	}
	int last() const
	{
		return _last;
	}

private:
	std::string _pattern;
	int _first, _last, _w, _h;
	bool _float;
	std::mutex _mutex;
	std::map< int, std::unique_ptr< ofxbridge::Frame > > _cache;
	int _loads = 0;
};

/// --strict-frames: answers only for times inside the ranges the plugin asked
/// for in getFramesNeeded (plus the render time itself), as a host that
/// prefetches exactly what was declared would. Anything else is a null image
/// and a warning, so an under-declared getFramesNeeded shows up.
class DeclaredOnlySource : public ofxbridge::FrameSource
{
public:
	DeclaredOnlySource( ofxbridge::FrameSource* inner, std::string clip, double renderTime,
						std::vector< OfxRangeD > ranges ) :
		_inner( inner ), _clip( std::move( clip ) ), _t( renderTime ), _ranges( std::move( ranges ) )
	{
	}
	ofxbridge::Frame* frameAt( double time ) override
	{
		const double eps = 1e-6;
		bool allowed     = std::fabs( time - _t ) < eps;
		for( const auto& r : _ranges )
			allowed |= time >= r.min - eps && time <= r.max + eps;
		if( !allowed )
		{
			std::lock_guard< std::mutex > lock( _mutex );
			if( _refused.insert( time ).second )
				printf( "  STRICT: %s fetched at t=%g, outside its declared frames needed -> no image\n",
						_clip.c_str(), time );
			return nullptr;
		}
		return _inner->frameAt( time );
	}
	void frameRange( double& first, double& last ) const override
	{
		_inner->frameRange( first, last );
	}
	std::string frameId( double time ) const override
	{
		return _inner->frameId( time );
	}
	size_t refusals() const
	{
		return _refused.size();
	}

private:
	ofxbridge::FrameSource* _inner;
	std::string _clip;
	double _t;
	std::vector< OfxRangeD > _ranges;
	std::mutex _mutex;
	std::set< double > _refused;
};

/// Find a sequence's frame range: from `first` (or 0, then 1, when unset)
/// while consecutive files exist.
bool probeSequence( const std::string& pattern, int firstHint, int& first, int& last, std::string& err )
{
	if( pattern.find( '%' ) == std::string::npos )
	{
		err = "--seq pattern '" + pattern + "' has no printf field (e.g. frames/f%04d.ppm)";
		return false;
	}
	if( firstHint >= 0 )
		first = firstHint;
	else if( fileExists( formatPattern( pattern, 0 ) ) )
		first = 0;
	else
		first = 1;
	if( !fileExists( formatPattern( pattern, first ) ) )
	{
		err = "sequence '" + pattern + "' has no frame " + std::to_string( first ) + " (" +
			  formatPattern( pattern, first ) + ")";
		return false;
	}
	last = first;
	while( last - first < 1000000 && fileExists( formatPattern( pattern, last + 1 ) ) )
		++last;
	return true;
}

// ===========================================================================
// Options
// ===========================================================================

using NumList   = std::vector< std::pair< std::string, std::vector< double > > >;
using KeyList   = std::vector< std::pair< std::string, std::vector< ofxbridge::ParamKey > > >;
using StrList   = std::vector< std::pair< std::string, std::string > >;

/// Everything that may differ per render (one command line, or one --batch line).
struct Job
{
	// sources ("" = unset)
	std::string in, seq;
	std::string from, seqFrom;
	std::string to, seqTo;
	int seqFirst = -1;
	bool haveSeqFirst = false;

	bool haveTime = false;
	double time   = 0.0;
	bool haveSize = false;
	int width     = 0;
	int height    = 0;

	std::string out, outOnly;

	// actions, applied before this render
	StrList strings;
	NumList sets;
	NumList edits;
	KeyList keys;
	std::vector< std::string > presses;
	bool haveTransition = false;
	double transition   = 0.0;
	bool haveRamp       = false;
	double rampFirst = 0.0, rampLast = 0.0;

	bool hasActions() const
	{
		return !strings.empty() || !sets.empty() || !edits.empty() || !keys.empty() || !presses.empty() ||
			   haveTransition || haveRamp;
	}
	bool usesSequences() const
	{
		return !seq.empty() || !seqFrom.empty() || !seqTo.empty();
	}
};

bool splitKv( const std::string& kv, std::string& k, std::string& v )
{
	const size_t eq = kv.find( '=' );
	if( eq == std::string::npos || eq == 0 )
		return false;
	k = kv.substr( 0, eq );
	v = kv.substr( eq + 1 );
	return true;
}

bool parseNumber( const std::string& s, double& v )
{
	char* end = nullptr;
	v         = strtod( s.c_str(), &end );
	return !s.empty() && end && *end == '\0';
}

bool parseRange( const std::string& s, double& a, double& b )
{
	const size_t c = s.find( ':' );
	return c != std::string::npos && parseNumber( s.substr( 0, c ), a ) && parseNumber( s.substr( c + 1 ), b );
}

/// "t:v,t:v" with "/" between components: "0:0.5/0.5,10:0.2/0.8".
bool parseKeys( const std::string& s, std::vector< ofxbridge::ParamKey >& keys, std::string& err )
{
	std::stringstream ss( s );
	std::string item;
	while( std::getline( ss, item, ',' ) )
	{
		const size_t c = item.find( ':' );
		double t       = 0.0;
		if( c == std::string::npos || !parseNumber( item.substr( 0, c ), t ) )
		{
			err = "key '" + item + "' is not time:value";
			return false;
		}
		std::vector< double > vals;
		std::stringstream vs( item.substr( c + 1 ) );
		std::string comp;
		while( std::getline( vs, comp, '/' ) )
		{
			double v = 0.0;
			if( !parseNumber( comp, v ) )
			{
				err = "key value '" + item.substr( c + 1 ) + "' is not numeric";
				return false;
			}
			vals.push_back( v );
		}
		keys.emplace_back( t, vals );
	}
	if( keys.empty() )
	{
		err = "no keys in '" + s + "'";
		return false;
	}
	return true;
}

/// Parse one per-render flag at args[i]. Returns 1 if consumed (advancing i
/// past any value), 0 if it is not a per-render flag, -1 on a bad value.
int parseJobArg( const std::vector< std::string >& args, size_t& i, Job& job, std::string& err )
{
	const std::string& a = args[ i ];
	auto value = [ & ]( std::string& v ) {
		if( i + 1 >= args.size() )
		{
			err = a + " needs a value";
			return false;
		}
		v = args[ ++i ];
		return true;
	};
	std::string v;

	if( a == "--set" || a == "--edit" || a == "--set-string" )
	{
		if( !value( v ) )
			return -1;
		std::string k, val;
		if( !splitKv( v, k, val ) )
		{
			err = a + " expects name=value, got '" + v + "'";
			return -1;
		}
		if( a == "--set" )
			job.sets.emplace_back( k, parseValues( val.c_str() ) );
		else if( a == "--edit" )
			job.edits.emplace_back( k, parseValues( val.c_str() ) );
		else
			job.strings.emplace_back( k, val );
		return 1;
	}
	if( a == "--size" )
	{
		if( !value( v ) )
			return -1;
		if( v.find( 'x' ) == std::string::npos || sscanf( v.c_str(), "%dx%d", &job.width, &job.height ) != 2 ||
			job.width < 1 || job.height < 1 )
		{
			err = "--size expects WxH, got '" + v + "'";
			return -1;
		}
		job.haveSize = true;
		return 1;
	}
	if( a == "--out" )
		return value( job.out ) ? 1 : -1;
	if( a == "--out-only" )
		return value( job.outOnly ) ? 1 : -1;
	if( a == "--in" )
		return value( job.in ) ? 1 : -1;
	if( a == "--seq" )
		return value( job.seq ) ? 1 : -1;
	if( a == "--from" )
		return value( job.from ) ? 1 : -1;
	if( a == "--to" )
		return value( job.to ) ? 1 : -1;
	if( a == "--seq-from" )
		return value( job.seqFrom ) ? 1 : -1;
	if( a == "--seq-to" )
		return value( job.seqTo ) ? 1 : -1;
	if( a == "--seq-first" )
	{
		double n = 0;
		if( !value( v ) || !parseNumber( v, n ) || n < 0 )
		{
			err = "--seq-first expects a frame number >= 0";
			return -1;
		}
		job.seqFirst     = (int)n;
		job.haveSeqFirst = true;
		return 1;
	}
	if( a == "--time" )
	{
		if( !value( v ) || !parseNumber( v, job.time ) )
		{
			err = "--time expects a number (frames)";
			return -1;
		}
		job.haveTime = true;
		return 1;
	}
	if( a == "--transition" )
	{
		if( !value( v ) || !parseNumber( v, job.transition ) )
		{
			err = "--transition expects a number";
			return -1;
		}
		job.haveTransition = true;
		return 1;
	}
	if( a == "--transition-ramp" )
	{
		if( !value( v ) || !parseRange( v, job.rampFirst, job.rampLast ) || job.rampLast == job.rampFirst )
		{
			err = "--transition-ramp expects FIRST:LAST frames (different), e.g. 0:24";
			return -1;
		}
		job.haveRamp = true;
		return 1;
	}
	if( a == "--key" )
	{
		if( !value( v ) )
			return -1;
		std::string k, val;
		if( !splitKv( v, k, val ) )
		{
			err = "--key expects name=time:value,time:value,...";
			return -1;
		}
		std::vector< ofxbridge::ParamKey > keys;
		if( !parseKeys( val, keys, err ) )
			return -1;
		job.keys.emplace_back( k, keys );
		return 1;
	}
	if( a == "--press" )
	{
		if( !value( v ) )
			return -1;
		job.presses.push_back( v );
		return 1;
	}
	return 0;
}

/// Process-wide options: what is loaded, and how the host presents itself.
struct Options
{
	std::vector< std::string > dirs;
	bool noSystemDirs = false;
	bool wantJson     = false;
	bool quiet        = false;
	std::string manifestFor;
	std::string renderFor;
	std::string context = kOfxImageEffectContextFilter;
	int temporal        = -1;// -1 = auto: on when any sequence is used
	// 60, what ofxprobe and the bridge have always reported: fleet checks
	// compare a probe render against an FFGL render on a 60 fps clock.
	double frameRate    = 60.0;
	std::string depth   = "byte";
	bool haveRange      = false;
	double rangeFirst = 0.0, rangeLast = 0.0;
	bool identity       = false;
	bool framesNeeded   = false;
	bool strictFrames   = false;
	bool fusionQuirks   = false;
	bool allowLinkPress = false;
	std::string batchFile;
};

/// Split a batch line into tokens: whitespace-separated, "..." and '...'
/// quoting, # starts a comment.
std::vector< std::string > tokenize( const std::string& line )
{
	std::vector< std::string > out;
	std::string cur;
	bool inTok = false;
	char quote = 0;
	for( size_t i = 0; i < line.size(); ++i )
	{
		const char c = line[ i ];
		if( quote )
		{
			if( c == quote )
				quote = 0;
			else
				cur += c;
			continue;
		}
		if( c == '"' || c == '\'' )
		{
			quote = c;
			inTok = true;
			continue;
		}
		if( c == '#' && !inTok )
			break;
		if( isspace( (unsigned char)c ) )
		{
			if( inTok )
			{
				out.push_back( cur );
				cur.clear();
				inTok = false;
			}
			continue;
		}
		cur += c;
		inTok = true;
	}
	if( inTok )
		out.push_back( cur );
	return out;
}

/// A batch line starts from the command line's sources, size and time, and
/// overrides them; outputs and actions belong to the line alone.
Job inheritJob( const Job& base, const Job& line )
{
	Job j = line;
	if( line.in.empty() && line.seq.empty() )
	{
		j.in  = base.in;
		j.seq = base.seq;
	}
	if( line.from.empty() && line.seqFrom.empty() )
	{
		j.from    = base.from;
		j.seqFrom = base.seqFrom;
	}
	if( line.to.empty() && line.seqTo.empty() )
	{
		j.to    = base.to;
		j.seqTo = base.seqTo;
	}
	if( !line.haveSeqFirst )
	{
		j.seqFirst     = base.seqFirst;
		j.haveSeqFirst = base.haveSeqFirst;
	}
	if( !line.haveTime )
	{
		j.time     = base.time;
		j.haveTime = base.haveTime;
	}
	if( !line.haveSize )
	{
		j.width    = base.width;
		j.height   = base.height;
		j.haveSize = base.haveSize;
	}
	return j;
}

// ===========================================================================
// Rendering
// ===========================================================================

std::string contextFromFlag( const std::string& v )
{
	if( v == "filter" )
		return kOfxImageEffectContextFilter;
	if( v == "transition" )
		return kOfxImageEffectContextTransition;
	if( v == "generator" )
		return kOfxImageEffectContextGenerator;
	if( v == "general" )
		return kOfxImageEffectContextGeneral;
	return "";
}

std::string shortContext( const std::string& c )
{
	const std::string prefix = "OfxImageEffectContext";
	return c.compare( 0, prefix.size(), prefix ) == 0 ? c.substr( prefix.size() ) : c;
}

/// The input clips a context feeds, in display order, with the job field for
/// each: still path, sequence pattern.
struct InputSpec
{
	std::string clip;
	std::string still;
	std::string seq;
	bool isTransitionTo = false;
};

std::vector< InputSpec > inputsFor( const std::string& context, const Job& job )
{
	std::vector< InputSpec > v;
	if( context == kOfxImageEffectContextFilter || context == kOfxImageEffectContextGeneral )
		v.push_back( { kOfxImageEffectSimpleSourceClipName, job.in, job.seq } );
	else if( context == kOfxImageEffectContextTransition )
	{
		v.push_back( { kOfxImageEffectTransitionSourceFromClipName, job.from, job.seqFrom } );
		v.push_back( { kOfxImageEffectTransitionSourceToClipName, job.to, job.seqTo, true } );
	}
	return v;
}

class Session
{
public:
	Session( const Options& opt ) : _opt( opt )
	{
	}

	int run( const ofxbridge::PluginDesc& target, Job base, std::vector< Job > batch );

private:
	struct Bound
	{
		std::string key;
		ofxbridge::FrameSource* source = nullptr;
	};

	bool resolveSize( const Job& job, int& w, int& h, std::string& err );
	ofxbridge::FrameSource* sourceFor( const InputSpec& in, const Job& job, int w, int h, std::string& err );
	bool applyActions( const Job& job, bool setup );
	bool renderJob( const Job& job, int index, int total );

	const Options& _opt;
	ofxbridge::Host _host;
	std::unique_ptr< ofxbridge::Effect > _effect;
	std::string _identifier;
	bool _float = false;
	double _tlFirst = 0.0, _tlLast = 0.0;
	std::map< std::string, std::unique_ptr< ofxbridge::FrameSource > > _sources;
	std::map< std::string, Bound > _bound;// clip -> current source
	std::vector< std::string > _clipNames;
	bool _warnedGeneratorSize = false;
};

bool Session::resolveSize( const Job& job, int& w, int& h, std::string& err )
{
	if( job.haveSize )
	{
		w = job.width;
		h = job.height;
		return true;
	}
	// The first input that names a file or sequence decides; otherwise the
	// original probe's 64x32.
	for( const InputSpec& in : inputsFor( _opt.context, job ) )
	{
		std::string path = in.still;
		if( path.empty() && !in.seq.empty() )
		{
			int first = 0, last = 0;
			if( !probeSequence( in.seq, job.haveSeqFirst ? job.seqFirst : -1, first, last, err ) )
				return false;
			path = formatPattern( in.seq, first );
		}
		if( path.empty() )
			continue;
		Rgba8 img;
		if( !loadImage( path, img, err ) )
			return false;
		w = img.w;
		h = img.h;
		return true;
	}
	if( _opt.context == kOfxImageEffectContextGenerator )
	{
		w = 64;
		h = 32;
		if( !_warnedGeneratorSize )
			fprintf( stderr, "  NOTE: generator context without --size; rendering the default 64x32\n" );
		_warnedGeneratorSize = true;
		return true;
	}
	w = 64;
	h = 32;
	return true;
}

ofxbridge::FrameSource* Session::sourceFor( const InputSpec& in, const Job& job, int w, int h, std::string& err )
{
	std::ostringstream key;
	key << w << "x" << h << ( _float ? "f" : "b" ) << "|";
	if( !in.seq.empty() )
		key << "seq|" << in.seq << "|" << ( job.haveSeqFirst ? job.seqFirst : -1 );
	else if( !in.still.empty() )
		key << "still|" << in.still;
	else
		key << "synth|" << ( in.isTransitionTo ? "checker" : "ramp" );

	auto it = _sources.find( key.str() );
	if( it != _sources.end() )
		return it->second.get();

	std::unique_ptr< ofxbridge::FrameSource > src;
	if( !in.seq.empty() )
	{
		int first = 0, last = 0;
		if( !probeSequence( in.seq, job.haveSeqFirst ? job.seqFirst : -1, first, last, err ) )
			return nullptr;
		src = std::make_unique< SeqSource >( in.seq, first, last, w, h, _float );
		printf( "  clip %s <- sequence %s, frames %d..%d\n", in.clip.c_str(), in.seq.c_str(), first, last );
	}
	else if( !in.still.empty() )
	{
		Rgba8 img;
		if( !loadImage( in.still, img, err ) )
			return nullptr;
		if( img.w != w || img.h != h )
			printf( "  NOTE: %s is %dx%d; resampled (nearest) to %dx%d\n", in.still.c_str(), img.w, img.h, w, h );
		src = std::make_unique< StillSource >( resizeNearest( img, w, h ), _float, _tlFirst, _tlLast, "still" );
	}
	else
	{
		src = std::make_unique< StillSource >( in.isTransitionTo ? makeChecker( w, h ) : makeRamp( w, h ), _float,
											   _tlFirst, _tlLast, in.isTransitionTo ? "checker" : "ramp" );
	}
	ofxbridge::FrameSource* raw = src.get();
	_sources.emplace( key.str(), std::move( src ) );
	return raw;
}

bool Session::applyActions( const Job& job, bool setup )
{
	ofxbridge::Effect& effect = *_effect;
	bool ok                   = true;
	auto printValues = []( const std::vector< double >& v ) {
		for( size_t i = 0; i < v.size(); ++i )
			printf( "%s%g", i ? "," : "", v[ i ] );
	};
	(void)setup;

	// String parameters first: a plugin that loads a file off one of these
	// generally needs it before anything numeric means very much.
	for( const auto& kv : job.strings )
	{
		if( effect.setParamString( kv.first, kv.second ) )
			printf( "  set %s = \"%s\"\n", kv.first.c_str(), kv.second.c_str() );
		else
			fprintf( stderr, "  WARNING: no string parameter named '%s'\n", kv.first.c_str() );
	}

	for( const auto& kv : job.sets )
	{
		if( effect.setParamValue( kv.first, kv.second ) )
		{
			printf( "  set %s = ", kv.first.c_str() );
			printValues( kv.second );
			printf( "\n" );
		}
		else
			fprintf( stderr, "  WARNING: no numeric parameter named '%s'\n", kv.first.c_str() );
	}

	// The host drives a transition's own param.
	const bool isTransition = _opt.context == kOfxImageEffectContextTransition;
	if( job.haveTransition || job.haveRamp )
	{
		const char* tp = kOfxImageEffectTransitionParamName;
		if( effect.getParam( tp ) == nullptr )
		{
			fprintf( stderr, "  WARNING: the plugin has no '%s' param%s\n", tp,
					 isTransition ? "" : " (it is only defined in the Transition context)" );
			ok = false;
		}
		else if( job.haveTransition )
		{
			effect.setParamValue( tp, { job.transition } );
			printf( "  set %s = %g (constant)\n", tp, job.transition );
		}
		else
		{
			std::string err;
			if( effect.setParamKeys( tp, { { job.rampFirst, { 0.0 } }, { job.rampLast, { 1.0 } } }, err ) )
				printf( "  key %s: 0 at frame %g -> 1 at frame %g (linear, held outside)\n", tp, job.rampFirst,
						job.rampLast );
			else
			{
				fprintf( stderr, "  WARNING: %s\n", err.c_str() );
				ok = false;
			}
		}
	}

	for( const auto& kv : job.keys )
	{
		std::string err;
		if( effect.setParamKeys( kv.first, kv.second, err ) )
		{
			printf( "  key %s:", kv.first.c_str() );
			for( const auto& k : kv.second )
			{
				printf( " %g->", k.first );
				printValues( k.second );
			}
			printf( "\n" );
		}
		else
		{
			fprintf( stderr, "  WARNING: %s\n", err.c_str() );
			ok = false;
		}
	}

	for( const auto& kv : job.edits )
	{
		if( effect.editParamValue( kv.first, kv.second ) )
		{
			printf( "  edit %s = ", kv.first.c_str() );
			printValues( kv.second );
			printf( "\n" );
		}
		else
			fprintf( stderr, "  WARNING: no numeric parameter named '%s'\n", kv.first.c_str() );
	}

	for( const std::string& name : job.presses )
	{
		OFX::Host::Param::Instance* p = effect.getParam( name );
		if( p == nullptr )
		{
			fprintf( stderr, "  WARNING: --press: no parameter named '%s'\n", name.c_str() );
			ok = false;
			continue;
		}
		const std::string parent = p->getProperties().getStringProperty( kOfxParamPropParent );
		const bool aboutLink = name.rfind( "stoatworksAboutLink", 0 ) == 0 || parent == "stoatworksAbout";
		if( aboutLink && !_opt.allowLinkPress )
		{
			fprintf( stderr,
					 "  REFUSED: --press %s is a Stoatworks About link (pressing it opens a browser). "
					 "Pass --allow-link-press to really press it.\n",
					 name.c_str() );
			ok = false;
			continue;
		}
		if( p->getType() != kOfxParamTypePushButton )
			printf( "  NOTE: %s is a %s, not a push button; delivering InstanceChanged anyway\n", name.c_str(),
					p->getType().c_str() );
		OfxStatus st = kOfxStatOK;
		std::string err;
		if( effect.pressParam( name, st, err ) )
			printf( "  press %s at t=%g -> %s\n", name.c_str(), effect.paramTime(),
					st == kOfxStatOK ? "kOfxStatOK" : st == kOfxStatReplyDefault ? "kOfxStatReplyDefault" : "failed" );
		else
		{
			fprintf( stderr, "  WARNING: %s\n", err.c_str() );
			ok = false;
		}
	}
	return ok;
}

bool Session::renderJob( const Job& job, int index, int total )
{
	ofxbridge::Effect& effect = *_effect;
	std::string err;
	int w = 0, h = 0;
	if( !resolveSize( job, w, h, err ) )
	{
		fprintf( stderr, "%s\n", err.c_str() );
		return false;
	}

	if( total > 1 )
		printf( "\n[render %d/%d]\n", index + 1, total );

	// (Re)bind the inputs for this render.
	std::vector< std::pair< std::string, ofxbridge::FrameSource* > > inputs;
	for( const InputSpec& in : inputsFor( _opt.context, job ) )
	{
		if( effect.getClip( in.clip ) == nullptr )
			continue;
		ofxbridge::FrameSource* src = sourceFor( in, job, w, h, err );
		if( src == nullptr )
		{
			fprintf( stderr, "%s\n", err.c_str() );
			return false;
		}
		effect.bindSource( in.clip, src );
		inputs.emplace_back( in.clip, src );
	}

	const double t = job.time;
	effect.setCurrentTime( t );
	// A failed --press/--key/--transition still renders, but fails the run
	// (exit 1). Unknown --set/--edit names stay warnings, as they always were.
	const bool actionsOk = applyActions( job, false );
	if( !actionsOk )
		fprintf( stderr, "  (continuing with the render; this render will count as failed)\n" );

	std::vector< std::unique_ptr< DeclaredOnlySource > > strict;
	if( _opt.framesNeeded || _opt.strictFrames )
	{
		bool answered = false;
		auto needed   = effect.queryFramesNeeded( t, &answered );
		if( _opt.framesNeeded )
			for( const auto& kv : needed )
			{
				printf( "  frames needed %s:", kv.first.c_str() );
				for( const auto& r : kv.second )
					printf( " [%g, %g]", r.min, r.max );
				printf( "%s\n", answered ? "" : "  (plugin did not answer: the default, this frame only)" );
			}
		if( _opt.strictFrames )
			for( auto& in : inputs )
			{
				strict.push_back( std::make_unique< DeclaredOnlySource >( in.second, in.first, t, needed[ in.first ] ) );
				effect.bindSource( in.first, strict.back().get() );
			}
	}

	// However this render ends, put the unfiltered sources back before the
	// strict wrappers above are destroyed (locals go in reverse order).
	struct Rebind
	{
		ofxbridge::Effect& effect;
		std::vector< std::pair< std::string, ofxbridge::FrameSource* > >& inputs;
		~Rebind()
		{
			for( auto& in : inputs )
				effect.bindSource( in.first, in.second );
		}
	} rebind{ effect, inputs };

	ofxbridge::Frame out;
	out.allocate( w, h, _float );

	bool identity = false;
	if( _opt.identity )
	{
		std::string clip;
		double it = t;
		if( effect.queryIdentity( t, w, h, clip, it ) )
		{
			identity = true;
			printf( "  isIdentity: yes -> clip %s at t=%g (render skipped, output copied)\n", clip.c_str(), it );
			ofxbridge::Frame* f = nullptr;
			for( auto& in : inputs )
				if( in.first == clip )
					f = in.second->frameAt( it );
			if( f != nullptr && f->width == w && f->height == h )
				out.data = f->data;
		}
		else
			printf( "  isIdentity: no\n" );
	}

	double ms = 0.0;
	if( !identity )
	{
		const auto t0 = std::chrono::steady_clock::now();
		const bool rendered = effect.renderBound( out, t, err );
		ms = std::chrono::duration< double, std::milli >( std::chrono::steady_clock::now() - t0 ).count();
		if( !rendered )
		{
			fprintf( stderr, "render failed: %s\n", err.c_str() );
			for( const auto& m : effect.messages() )
				fprintf( stderr, "  plugin said: %s\n", m.c_str() );
			effect.clearMessages();
			return false;
		}
	}

	// What each input showed at t, for the report and the --out panels.
	std::vector< Rgba8 > shown( inputs.size() );
	std::vector< bool > present( inputs.size(), false );
	for( size_t i = 0; i < inputs.size(); ++i )
	{
		ofxbridge::Frame* f = inputs[ i ].second->frameAt( t );
		if( f != nullptr )
		{
			shown[ i ]   = fromFrame( *f );
			present[ i ] = true;
		}
	}
	const Rgba8 outImg = fromFrame( out );

	printf( "rendered %dx%d through %s\n", w, h, _identifier.c_str() );
	printf( "  time %g  context %s  depth %s  frame rate %g  render %.3f ms\n", t,
			shortContext( _opt.context ).c_str(), _float ? "float RGBA" : "8-bit RGBA", _opt.frameRate, ms );

	auto pix = []( const Rgba8& img, int x, int y ) { return img.at( x, y ); };
	const bool filterLike = inputs.size() == 1;
	if( filterLike && present[ 0 ] )
		printf( "  in  [0,0]      RGBA %3u %3u %3u %3u\n", pix( shown[ 0 ], 0, 0 )[ 0 ], pix( shown[ 0 ], 0, 0 )[ 1 ],
				pix( shown[ 0 ], 0, 0 )[ 2 ], pix( shown[ 0 ], 0, 0 )[ 3 ] );
	else
		for( size_t i = 0; i < inputs.size(); ++i )
			if( !present[ i ] )
				printf( "  %s: no frame at t=%g\n", inputs[ i ].first.c_str(), t );
	printf( "  out [0,0]      RGBA %3u %3u %3u %3u\n", pix( outImg, 0, 0 )[ 0 ], pix( outImg, 0, 0 )[ 1 ],
			pix( outImg, 0, 0 )[ 2 ], pix( outImg, 0, 0 )[ 3 ] );
	const int cx = w / 2, cy = h / 2;
	if( filterLike && present[ 0 ] )
		printf( "  in  [centre]   RGBA %3u %3u %3u %3u\n", pix( shown[ 0 ], cx, cy )[ 0 ], pix( shown[ 0 ], cx, cy )[ 1 ],
				pix( shown[ 0 ], cx, cy )[ 2 ], pix( shown[ 0 ], cx, cy )[ 3 ] );
	else
		for( size_t i = 0; i < inputs.size(); ++i )
			if( present[ i ] )
				printf( "  %-12s[centre] RGBA %3u %3u %3u %3u\n", inputs[ i ].first.c_str(),
						pix( shown[ i ], cx, cy )[ 0 ], pix( shown[ i ], cx, cy )[ 1 ], pix( shown[ i ], cx, cy )[ 2 ],
						pix( shown[ i ], cx, cy )[ 3 ] );
	printf( "  out [centre]   RGBA %3u %3u %3u %3u\n", pix( outImg, cx, cy )[ 0 ], pix( outImg, cx, cy )[ 1 ],
			pix( outImg, cx, cy )[ 2 ], pix( outImg, cx, cy )[ 3 ] );

	double mean[ 4 ] = { 0, 0, 0, 0 };
	for( size_t i = 0; i < outImg.px.size(); ++i )
		mean[ i % 4 ] += outImg.px[ i ];
	const double npx = (double)w * h;
	printf( "  out mean       RGBA %.2f %.2f %.2f %.2f\n", mean[ 0 ] / npx, mean[ 1 ] / npx, mean[ 2 ] / npx,
			mean[ 3 ] / npx );
	printf( "  out hash       fnv1a64 %016llx (8-bit RGBA)\n", (unsigned long long)fnv1a( outImg.px ) );

	if( filterLike && present[ 0 ] )
	{
		size_t changed = 0;
		for( size_t i = 0; i < outImg.px.size(); ++i )
			if( outImg.px[ i ] != shown[ 0 ].px[ i ] )
				++changed;
		printf( "  %zu of %zu bytes differ from the input\n", changed, outImg.px.size() );
	}

	for( const auto& m : effect.messages() )
		printf( "  plugin said: %s\n", m.c_str() );
	effect.clearMessages();

	bool ok = true;
	if( !job.out.empty() )
	{
		std::vector< const Rgba8* > panels;
		std::string legend;
		for( size_t i = 0; i < inputs.size(); ++i )
		{
			panels.push_back( present[ i ] ? &shown[ i ] : nullptr );
			legend += ( filterLike ? std::string( "input" ) : inputs[ i ].first ) + " | ";
		}
		panels.push_back( &outImg );
		legend += "output";
		if( writeBmpPanels( job.out, panels, w, h ) )
			printf( "  wrote %s (%s)\n", job.out.c_str(), legend.c_str() );
		else
		{
			fprintf( stderr, "could not write %s\n", job.out.c_str() );
			ok = false;
		}
	}
	if( !job.outOnly.empty() )
	{
		if( writeImage( job.outOnly, outImg, err ) )
			printf( "  wrote %s (output only)\n", job.outOnly.c_str() );
		else
		{
			fprintf( stderr, "%s\n", err.c_str() );
			ok = false;
		}
	}
	return ok && actionsOk;
}

int Session::run( const ofxbridge::PluginDesc& target, Job base, std::vector< Job > batch )
{
	_identifier = target.identifier;
	std::string error, chosen;
	_effect = ofxbridge::createEffect( _host, target.bundlePath, target.identifier, error, _opt.context, &chosen );
	if( !_effect )
	{
		fprintf( stderr, "createEffect failed: %s\n", error.c_str() );
		return 1;
	}
	printf( "  instance from %s (%s context)\n", chosen.c_str(), shortContext( _opt.context ).c_str() );

	// Pixel depth: 8-bit RGBA unless --depth float, and every clip is told the
	// same depth the images carry.
	std::string depth = _opt.depth == "float" ? kOfxBitDepthFloat : kOfxBitDepthByte;
	if( !_effect->getDescriptor().isPixelDepthSupported( depth ) )
	{
		const std::string other = depth == kOfxBitDepthByte ? kOfxBitDepthFloat : kOfxBitDepthByte;
		if( _effect->getDescriptor().isPixelDepthSupported( other ) )
		{
			printf( "  NOTE: plugin does not support %s; using %s\n", depth.c_str(), other.c_str() );
			depth = other;
		}
		else
		{
			fprintf( stderr, "plugin supports neither 8-bit nor float RGBA; this host offers no other depth\n" );
			return 1;
		}
	}
	_float = depth == kOfxBitDepthFloat;
	_effect->setForcedDepth( depth );
	_effect->setFrameRateValue( _opt.frameRate );

	// The run's jobs: the command line alone, or one per batch line (each
	// inheriting the command line's sources, size and time).
	std::vector< Job > jobs;
	const bool isBatch = !batch.empty();
	if( isBatch )
		for( const Job& line : batch )
			jobs.push_back( inheritJob( base, line ) );
	else
		jobs.push_back( base );

	// Timeline: --range, else every sequence's range, every render time and
	// every transition ramp, together. One timeline for the whole run, so a
	// render does not depend on which other renders share the process.
	if( _opt.haveRange )
	{
		_tlFirst = _opt.rangeFirst;
		_tlLast  = _opt.rangeLast;
	}
	else
	{
		bool any = false;
		auto take = [ & ]( double v ) {
			_tlFirst = any ? std::min( _tlFirst, v ) : v;
			_tlLast  = any ? std::max( _tlLast, v ) : v;
			any      = true;
		};
		std::vector< Job > all = jobs;
		all.push_back( base );
		// Frame 0 belongs to the timeline unless sequences say where it is.
		bool anySeq = false;
		for( const Job& j : all )
			anySeq |= j.usesSequences();
		if( !anySeq )
			take( 0.0 );
		for( const Job& j : all )
		{
			take( j.time );
			if( j.haveRamp )
			{
				take( j.rampFirst );
				take( j.rampLast );
			}
			for( const InputSpec& in : inputsFor( _opt.context, j ) )
			{
				int first = 0, last = 0;
				std::string err;
				if( !in.seq.empty() && probeSequence( in.seq, j.haveSeqFirst ? j.seqFirst : -1, first, last, err ) )
				{
					take( first );
					take( last );
				}
			}
		}
	}
	_effect->setTimeline( _tlFirst, _tlLast );
	printf( "  timeline [%g, %g] at %g fps (effect duration %g frames)\n", _tlFirst, _tlLast, _opt.frameRate,
			_tlLast - _tlFirst + 1 );

	// Bind the first render's inputs before the instance is created, so the
	// clip-preferences pass sees them connected, as a host's would be.
	int w = 0, h = 0;
	if( !resolveSize( jobs.front(), w, h, error ) )
	{
		fprintf( stderr, "%s\n", error.c_str() );
		return 1;
	}
	for( const InputSpec& in : inputsFor( _opt.context, jobs.front() ) )
	{
		if( _effect->getClip( in.clip ) == nullptr )
		{
			printf( "  NOTE: plugin defines no clip '%s' in this context\n", in.clip.c_str() );
			continue;
		}
		ofxbridge::FrameSource* src = sourceFor( in, jobs.front(), w, h, error );
		if( src == nullptr )
		{
			fprintf( stderr, "%s\n", error.c_str() );
			return 1;
		}
		_effect->bindSource( in.clip, src );
	}

	_effect->setFrameSize( w, h );
	_effect->setCurrentTime( jobs.front().time );
	if( !_effect->init( error ) )
	{
		fprintf( stderr, "init failed: %s\n", error.c_str() );
		for( const auto& m : _effect->messages() )
			fprintf( stderr, "  %s\n", m.c_str() );
		return 1;
	}

	// Anything the plugin writes back is reported, because that is usually
	// the point of an edit or a press.
	_effect->onParamChangedByPlugin = [ & ]( const std::string& name ) {
		std::vector< double > v;
		if( _effect->getParamValue( name, v ) && !v.empty() )
			printf( "  plugin set %s = %g\n", name.c_str(), v[ 0 ] );
		else
			printf( "  plugin set %s\n", name.c_str() );
	};

	if( _opt.context == kOfxImageEffectContextTransition )
	{
		bool driven = base.haveTransition || base.haveRamp;
		for( const Job& j : jobs )
			driven |= j.haveTransition || j.haveRamp;
		bool keyed = false;
		for( const Job& j : jobs )
			for( const auto& k : j.keys )
				keyed |= k.first == kOfxImageEffectTransitionParamName;
		for( const auto& k : base.keys )
			keyed |= k.first == kOfxImageEffectTransitionParamName;
		if( !driven && !keyed )
			printf( "  NOTE: no --transition / --transition-ramp given; the Transition param keeps its default\n" );
	}

	int failures = 0;
	if( isBatch )
	{
		// The command line's own actions run once, before the first line.
		if( base.hasActions() )
		{
			_effect->setCurrentTime( base.time );
			printf( "  [setup from the command line]\n" );
			if( !applyActions( base, true ) )
				++failures;
		}
		if( !base.out.empty() || !base.outOnly.empty() )
			fprintf( stderr, "  NOTE: --out/--out-only on the command line are ignored with --batch; put them on "
							 "the lines\n" );
	}

	for( size_t i = 0; i < jobs.size(); ++i )
		if( !renderJob( jobs[ i ], (int)i, (int)jobs.size() ) )
			++failures;

	if( isBatch )
		printf( "\nbatch: %zu render(s), %d failed\n", jobs.size(), failures );
	return failures ? 1 : 0;
}

} // namespace

int main( int argc, char** argv )
{
	// Line-buffered, so stdout and a plugin's stderr interleave in order when
	// both go to one log. MSVC has no line buffering (_IOLBF is full buffering
	// there) and treats a size under 2 as an invalid parameter, which ends the
	// process before it prints a word; unbuffered keeps the order instead.
#if defined( _WIN32 )
	setvbuf( stdout, nullptr, _IONBF, 0 );
#else
	setvbuf( stdout, nullptr, _IOLBF, 0 );
#endif

	Options opt;
	Job base;
	std::vector< std::string > args( argv + 1, argv + argc );

	for( size_t i = 0; i < args.size(); ++i )
	{
		const std::string& a = args[ i ];
		std::string err;
		const int r = parseJobArg( args, i, base, err );
		if( r == 1 )
			continue;
		if( r < 0 )
		{
			fprintf( stderr, "%s\n", err.c_str() );
			return 2;
		}
		auto value = [ & ]( std::string& v ) {
			if( i + 1 >= args.size() )
				return false;
			v = args[ ++i ];
			return true;
		};
		std::string v;
		if( a == "--dir" && i + 1 < args.size() )
		{
			// HostSupport scans directories for *.ofx.bundle; a path to the
			// bundle itself would find nothing, so scan its parent instead.
			std::string d = args[ ++i ];
			while( d.size() > 1 && d.back() == '/' )
				d.pop_back();
			const std::string suffix = ".ofx.bundle";
			if( d.size() > suffix.size() && d.compare( d.size() - suffix.size(), suffix.size(), suffix ) == 0 )
			{
				const size_t slash = d.find_last_of( '/' );
				const std::string parent = slash == std::string::npos ? "." : ( slash == 0 ? "/" : d.substr( 0, slash ) );
				fprintf( stderr, "  NOTE: --dir %s is a bundle; scanning its directory %s\n", d.c_str(), parent.c_str() );
				d = parent;
			}
			opt.dirs.push_back( d );
		}
		else if( a == "--json" )
			opt.wantJson = true;
		else if( a == "--quiet" )
			opt.quiet = true;
		else if( a == "--manifest" && i + 1 < args.size() )
			opt.manifestFor = args[ ++i ];
		else if( a == "--render" && i + 1 < args.size() )
			opt.renderFor = args[ ++i ];
		else if( a == "--no-system-dirs" )
			opt.noSystemDirs = true;
		else if( a == "--identity" )
			opt.identity = true;
		else if( a == "--frames-needed" )
			opt.framesNeeded = true;
		else if( a == "--strict-frames" )
			opt.strictFrames = true;
		else if( a == "--allow-link-press" )
			opt.allowLinkPress = true;
		else if( a == "--context" && value( v ) )
		{
			opt.context = contextFromFlag( v );
			if( opt.context.empty() )
			{
				fprintf( stderr, "--context expects filter|transition|generator|general, got '%s'\n", v.c_str() );
				return 2;
			}
		}
		else if( a == "--frame-rate" && value( v ) )
		{
			if( !parseNumber( v, opt.frameRate ) || opt.frameRate <= 0 )
			{
				fprintf( stderr, "--frame-rate expects a positive number\n" );
				return 2;
			}
		}
		else if( a == "--depth" && value( v ) )
		{
			if( v != "byte" && v != "float" )
			{
				fprintf( stderr, "--depth expects byte|float\n" );
				return 2;
			}
			opt.depth = v;
		}
		else if( a == "--temporal" && value( v ) )
		{
			if( v != "0" && v != "1" )
			{
				fprintf( stderr, "--temporal expects 0|1\n" );
				return 2;
			}
			opt.temporal = v == "1" ? 1 : 0;
		}
		else if( a == "--range" && value( v ) )
		{
			if( !parseRange( v, opt.rangeFirst, opt.rangeLast ) || opt.rangeLast < opt.rangeFirst )
			{
				fprintf( stderr, "--range expects FIRST:LAST frames\n" );
				return 2;
			}
			opt.haveRange = true;
		}
		else if( a == "--batch" && value( v ) )
			opt.batchFile = v;
		else if( a == "--quirks" && value( v ) )
		{
			// A comma list, so more host personalities can join later.
			std::stringstream ss( v );
			std::string q;
			while( std::getline( ss, q, ',' ) )
			{
				if( q == "fusion" )
					opt.fusionQuirks = true;
				else if( q != "none" && !q.empty() )
				{
					fprintf( stderr, "--quirks: unknown quirk set '%s' (known: fusion, none)\n", q.c_str() );
					return 2;
				}
			}
		}
		else if( a == "-h" || a == "--help" )
		{
			usage();
			return 0;
		}
		else
		{
			fprintf( stderr, "unknown argument: %s\n", a.c_str() );
			usage();
			return 2;
		}
	}

	// Conflicting sources are a usage error, not a guess.
	if( ( !base.in.empty() && !base.seq.empty() ) || ( !base.from.empty() && !base.seqFrom.empty() ) ||
		( !base.to.empty() && !base.seqTo.empty() ) )
	{
		fprintf( stderr, "give a still (--in/--from/--to) or a sequence (--seq/--seq-from/--seq-to), not both\n" );
		return 2;
	}

	std::vector< Job > batch;
	if( !opt.batchFile.empty() )
	{
		std::ifstream f( opt.batchFile );
		if( !f )
		{
			fprintf( stderr, "cannot read --batch file %s\n", opt.batchFile.c_str() );
			return 2;
		}
		std::string line;
		int lineNo = 0;
		while( std::getline( f, line ) )
		{
			++lineNo;
			std::vector< std::string > toks = tokenize( line );
			if( toks.empty() )
				continue;
			Job job;
			for( size_t i = 0; i < toks.size(); ++i )
			{
				std::string err;
				const int r = parseJobArg( toks, i, job, err );
				if( r == 1 )
					continue;
				fprintf( stderr, "%s:%d: %s\n", opt.batchFile.c_str(), lineNo,
						 r < 0 ? err.c_str() : ( "'" + toks[ i ] + "' is not a per-render flag" ).c_str() );
				return 2;
			}
			if( ( !job.in.empty() && !job.seq.empty() ) || ( !job.from.empty() && !job.seqFrom.empty() ) ||
				( !job.to.empty() && !job.seqTo.empty() ) )
			{
				fprintf( stderr, "%s:%d: a still and a sequence for the same clip\n", opt.batchFile.c_str(), lineNo );
				return 2;
			}
			batch.push_back( job );
		}
		if( batch.empty() )
		{
			fprintf( stderr, "--batch file %s has no renders\n", opt.batchFile.c_str() );
			return 2;
		}
	}

	// Host presentation, fixed before the first Host is constructed.
	{
		ofxbridge::HostOptions& ho = ofxbridge::hostOptions();
		ho.contexts                = { kOfxImageEffectContextFilter };
		if( opt.context != kOfxImageEffectContextFilter )
			ho.contexts.push_back( opt.context );
		bool anySeq = base.usesSequences();
		for( const Job& j : batch )
			anySeq |= j.usesSequences();
		ho.temporalAccess = opt.temporal >= 0 ? opt.temporal : ( anySeq ? 1 : 0 );
		ho.fusionQuirks   = opt.fusionQuirks;
	}

	if( opt.noSystemDirs )
	{
#if defined( _WIN32 )
		_putenv_s( "OFXBRIDGE_NO_SYSTEM_DIRS", "1" );// MSVC has no setenv
#else
		setenv( "OFXBRIDGE_NO_SYSTEM_DIRS", "1", 1 );
#endif
	}

	std::vector< std::string > paths = ofxbridge::defaultSearchPaths();
	paths.insert( paths.end(), opt.dirs.begin(), opt.dirs.end() );

	std::string log;
	std::vector< ofxbridge::PluginDesc > plugins = ofxbridge::scanAndDescribe( paths, log, opt.context );

	if( !opt.quiet && opt.manifestFor.empty() && opt.renderFor.empty() && !opt.wantJson )
		fputs( log.c_str(), stderr );

	if( !opt.renderFor.empty() )
	{
		const ofxbridge::PluginDesc* target = nullptr;
		std::vector< const ofxbridge::PluginDesc* > matches;
		for( const auto& p : plugins )
			if( p.identifier == opt.renderFor )
			{
				target = &p;
				matches.push_back( &p );
			}
		if( target == nullptr )
		{
			fprintf( stderr, "no plugin with identifier '%s'\n", opt.renderFor.c_str() );
			if( opt.noSystemDirs && opt.dirs.empty() )
				fprintf( stderr, "  (--no-system-dirs with no --dir scans nothing)\n" );
			return 1;
		}
		if( matches.size() > 1 )
		{
			fprintf( stderr, "  WARNING: %zu bundles carry %s; using the last scanned:\n", matches.size(),
					 opt.renderFor.c_str() );
			for( const auto* m : matches )
				fprintf( stderr, "    %s %s\n", m == target ? "*" : " ", m->bundlePath.c_str() );
			fprintf( stderr, "  (--no-system-dirs scans only --dir paths)\n" );
		}
		if( !target->error.empty() )
		{
			fprintf( stderr, "plugin is not usable: %s\n", target->error.c_str() );
			if( !target->contexts.empty() )
			{
				fprintf( stderr, "  it offers:" );
				for( const auto& c : target->contexts )
					fprintf( stderr, " %s", shortContext( c ).c_str() );
				fprintf( stderr, "  (pick one with --context)\n" );
			}
			return 1;
		}
		printf( "  host: contexts" );
		for( const auto& c : ofxbridge::hostOptions().contexts )
			printf( " %s", shortContext( c ).c_str() );
		printf( ", temporal clip access %d, %s%s\n", ofxbridge::hostOptions().temporalAccess,
				opt.noSystemDirs ? "system plugin dirs NOT scanned" : "system plugin dirs scanned",
				opt.fusionQuirks ? ", QUIRKS fusion (no FrameRate on effect/clips, clip FrameRange [0,0], "
								   "Unmapped rate/range dim 0, no render-status args)"
								 : "" );
		Session session( opt );
		return session.run( *target, base, batch );
	}

	if( !opt.manifestFor.empty() )
	{
		for( const auto& p : plugins )
		{
			if( p.identifier == opt.manifestFor )
			{
				if( !p.error.empty() )
				{
					fprintf( stderr, "plugin %s is not usable: %s\n", p.identifier.c_str(), p.error.c_str() );
					return 1;
				}
				fputs( ofxbridge::toManifestJson( p ).c_str(), stdout );
				return 0;
			}
		}
		fprintf( stderr, "no plugin with identifier '%s'\n", opt.manifestFor.c_str() );
		return 1;
	}

	if( opt.wantJson )
	{
		printf( "[\n" );
		bool first = true;
		for( const auto& p : plugins )
		{
			if( !p.error.empty() )
				continue;
			if( !first )
				printf( ",\n" );
			first = false;
			fputs( ofxbridge::toManifestJson( p ).c_str(), stdout );
		}
		printf( "]\n" );
		return 0;
	}

	// Human-readable summary.
	int usable = 0;
	for( const auto& p : plugins )
	{
		printf( "\n%s\n", p.identifier.c_str() );
		printf( "  label      : %s\n", p.label.c_str() );
		printf( "  grouping   : %s\n", p.grouping.c_str() );
		printf( "  version    : %d.%d\n", p.versionMajor, p.versionMinor );
		printf( "  bundle     : %s\n", p.bundlePath.c_str() );
		printf( "  gl render  : %s\n", p.supportsOpenGLRender ? "yes" : "no" );
		printf( "  metal      : %s\n", p.supportsMetalRender ? "yes" : "no" );
		printf( "  opencl     : %s\n", p.supportsOpenCLRender ? "yes" : "no" );
		printf( "  cuda       : %s%s\n", p.supportsCudaRender ? "yes" : "no",
				p.supportsCudaRender ? "  (bridge UNVERIFIED -- see docs/04)" : "" );
		printf( "  contexts   : " );
		for( const auto& c : p.contexts )
			printf( "%s ", c.c_str() );
		printf( "\n" );

		if( !p.error.empty() )
		{
			printf( "  UNUSABLE   : %s\n", p.error.c_str() );
			continue;
		}
		++usable;

		printf( "  parameters : %zu\n", p.params.size() );
		for( const auto& pd : p.params )
		{
			printf( "    %-24s %-22s %s", pd.name.c_str(), pd.type.c_str(), pd.label.c_str() );
			if( !pd.choices.empty() )
			{
				printf( "  [" );
				for( size_t i = 0; i < pd.choices.size(); ++i )
					printf( "%s%s", i ? "|" : "", pd.choices[ i ].c_str() );
				printf( "]" );
			}
			else if( pd.hasDisplayRange && !pd.displayMin.empty() )
			{
				printf( "  (%g..%g)", pd.displayMin[ 0 ], pd.displayMax[ 0 ] );
			}
			if( pd.secret )
				printf( "  [secret]" );
			printf( "\n" );
		}
	}

	if( opt.context == kOfxImageEffectContextFilter )
		printf( "\n%d of %zu plugin(s) usable as Resolume effects\n", usable, plugins.size() );
	else
		printf( "\n%d of %zu plugin(s) usable in the %s context\n", usable, plugins.size(),
				shortContext( opt.context ).c_str() );
	return 0;
}
