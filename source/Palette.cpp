#include "Palette.h"

#include <algorithm>
#include <cmath>

namespace tinsel
{
namespace
{
struct Stop
{
	float position;
	float r, g, b;
};

/**
    The palettes.

    Two rules were followed throughout, and both are worth knowing before
    editing one.

    **A palette must not go to black unless black is the point.** An LED that
    lands on a black stop is an LED that has failed, and on a sparse strip a
    dark stop reads as a dead bulb rather than as a colour. Fire is the one
    deliberate exception: its black end is what makes the embers work.

    **The stops are placed where the eye puts the boundary, not where the
    arithmetic does.** Candy Cane's red runs to 0.45 and its white from 0.55
    because equal thirds of red/white/red looked pink at a distance; Christmas
    holds red and green wide and lets white through only briefly, because an
    even three-way split reads as a flag rather than as a tree.
*/
const Stop kRainbow[] = {
	{ 0.000f, 1.00f, 0.00f, 0.00f },
	{ 0.167f, 1.00f, 1.00f, 0.00f },
	{ 0.333f, 0.00f, 1.00f, 0.00f },
	{ 0.500f, 0.00f, 1.00f, 1.00f },
	{ 0.667f, 0.00f, 0.00f, 1.00f },
	{ 0.833f, 1.00f, 0.00f, 1.00f },
	{ 1.000f, 1.00f, 0.00f, 0.00f },
};

//Warm end of the spectrum only, no green or blue. This is the one that makes a
//generic "colourful" look read as festive rather than as a test pattern.
const Stop kParty[] = {
	{ 0.000f, 0.34f, 0.00f, 0.66f },
	{ 0.200f, 0.78f, 0.00f, 0.35f },
	{ 0.400f, 1.00f, 0.10f, 0.00f },
	{ 0.600f, 1.00f, 0.45f, 0.00f },
	{ 0.800f, 1.00f, 0.80f, 0.05f },
	{ 1.000f, 0.34f, 0.00f, 0.66f },
};

const Stop kChristmas[] = {
	{ 0.000f, 0.90f, 0.03f, 0.03f },
	{ 0.400f, 0.90f, 0.03f, 0.03f },
	{ 0.460f, 1.00f, 0.85f, 0.60f },
	{ 0.540f, 1.00f, 0.85f, 0.60f },
	{ 0.600f, 0.02f, 0.65f, 0.10f },
	{ 1.000f, 0.02f, 0.65f, 0.10f },
};

const Stop kCandyCane[] = {
	{ 0.000f, 0.95f, 0.02f, 0.06f },
	{ 0.450f, 0.95f, 0.02f, 0.06f },
	{ 0.550f, 1.00f, 0.96f, 0.92f },
	{ 1.000f, 1.00f, 0.96f, 0.92f },
};

//Tungsten through to a cool filament white. The narrow range is the point: it
//is the palette for when the effect should read as bulbs rather than as colour.
const Stop kWarmWhite[] = {
	{ 0.000f, 1.00f, 0.58f, 0.18f },
	{ 0.400f, 1.00f, 0.78f, 0.46f },
	{ 0.750f, 1.00f, 0.92f, 0.78f },
	{ 1.000f, 1.00f, 0.99f, 0.95f },
};

const Stop kFrost[] = {
	{ 0.000f, 0.02f, 0.10f, 0.45f },
	{ 0.350f, 0.05f, 0.45f, 0.85f },
	{ 0.700f, 0.45f, 0.85f, 1.00f },
	{ 1.000f, 0.92f, 0.99f, 1.00f },
};

//The one palette allowed to reach black, because embers are what it is for.
const Stop kFire[] = {
	{ 0.000f, 0.00f, 0.00f, 0.00f },
	{ 0.180f, 0.55f, 0.02f, 0.00f },
	{ 0.450f, 1.00f, 0.16f, 0.00f },
	{ 0.720f, 1.00f, 0.60f, 0.02f },
	{ 1.000f, 1.00f, 0.98f, 0.72f },
};

const Stop kOcean[] = {
	{ 0.000f, 0.00f, 0.06f, 0.28f },
	{ 0.300f, 0.00f, 0.28f, 0.52f },
	{ 0.600f, 0.02f, 0.62f, 0.60f },
	{ 0.850f, 0.35f, 0.85f, 0.72f },
	{ 1.000f, 0.85f, 0.97f, 0.92f },
};

const Stop kForest[] = {
	{ 0.000f, 0.01f, 0.16f, 0.04f },
	{ 0.300f, 0.05f, 0.40f, 0.06f },
	{ 0.600f, 0.30f, 0.62f, 0.05f },
	{ 0.850f, 0.58f, 0.72f, 0.12f },
	{ 1.000f, 0.28f, 0.50f, 0.08f },
};

const Stop kSunset[] = {
	{ 0.000f, 0.20f, 0.03f, 0.35f },
	{ 0.250f, 0.60f, 0.06f, 0.38f },
	{ 0.500f, 0.95f, 0.24f, 0.18f },
	{ 0.750f, 1.00f, 0.55f, 0.10f },
	{ 1.000f, 1.00f, 0.85f, 0.42f },
};

const Stop kCyberpunk[] = {
	{ 0.000f, 1.00f, 0.03f, 0.55f },
	{ 0.350f, 0.65f, 0.05f, 0.95f },
	{ 0.650f, 0.05f, 0.55f, 1.00f },
	{ 0.850f, 0.05f, 1.00f, 0.90f },
	{ 1.000f, 1.00f, 0.03f, 0.55f },
};

const Stop kGold[] = {
	{ 0.000f, 0.42f, 0.18f, 0.00f },
	{ 0.350f, 0.85f, 0.48f, 0.03f },
	{ 0.700f, 1.00f, 0.78f, 0.20f },
	{ 1.000f, 1.00f, 0.95f, 0.62f },
};

const Stop kMagenta[] = {
	{ 0.000f, 0.35f, 0.00f, 0.45f },
	{ 0.500f, 1.00f, 0.05f, 0.65f },
	{ 1.000f, 1.00f, 0.72f, 0.92f },
};

//Deliberately flat. A strip's worth of one colour, for when the effect should
//carry the whole look and the palette should get out of the way.
const Stop kMono[] = {
	{ 0.000f, 1.00f, 1.00f, 1.00f },
	{ 1.000f, 1.00f, 1.00f, 1.00f },
};

struct Definition
{
	const Stop* stops;
	int count;
	const char* name;
};

template< int N >
constexpr Definition define( const Stop ( &stops )[ N ], const char* name )
{
	return Definition { stops, N, name };
}

const Definition kDefinitions[] = {
	Definition { nullptr, 0, "Colour 1" },
	Definition { nullptr, 0, "Colour 1 > 2" },
	define( kRainbow, "Rainbow" ),
	define( kParty, "Party" ),
	define( kChristmas, "Christmas" ),
	define( kCandyCane, "Candy Cane" ),
	define( kWarmWhite, "Warm White" ),
	define( kFrost, "Frost" ),
	define( kFire, "Fire" ),
	define( kOcean, "Ocean" ),
	define( kForest, "Forest" ),
	define( kSunset, "Sunset" ),
	define( kCyberpunk, "Cyberpunk" ),
	define( kGold, "Gold" ),
	define( kMagenta, "Magenta" ),
	define( kMono, "Mono" ),
};

static_assert( sizeof( kDefinitions ) / sizeof( kDefinitions[ 0 ] ) == static_cast< size_t >( Palette::Count ),
               "every Palette enumerator needs a definition, and in the same order" );

/// Interpolate a stop list. The list is sorted and its first stop sits at 0,
/// so a position below the first stop cannot happen and is not handled.
Rgb sample( const Definition& definition, float position )
{
	position = std::clamp( position, 0.0f, 1.0f );

	for( int i = 1; i < definition.count; ++i )
	{
		const Stop& previous = definition.stops[ i - 1 ];
		const Stop& current  = definition.stops[ i ];
		if( position > current.position )
			continue;

		const float span = current.position - previous.position;
		//Two stops at the same position are how a hard edge is written --
		//Candy Cane would need them if its transition were sharper. Guard the
		//divide rather than forbidding it.
		const float t = span > 0.0f ? ( position - previous.position ) / span : 0.0f;

		return Rgb {
			previous.r + ( current.r - previous.r ) * t,
			previous.g + ( current.g - previous.g ) * t,
			previous.b + ( current.b - previous.b ) * t,
		};
	}

	const Stop& last = definition.stops[ definition.count - 1 ];
	return Rgb { last.r, last.g, last.b };
}
} // namespace

const char* PaletteName( Palette palette )
{
	const int index = static_cast< int >( palette );
	if( index < 0 || index >= static_cast< int >( Palette::Count ) )
		return "?";
	return kDefinitions[ index ].name;
}

Rgb PaletteLookup( Palette palette, float position, const Rgb& colour1, const Rgb& colour2 )
{
	//Wrap rather than clamp. Every effect that walks the palette walks it
	//round, and clamping would park a rainbow on red at the end of each cycle.
	position = position - std::floor( position );

	switch( palette )
	{
	case Palette::Primary:
		return colour1;

	case Palette::PrimarySecondary:
		return Rgb {
			colour1.r + ( colour2.r - colour1.r ) * position,
			colour1.g + ( colour2.g - colour1.g ) * position,
			colour1.b + ( colour2.b - colour1.b ) * position,
		};

	default:
		break;
	}

	const int index = static_cast< int >( palette );
	if( index < kFirstBakedPalette || index >= static_cast< int >( Palette::Count ) )
		return colour1;

	return sample( kDefinitions[ index ], position );
}

std::vector< float > BakePaletteTable()
{
	const int rows = static_cast< int >( Palette::Count );
	std::vector< float > table( static_cast< size_t >( rows ) * kPaletteSize * 4, 0.0f );

	for( int row = kFirstBakedPalette; row < rows; ++row )
	{
		for( int x = 0; x < kPaletteSize; ++x )
		{
			//Texel centres, so entry 0 is position 0 and entry 255 is position
			//1 -- not 255/256, which would leave a seam where a wrapping
			//palette meets itself.
			const float position = static_cast< float >( x ) / static_cast< float >( kPaletteSize - 1 );
			const Rgb colour     = sample( kDefinitions[ row ], position );

			const size_t offset  = ( static_cast< size_t >( row ) * kPaletteSize + x ) * 4;
			table[ offset + 0 ]  = colour.r;
			table[ offset + 1 ]  = colour.g;
			table[ offset + 2 ]  = colour.b;
			table[ offset + 3 ]  = 1.0f;
		}
	}

	return table;
}

} // namespace tinsel
