#include "Params.h"

#include "ofxParam.h"

#include <algorithm>
#include <cmath>

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

/// Keys closer than this are the same key.
constexpr double kKeyEpsilon = 1e-9;

} // namespace detail

// ---------------------------------------------------------------------------
// NumericStore: keyframes
// ---------------------------------------------------------------------------

template< int N >
bool NumericStore< N >::setKeys( const std::vector< ParamKey >& keys )
{
	_keys.clear();
	for( const auto& k : keys )
	{
		std::vector< double > v( N );
		for( int i = 0; i < N; ++i )
			v[ i ] = i < (int)k.second.size() ? k.second[ i ] : _v[ i ];
		auto it = std::find_if( _keys.begin(), _keys.end(), [ & ]( const ParamKey& e ) {
			return std::fabs( e.first - k.first ) < detail::kKeyEpsilon;
		} );
		if( it != _keys.end() )
			it->second = v;
		else
			_keys.emplace_back( k.first, v );
	}
	std::sort( _keys.begin(), _keys.end(),
			   []( const ParamKey& a, const ParamKey& b ) { return a.first < b.first; } );
	return true;
}

template< int N >
void NumericStore< N >::evalAt( double t, double* out ) const
{
	if( _keys.empty() )
	{
		for( int i = 0; i < N; ++i )
			out[ i ] = _v[ i ];
		return;
	}
	const ParamKey* lo = &_keys.front();
	const ParamKey* hi = &_keys.front();
	if( t <= _keys.front().first )
		lo = hi = &_keys.front();
	else if( t >= _keys.back().first )
		lo = hi = &_keys.back();
	else
	{
		for( size_t i = 0; i + 1 < _keys.size(); ++i )
		{
			if( t >= _keys[ i ].first && t < _keys[ i + 1 ].first )
			{
				lo = &_keys[ i ];
				hi = &_keys[ i + 1 ];
				break;
			}
		}
	}

	if( lo == hi || _interp == Interp::Step )
	{
		for( int i = 0; i < N; ++i )
			out[ i ] = lo->second[ (size_t)i ];
	}
	else
	{
		const double a = ( t - lo->first ) / ( hi->first - lo->first );
		for( int i = 0; i < N; ++i )
		{
			const double v = lo->second[ (size_t)i ] + ( hi->second[ (size_t)i ] - lo->second[ (size_t)i ] ) * a;
			out[ i ]       = _interp == Interp::Round ? std::round( v ) : v;
		}
	}
}

template< int N >
void NumericStore< N >::storeAt( double t, const double* in )
{
	if( _keys.empty() )
	{
		for( int i = 0; i < N; ++i )
			_v[ i ] = in[ i ];
		return;
	}
	std::vector< ParamKey > keys = _keys;
	keys.emplace_back( t, std::vector< double >( in, in + N ) );
	setKeys( keys );
}

template< int N >
OfxStatus NumericStore< N >::kfIndex( OfxTime time, int direction, int& index ) const
{
	if( direction == 0 )
	{
		for( size_t i = 0; i < _keys.size(); ++i )
			if( std::fabs( _keys[ i ].first - time ) < detail::kKeyEpsilon )
			{
				index = (int)i;
				return kOfxStatOK;
			}
		return kOfxStatFailed;
	}
	if( direction < 0 )
	{
		for( size_t i = _keys.size(); i-- > 0; )
			if( _keys[ i ].first < time - detail::kKeyEpsilon )
			{
				index = (int)i;
				return kOfxStatOK;
			}
		return kOfxStatFailed;
	}
	for( size_t i = 0; i < _keys.size(); ++i )
		if( _keys[ i ].first > time + detail::kKeyEpsilon )
		{
			index = (int)i;
			return kOfxStatOK;
		}
	return kOfxStatFailed;
}

