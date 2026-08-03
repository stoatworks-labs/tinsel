#pragma once

/**
    Factory presets: named string-light looks an operator can reach in one
    gesture. Each entry is a curated pattern-and-palette pairing — the ones
    that actually get asked for on shows — not a random collection of slider
    positions.

    The values live in the same 0..1 parameter space both builds expose (the
    FFGL and OFX builds deliberately share it), so ONE table drives both and a
    preset looks identical in Resolume and Resolve. Plain data only; the
    application machinery lives with each host's glue.

    Element 0 of the host-facing dropdown is "Custom" and is not in this
    table: it means "the sliders are the truth".

    A preset covers the layout, animation, colour and look parameters. The
    Detection group is deliberately left alone — sensitivity and thickness
    are tuned to the operator's artwork, and a preset that undid that tuning
    every time it was picked would cost more than it gave. Mix stays the
    operator's too.
*/

namespace tinsel
{
namespace presets
{
/// The parameters a preset sets, in one fixed order. The FFGL build binds
/// this order to its ParamIDs and the OFX build to its param handles; both
/// static_assert against kParamCount so the three lists cannot drift apart
/// silently.
enum Param
{
	kLayout,
	kTurns,
	kLayoutAngle,
	kDensity,
	kBulbSize,
	kReverse,
	kEffect,
	kSpeed,
	kIntensity,
	kPalette,
	kSpread,
	kC1R,
	kC1G,
	kC1B,
	kC2R,
	kC2G,
	kC2B,
	kSaturation,
	kBrightness,
	kSourceTint,
	kGlow,
	kGlowSize,
	kBackground,
	kDim,
	kParamCount
};

struct Preset
{
	const char* name;
	float v[ kParamCount ];
};

// Option values are element indices: Layout 0 Spiral / 2 Linear; Effect is
// the Effect enum (4 Chase, 8 Meteor, 9 Larson, 10 Wipe, 11 Twinkle,
// 14 FireFlicker, 15 Breathe, 19 Fairy); Palette is the Palette enum
// (3 Party, 4 Christmas, 5 CandyCane, 6 WarmWhite, 7 Frost, 8 Fire, 9 Ocean,
// 12 Cyberpunk); Background 0 Black. Saturation 0.667 and Brightness 0.5 are
// unity after mapping.
inline constexpr Preset kPresets[] = {
	// Warm white bulbs twinkling on the default spiral: the plugin's own
	// defaults, named so they stay reachable.
	{ "Warm Twinkle",
	  { /*Layout*/ 0, /*Turns*/ 0.08f, /*Angle*/ 0.0f, /*Density*/ 0.55f, /*Bulb*/ 0.45f, /*Rev*/ 0.0f,
	    /*Effect*/ 11, /*Speed*/ 0.25f, /*Intensity*/ 0.5f, /*Palette*/ 6, /*Spread*/ 0.4f,
	    /*C1*/ 1.0f, 0.72f, 0.36f, /*C2*/ 0.1f, 0.55f, 1.0f, /*Sat*/ 0.667f, /*Bright*/ 0.5f, /*Tint*/ 0.0f,
	    /*Glow*/ 0.4f, /*GlowSz*/ 0.35f, /*Back*/ 0, /*Dim*/ 0.25f } },

	// Red and green running round the string: the December standard.
	{ "Christmas Chase",
	  { /*Layout*/ 0, /*Turns*/ 0.08f, /*Angle*/ 0.0f, /*Density*/ 0.6f, /*Bulb*/ 0.45f, /*Rev*/ 0.0f,
	    /*Effect*/ 4, /*Speed*/ 0.35f, /*Intensity*/ 0.6f, /*Palette*/ 4, /*Spread*/ 0.5f,
	    /*C1*/ 1.0f, 0.72f, 0.36f, /*C2*/ 0.1f, 0.55f, 1.0f, /*Sat*/ 0.667f, /*Bright*/ 0.5f, /*Tint*/ 0.0f,
	    /*Glow*/ 0.5f, /*GlowSz*/ 0.35f, /*Back*/ 0, /*Dim*/ 0.25f } },

	// Stripes of red and white wiping along the run.
	{ "Candy Wipe",
	  { /*Layout*/ 0, /*Turns*/ 0.08f, /*Angle*/ 0.0f, /*Density*/ 0.6f, /*Bulb*/ 0.5f, /*Rev*/ 0.0f,
	    /*Effect*/ 10, /*Speed*/ 0.3f, /*Intensity*/ 0.6f, /*Palette*/ 5, /*Spread*/ 0.5f,
	    /*C1*/ 1.0f, 0.72f, 0.36f, /*C2*/ 0.1f, 0.55f, 1.0f, /*Sat*/ 0.667f, /*Bright*/ 0.5f, /*Tint*/ 0.0f,
	    /*Glow*/ 0.45f, /*GlowSz*/ 0.35f, /*Back*/ 0, /*Dim*/ 0.25f } },

	// Every bulb a small flame, big soft glow.
	{ "Fire Flicker",
	  { /*Layout*/ 0, /*Turns*/ 0.08f, /*Angle*/ 0.0f, /*Density*/ 0.55f, /*Bulb*/ 0.5f, /*Rev*/ 0.0f,
	    /*Effect*/ 14, /*Speed*/ 0.3f, /*Intensity*/ 0.65f, /*Palette*/ 8, /*Spread*/ 0.45f,
	    /*C1*/ 1.0f, 0.72f, 0.36f, /*C2*/ 0.1f, 0.55f, 1.0f, /*Sat*/ 0.667f, /*Bright*/ 0.5f, /*Tint*/ 0.0f,
	    /*Glow*/ 0.6f, /*GlowSz*/ 0.5f, /*Back*/ 0, /*Dim*/ 0.25f } },

	// A neon eye sweeping back and forth along the outline.
	{ "Cyber Larson",
	  { /*Layout*/ 2, /*Turns*/ 0.08f, /*Angle*/ 0.0f, /*Density*/ 0.55f, /*Bulb*/ 0.45f, /*Rev*/ 0.0f,
	    /*Effect*/ 9, /*Speed*/ 0.45f, /*Intensity*/ 0.7f, /*Palette*/ 12, /*Spread*/ 0.45f,
	    /*C1*/ 1.0f, 0.72f, 0.36f, /*C2*/ 0.1f, 0.55f, 1.0f, /*Sat*/ 0.667f, /*Bright*/ 0.5f, /*Tint*/ 0.0f,
	    /*Glow*/ 0.55f, /*GlowSz*/ 0.45f, /*Back*/ 0, /*Dim*/ 0.25f } },

	// The whole string inhaling and exhaling in sea colours.
	{ "Ocean Breathe",
	  { /*Layout*/ 0, /*Turns*/ 0.08f, /*Angle*/ 0.0f, /*Density*/ 0.55f, /*Bulb*/ 0.45f, /*Rev*/ 0.0f,
	    /*Effect*/ 15, /*Speed*/ 0.15f, /*Intensity*/ 0.55f, /*Palette*/ 9, /*Spread*/ 0.6f,
	    /*C1*/ 1.0f, 0.72f, 0.36f, /*C2*/ 0.1f, 0.55f, 1.0f, /*Sat*/ 0.667f, /*Bright*/ 0.5f, /*Tint*/ 0.0f,
	    /*Glow*/ 0.45f, /*GlowSz*/ 0.4f, /*Back*/ 0, /*Dim*/ 0.25f } },

	// Meteors streaking through party colours.
	{ "Party Meteor",
	  { /*Layout*/ 0, /*Turns*/ 0.08f, /*Angle*/ 0.0f, /*Density*/ 0.6f, /*Bulb*/ 0.45f, /*Rev*/ 0.0f,
	    /*Effect*/ 8, /*Speed*/ 0.5f, /*Intensity*/ 0.7f, /*Palette*/ 3, /*Spread*/ 0.55f,
	    /*C1*/ 1.0f, 0.72f, 0.36f, /*C2*/ 0.1f, 0.55f, 1.0f, /*Sat*/ 0.667f, /*Bright*/ 0.5f, /*Tint*/ 0.0f,
	    /*Glow*/ 0.5f, /*GlowSz*/ 0.4f, /*Back*/ 0, /*Dim*/ 0.25f } },

	// Frost-blue fairy lights, barely moving.
	{ "Fairy Frost",
	  { /*Layout*/ 0, /*Turns*/ 0.08f, /*Angle*/ 0.0f, /*Density*/ 0.5f, /*Bulb*/ 0.4f, /*Rev*/ 0.0f,
	    /*Effect*/ 19, /*Speed*/ 0.2f, /*Intensity*/ 0.5f, /*Palette*/ 7, /*Spread*/ 0.5f,
	    /*C1*/ 1.0f, 0.72f, 0.36f, /*C2*/ 0.1f, 0.55f, 1.0f, /*Sat*/ 0.667f, /*Bright*/ 0.5f, /*Tint*/ 0.0f,
	    /*Glow*/ 0.45f, /*GlowSz*/ 0.4f, /*Back*/ 0, /*Dim*/ 0.25f } },
};

inline constexpr int kCount = int( sizeof( kPresets ) / sizeof( kPresets[ 0 ] ) );

} // namespace presets
} // namespace tinsel
