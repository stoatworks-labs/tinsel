#pragma once

#include "PassBuffer.h"

#include <FFGLSDK.h>

/**
    Tinsel -- edge-detected outlines lit as a string of LEDs, for Resolume.

    Find the outline of whatever is on the layer, hang a virtual string of
    lamps along it, and run the sort of pattern an LED controller runs: chases,
    comets, twinkles, fire, a scanner, a wipe. A logo becomes a neon sign or a
    Christmas tree depending on two dropdowns.

    **The one idea.** An effect never chooses a colour -- it says where in the
    palette a lamp sits and how bright it is, and the palette turns the first of
    those into RGB. That is how an LED controller is built, and it is why twenty
    effects and sixteen palettes are worth more than thirty-six of either.
    `Effects.h` has the long version.

    **What this release actually does, and what it does not.** The strip
    coordinate here is a *field*, not an arc length: the plugin computes a
    scalar for every pixel -- a spiral about the artwork's centroid by default
    -- quantises it into lamps, and runs the effects on that. It does not trace
    the contour. So a chase climbs the shape the way a string wound round a tree
    climbs it, which is the intended look and is faithful to what a real strip
    does; but it is not a chase that follows the letterform of an 'S' round its
    own curve, and on a shape with several separate parts the lamps of each part
    share a coordinate rather than being wired in series. Contour tracing is
    v0.2, and the parameters here are the ones it will need, so it can be
    swapped in underneath without the controls changing. See AGENTS.md.

    **Six passes**, in Shaders.h. The one that matters for footage is
    `stabilise`: its temporal filter is deliberately asymmetric -- fast up, slow
    down -- because what goes wrong on video is not that edges move, it is that
    they drop out for a frame and the lamp blinks.

    See AGENTS.md for the traps.
*/
class Tinsel : public CFFGLPlugin
{
public:
	Tinsel();

	//CFFGLPlugin
	FFResult InitGL( const FFGLViewportStruct* vp ) override;
	FFResult ProcessOpenGL( ProcessOpenGLStruct* pGL ) override;
	FFResult DeInitGL() override;

	FFResult SetFloatParameter( unsigned int index, float value ) override;
	float GetFloatParameter( unsigned int index ) override;

	FFResult SetTime( double time ) override;

	/// The order the host shows them in: find the outline, decide where the
	/// string runs, choose the pattern, colour it, and put it back over the
	/// picture.
	enum ParamID : FFUInt32
	{
		//Edge
		PT_SOURCE,
		PT_SENSITIVITY,
		PT_SOFTNESS,
		PT_DETAIL,
		PT_THICKNESS,
		PT_STABILITY,

		//Strip
		PT_LAYOUT,
		PT_TURNS,
		PT_LAYOUT_ANGLE,
		PT_DENSITY,
		PT_BULB_SIZE,
		PT_REVERSE,

		//Effect
		PT_EFFECT,
		PT_SPEED,
		PT_INTENSITY,

		//Colour
		PT_PALETTE,
		PT_SPREAD,
		PT_C1_R,
		PT_C1_G,
		PT_C1_B,
		PT_C2_R,
		PT_C2_G,
		PT_C2_B,
		PT_SATURATION,
		PT_BRIGHTNESS,
		PT_SOURCE_TINT,

		//Output
		PT_GLOW,
		PT_GLOW_SIZE,
		PT_BACKGROUND,
		PT_DIM,
		PT_MIX,

		PT_COUNT
	};

private:
	/// Bake the palettes and upload them. Once, at InitGL: the table does not
	/// depend on any parameter, which is the point of keeping the two
	/// colour-driven palettes out of it.
	bool UploadPalettes();

	ffglex::FFGLShader copyShader;
	ffglex::FFGLShader edgeShader;
	ffglex::FFGLShader stabiliseShader;
	ffglex::FFGLShader lightShader;
	ffglex::FFGLShader blurShader;
	ffglex::FFGLShader compositeShader;
	ffglex::FFGLScreenQuad quad;

	tinsel::PassBuffer copyBuffer;      ///< the picture, ours, mipmapped
	tinsel::PassBuffer edgeBuffer;      ///< raw gradient magnitude
	tinsel::PassBuffer stableBuffer[ 2 ];///< ping-pong: stabilised edge + moments
	tinsel::PassBuffer lightBuffer;     ///< the lamps, premultiplied
	tinsel::PassBuffer glowBuffer[ 2 ]; ///< quarter size, ping-ponged by the blur

	/// Which of stableBuffer[] holds the frame just rendered. The other one is
	/// the history the next frame reads.
	int stableCurrent = 0;

	GLuint paletteTexture = 0;

	//---------------------------------------------------------------------
	// Time.
	//
	// The phase is accumulated from the host's clock rather than computed from
	// it. `time * speed` is the obvious form and it is wrong: moving Speed
	// rescales the whole history, so every effect jumps to a different point in
	// its cycle the instant the control is touched -- worst at the moment an
	// operator is nudging it, which is exactly when it is being watched.
	// Integrating the rate instead means Speed changes what happens next and
	// nothing else.
	//---------------------------------------------------------------------
	double hostTime     = -1.0;
	double lastHostTime = -1.0;
	double phase        = 0.0;

	/// Set when the history buffers hold nothing worth blending against -- the
	/// first frame, and any frame after a resize. Without it the first
	/// stabilised frame blends the new picture against the last clip's
	/// outlines, which lingers for as long as Stability is set to hold things.
	bool historyValid = false;

	float params[ PT_COUNT ];
};