template< int N >
OfxStatus NumericStore< N >::kfDelete( OfxTime time )
{
	for( auto it = _keys.begin(); it != _keys.end(); ++it )
		if( std::fabs( it->first - time ) < detail::kKeyEpsilon )
		{
			// Deleting the last key leaves the value it held, as hosts do.
			if( _keys.size() == 1 )
				for( int i = 0; i < N; ++i )
					_v[ i ] = it->second[ (size_t)i ];
			_keys.erase( it );
			return kOfxStatOK;
		}
	return kOfxStatFailed;
}

template< int N >
double NumericStore< N >::slope0( double t ) const
{
	if( _keys.size() < 2 || _interp == Interp::Step )
		return 0.0;
	for( size_t i = 0; i + 1 < _keys.size(); ++i )
		if( t >= _keys[ i ].first && t < _keys[ i + 1 ].first )
			return ( _keys[ i + 1 ].second[ 0 ] - _keys[ i ].second[ 0 ] ) / ( _keys[ i + 1 ].first - _keys[ i ].first );
	return 0.0;
}

template< int N >
double NumericStore< N >::integral0( double t1, double t2 ) const
{
	if( t2 < t1 )
		return -integral0( t2, t1 );
	std::vector< double > xs{ t1, t2 };
	for( const auto& k : _keys )
		if( k.first > t1 && k.first < t2 )
			xs.push_back( k.first );
	std::sort( xs.begin(), xs.end() );
	double sum = 0.0;
	for( size_t i = 0; i + 1 < xs.size(); ++i )
	{
		double a[ N ], b[ N ];
		evalAt( xs[ i ], a );
		evalAt( xs[ i + 1 ], b );
		sum += ( xs[ i + 1 ] - xs[ i ] ) * ( a[ 0 ] + b[ 0 ] ) * 0.5;
	}
	return sum;
}

template class NumericStore< 1 >;
template class NumericStore< 2 >;
template class NumericStore< 3 >;
template class NumericStore< 4 >;

// ---------------------------------------------------------------------------
// Integer / Choice
// ---------------------------------------------------------------------------

IntegerParam::IntegerParam( OFX::Host::Param::Descriptor& d, OFX::Host::Param::SetInstance* s ) :
	OFX::Host::Param::IntegerInstance( d, s ), NumericStore< 1 >( Interp::Round )
{
	_v[ 0 ] = detail::defaultInt( getProperties(), 0 );
}
OfxStatus IntegerParam::get( int& v )
{
	double d;
	evalNow( &d );
	v = (int)d;
	return kOfxStatOK;
}
OfxStatus IntegerParam::get( OfxTime t, int& v )
{
	double d;
	evalAt( t, &d );
	v = (int)d;
	return kOfxStatOK;
}
OfxStatus IntegerParam::set( int v )
{
	const double d = v;
	storeNow( &d );
	return kOfxStatOK;
}
OfxStatus IntegerParam::set( OfxTime t, int v )
{
	const double d = v;
	storeAt( t, &d );
	return kOfxStatOK;
}

ChoiceParam::ChoiceParam( OFX::Host::Param::Descriptor& d, OFX::Host::Param::SetInstance* s ) :
	OFX::Host::Param::ChoiceInstance( d, s ), NumericStore< 1 >( Interp::Step )
{
	_v[ 0 ] = detail::defaultInt( getProperties(), 0 );
}
OfxStatus ChoiceParam::get( int& v )
{
	double d;
	evalNow( &d );
	v = (int)d;
	return kOfxStatOK;
}
OfxStatus ChoiceParam::get( OfxTime t, int& v )
{
	double d;
	evalAt( t, &d );
	v = (int)d;
	return kOfxStatOK;
}
OfxStatus ChoiceParam::set( int v )
{
	const double d = v;
	storeNow( &d );
	return kOfxStatOK;
}
OfxStatus ChoiceParam::set( OfxTime t, int v )
{
	const double d = v;
	storeAt( t, &d );
	return kOfxStatOK;
}

// ---------------------------------------------------------------------------
// Double
// ---------------------------------------------------------------------------

