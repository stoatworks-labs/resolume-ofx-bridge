#pragma once
//
// A small JSON reader.
//
// The generated FFGL bundle has to parse its manifest while it is being loaded
// by Resolume, before any of our own code has really started. Pulling in a
// third-party JSON library for that would mean shipping and initialising it in
// that same fragile window, so this is a self-contained parser sized to the one
// schema we control.
//
// Strict enough to reject a truncated or corrupted manifest rather than silently
// producing a plugin with no parameters.
//

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace ofxffgl {

class Json
{
public:
	enum class Type
	{
		Null,
		Bool,
		Number,
		String,
		Array,
		Object
	};

	Type type = Type::Null;
	bool boolValue    = false;
	double numberValue = 0.0;
	std::string stringValue;
	std::vector< Json > arrayValue;
	std::map< std::string, Json > objectValue;

	/// Look up a key on an object. Returns nullptr if absent or not an object.
	const Json* find( const std::string& key ) const;

	// Accessors that fall back rather than throw: a manifest missing an optional
	// field should degrade, not abort the load.
	double number( double fallback = 0.0 ) const;
	std::string string( const std::string& fallback = std::string() ) const;
	bool boolean( bool fallback = false ) const;

	/// Numbers out of an array field, e.g. "defaults": [1, 0, 0, 1].
	std::vector< double > numberArray() const;
	std::vector< std::string > stringArray() const;

	/// Parse `text`. On failure returns false and sets `error`.
	static bool parse( const std::string& text, Json& out, std::string& error );

	/// Read and parse a file.
	static bool parseFile( const std::string& path, Json& out, std::string& error );
};

} // namespace ofxffgl
