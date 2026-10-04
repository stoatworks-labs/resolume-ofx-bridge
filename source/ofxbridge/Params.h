#pragma once
//
// Concrete parameter instances.
//
// HostSupport declares one abstract class per OFX parameter type and leaves the
// storage to the host. These implementations are deliberately plain: a value (or
// a small fixed array of them). Resolume owns the automation and hands us a
// settled value per frame, so the bridge never sets keys.
//
// Numeric params can additionally carry linear
// keyframes, set only by the test host (ofxprobe --key / --transition-ramp).
// With no keys every path below behaves exactly as before: one constant value
// regardless of time. With keys, get(time) interpolates, get() answers for the
// effect's current time, and the OFX keyframe suite (getNumKeys/getKeyTime/
// getKeyIndex/deleteKey/deleteAllKeys) reports them.
//
// Every instance also exposes a uniform numeric accessor so the FFGL layer can
// push values in without knowing the OFX type it is talking to.
//

#include "ofxhParam.h"

#include <string>
#include <utility>
#include <vector>

namespace ofxbridge {

/// Who answers "what time is it now" for a param read with no time argument.
/// Implemented by Effect; looked up from the param's SetInstance.
class TimeSource
{
public:
	virtual ~TimeSource() = default;
	virtual double paramTime() const = 0;
};

/// One keyframe: a time (in frames) and the value of every component.
using ParamKey = std::pair< double, std::vector< double > >;

/// Type-agnostic access to a parameter's value, used by the FFGL layer.
///
/// Numeric params (including colours and 2D/3D vectors) are addressed as an
/// array of doubles; string params go through the string overloads. This keeps
/// the FFGL-side param table a flat list of floats regardless of OFX type.
class ValueAccess
{
public:
	virtual ~ValueAccess() = default;

	/// Number of numeric components (0 for string-valued params).
	virtual int componentCount() const
	{
		return 0;
	}
	virtual void getValues( std::vector< double >& out ) const
	{
		(void)out;
	}
	virtual void setValues( const std::vector< double >& in )
	{
		(void)in;
	}

	/// Value at an arbitrary time (equal to getValues when there are no keys).
	virtual void getValuesAt( double time, std::vector< double >& out ) const
	{
		(void)time;
		getValues( out );
	}

	/// Replace the param's animation with `keys` (empty = back to constant).
	/// Returns false for params that cannot be keyed.
	virtual bool setKeys( const std::vector< ParamKey >& keys )
	{
		(void)keys;
		return false;
	}
	virtual size_t keyCount() const
	{
		return 0;
	}

	virtual bool isString() const
	{
		return false;
	}
	virtual std::string getString() const
	{
		return std::string();
	}
	virtual void setString( const std::string& s )
	{
		(void)s;
	}
};

namespace detail {

/// Reads component `i` of a param's default property, tolerating a missing or
/// short property (plugins routinely omit defaults for extra components).
double defaultDouble( const OFX::Host::Property::Set& props, int i );
int defaultInt( const OFX::Host::Property::Set& props, int i );

} // namespace detail

// ---------------------------------------------------------------------------
// Numeric instances
//
// The N-component classes share their storage, keyframes and ValueAccess
// implementation through this small base; the OFX get/set overloads still have
// to be written out per type because their arities differ.
// ---------------------------------------------------------------------------

enum class Interp
{
	Linear,// doubles, colours, double vectors
	Round, // integers: linear, then rounded
	Step   // choice, boolean: hold the previous key
};

template< int N >
class NumericStore : public ValueAccess
{
public:
	explicit NumericStore( Interp interp = Interp::Linear ) : _interp( interp )
	{
	}

	void setClock( const TimeSource* clock )
	{
		_clock = clock;
	}

	int componentCount() const override
	{
		return N;
	}
	void getValues( std::vector< double >& out ) const override
	{
		double v[ N ];
		evalNow( v );
		out.assign( v, v + N );
	}
	/// A plain set makes the param constant again (drops any keys).
	void setValues( const std::vector< double >& in ) override
	{
		_keys.clear();
		for( int i = 0; i < N && i < (int)in.size(); ++i )
			_v[ i ] = in[ i ];
	}
	void getValuesAt( double time, std::vector< double >& out ) const override
	{
		double v[ N ];
		evalAt( time, v );
		out.assign( v, v + N );
	}
	bool setKeys( const std::vector< ParamKey >& keys ) override;
	size_t keyCount() const override
	{
		return _keys.size();
	}

	// -- shared implementation for the concrete classes -----------------------
	void evalAt( double t, double* out ) const;
	void evalNow( double* out ) const
	{
		if( _keys.empty() )
		{
			for( int i = 0; i < N; ++i )
				out[ i ] = _v[ i ];
			return;
		}
		evalAt( _clock ? _clock->paramTime() : 0.0, out );
	}
	/// set(value): constant, as before.
	void storeNow( const double* in )
	{
		_keys.clear();
		for( int i = 0; i < N; ++i )
			_v[ i ] = in[ i ];
	}
	/// set(time, value): constant if unkeyed (as before); else add/replace a key.
	void storeAt( double t, const double* in );

