#include "Effects.h"

#include <algorithm>
#include <cmath>

namespace tinsel
{
namespace
{
constexpr float kTau = 6.283185307179586f;//= mirrored

/**
    GLSL's builtins, written out.

    These are not conveniences. Each one is spelled the way the GLSL
    specification defines it rather than the way it is usually written in C++,
    because the whole point of this file is that it computes the same numbers
    as the shader. `mix` is the one that bites: the obvious `a + (b - a) * t`
    is a different sequence of roundings from the specified
    `x * (1 - a) + y * a`, and the difference shows up exactly where a
    comparison against a threshold is close -- that is, on the bulbs whose
    state is about to change, which are the ones being looked at.
*/
inline float fract( float x )
{
	return x - std::floor( x );
}

inline float glslMod( float x, float y )
{
	return x - y * std::floor( x / y );
}

inline float mix( float x, float y, float a )
{
	return x * ( 1.0f - a ) + y * a;
}

inline float clamp01( float x )
{
	return std::min( std::max( x, 0.0f ), 1.0f );
}

inline float smoothstep( float edge0, float edge1, float x )
{
	const float t = clamp01( ( x - edge0 ) / ( edge1 - edge0 ) );
	return t * t * ( 3.0f - 2.0f * t );
}

/// A time slot as hash input. Both sides go through int first, so the
/// conversion of a negative time is the same truncation on both -- GLSL's
/// float-to-uint conversion is undefined for negatives and this avoids relying
/// on it.
inline uint32_t slotBits( float slot )
{
	return static_cast< uint32_t >( static_cast< int32_t >( slot ) );
}

/// Knuth's multiplicative constant, used to decorrelate the bulb index from
/// the time slot before they are mixed. Without it, bulb N at slot M and bulb
/// M at slot N hash identically and the sparkle falls into diagonal lines.
constexpr uint32_t kOdd = 2654435761u;//= mirrored

const char* const kNames[] = {
	"Solid",
	"Gradient",
	"Rainbow",
	"Colour Loop",
	"Chase",
	"Theater Chase",
	"Running Lights",
	"Comet",
	"Meteor",
	"Larson Scanner",
	"Colour Wipe",
	"Twinkle",
	"Sparkle",
	"Glitter",
	"Fire Flicker",
	"Breathe",
	"Strobe",
	"Dissolve",
	"Colorwaves",
	"Fairy Lights",
};

static_assert( sizeof( kNames ) / sizeof( kNames[ 0 ] ) == static_cast< size_t >( Effect::Count ),
               "every Effect enumerator needs a name, and in the same order" );
} // namespace

uint32_t HashInt( uint32_t x )
{
	//= mirrored -- PCG output-mixed hash. Integer throughout on purpose: this
	//is the only randomness in the plugin and it has to give the same answer
	//on the GPU as it does here, which rules out anything built on sin().
	x           = x * 747796405u + 2891336453u;
	uint32_t w  = ( ( x >> ( ( x >> 28u ) + 4u ) ) ^ x ) * 277803737u;
	return ( w >> 22u ) ^ w;
}

float Hash01( uint32_t x )
{
	//= mirrored
	return static_cast< float >( HashInt( x ) ) * ( 1.0f / 4294967296.0f );
}

const char* EffectName( Effect effect )
{
	const int index = static_cast< int >( effect );
	if( index < 0 || index >= static_cast< int >( Effect::Count ) )
		return "?";
	return kNames[ index ];
}

Bulb Evaluate( Effect effect, float s, uint32_t bulb, float t, float intensity, float spread )
{
	//Where this bulb sits in the palette before the effect has an opinion.
	//Every effect that has nothing particular to say about colour returns this,
	//which is what makes Spread mean the same thing across the whole library.
	const float base = s * spread;//= mirrored

	switch( effect )
	{
	case Effect::Solid:
		//= mirrored
		return Bulb { 0.0f, 1.0f };

	case Effect::Gradient:
		//= mirrored
		return Bulb { base, 1.0f };

	case Effect::Rainbow:
		//= mirrored
		return Bulb { base + t, 1.0f };

	case Effect::ColourLoop:
		//The whole strip one colour, walking the palette. Reads as nothing at
		//all on Mono, which is correct and worth knowing before reporting it.
		//= mirrored
		return Bulb { t, 1.0f };

	case Effect::Chase:
	{
		//= mirrored
		const float groups = 1.0f + std::floor( intensity * 7.0f );
		const float phase  = fract( s * groups - t );
		const float duty   = 0.15f + 0.35f * ( 1.0f - intensity );
		return Bulb { base, 1.0f - smoothstep( duty * 0.6f, duty, phase ) };
	}

	case Effect::TheaterChase:
	{
		//Every nth lamp, the pattern stepping one lamp at a time. The step rate
		//is tied to the spacing so that one unit of time is one full cycle of
		//the pattern whatever the spacing -- otherwise Intensity would change
		//the speed as a side effect of changing the look.
		//
		//Integer arithmetic, and it has to be. Written the obvious way, as
		//`mod(float(bulb) - slot, spacing)`, this is wrong on the GPU wherever
		//the subtraction lands on an exact multiple of the spacing: GLSL
		//defines mod as `x - y * floor( x / y )`, the division of an exact
		//multiple can round a hair below the integer, floor then takes it down
		//a whole step, and the result comes back as `spacing` instead of zero.
		//The lamp that should be the brightest in the pattern is the one that
		//goes out. Caught by `tinseltest --effects` as 60 disagreements out of
		//527,100, all of them in this effect, all of them a full 0-against-1.
		//= mirrored
		const uint32_t spacing = static_cast< uint32_t >( 2.0f + std::floor( intensity * 5.0f ) );
		const uint32_t slot    = slotBits( std::floor( t * static_cast< float >( spacing ) ) );
		const uint32_t lit     = ( bulb % spacing + spacing - slot % spacing ) % spacing;
		return Bulb { base, lit == 0u ? 1.0f : 0.0f };
	}

	case Effect::Running:
	{
		//= mirrored
		const float waves = 1.0f + std::floor( intensity * 5.0f );
		return Bulb { base, 0.5f + 0.5f * std::sin( kTau * ( s * waves - t ) ) };
	}

	case Effect::Comet:
	{
		//= mirrored
		const float head   = fract( t );
		const float behind = fract( head - s );
		const float tail   = 0.04f + 0.40f * intensity;
		return Bulb { base, std::exp( -behind / tail ) };
	}

	case Effect::Meteor:
	{
		//A comet whose tail is falling apart. The grain is applied in
		//proportion to how far back the lamp is, so the head stays solid and
		//only the trail breaks up -- apply it evenly and it reads as noise
		//rather than as debris.
		//= mirrored
		const float head   = fract( t );
		const float behind = fract( head - s );
		const float tail   = 0.04f + 0.40f * intensity;
		const float body   = std::exp( -behind / tail );
		const float grain  = Hash01( bulb * kOdd ^ slotBits( std::floor( t * 12.0f ) ) );
		const float bite   = clamp01( behind / tail );
		return Bulb { base, body * mix( 1.0f, grain, bite ) };
	}

	case Effect::Larson:
	{
		//Bounces rather than wraps, so the distance is absolute and not
		//wrapped. A wrapped distance here is the classic mistake: it makes the
		//scanner jump across the gap instead of turning round at the end.
		//= mirrored
		const float bounce = std::fabs( fract( t * 0.5f ) * 2.0f - 1.0f );
		const float tail   = 0.02f + 0.22f * intensity;
		return Bulb { base, std::exp( -std::fabs( s - bounce ) / tail ) };
	}

	case Effect::Wipe:
	{
		//Fills, then fills back over itself in the other colour, so the strip
		//is never blank and the wipe is always visible against what it is
		//replacing.
		//= mirrored
		const float level = fract( t );
		const float pass  = glslMod( std::floor( t ), 2.0f );
		const float on    = s < level ? 1.0f : 0.0f;
		return Bulb { mix( pass, 1.0f - pass, on ) * 0.5f, 1.0f };
	}

	case Effect::Twinkle:
	{
		//Each lamp has its own rate and phase, and Intensity widens the window
		//around each one's peak. Expressed as a window rather than as a
		//probability so that no lamp ever pops on abruptly -- a hard gate here
		//is what makes cheap twinkle look like noise.
		//= mirrored
		const float r         = Hash01( bulb );
		const float rate      = 0.4f + r * 0.9f;
		const float v         = 0.5f + 0.5f * std::sin( kTau * ( t * rate + r ) );
		const float threshold = 1.0f - std::min( std::max( intensity, 0.02f ), 0.98f );
		return Bulb { base + r, smoothstep( threshold, 1.0f, v ) };
	}

	case Effect::Sparkle:
	{
		//= mirrored
		const uint32_t slot    = slotBits( std::floor( t * 8.0f ) );
		const float pick       = Hash01( bulb * kOdd ^ slot );
		const float density    = 0.02f + intensity * 0.35f;
		const float decay      = std::exp( -fract( t * 8.0f ) * 4.0f );
		return Bulb { Hash01( bulb ^ slot ^ 0x9E3779B9u ), pick < density ? decay : 0.0f };
	}

	case Effect::Glitter:
	{
		//Sparkle over a lit strip rather than over darkness. This is the one
		//that reads as a tree that already had lights on it.
		//= mirrored
		const uint32_t slot = slotBits( std::floor( t * 8.0f ) );
		const float pick    = Hash01( bulb * kOdd ^ slot );
		const float density = 0.02f + intensity * 0.30f;
		const float decay   = std::exp( -fract( t * 8.0f ) * 5.0f );
		const float spark   = pick < density ? decay : 0.0f;
		return Bulb { base + t * 0.1f, clamp01( 0.35f + spark ) };
	}

	case Effect::FireFlicker:
	{
		//Value noise in time, per lamp: two hashes either side of the current
		//slot, smoothstepped between. Interpolating rather than switching is
		//the difference between a fire and a fault.
		//= mirrored
		const float slotF   = std::floor( t * 6.0f );
		const uint32_t slot = slotBits( slotF );
		const float a       = Hash01( bulb * kOdd ^ slot );
		const float b       = Hash01( bulb * kOdd ^ ( slot + 1u ) );
		const float n       = mix( a, b, smoothstep( 0.0f, 1.0f, fract( t * 6.0f ) ) );
		const float depth   = 0.25f + intensity * 0.70f;
		//Hotter lamps sit further up the palette, which is what makes this one
		//a fire on the Fire palette and merely a flicker on the others.
		return Bulb { 0.25f + n * 0.70f, clamp01( 1.0f - depth * ( 1.0f - n ) ) };
	}

	case Effect::Breathe:
	{
		//Squared, because a plain sine reads as a fade. A breath spends longer
		//at the bottom of its travel than at the top.
		//= mirrored
		const float v = 0.5f + 0.5f * std::sin( kTau * t );
		return Bulb { base, 0.08f + 0.92f * v * v };
	}

	case Effect::Strobe:
	{
		//= mirrored
		const float duty = 0.02f + intensity * 0.30f;
		return Bulb { base, fract( t ) < duty ? 1.0f : 0.0f };
	}

	case Effect::Dissolve:
	{
		//Wipe's structure with a random order instead of a spatial one.
		//= mirrored
		const float level = fract( t );
		const float pass  = glslMod( std::floor( t ), 2.0f );
		const float on    = Hash01( bulb ) < level ? 1.0f : 0.0f;
		return Bulb { mix( pass, 1.0f - pass, on ) * 0.5f, 1.0f };
	}

	case Effect::Colorwaves:
	{
		//Colour and brightness undulate on periods that do not divide into one
		//another, so the strip drifts and never visibly repeats.
		//= mirrored
		const float position = base + 0.30f * std::sin( kTau * ( s * 1.7f - t * 0.6f ) ) + t * 0.25f;
		const float bright   = 0.55f + 0.45f * std::sin( kTau * ( s * 2.3f + t * 0.4f ) );
		return Bulb { position, bright };
	}

	case Effect::Fairy:
	{
		//Slow, sparse and warm. Intensity is how many lamps take part at all
		//rather than how bright they get: the ones left out are what stop this
		//reading as a uniform shimmer.
		//= mirrored
		const float r      = Hash01( bulb );
		const float g      = Hash01( bulb ^ 0x51ED2701u );
		const float ph     = fract( t * ( 0.12f + r * 0.20f ) + r );
		const float v      = smoothstep( 0.0f, 0.18f, ph ) * ( 1.0f - smoothstep( 0.32f, 0.62f, ph ) );
		const float taking = g < ( 0.10f + intensity * 0.85f ) ? 1.0f : 0.0f;
		return Bulb { base + r * 0.35f, v * taking };
	}

	case Effect::Count:
		break;
	}

	return Bulb {};
}

} // namespace tinsel
