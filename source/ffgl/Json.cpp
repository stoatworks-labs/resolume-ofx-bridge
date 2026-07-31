#include "Json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace ofxffgl {

const Json* Json::find( const std::string& key ) const
{
	if( type != Type::Object )
		return nullptr;
	auto it = objectValue.find( key );
	return it == objectValue.end() ? nullptr : &it->second;
}

double Json::number( double fallback ) const
{
	return type == Type::Number ? numberValue : fallback;
}

std::string Json::string( const std::string& fallback ) const
{
	return type == Type::String ? stringValue : fallback;
}

bool Json::boolean( bool fallback ) const
{
	if( type == Type::Bool )
		return boolValue;
	// A manifest may encode a flag as 0/1; accept that too.
	if( type == Type::Number )
		return numberValue != 0.0;
	return fallback;
}

std::vector< double > Json::numberArray() const
{
	std::vector< double > out;
	if( type != Type::Array )
		return out;
	out.reserve( arrayValue.size() );
	for( const Json& v : arrayValue )
		if( v.type == Type::Number )
			out.push_back( v.numberValue );
	return out;
}

std::vector< std::string > Json::stringArray() const
{
	std::vector< std::string > out;
	if( type != Type::Array )
		return out;
	out.reserve( arrayValue.size() );
	for( const Json& v : arrayValue )
		if( v.type == Type::String )
			out.push_back( v.stringValue );
	return out;
}

// ---------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------

namespace {

struct Parser
{
	const char* p;
	const char* end;
	std::string error;

	bool fail( const char* what )
	{
		if( error.empty() )
		{
			char buf[ 128 ];
			snprintf( buf, sizeof( buf ), "%s at offset %ld", what, (long)( p - start ) );
			error = buf;
		}
		return false;
	}

	const char* start;

