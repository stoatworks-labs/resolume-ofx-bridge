#include "Params.h"

#include "ofxParam.h"

namespace ofxbridge {

namespace detail {

double defaultDouble( const OFX::Host::Property::Set& props, int i )
{
	const OFX::Host::Property::Property* p = props.fetchProperty( kOfxParamPropDefault );
	if( p == nullptr || i >= p->getDimension() )
		return 0.0;
	return props.getDoubleProperty( kOfxParamPropDefault, i );
}

int defaultInt( const OFX::Host::Property::Set& props, int i )
{
	const OFX::Host::Property::Property* p = props.fetchProperty( kOfxParamPropDefault );
	if( p == nullptr || i >= p->getDimension() )
		return 0;
	return props.getIntProperty( kOfxParamPropDefault, i );
}

} // namespace detail

// ---------------------------------------------------------------------------
// Integer / Choice
// ---------------------------------------------------------------------------

IntegerParam::IntegerParam( OFX::Host::Param::Descriptor& d, OFX::Host::Param::SetInstance* s ) :
	OFX::Host::Param::IntegerInstance( d, s )
{
	_v[ 0 ] = detail::defaultInt( getProperties(), 0 );
}
OfxStatus IntegerParam::get( int& v )
{
	v = (int)_v[ 0 ];
	return kOfxStatOK;
}
OfxStatus IntegerParam::get( OfxTime, int& v )
{
	return get( v );
}
OfxStatus IntegerParam::set( int v )
{
	_v[ 0 ] = v;
	return kOfxStatOK;
}
OfxStatus IntegerParam::set( OfxTime, int v )
{
	return set( v );
}

ChoiceParam::ChoiceParam( OFX::Host::Param::Descriptor& d, OFX::Host::Param::SetInstance* s ) :
	OFX::Host::Param::ChoiceInstance( d, s )
{
	_v[ 0 ] = detail::defaultInt( getProperties(), 0 );
}
OfxStatus ChoiceParam::get( int& v )
{
	v = (int)_v[ 0 ];
	return kOfxStatOK;
}
OfxStatus ChoiceParam::get( OfxTime, int& v )
{
	return get( v );
}
OfxStatus ChoiceParam::set( int v )
{
	_v[ 0 ] = v;
	return kOfxStatOK;
}
OfxStatus ChoiceParam::set( OfxTime, int v )
{
	return set( v );
}

// ---------------------------------------------------------------------------
// Double
// ---------------------------------------------------------------------------

DoubleParam::DoubleParam( OFX::Host::Param::Descriptor& d, OFX::Host::Param::SetInstance* s ) :
	OFX::Host::Param::DoubleInstance( d, s )
{
	_v[ 0 ] = detail::defaultDouble( getProperties(), 0 );
}
OfxStatus DoubleParam::get( double& v )
{
	v = _v[ 0 ];
	return kOfxStatOK;
}
OfxStatus DoubleParam::get( OfxTime, double& v )
{
	return get( v );
}
OfxStatus DoubleParam::set( double v )
{
	_v[ 0 ] = v;
	return kOfxStatOK;
}
OfxStatus DoubleParam::set( OfxTime, double v )
{
	return set( v );
}
OfxStatus DoubleParam::derive( OfxTime, double& v )
{
	// No animation, so the derivative is identically zero.
	v = 0.0;
	return kOfxStatOK;
}
OfxStatus DoubleParam::integrate( OfxTime t1, OfxTime t2, double& v )
{
	v = _v[ 0 ] * ( t2 - t1 );
	return kOfxStatOK;
}

// ---------------------------------------------------------------------------
// Boolean
// ---------------------------------------------------------------------------

BooleanParam::BooleanParam( OFX::Host::Param::Descriptor& d, OFX::Host::Param::SetInstance* s ) :
	OFX::Host::Param::BooleanInstance( d, s )
{
	_v[ 0 ] = detail::defaultInt( getProperties(), 0 ) ? 1.0 : 0.0;
}
OfxStatus BooleanParam::get( bool& v )
{
	v = _v[ 0 ] != 0.0;
	return kOfxStatOK;
}
OfxStatus BooleanParam::get( OfxTime, bool& v )
{
	return get( v );
}
OfxStatus BooleanParam::set( bool v )
{
	_v[ 0 ] = v ? 1.0 : 0.0;
	return kOfxStatOK;
}
OfxStatus BooleanParam::set( OfxTime, bool v )
{
	return set( v );
}

// ---------------------------------------------------------------------------
// Colours
// ---------------------------------------------------------------------------

RGBAParam::RGBAParam( OFX::Host::Param::Descriptor& d, OFX::Host::Param::SetInstance* s ) :
	OFX::Host::Param::RGBAInstance( d, s )
{
	for( int i = 0; i < 4; ++i )
		_v[ i ] = detail::defaultDouble( getProperties(), i );
}
OfxStatus RGBAParam::get( double& r, double& g, double& b, double& a )
{
	r = _v[ 0 ];
	g = _v[ 1 ];
	b = _v[ 2 ];
	a = _v[ 3 ];
	return kOfxStatOK;
}
OfxStatus RGBAParam::get( OfxTime, double& r, double& g, double& b, double& a )
{
	return get( r, g, b, a );
}
OfxStatus RGBAParam::set( double r, double g, double b, double a )
{
	_v[ 0 ] = r;
	_v[ 1 ] = g;
	_v[ 2 ] = b;
	_v[ 3 ] = a;
	return kOfxStatOK;
}
OfxStatus RGBAParam::set( OfxTime, double r, double g, double b, double a )
{
	return set( r, g, b, a );
}

RGBParam::RGBParam( OFX::Host::Param::Descriptor& d, OFX::Host::Param::SetInstance* s ) :
	OFX::Host::Param::RGBInstance( d, s )
{
	for( int i = 0; i < 3; ++i )
		_v[ i ] = detail::defaultDouble( getProperties(), i );
}
OfxStatus RGBParam::get( double& r, double& g, double& b )
{
	r = _v[ 0 ];
	g = _v[ 1 ];
	b = _v[ 2 ];
	return kOfxStatOK;
}
OfxStatus RGBParam::get( OfxTime, double& r, double& g, double& b )
{
	return get( r, g, b );
}
OfxStatus RGBParam::set( double r, double g, double b )
{
	_v[ 0 ] = r;
	_v[ 1 ] = g;
	_v[ 2 ] = b;
	return kOfxStatOK;
}
OfxStatus RGBParam::set( OfxTime, double r, double g, double b )
{
	return set( r, g, b );
}

// ---------------------------------------------------------------------------
// Vectors
// ---------------------------------------------------------------------------

Double2DParam::Double2DParam( OFX::Host::Param::Descriptor& d, OFX::Host::Param::SetInstance* s ) :
	OFX::Host::Param::Double2DInstance( d, s )
{
	for( int i = 0; i < 2; ++i )
		_v[ i ] = detail::defaultDouble( getProperties(), i );
}
OfxStatus Double2DParam::get( double& x, double& y )
{
	x = _v[ 0 ];
	y = _v[ 1 ];
	return kOfxStatOK;
}
OfxStatus Double2DParam::get( OfxTime, double& x, double& y )
{
	return get( x, y );
}
OfxStatus Double2DParam::set( double x, double y )
{
	_v[ 0 ] = x;
	_v[ 1 ] = y;
	return kOfxStatOK;
}
OfxStatus Double2DParam::set( OfxTime, double x, double y )
{
	return set( x, y );
}

Integer2DParam::Integer2DParam( OFX::Host::Param::Descriptor& d, OFX::Host::Param::SetInstance* s ) :
	OFX::Host::Param::Integer2DInstance( d, s )
{
	for( int i = 0; i < 2; ++i )
		_v[ i ] = detail::defaultInt( getProperties(), i );
}
OfxStatus Integer2DParam::get( int& x, int& y )
{
	x = (int)_v[ 0 ];
	y = (int)_v[ 1 ];
	return kOfxStatOK;
}
OfxStatus Integer2DParam::get( OfxTime, int& x, int& y )
{
	return get( x, y );
}
OfxStatus Integer2DParam::set( int x, int y )
{
	_v[ 0 ] = x;
	_v[ 1 ] = y;
	return kOfxStatOK;
}
OfxStatus Integer2DParam::set( OfxTime, int x, int y )
{
	return set( x, y );
}

Double3DParam::Double3DParam( OFX::Host::Param::Descriptor& d, OFX::Host::Param::SetInstance* s ) :
	OFX::Host::Param::Double3DInstance( d, s )
{
	for( int i = 0; i < 3; ++i )
		_v[ i ] = detail::defaultDouble( getProperties(), i );
}
OfxStatus Double3DParam::get( double& x, double& y, double& z )
{
	x = _v[ 0 ];
	y = _v[ 1 ];
	z = _v[ 2 ];
	return kOfxStatOK;
}
OfxStatus Double3DParam::get( OfxTime, double& x, double& y, double& z )
{
	return get( x, y, z );
}
OfxStatus Double3DParam::set( double x, double y, double z )
{
	_v[ 0 ] = x;
	_v[ 1 ] = y;
	_v[ 2 ] = z;
	return kOfxStatOK;
}
OfxStatus Double3DParam::set( OfxTime, double x, double y, double z )
{
	return set( x, y, z );
}

Integer3DParam::Integer3DParam( OFX::Host::Param::Descriptor& d, OFX::Host::Param::SetInstance* s ) :
	OFX::Host::Param::Integer3DInstance( d, s )
{
	for( int i = 0; i < 3; ++i )
		_v[ i ] = detail::defaultInt( getProperties(), i );
}
OfxStatus Integer3DParam::get( int& x, int& y, int& z )
{
	x = (int)_v[ 0 ];
	y = (int)_v[ 1 ];
	z = (int)_v[ 2 ];
	return kOfxStatOK;
}
OfxStatus Integer3DParam::get( OfxTime, int& x, int& y, int& z )
{
	return get( x, y, z );
}
OfxStatus Integer3DParam::set( int x, int y, int z )
{
	_v[ 0 ] = x;
	_v[ 1 ] = y;
	_v[ 2 ] = z;
	return kOfxStatOK;
}
OfxStatus Integer3DParam::set( OfxTime, int x, int y, int z )
{
	return set( x, y, z );
}

// ---------------------------------------------------------------------------
// String
// ---------------------------------------------------------------------------

StringParam::StringParam( OFX::Host::Param::Descriptor& d, OFX::Host::Param::SetInstance* s ) :
	OFX::Host::Param::StringInstance( d, s )
{
	_v = getProperties().getStringProperty( kOfxParamPropDefault );
}
OfxStatus StringParam::get( std::string& v )
{
	v = _v;
	return kOfxStatOK;
}
OfxStatus StringParam::get( OfxTime, std::string& v )
{
	return get( v );
}
OfxStatus StringParam::set( const char* v )
{
	_v = v ? v : "";
	return kOfxStatOK;
}
OfxStatus StringParam::set( OfxTime, const char* v )
{
	return set( v );
}

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------

OFX::Host::Param::Instance* makeParamInstance( const std::string& /*name*/,
											   OFX::Host::Param::Descriptor& d,
											   OFX::Host::Param::SetInstance* s )
{
	const std::string& t = d.getType();

	if( t == kOfxParamTypeInteger )
		return new IntegerParam( d, s );
	if( t == kOfxParamTypeDouble )
		return new DoubleParam( d, s );
	if( t == kOfxParamTypeBoolean )
		return new BooleanParam( d, s );
	if( t == kOfxParamTypeChoice )
		return new ChoiceParam( d, s );
	if( t == kOfxParamTypeRGBA )
		return new RGBAParam( d, s );
	if( t == kOfxParamTypeRGB )
		return new RGBParam( d, s );
	if( t == kOfxParamTypeDouble2D )
		return new Double2DParam( d, s );
	if( t == kOfxParamTypeInteger2D )
		return new Integer2DParam( d, s );
	if( t == kOfxParamTypeDouble3D )
		return new Double3DParam( d, s );
	if( t == kOfxParamTypeInteger3D )
		return new Integer3DParam( d, s );
	if( t == kOfxParamTypeString || t == kOfxParamTypeCustom )
		return new StringParam( d, s );
	if( t == kOfxParamTypeGroup )
		return new GroupParam( d, s );
	if( t == kOfxParamTypePage )
		return new PageParam( d, s );
	if( t == kOfxParamTypePushButton )
		return new PushbuttonParam( d, s );

	// Parametric (curve) params have no FFGL equivalent and no sane flattening,
	// so we decline them rather than silently misrepresent the plugin's UI.
	return nullptr;
}

} // namespace ofxbridge
