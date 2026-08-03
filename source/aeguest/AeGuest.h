#pragma once
//
// C++ face of the Rust AE-guest host (aeguest/). The Rust side owns every
// After Effects structure — the community -sys bindings give it exact layouts
// — and this wrapper sees only a narrow C ABI: open, describe (JSON), set a
// param, render RGBA8 frames.
//

#include <string>
#include <vector>

namespace aeguest {

struct GuestParam
{
	int index = 0;          //!< position in the effect's own parameter list
	std::string name;
	std::string kind;       //!< "float" | "bool" | "popup" | "color" | "unsupported"
	double defaultValue = 0.0;
	double rangeMin     = 0.0;
	double rangeMax     = 1.0;
	std::vector< std::string > options;//!< popup only
};

class AeGuest
{
public:
	~AeGuest();

	AeGuest()                            = default;
	AeGuest( const AeGuest& )            = delete;
	AeGuest& operator=( const AeGuest& ) = delete;

	/// Load the .plugin, run GLOBAL_SETUP + PARAMS_SETUP, read the parameters.
	bool open( const std::string& bundlePath, std::string& error );

	const std::vector< GuestParam >& params() const { return guestParams; }

	void setParam( int index, double value );

	/// Render one frame; buffers are width*height*4 RGBA8, row 0 first.
	/// `rgbaIn` may be null for a blank input layer.
	bool render( const uint8_t* rgbaIn, uint8_t* rgbaOut, int width, int height,
				 double frame, double fps, std::string& error );

private:
	void* guest = nullptr;//!< the Rust Guest, opaque
	std::vector< GuestParam > guestParams;
};

} // namespace aeguest