	OfxStatus kfNum( unsigned int& n ) const
	{
		n = (unsigned int)_keys.size();
		return kOfxStatOK;
	}
	OfxStatus kfTime( int nth, OfxTime& time ) const
	{
		if( nth < 0 || nth >= (int)_keys.size() )
			return kOfxStatErrBadIndex;
		time = _keys[ (size_t)nth ].first;
		return kOfxStatOK;
	}
	OfxStatus kfIndex( OfxTime time, int direction, int& index ) const;
	OfxStatus kfDelete( OfxTime time );
	OfxStatus kfDeleteAll()
	{
		_keys.clear();
		return kOfxStatOK;
	}

	/// Slope of component 0 at t (0 when constant or outside the keys).
	double slope0( double t ) const;
	/// Integral of component 0 over [t1, t2] (exact for piecewise linear).
	double integral0( double t1, double t2 ) const;

protected:
	double _v[ N ] = {};
	std::vector< ParamKey > _keys;// sorted by time
	Interp _interp;
	const TimeSource* _clock = nullptr;
};

/// The OFX keyframe-suite overrides every keyable concrete class needs, all
/// forwarding to NumericStore.
#define OFXBRIDGE_KEYFRAME_FORWARDS                                                    \
	OfxStatus getNumKeys( unsigned int& n ) const override                             \
	{                                                                                  \
		return kfNum( n );                                                             \
	}                                                                                  \
	OfxStatus getKeyTime( int nth, OfxTime& time ) const override                      \
	{                                                                                  \
		return kfTime( nth, time );                                                    \
	}                                                                                  \
	OfxStatus getKeyIndex( OfxTime time, int direction, int& index ) const override    \
	{                                                                                  \
		return kfIndex( time, direction, index );                                      \
	}                                                                                  \
	OfxStatus deleteKey( OfxTime time ) override                                       \
	{                                                                                  \
		return kfDelete( time );                                                       \
	}                                                                                  \
	OfxStatus deleteAllKeys() override                                                 \
	{                                                                                  \
		return kfDeleteAll();                                                          \
	}

class IntegerParam : public OFX::Host::Param::IntegerInstance, public NumericStore< 1 >
{
public:
	IntegerParam( OFX::Host::Param::Descriptor& d, OFX::Host::Param::SetInstance* s );
	OfxStatus get( int& v ) override;
	OfxStatus get( OfxTime, int& v ) override;
	OfxStatus set( int v ) override;
	OfxStatus set( OfxTime, int v ) override;
	OFXBRIDGE_KEYFRAME_FORWARDS
};

class ChoiceParam : public OFX::Host::Param::ChoiceInstance, public NumericStore< 1 >
{
public:
	ChoiceParam( OFX::Host::Param::Descriptor& d, OFX::Host::Param::SetInstance* s );
	OfxStatus get( int& v ) override;
	OfxStatus get( OfxTime, int& v ) override;
	OfxStatus set( int v ) override;
	OfxStatus set( OfxTime, int v ) override;
	OFXBRIDGE_KEYFRAME_FORWARDS
};

class DoubleParam : public OFX::Host::Param::DoubleInstance, public NumericStore< 1 >
{
public:
	DoubleParam( OFX::Host::Param::Descriptor& d, OFX::Host::Param::SetInstance* s );
	OfxStatus get( double& v ) override;
	OfxStatus get( OfxTime, double& v ) override;
	OfxStatus set( double v ) override;
	OfxStatus set( OfxTime, double v ) override;
	OfxStatus derive( OfxTime, double& v ) override;
	OfxStatus integrate( OfxTime, OfxTime, double& v ) override;
	OFXBRIDGE_KEYFRAME_FORWARDS
};

class BooleanParam : public OFX::Host::Param::BooleanInstance, public NumericStore< 1 >
{
public:
	BooleanParam( OFX::Host::Param::Descriptor& d, OFX::Host::Param::SetInstance* s );
	OfxStatus get( bool& v ) override;
	OfxStatus get( OfxTime, bool& v ) override;
	OfxStatus set( bool v ) override;
	OfxStatus set( OfxTime, bool v ) override;
	OFXBRIDGE_KEYFRAME_FORWARDS
};

class RGBAParam : public OFX::Host::Param::RGBAInstance, public NumericStore< 4 >
{
public:
	RGBAParam( OFX::Host::Param::Descriptor& d, OFX::Host::Param::SetInstance* s );
	OfxStatus get( double& r, double& g, double& b, double& a ) override;
	OfxStatus get( OfxTime, double& r, double& g, double& b, double& a ) override;
	OfxStatus set( double r, double g, double b, double a ) override;
	OfxStatus set( OfxTime, double r, double g, double b, double a ) override;
	OFXBRIDGE_KEYFRAME_FORWARDS
};

class RGBParam : public OFX::Host::Param::RGBInstance, public NumericStore< 3 >
{
public:
	RGBParam( OFX::Host::Param::Descriptor& d, OFX::Host::Param::SetInstance* s );
	OfxStatus get( double& r, double& g, double& b ) override;
	OfxStatus get( OfxTime, double& r, double& g, double& b ) override;
	OfxStatus set( double r, double g, double b ) override;
	OfxStatus set( OfxTime, double r, double g, double b ) override;
	OFXBRIDGE_KEYFRAME_FORWARDS
};

class Double2DParam : public OFX::Host::Param::Double2DInstance, public NumericStore< 2 >
{
public:
	Double2DParam( OFX::Host::Param::Descriptor& d, OFX::Host::Param::SetInstance* s );
	OfxStatus get( double& x, double& y ) override;
	OfxStatus get( OfxTime, double& x, double& y ) override;
	OfxStatus set( double x, double y ) override;
	OfxStatus set( OfxTime, double x, double y ) override;
	OFXBRIDGE_KEYFRAME_FORWARDS
};

class Integer2DParam : public OFX::Host::Param::Integer2DInstance, public NumericStore< 2 >
{
public:
	Integer2DParam( OFX::Host::Param::Descriptor& d, OFX::Host::Param::SetInstance* s );
	OfxStatus get( int& x, int& y ) override;
	OfxStatus get( OfxTime, int& x, int& y ) override;
	OfxStatus set( int x, int y ) override;
	OfxStatus set( OfxTime, int x, int y ) override;
	OFXBRIDGE_KEYFRAME_FORWARDS
};

class Double3DParam : public OFX::Host::Param::Double3DInstance, public NumericStore< 3 >
{
public:
	Double3DParam( OFX::Host::Param::Descriptor& d, OFX::Host::Param::SetInstance* s );
	OfxStatus get( double& x, double& y, double& z ) override;
	OfxStatus get( OfxTime, double& x, double& y, double& z ) override;
	OfxStatus set( double x, double y, double z ) override;
	OfxStatus set( OfxTime, double x, double y, double z ) override;
	OFXBRIDGE_KEYFRAME_FORWARDS
};

class Integer3DParam : public OFX::Host::Param::Integer3DInstance, public NumericStore< 3 >
{
public:
	Integer3DParam( OFX::Host::Param::Descriptor& d, OFX::Host::Param::SetInstance* s );
	OfxStatus get( int& x, int& y, int& z ) override;
	OfxStatus get( OfxTime, int& x, int& y, int& z ) override;
	OfxStatus set( int x, int y, int z ) override;
	OfxStatus set( OfxTime, int x, int y, int z ) override;
	OFXBRIDGE_KEYFRAME_FORWARDS
};

/// Also serves the Custom param type, which is a String with a plugin-defined
/// interpretation the host never has to understand.
class StringParam : public OFX::Host::Param::StringInstance, public ValueAccess
{
public:
	StringParam( OFX::Host::Param::Descriptor& d, OFX::Host::Param::SetInstance* s );
	OfxStatus get( std::string& v ) override;
	OfxStatus get( OfxTime, std::string& v ) override;
	OfxStatus set( const char* v ) override;
	OfxStatus set( OfxTime, const char* v ) override;

