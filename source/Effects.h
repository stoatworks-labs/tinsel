#pragma once

#include <cstdint>

/**
    What a bulb is doing at a given moment.

    **The one idea.** An effect never chooses a colour. It answers two
    questions about one bulb -- *where in the palette* it sits and *how bright*
    it is -- and the palette turns the first of those into RGB. That is how an
    LED controller is built, and it is the reason twenty effects and sixteen
    palettes are worth more than thirty-six of either: Comet on Christmas and
    Comet on Frost are not the same effect wearing a hat, they are a red-green
    chaser and a shooting star.

    It also means an effect is a pure function of (bulb, time) with no state, no
    buffer and no history -- which is what lets the whole library exist twice,
    once here for the offline harness to measure and once in GLSL for the GPU
    to run, and lets `tinseltest --effects` prove the two agree.

    **Everything below is mirrored in `Shaders.cpp`.** Every line that has a
    counterpart there carries a `//= mirrored` marker. Change one, change both,
    and run `tinseltest --effects` -- which is the only thing that will notice.
    A drifted constant does not fail the build and does not look like an error;
    it looks like the effect being slightly different from the one you designed.
*/
namespace tinsel
{

enum class Effect
{
	Solid = 0,
	Gradient,
	Rainbow,
	ColourLoop,
	Chase,
	TheaterChase,
	Running,
	Comet,
	Meteor,
	Larson,
	Wipe,
	Twinkle,
	Sparkle,
	Glitter,
	FireFlicker,
	Breathe,
	Strobe,
	Dissolve,
	Colorwaves,
	Fairy,
	Count
};

const char* EffectName( Effect effect );

/// One bulb's answer.
struct Bulb
{
	float position = 0.0f;   ///< where in the palette, 0..1 wrapping
	float brightness = 0.0f; ///< 0..1, before glow and before the edge mask
};

/**
    Evaluate one bulb.

    @param effect     which pattern
    @param s          the bulb's coordinate along the strip, 0..1, already
                      quantised to the bulb's own centre. Two pixels of the
                      same bulb must pass the same `s` or they will disagree
                      about the colour of one lamp.
    @param bulb       the bulb's integer index, for per-bulb randomness. Derived
                      from `s` upstream, and passed rather than recomputed so
                      there is one definition of which bulb this is.
    @param t          animation time in cycles: seconds already multiplied by
                      the Speed control. An effect treats 1.0 as one period of
                      whatever its period means.
    @param intensity  the effect's second knob, 0..1. What it means is the
                      effect's business -- tail length for Comet, how many
                      lamps are lit for Twinkle, duty cycle for Strobe.
    @param spread     how many times the palette is laid across the strip.
                      Applied to `position` only, never to the geometry, so
                      turning it up recolours a chase without changing where
                      the chase is.
*/
Bulb Evaluate( Effect effect, float s, uint32_t bulb, float t, float intensity, float spread );

/// PCG-style integer hash, exact in 32 bits and therefore reproducible between
/// C++ and GLSL. The usual `fract(sin(x)*43758.5)` is not: it depends on the
/// driver's sin, and two GPUs disagree about which lamps are lit.
uint32_t HashInt( uint32_t x );

/// The same hash as a float in 0..1.
float Hash01( uint32_t x );

} // namespace tinsel