	void skipWhitespace()
	{
		while( p < end && ( *p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' ) )
			++p;
	}

	bool parseValue( Json& out );
	bool parseString( std::string& out );
	bool parseNumber( Json& out );
	bool parseArray( Json& out );
	bool parseObject( Json& out );
	bool parseLiteral( const char* lit, size_t n );
};

bool Parser::parseLiteral( const char* lit, size_t n )
{
	if( (size_t)( end - p ) < n )
		return false;
	for( size_t i = 0; i < n; ++i )
		if( p[ i ] != lit[ i ] )
			return false;
	p += n;
	return true;
}

/// Appends `cp` to `out` as UTF-8.
void appendUtf8( std::string& out, uint32_t cp )
{
	if( cp < 0x80 )
	{
		out += (char)cp;
	}
	else if( cp < 0x800 )
	{
		out += (char)( 0xC0 | ( cp >> 6 ) );
		out += (char)( 0x80 | ( cp & 0x3F ) );
	}
	else if( cp < 0x10000 )
	{
		out += (char)( 0xE0 | ( cp >> 12 ) );
		out += (char)( 0x80 | ( ( cp >> 6 ) & 0x3F ) );
		out += (char)( 0x80 | ( cp & 0x3F ) );
	}
	else
	{
		out += (char)( 0xF0 | ( cp >> 18 ) );
		out += (char)( 0x80 | ( ( cp >> 12 ) & 0x3F ) );
		out += (char)( 0x80 | ( ( cp >> 6 ) & 0x3F ) );
		out += (char)( 0x80 | ( cp & 0x3F ) );
	}
}

bool parseHex4( const char*& p, const char* end, uint32_t& out )
{
	if( end - p < 4 )
		return false;
	out = 0;
	for( int i = 0; i < 4; ++i )
	{
		const char c = p[ i ];
		out <<= 4;
		if( c >= '0' && c <= '9' )
			out |= (uint32_t)( c - '0' );
		else if( c >= 'a' && c <= 'f' )
			out |= (uint32_t)( c - 'a' + 10 );
		else if( c >= 'A' && c <= 'F' )
			out |= (uint32_t)( c - 'A' + 10 );
		else
			return false;
	}
	p += 4;
	return true;
}

bool Parser::parseString( std::string& out )
{
	if( p >= end || *p != '"' )
		return fail( "expected string" );
	++p;

	out.clear();
	while( p < end )
	{
		const char c = *p;
		if( c == '"' )
		{
			++p;
			return true;
		}
		if( c == '\\' )
		{
			++p;
			if( p >= end )
				return fail( "truncated escape" );
			switch( *p )
			{
			case '"': out += '"'; ++p; break;
			case '\\': out += '\\'; ++p; break;
			case '/': out += '/'; ++p; break;
			case 'b': out += '\b'; ++p; break;
			case 'f': out += '\f'; ++p; break;
			case 'n': out += '\n'; ++p; break;
			case 'r': out += '\r'; ++p; break;
			case 't': out += '\t'; ++p; break;
			case 'u':
			{
				++p;
				uint32_t cp = 0;
				if( !parseHex4( p, end, cp ) )
					return fail( "bad \\u escape" );
				// Surrogate pair.
				if( cp >= 0xD800 && cp <= 0xDBFF && end - p >= 6 && p[ 0 ] == '\\' && p[ 1 ] == 'u' )
				{
					const char* save = p;
					p += 2;
					uint32_t lo = 0;
					if( parseHex4( p, end, lo ) && lo >= 0xDC00 && lo <= 0xDFFF )
						cp = 0x10000 + ( ( cp - 0xD800 ) << 10 ) + ( lo - 0xDC00 );
					else
						p = save;
				}
				appendUtf8( out, cp );
				break;
			}
			default: return fail( "unknown escape" );
			}
			continue;
		}
		out += c;
		++p;
	}
	return fail( "unterminated string" );
}

bool Parser::parseNumber( Json& out )
{
	const char* first = p;
	if( p < end && ( *p == '-' || *p == '+' ) )
		++p;
	while( p < end && ( ( *p >= '0' && *p <= '9' ) || *p == '.' || *p == 'e' || *p == 'E' || *p == '+' || *p == '-' ) )
		++p;
	if( p == first )
		return fail( "expected number" );

	const std::string text( first, p );
	char* endPtr = nullptr;
	const double v = strtod( text.c_str(), &endPtr );
	if( endPtr == text.c_str() )
		return fail( "malformed number" );

	out.type        = Json::Type::Number;
	out.numberValue = v;
	return true;
}

bool Parser::parseArray( Json& out )
{
	++p;// consume '['
	out.type = Json::Type::Array;
	skipWhitespace();
	if( p < end && *p == ']' )
	{
		++p;
		return true;
	}
	while( p < end )
	{
		Json item;
		if( !parseValue( item ) )
			return false;
		out.arrayValue.push_back( std::move( item ) );
		skipWhitespace();
		if( p < end && *p == ',' )
		{
			++p;
			skipWhitespace();
			continue;
		}
		if( p < end && *p == ']' )
		{
			++p;
			return true;
		}
		return fail( "expected ',' or ']'" );
	}
	return fail( "unterminated array" );
}

bool Parser::parseObject( Json& out )
{
	++p;// consume '{'
	out.type = Json::Type::Object;
	skipWhitespace();
	if( p < end && *p == '}' )
	{
		++p;
		return true;
	}
	while( p < end )
	{
		skipWhitespace();
		std::string key;
		if( !parseString( key ) )
			return false;
		skipWhitespace();
		if( p >= end || *p != ':' )
			return fail( "expected ':'" );
		++p;
		skipWhitespace();

		Json value;
		if( !parseValue( value ) )
			return false;
		out.objectValue[ key ] = std::move( value );

		skipWhitespace();
		if( p < end && *p == ',' )
		{
			++p;
			continue;
		}
		if( p < end && *p == '}' )
		{
			++p;
			return true;
		}
		return fail( "expected ',' or '}'" );
	}
	return fail( "unterminated object" );
}

bool Parser::parseValue( Json& out )
{
	skipWhitespace();
	if( p >= end )
		return fail( "unexpected end of input" );

	switch( *p )
	{
	case '{': return parseObject( out );
	case '[': return parseArray( out );
	case '"':
		out.type = Json::Type::String;
		return parseString( out.stringValue );
	case 't':
		if( !parseLiteral( "true", 4 ) )
			return fail( "expected 'true'" );
		out.type      = Json::Type::Bool;
		out.boolValue = true;
		return true;
	case 'f':
		if( !parseLiteral( "false", 5 ) )
			return fail( "expected 'false'" );
		out.type      = Json::Type::Bool;
		out.boolValue = false;
		return true;
	case 'n':
		if( !parseLiteral( "null", 4 ) )
			return fail( "expected 'null'" );
		out.type = Json::Type::Null;
		return true;
	default: return parseNumber( out );
	}
}

} // namespace

bool Json::parse( const std::string& text, Json& out, std::string& error )
{
	Parser parser;
	parser.start = text.c_str();
	parser.p     = text.c_str();
	parser.end   = text.c_str() + text.size();

	out = Json();
	if( !parser.parseValue( out ) )
	{
		error = parser.error.empty() ? "parse failed" : parser.error;
		return false;
	}

	parser.skipWhitespace();
	if( parser.p != parser.end )
	{
		error = "trailing content after JSON value";
		return false;
	}
	return true;
}

bool Json::parseFile( const std::string& path, Json& out, std::string& error )
{
	FILE* f = fopen( path.c_str(), "rb" );
	if( f == nullptr )
	{
		error = "cannot open " + path;
		return false;
	}

	std::string text;
	char buf[ 8192 ];
	size_t n;
	while( ( n = fread( buf, 1, sizeof( buf ), f ) ) > 0 )
		text.append( buf, n );
	fclose( f );

	return parse( text, out, error );
}

} // namespace ofxffgl