DoubleParam::DoubleParam( OFX::Host::Param::Descriptor& d, OFX::Host::Param::SetInstance* s ) :
	OFX::Host::Param::DoubleInstance( d, s ), NumericStore< 1 >( Interp::Linear )
{
	_v[ 0 ] = detail::defaultDouble( getProperties(), 0 );
}
OfxStatus DoubleParam::get( double& v )
{
	evalNow( &v );
	return kOfxStatOK;
}
OfxStatus DoubleParam::get( OfxTime t, double& v )
{
	evalAt( t, &v );
	return kOfxStatOK;
}
OfxStatus DoubleParam::set( double v )
{
	storeNow( &v );
	return kOfxStatOK;
}
OfxStatus DoubleParam::set( OfxTime t, double v )
{
	storeAt( t, &v );
	return kOfxStatOK;
}
OfxStatus DoubleParam::derive( OfxTime t, double& v )
{
	// Constant: identically zero. Keyed: the slope of the segment at t.
	v = _keys.empty() ? 0.0 : slope0( t );
	return kOfxStatOK;
}
OfxStatus DoubleParam::integrate( OfxTime t1, OfxTime t2, double& v )
{
	v = _keys.empty() ? _v[ 0 ] * ( t2 - t1 ) : integral0( t1, t2 );
	return kOfxStatOK;
}

// ---------------------------------------------------------------------------
// Boolean
// ---------------------------------------------------------------------------

BooleanParam::BooleanParam( OFX::Host::Param::Descriptor& d, OFX::Host::Param::SetInstance* s ) :
	OFX::Host::Param::BooleanInstance( d, s ), NumericStore< 1 >( Interp::Step )
{
	_v[ 0 ] = detail::defaultInt( getProperties(), 0 ) ? 1.0 : 0.0;
}
OfxStatus BooleanParam::get( bool& v )
{
	double d;
	evalNow( &d );
	v = d != 0.0;
	return kOfxStatOK;
}
OfxStatus BooleanParam::get( OfxTime t, bool& v )
{
	double d;
	evalAt( t, &d );
	v = d != 0.0;
	return kOfxStatOK;
}
OfxStatus BooleanParam::set( bool v )
{
	const double d = v ? 1.0 : 0.0;
	storeNow( &d );
	return kOfxStatOK;
}
OfxStatus BooleanParam::set( OfxTime t, bool v )
{
	const double d = v ? 1.0 : 0.0;
	storeAt( t, &d );
	return kOfxStatOK;
}

// ---------------------------------------------------------------------------
// Colours
// ---------------------------------------------------------------------------

RGBAParam::RGBAParam( OFX::Host::Param::Descriptor& d, OFX::Host::Param::SetInstance* s ) :
	OFX::Host::Param::RGBAInstance( d, s ), NumericStore< 4 >( Interp::Linear )
{
	for( int i = 0; i < 4; ++i )
		_v[ i ] = detail::defaultDouble( getProperties(), i );
}
OfxStatus RGBAParam::get( double& r, double& g, double& b, double& a )
{
	double v[ 4 ];
	evalNow( v );
	r = v[ 0 ], g = v[ 1 ], b = v[ 2 ], a = v[ 3 ];
	return kOfxStatOK;
}
OfxStatus RGBAParam::get( OfxTime t, double& r, double& g, double& b, double& a )
{
	double v[ 4 ];
	evalAt( t, v );
	r = v[ 0 ], g = v[ 1 ], b = v[ 2 ], a = v[ 3 ];
	return kOfxStatOK;
}
OfxStatus RGBAParam::set( double r, double g, double b, double a )
{
	const double v[ 4 ] = { r, g, b, a };
	storeNow( v );
	return kOfxStatOK;
}
OfxStatus RGBAParam::set( OfxTime t, double r, double g, double b, double a )
{
	const double v[ 4 ] = { r, g, b, a };
	storeAt( t, v );
	return kOfxStatOK;
}

