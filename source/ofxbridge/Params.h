#pragma once
//
// Concrete parameter instances.
//
// HostSupport declares one abstract class per OFX parameter type and leaves the
// storage to the host. These implementations are deliberately plain: a value (or
// a small fixed array of them) with no animation. Resolume owns the automation
// and hands us a settled value per frame, so keyframes here would be a second,
// conflicting source of truth.
//
// Every instance also exposes a uniform numeric accessor so the FFGL layer can
// push values in without knowing the OFX type it is talking to.
//

#include "ofxhParam.h"

#include <string>
#include <vector>

namespace ofxbridge {

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
// The N-component classes share their storage and ValueAccess implementation
// through this small CRTP-free base; the OFX get/set overloads still have to be
// written out per type because their arities differ.
// ---------------------------------------------------------------------------

template< int N >
class NumericStore : public ValueAccess
{
public:
	int componentCount() const override
	{
		return N;
	}
	void getValues( std::vector< double >& out ) const override
	{
		out.assign( _v, _v + N );
	}
	void setValues( const std::vector< double >& in ) override
	{
		for( int i = 0; i < N && i < (int)in.size(); ++i )
			_v[ i ] = in[ i ];
	}

protected:
	double _v[ N ] = {};
};

class IntegerParam : public OFX::Host::Param::IntegerInstance, public NumericStore< 1 >
{
public:
	IntegerParam( OFX::Host::Param::Descriptor& d, OFX::Host::Param::SetInstance* s );
	OfxStatus get( int& v ) override;
	OfxStatus get( OfxTime, int& v ) override;
	OfxStatus set( int v ) override;
	OfxStatus set( OfxTime, int v ) override;
};

class ChoiceParam : public OFX::Host::Param::ChoiceInstance, public NumericStore< 1 >
{
public:
	ChoiceParam( OFX::Host::Param::Descriptor& d, OFX::Host::Param::SetInstance* s );
	OfxStatus get( int& v ) override;
	OfxStatus get( OfxTime, int& v ) override;
	OfxStatus set( int v ) override;
	OfxStatus set( OfxTime, int v ) override;
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
};

class BooleanParam : public OFX::Host::Param::BooleanInstance, public NumericStore< 1 >
{
public:
	BooleanParam( OFX::Host::Param::Descriptor& d, OFX::Host::Param::SetInstance* s );
	OfxStatus get( bool& v ) override;
	OfxStatus get( OfxTime, bool& v ) override;
	OfxStatus set( bool v ) override;
	OfxStatus set( OfxTime, bool v ) override;
};

class RGBAParam : public OFX::Host::Param::RGBAInstance, public NumericStore< 4 >
{
public:
	RGBAParam( OFX::Host::Param::Descriptor& d, OFX::Host::Param::SetInstance* s );
	OfxStatus get( double& r, double& g, double& b, double& a ) override;
	OfxStatus get( OfxTime, double& r, double& g, double& b, double& a ) override;
	OfxStatus set( double r, double g, double b, double a ) override;
	OfxStatus set( OfxTime, double r, double g, double b, double a ) override;
};

class RGBParam : public OFX::Host::Param::RGBInstance, public NumericStore< 3 >
{
public:
	RGBParam( OFX::Host::Param::Descriptor& d, OFX::Host::Param::SetInstance* s );
	OfxStatus get( double& r, double& g, double& b ) override;
	OfxStatus get( OfxTime, double& r, double& g, double& b ) override;
	OfxStatus set( double r, double g, double b ) override;
	OfxStatus set( OfxTime, double r, double g, double b ) override;
};

class Double2DParam : public OFX::Host::Param::Double2DInstance, public NumericStore< 2 >
{
public:
	Double2DParam( OFX::Host::Param::Descriptor& d, OFX::Host::Param::SetInstance* s );
	OfxStatus get( double& x, double& y ) override;
	OfxStatus get( OfxTime, double& x, double& y ) override;
	OfxStatus set( double x, double y ) override;
	OfxStatus set( OfxTime, double x, double y ) override;
};

class Integer2DParam : public OFX::Host::Param::Integer2DInstance, public NumericStore< 2 >
{
public:
	Integer2DParam( OFX::Host::Param::Descriptor& d, OFX::Host::Param::SetInstance* s );
	OfxStatus get( int& x, int& y ) override;
	OfxStatus get( OfxTime, int& x, int& y ) override;
	OfxStatus set( int x, int y ) override;
	OfxStatus set( OfxTime, int x, int y ) override;
};

class Double3DParam : public OFX::Host::Param::Double3DInstance, public NumericStore< 3 >
{
public:
	Double3DParam( OFX::Host::Param::Descriptor& d, OFX::Host::Param::SetInstance* s );
	OfxStatus get( double& x, double& y, double& z ) override;
	OfxStatus get( OfxTime, double& x, double& y, double& z ) override;
	OfxStatus set( double x, double y, double z ) override;
	OfxStatus set( OfxTime, double x, double y, double z ) override;
};

class Integer3DParam : public OFX::Host::Param::Integer3DInstance, public NumericStore< 3 >
{
public:
	Integer3DParam( OFX::Host::Param::Descriptor& d, OFX::Host::Param::SetInstance* s );
	OfxStatus get( int& x, int& y, int& z ) override;
	OfxStatus get( OfxTime, int& x, int& y, int& z ) override;
	OfxStatus set( int x, int y, int z ) override;
	OfxStatus set( OfxTime, int x, int y, int z ) override;
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
