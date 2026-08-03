#include "AeGuest.h"

#include "../ffgl/Json.h"

extern "C" {
void* aeg_open( const char* path );
char* aeg_describe_json( void* guest );
void aeg_free_string( char* s );
void aeg_set_param( void* guest, int index, double value );
int aeg_render( void* guest, const uint8_t* in_rgba, uint8_t* out_rgba, int width, int height,
				double frame, double fps );
const char* aeg_last_error( void* guest );
void aeg_close( void* guest );
}

namespace aeguest {

AeGuest::~AeGuest()
{
	if( guest != nullptr )
		aeg_close( guest );
}

bool AeGuest::open( const std::string& bundlePath, std::string& error )
{
	guest = aeg_open( bundlePath.c_str() );
	if( guest == nullptr )
	{
		error = "could not open " + bundlePath + " as an After Effects plugin (see stderr)";
		return false;
	}

	char* json = aeg_describe_json( guest );
	if( json == nullptr )
	{
		error = "describe failed";
		return false;
	}
	const std::string text = json;
	aeg_free_string( json );

	ofxffgl::Json root;
	if( !ofxffgl::Json::parse( text, root, error ) )
		return false;

	const ofxffgl::Json* params = root.find( "params" );
	if( params != nullptr )
	{
		for( const ofxffgl::Json& p : params->arrayValue )
		{
			GuestParam gp;
			if( const ofxffgl::Json* v = p.find( "index" ) )
				gp.index = (int)v->number( 0 );
			if( const ofxffgl::Json* v = p.find( "name" ) )
				gp.name = v->string();
			if( const ofxffgl::Json* v = p.find( "kind" ) )
				gp.kind = v->string( "float" );
			if( const ofxffgl::Json* v = p.find( "default" ) )
				gp.defaultValue = v->number( 0.0 );
			if( const ofxffgl::Json* v = p.find( "min" ) )
				gp.rangeMin = v->number( 0.0 );
			if( const ofxffgl::Json* v = p.find( "max" ) )
				gp.rangeMax = v->number( 1.0 );
			if( const ofxffgl::Json* v = p.find( "options" ) )
				gp.options = v->stringArray();
			guestParams.push_back( std::move( gp ) );
		}
	}
	return true;
}

void AeGuest::setParam( int index, double value )
{
	if( guest != nullptr )
		aeg_set_param( guest, index, value );
}

bool AeGuest::render( const uint8_t* rgbaIn, uint8_t* rgbaOut, int width, int height,
					  double frame, double fps, std::string& error )
{
	if( guest == nullptr )
	{
		error = "no guest";
		return false;
	}
	if( aeg_render( guest, rgbaIn, rgbaOut, width, height, frame, fps ) != 0 )
	{
		error = aeg_last_error( guest );
		return false;
	}
	return true;
}

} // namespace aeguest