RGBParam::RGBParam( OFX::Host::Param::Descriptor& d, OFX::Host::Param::SetInstance* s ) :
	OFX::Host::Param::RGBInstance( d, s ), NumericStore< 3 >( Interp::Linear )
{
	for( int i = 0; i < 3; ++i )
		_v[ i ] = detail::defaultDouble( getProperties(), i );
}
OfxStatus RGBParam::get( double& r, double& g, double& b )
{
	double v[ 3 ];
	evalNow( v );
	r = v[ 0 ], g = v[ 1 ], b = v[ 2 ];
	return kOfxStatOK;
}
OfxStatus RGBParam::get( OfxTime t, double& r, double& g, double& b )
{
	double v[ 3 ];
	evalAt( t, v );
	r = v[ 0 ], g = v[ 1 ], b = v[ 2 ];
	return kOfxStatOK;
}
OfxStatus RGBParam::set( double r, double g, double b )
{
	const double v[ 3 ] = { r, g, b };
	storeNow( v );
	return kOfxStatOK;
}
OfxStatus RGBParam::set( OfxTime t, double r, double g, double b )
{
	const double v[ 3 ] = { r, g, b };
	storeAt( t, v );
	return kOfxStatOK;
}

// ---------------------------------------------------------------------------
// Vectors
// ---------------------------------------------------------------------------

Double2DParam::Double2DParam( OFX::Host::Param::Descriptor& d, OFX::Host::Param::SetInstance* s ) :
	OFX::Host::Param::Double2DInstance( d, s ), NumericStore< 2 >( Interp::Linear )
{
	for( int i = 0; i < 2; ++i )
		_v[ i ] = detail::defaultDouble( getProperties(), i );
}
OfxStatus Double2DParam::get( double& x, double& y )
{
	double v[ 2 ];
	evalNow( v );
	x = v[ 0 ], y = v[ 1 ];
	return kOfxStatOK;
}
OfxStatus Double2DParam::get( OfxTime t, double& x, double& y )
{
	double v[ 2 ];
	evalAt( t, v );
	x = v[ 0 ], y = v[ 1 ];
	return kOfxStatOK;
}
OfxStatus Double2DParam::set( double x, double y )
{
	const double v[ 2 ] = { x, y };
	storeNow( v );
	return kOfxStatOK;
}
OfxStatus Double2DParam::set( OfxTime t, double x, double y )
{
	const double v[ 2 ] = { x, y };
	storeAt( t, v );
	return kOfxStatOK;
}

Integer2DParam::Integer2DParam( OFX::Host::Param::Descriptor& d, OFX::Host::Param::SetInstance* s ) :
	OFX::Host::Param::Integer2DInstance( d, s ), NumericStore< 2 >( Interp::Round )
{
	for( int i = 0; i < 2; ++i )
		_v[ i ] = detail::defaultInt( getProperties(), i );
}
OfxStatus Integer2DParam::get( int& x, int& y )
{
	double v[ 2 ];
	evalNow( v );
	x = (int)v[ 0 ], y = (int)v[ 1 ];
	return kOfxStatOK;
}
OfxStatus Integer2DParam::get( OfxTime t, int& x, int& y )
{
	double v[ 2 ];
	evalAt( t, v );
	x = (int)v[ 0 ], y = (int)v[ 1 ];
	return kOfxStatOK;
}
OfxStatus Integer2DParam::set( int x, int y )
{
	const double v[ 2 ] = { (double)x, (double)y };
	storeNow( v );
	return kOfxStatOK;
}
OfxStatus Integer2DParam::set( OfxTime t, int x, int y )
{
	const double v[ 2 ] = { (double)x, (double)y };
	storeAt( t, v );
	return kOfxStatOK;
}