	bool isString() const override
	{
		return true;
	}
	std::string getString() const override
	{
		return _v;
	}
	void setString( const std::string& s ) override
	{
		_v = s;
	}

private:
	std::string _v;
};

/// Group, Page and Pushbutton carry no value; HostSupport's classes are already
/// concrete, so these exist only so newParam() can return something typed.
class GroupParam : public OFX::Host::Param::GroupInstance, public ValueAccess
{
public:
	GroupParam( OFX::Host::Param::Descriptor& d, OFX::Host::Param::SetInstance* s ) :
		OFX::Host::Param::GroupInstance( d, s )
	{
	}
};

class PageParam : public OFX::Host::Param::PageInstance, public ValueAccess
{
public:
	PageParam( OFX::Host::Param::Descriptor& d, OFX::Host::Param::SetInstance* s ) :
		OFX::Host::Param::PageInstance( d, s )
	{
	}
};

class PushbuttonParam : public OFX::Host::Param::PushbuttonInstance, public ValueAccess
{
public:
	PushbuttonParam( OFX::Host::Param::Descriptor& d, OFX::Host::Param::SetInstance* s ) :
		OFX::Host::Param::PushbuttonInstance( d, s )
	{
	}
};

/// Build the right instance for a descriptor's type. Returns nullptr for types
/// we deliberately do not host (currently only Parametric).
OFX::Host::Param::Instance* makeParamInstance( const std::string& name,
											   OFX::Host::Param::Descriptor& descriptor,
											   OFX::Host::Param::SetInstance* setInstance );

} // namespace ofxbridge
