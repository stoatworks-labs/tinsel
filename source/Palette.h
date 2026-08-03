#pragma once

#include <vector>

/**
    The colours a bulb can be.

    A palette here is what a palette is on an LED controller: a short list of
    gradient stops that gets interpolated into a lookup table, and which an
    effect indexes with a position in 0..1 rather than choosing an RGB value
    for itself. That split is the reason twenty effects and sixteen palettes
    are three hundred and twenty looks and not thirty-six controls.

    **These are authored here, not imported.** WLED is EUPL-1.2 and FastLED's
    gradient palettes carry cpt-city's own terms; neither is MIT, and this repo
    is. Nothing in this file was copied from either -- the palettes below were
    picked by eye to cover the same ground, and any resemblance in a name is
    because "Ocean" is what a blue-green palette is called.

    The table is baked once at InitGL into a 256 x kPaletteCount float texture
    and never touched again. The shader interpolates between two GL_NEAREST
    fetches rather than relying on GL_LINEAR, because one texture has one
    filter for both axes and bilinear on this one would blend a palette into
    its neighbour along the way.
*/
namespace tinsel
{

/// Entries per palette in the baked lookup table. 256 is far more than the eye
/// needs across a gradient; it is chosen so the shader's interpolation weight
/// is small enough that the two-fetch mix is doing almost nothing, which is
/// what keeps the C++ and GLSL lookups agreeing to float tolerance.
constexpr int kPaletteSize = 256;

enum class Palette
{
	Primary = 0,   ///< Colour 1, flat. Special-cased: not in the table.
	PrimarySecondary, ///< Colour 1 to Colour 2. Special-cased: not in the table.
	Rainbow,
	Party,
	Christmas,
	CandyCane,
	WarmWhite,
	Frost,
	Fire,
	Ocean,
	Forest,
	Sunset,
	Cyberpunk,
	Gold,
	Magenta,
	Mono,
	Count
};

/// The first palette that actually lives in the baked table. The two before it
/// are drawn from the Colour 1 / Colour 2 parameters and so cannot be baked.
constexpr int kFirstBakedPalette = 2;

const char* PaletteName( Palette palette );

/// RGB at `position` (0..1, wrapping) in the given palette, in the same 0..1
/// float space the shader works in. Returns Colour 1 / a Colour 1-2 blend for
/// the two special palettes, which is why it needs them passed in.
struct Rgb
{
	float r = 0.0f;
	float g = 0.0f;
	float b = 0.0f;
};

Rgb PaletteLookup( Palette palette, float position, const Rgb& colour1, const Rgb& colour2 );

/// Bake every table-backed palette into a row-major RGBA float buffer,
/// kPaletteSize wide and kPaletteCount tall. Rows 0 and 1 are written as black
/// so the texture's geometry matches the enum and the shader can index it with
/// the raw palette number instead of an adjusted one -- an off-by-two in a
/// texture coordinate is a bug that looks like a colour choice.
std::vector< float > BakePaletteTable();

} // namespace tinsel