Double3DParam::Double3DParam( OFX::Host::Param::Descriptor& d, OFX::Host::Param::SetInstance* s ) :
	OFX::Host::Param::Double3DInstance( d, s ), NumericStore< 3 >( Interp::Linear )
{
	for( int i = 0; i < 3; ++i )
		_v[ i ] = detail::defaultDouble( getProperties(), i );
}
OfxStatus Double3DParam::get( double& x, double& y, double& z )
{
	double v[ 3 ];
	evalNow( v );
	x = v[ 0 ], y = v[ 1 ], z = v[ 2 ];
	return kOfxStatOK;
}
OfxStatus Double3DParam::get( OfxTime t, double& x, double& y, double& z )
{
	double v[ 3 ];
	evalAt( t, v );
	x = v[ 0 ], y = v[ 1 ], z = v[ 2 ];
	return kOfxStatOK;
}
OfxStatus Double3DParam::set( double x, double y, double z )
{
	const double v[ 3 ] = { x, y, z };
	storeNow( v );
	return kOfxStatOK;
}
OfxStatus Double3DParam::set( OfxTime t, double x, double y, double z )
{
	const double v[ 3 ] = { x, y, z };
	storeAt( t, v );
	return kOfxStatOK;
}

Integer3DParam::Integer3DParam( OFX::Host::Param::Descriptor& d, OFX::Host::Param::SetInstance* s ) :
	OFX::Host::Param::Integer3DInstance( d, s ), NumericStore< 3 >( Interp::Round )
{
	for( int i = 0; i < 3; ++i )
		_v[ i ] = detail::defaultInt( getProperties(), i );
}
OfxStatus Integer3DParam::get( int& x, int& y, int& z )
{
	double v[ 3 ];
	evalNow( v );
	x = (int)v[ 0 ], y = (int)v[ 1 ], z = (int)v[ 2 ];
	return kOfxStatOK;
}
OfxStatus Integer3DParam::get( OfxTime t, int& x, int& y, int& z )
{
	double v[ 3 ];
	evalAt( t, v );
	x = (int)v[ 0 ], y = (int)v[ 1 ], z = (int)v[ 2 ];
	return kOfxStatOK;
}
OfxStatus Integer3DParam::set( int x, int y, int z )
{
	const double v[ 3 ] = { (double)x, (double)y, (double)z };
	storeNow( v );
	return kOfxStatOK;
}
OfxStatus Integer3DParam::set( OfxTime t, int x, int y, int z )
{
	const double v[ 3 ] = { (double)x, (double)y, (double)z };
	storeAt( t, v );
	return kOfxStatOK;
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

namespace {
template< class T >
OFX::Host::Param::Instance* clocked( T* p, OFX::Host::Param::SetInstance* s )
{
	p->setClock( dynamic_cast< const TimeSource* >( s ) );
	return p;
}
} // namespace

OFX::Host::Param::Instance* makeParamInstance( const std::string& /*name*/,
											   OFX::Host::Param::Descriptor& d,
											   OFX::Host::Param::SetInstance* s )
{
	const std::string& t = d.getType();

	if( t == kOfxParamTypeInteger )
		return clocked( new IntegerParam( d, s ), s );
	if( t == kOfxParamTypeDouble )
		return clocked( new DoubleParam( d, s ), s );
	if( t == kOfxParamTypeBoolean )
		return clocked( new BooleanParam( d, s ), s );
	if( t == kOfxParamTypeChoice )
		return clocked( new ChoiceParam( d, s ), s );
	if( t == kOfxParamTypeRGBA )
		return clocked( new RGBAParam( d, s ), s );
	if( t == kOfxParamTypeRGB )
		return clocked( new RGBParam( d, s ), s );
	if( t == kOfxParamTypeDouble2D )
		return clocked( new Double2DParam( d, s ), s );
	if( t == kOfxParamTypeInteger2D )
		return clocked( new Integer2DParam( d, s ), s );
	if( t == kOfxParamTypeDouble3D )
		return clocked( new Double3DParam( d, s ), s );
	if( t == kOfxParamTypeInteger3D )
		return clocked( new Integer3DParam( d, s ), s );
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
