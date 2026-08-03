#include "Controls.h"

#include <algorithm>
#include <cmath>

namespace tinsel
{
namespace
{
inline float clamp01( float value )
{
	return std::min( std::max( value, 0.0f ), 1.0f );
}

inline float lerp( float from, float to, float t )
{
	return from + ( to - from ) * clamp01( t );
}

/// Geometric interpolation. Equal slider movements are equal *ratios*, which
/// is the right behaviour for any quantity where the question is "how many
/// times more" rather than "how much more".
inline float geometric( float from, float to, float t )
{
	return from * std::pow( to / from, clamp01( t ) );
}
} // namespace

float SensitivityFromParam( float value )
{
	return geometric( 0.01f, 1.0f, value );
}

float SoftnessFromParam( float value )
{
	return lerp( 0.05f, 1.0f, value );
}

float DetailFromParam( float value )
{
	return lerp( 0.0f, 4.0f, value );
}

float ThicknessFromParam( float value )
{
	return lerp( 0.0f, 3.5f, value );
}

float AttackFromParam( float value )
{
	//Stays fast across the whole range. Stability is allowed to buy
	//persistence but is not allowed to buy lag: an outline that arrives late
	//is a worse artefact than the flicker being traded away, because it is
	//attached to something the eye is already tracking.
	return lerp( 1.0f, 0.75f, value );
}

float ReleaseFromParam( float value )
{
	//1.0 is no persistence at all. 0.02 is a time constant of fifty frames,
	//which is a little under a second at 60fps and about as long as a lamp can
	//hang on after its edge has gone before it reads as a ghost.
	return geometric( 1.0f, 0.02f, value );
}

float BulbCountFromParam( float value )
{
	return std::floor( geometric( 8.0f, 1000.0f, value ) + 0.5f );
}

float BulbSizeFromParam( float value )
{
	return geometric( 0.75f, 40.0f, value );
}

float TurnsFromParam( float value )
{
	return lerp( 0.0f, 8.0f, value );
}

float LayoutAngleFromParam( float value )
{
	return clamp01( value );
}

float SpeedFromParam( float value )
{
	return lerp( 0.0f, 4.0f, value );
}

float SpreadFromParam( float value )
{
	return geometric( 0.25f, 8.0f, value );
}

float BrightnessFromParam( float value )
{
	return lerp( 0.0f, 2.0f, value );
}

float SaturationFromParam( float value )
{
	return lerp( 0.0f, 1.5f, value );
}

float GlowFromParam( float value )
{
	return lerp( 0.0f, 3.0f, value );
}

float GlowSizeFromParam( float value )
{
	return geometric( 0.5f, 8.0f, value );
}

} // namespace tinsel
