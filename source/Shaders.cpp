#include "Shaders.h"

namespace tinsel
{

const char* const kVertexShader = R"(#version 410 core

layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;

out vec2 uv;

void main()
{
	gl_Position = vPosition;

	//Straight through, in 0..1 picture space. The usual FFGL vertex shader
	//folds MaxUV in here; that happens once in the copy pass instead, and
	//every pass after it works on a texture we allocated, where the picture
	//really does fill the texture.
	uv = vUV;
}
)";

//---------------------------------------------------------------------------
// Pass 1: copy.
//---------------------------------------------------------------------------
const char* const kCopyShader = R"(#version 410 core

uniform sampler2D InputTexture;
uniform vec2 MaxUV;      //the part of the input texture that is really picture
uniform vec2 HalfTexel;  //half an input texel, in picture space

in vec2 uv;
out vec4 fragColor;

void main()
{
	//Half a texel in from the edge. GL_LINEAR at the picture boundary takes
	//half its weight from the texture's undrawn padding, and on a logo that
	//shows up as a false edge running down the side of the frame -- which this
	//plugin would then dutifully hang lamps on.
	vec2 picture = clamp( uv, HalfTexel, vec2( 1.0 ) - HalfTexel );

	//Premultiplied in, premultiplied out. The mip chain built on this texture
	//is a box filter, and averaging premultiplied samples is the correct
	//filter; averaging straight colour smears the colour of transparent pixels
	//into the picture.
	fragColor = texture( InputTexture, picture * MaxUV );
}
)";

//---------------------------------------------------------------------------
// Pass 2: edge.
//---------------------------------------------------------------------------
const char* const kEdgeShader = R"(#version 410 core

uniform sampler2D CopyTexture;
uniform vec2 TexelSize;   //one texel of the copy buffer, in picture space
uniform float Detail;     //mip level to detect at, 0 = per pixel
uniform float SourceMode; //0 luma, 1 alpha, 2 chroma, 3 luma or alpha

in vec2 uv;
out vec4 fragColor;

//What "different" means between two pixels. The choice matters more than the
//operator does: a logo delivered with alpha has a perfect edge already in the
//alpha channel and running a luma Sobel over it instead is throwing away the
//only clean signal in the frame.
float channel( vec2 at )
{
	vec4 c = textureLod( CopyTexture, at, Detail );
	int mode = int( SourceMode + 0.5 );

	if( mode == 1 )
		return c.a;

	if( mode == 2 )
	{
		//Chroma distance from the pixel's own grey. Finds the boundary between
		//two colours of equal brightness, which is exactly the case a luma
		//Sobel is blind to and which brand artwork is full of.
		float y = dot( c.rgb, vec3( 0.2126, 0.7152, 0.0722 ) );
		return length( c.rgb - vec3( y ) ) + y * 0.25;
	}

	//Un-premultiply before taking luma, or a soft edge in the alpha channel
	//reads as a brightness ramp and the Sobel finds a wide smear where there
	//is a hard boundary.
	vec3 straight = c.a > 0.0031 ? c.rgb / c.a : c.rgb;
	float luma = dot( straight, vec3( 0.2126, 0.7152, 0.0722 ) );

	if( mode == 3 )
	{
		//Both channels at once, for artwork that could be delivered either way.
		//
		//The alpha sets a floor so that a *dark* logo on transparency still has
		//a boundary -- weight it purely by luma and a black mark on nothing is
		//zero on both sides of its own edge and vanishes. The luma term then
		//adds the detail inside the shape on top of that floor.
		//
		//Written as `max( luma * c.a, c.a )` this is identically 1.0 for every
		//opaque pixel, so the mode found no edges at all on any clip without an
		//alpha channel -- which, being the default, meant the effect did
		//nothing at all out of the box. It cost 27 of 31 controls reading as
		//dead in `tools/sweep.py`, which is the only reason it was found: one
		//dead control looks like a typo, twenty-seven looks like a pipeline.
		return c.a * ( 0.35 + 0.65 * luma );
	}

	return luma;
}

void main()
{
	//Tap spacing follows the detail level. Detecting at mip 2 with taps one
	//full-resolution texel apart samples the same texel three times and
	//reports no edge anywhere -- the scale of the blur and the scale of the
	//operator have to move together.
	vec2 step = TexelSize * exp2( Detail );

	//Sobel. Two 3x3 convolutions; the magnitude of the pair is the gradient.
	float tl = channel( uv + vec2( -step.x,  step.y ) );
	float tc = channel( uv + vec2(     0.0,  step.y ) );
	float tr = channel( uv + vec2(  step.x,  step.y ) );
	float ml = channel( uv + vec2( -step.x,     0.0 ) );
	float mr = channel( uv + vec2(  step.x,     0.0 ) );
	float bl = channel( uv + vec2( -step.x, -step.y ) );
	float bc = channel( uv + vec2(     0.0, -step.y ) );
	float br = channel( uv + vec2(  step.x, -step.y ) );

	float gx = ( tr + 2.0 * mr + br ) - ( tl + 2.0 * ml + bl );
	float gy = ( tl + 2.0 * tc + tr ) - ( bl + 2.0 * bc + br );

	//Divide by four, which is the sum of one side of the kernel, so that a
	//clean black-to-white step gives exactly 1.0 and the Sensitivity control
	//has the same meaning whatever the footage.
	fragColor = vec4( length( vec2( gx, gy ) ) * 0.25, 0.0, 0.0, 1.0 );
}
)";

//---------------------------------------------------------------------------
// Pass 3: stabilise.
//---------------------------------------------------------------------------
const char* const kStabiliseShader = R"(#version 410 core

uniform sampler2D EdgeTexture;
uniform sampler2D HistoryTexture; //the previous frame's output of this pass
uniform float Attack;             //0..1 blend towards a *stronger* edge
uniform float Release;            //0..1 blend towards a *weaker* edge
uniform float Sensitivity;        //gradient magnitude at which a lamp is fully lit
uniform float Softness;           //width of the threshold, as a fraction of it
uniform float Reset;              //1 to ignore history entirely

in vec2 uv;
out vec4 fragColor;

void main()
{
	float current = texture( EdgeTexture, uv ).r;
	float history = texture( HistoryTexture, uv ).r;

	//Asymmetric on purpose, and this is the whole of "survives video".
	//
	//A symmetric IIR is a low-pass, and a low-pass on an edge signal trades
	//flicker for lag: the outline of anything moving arrives late and smeared
	//behind it. What actually goes wrong on footage is not that edges move,
	//it is that they *drop out* -- a boundary that grades through the
	//threshold for one frame, or sensor noise on a nearly-flat gradient, and
	//the lamp blinks. So rise fast enough to be immediate and fall slowly
	//enough to bridge the gap. An edge that appears is believed at once; an
	//edge that vanishes is given a few frames to come back.
	float blend = current > history ? Attack : Release;
	float stable = mix( history, current, blend ) * ( 1.0 - Reset ) + current * Reset;

	//Threshold with a soft shoulder rather than a step. A hard threshold makes
	//Sensitivity a control that does nothing at all and then everything at
	//once, and puts a stack of aliasing on every diagonal.
	float lower = Sensitivity * ( 1.0 - Softness );
	float upper = Sensitivity * ( 1.0 + Softness );
	float mask = smoothstep( lower, max( upper, lower + 1e-5 ), stable );

	//r: what feeds back, before the threshold, so that Sensitivity can be
	//   moved without the history having to re-converge.
	//gb: the first moments of the mask, for the centroid. Reduced to a single
	//   texel by the mip chain, so the strip coordinate can rotate about the
	//   artwork instead of about the middle of the frame.
	//a: the mask itself, which is also the denominator of that average.
	fragColor = vec4( stable, uv.x * mask, uv.y * mask, mask );
}
)";

//---------------------------------------------------------------------------
// Pass 4: light. The plugin.
//
// Assembled from three pieces by LightShaderSource(), because the middle one
// is shared with the harness. See Shaders.h.
//---------------------------------------------------------------------------
const char* const kLightPreamble = R"(#version 410 core

uniform sampler2D StableTexture;  //r = raw stable edge, a = mask, gb = moments
uniform sampler2D CopyTexture;    //the picture, for Source Tint
uniform sampler2D PaletteTexture; //kPaletteSize x Palette::Count, texelFetch only

uniform float Aspect;        //width / height, so a circle is a circle
uniform float CentroidLod;   //top of the stable buffer's mip chain
uniform float Thickness;     //mip level the mask is read at, which dilates it
uniform float Layout;        //0 spiral, 1 angle, 2 linear, 3 radial, 4 random
uniform float LayoutAngle;   //turns, for Linear
uniform float Turns;         //for Spiral
uniform float BulbCount;
uniform float BulbSize;      //1.0 and over is a continuous rope
uniform float Reverse;       //1 to run the strip the other way

uniform float EffectIndex;
uniform float Time;          //seconds, already multiplied by Speed
uniform float Intensity;
uniform float Spread;

uniform float PaletteIndex;
uniform float PaletteCount;
uniform vec3 Colour1;
uniform vec3 Colour2;
uniform float Saturation;
uniform float Brightness;
uniform float SourceTint;

uniform float Audio[ 64 ]; //smoothed spectrum, low frequencies first
uniform float AudioLevel;  //0 ignores the spectrum entirely

in vec2 uv;
out vec4 fragColor;

const int kPaletteSize = 256;
)";

//---------------------------------------------------------------------------
// The effect library. A fragment: no #version, no main, no uniforms, and no
// dependency on anything outside itself. Shared verbatim between the light
// pass and the harness's probe.
//---------------------------------------------------------------------------
const char* const kEffectLibrarySource = R"(
const float kTau = 6.283185307179586;             //= mirrored
const uint kOdd = 2654435761u;                    //= mirrored

//---------------------------------------------------------------------------
// Randomness. Integer throughout, because this has to give the same answer as
// Effects.cpp and anything built on sin() gives the driver's answer instead.
//---------------------------------------------------------------------------
uint HashInt( uint x )
{
	//= mirrored
	x = x * 747796405u + 2891336453u;
	uint w = ( ( x >> ( ( x >> 28u ) + 4u ) ) ^ x ) * 277803737u;
	return ( w >> 22u ) ^ w;
}

float Hash01( uint x )
{
	//= mirrored
	return float( HashInt( x ) ) * ( 1.0 / 4294967296.0 );
}

uint slotBits( float slot )
{
	//= mirrored -- via int, because float-to-uint of a negative is undefined
	return uint( int( slot ) );
}

//---------------------------------------------------------------------------
// The effect library. Mirrored from Effects.cpp; see the header there.
// Returns (palette position, brightness).
//---------------------------------------------------------------------------
vec2 evaluate( int effect, float s, uint bulb, float t, float intensity, float spread )
{
	float base = s * spread;//= mirrored

	if( effect == 0 )       //Solid
		return vec2( 0.0, 1.0 );//= mirrored

	if( effect == 1 )       //Gradient
		return vec2( base, 1.0 );//= mirrored

	if( effect == 2 )       //Rainbow
		return vec2( base + t, 1.0 );//= mirrored

	if( effect == 3 )       //Colour Loop
		return vec2( t, 1.0 );//= mirrored

	if( effect == 4 )       //Chase
	{
		//= mirrored
		float groups = 1.0 + floor( intensity * 7.0 );
		float phase = fract( s * groups - t );
		float duty = 0.15 + 0.35 * ( 1.0 - intensity );
		return vec2( base, 1.0 - smoothstep( duty * 0.6, duty, phase ) );
	}

	if( effect == 5 )       //Theater Chase
	{
		//Integer arithmetic on purpose -- see the long comment in Effects.cpp.
		//`mod( float( bulb ) - slot, spacing )` returns `spacing` rather than
		//zero on an exact multiple here, and puts out the one lamp that should
		//be brightest.
		//= mirrored
		uint spacing = uint( 2.0 + floor( intensity * 5.0 ) );
		uint slot = slotBits( floor( t * float( spacing ) ) );
		uint lit = ( bulb % spacing + spacing - slot % spacing ) % spacing;
		return vec2( base, lit == 0u ? 1.0 : 0.0 );
	}

	if( effect == 6 )       //Running Lights
	{
		//= mirrored
		float waves = 1.0 + floor( intensity * 5.0 );
		return vec2( base, 0.5 + 0.5 * sin( kTau * ( s * waves - t ) ) );
	}

	if( effect == 7 )       //Comet
	{
		//= mirrored
		float head = fract( t );
		float behind = fract( head - s );
		float tail = 0.04 + 0.40 * intensity;
		return vec2( base, exp( -behind / tail ) );
	}

	if( effect == 8 )       //Meteor
	{
		//= mirrored
		float head = fract( t );
		float behind = fract( head - s );
		float tail = 0.04 + 0.40 * intensity;
		float body = exp( -behind / tail );
		float grain = Hash01( bulb * kOdd ^ slotBits( floor( t * 12.0 ) ) );
		float bite = clamp( behind / tail, 0.0, 1.0 );
		return vec2( base, body * mix( 1.0, grain, bite ) );
	}

	if( effect == 9 )       //Larson Scanner
	{
		//= mirrored
		float bounce = abs( fract( t * 0.5 ) * 2.0 - 1.0 );
		float tail = 0.02 + 0.22 * intensity;
		return vec2( base, exp( -abs( s - bounce ) / tail ) );
	}

	if( effect == 10 )      //Colour Wipe
	{
		//= mirrored
		float level = fract( t );
		float pass = mod( floor( t ), 2.0 );
		float on = s < level ? 1.0 : 0.0;
		return vec2( mix( pass, 1.0 - pass, on ) * 0.5, 1.0 );
	}

	if( effect == 11 )      //Twinkle
	{
		//= mirrored
		float r = Hash01( bulb );
		float rate = 0.4 + r * 0.9;
		float v = 0.5 + 0.5 * sin( kTau * ( t * rate + r ) );
		float threshold = 1.0 - clamp( intensity, 0.02, 0.98 );
		return vec2( base + r, smoothstep( threshold, 1.0, v ) );
	}

	if( effect == 12 )      //Sparkle
	{
		//= mirrored
		uint slot = slotBits( floor( t * 8.0 ) );
		float pick = Hash01( bulb * kOdd ^ slot );
		float density = 0.02 + intensity * 0.35;
		float decay = exp( -fract( t * 8.0 ) * 4.0 );
		return vec2( Hash01( bulb ^ slot ^ 0x9E3779B9u ), pick < density ? decay : 0.0 );
	}

	if( effect == 13 )      //Glitter
	{
		//= mirrored
		uint slot = slotBits( floor( t * 8.0 ) );
		float pick = Hash01( bulb * kOdd ^ slot );
		float density = 0.02 + intensity * 0.30;
		float decay = exp( -fract( t * 8.0 ) * 5.0 );
		float spark = pick < density ? decay : 0.0;
		return vec2( base + t * 0.1, clamp( 0.35 + spark, 0.0, 1.0 ) );
	}

	if( effect == 14 )      //Fire Flicker
	{
		//= mirrored
		float slotF = floor( t * 6.0 );
		uint slot = slotBits( slotF );
		float a = Hash01( bulb * kOdd ^ slot );
		float b = Hash01( bulb * kOdd ^ ( slot + 1u ) );
		float n = mix( a, b, smoothstep( 0.0, 1.0, fract( t * 6.0 ) ) );
		float depth = 0.25 + intensity * 0.70;
		return vec2( 0.25 + n * 0.70, clamp( 1.0 - depth * ( 1.0 - n ), 0.0, 1.0 ) );
	}

	if( effect == 15 )      //Breathe
	{
		//= mirrored
		float v = 0.5 + 0.5 * sin( kTau * t );
		return vec2( base, 0.08 + 0.92 * v * v );
	}

	if( effect == 16 )      //Strobe
	{
		//= mirrored
		float duty = 0.02 + intensity * 0.30;
		return vec2( base, fract( t ) < duty ? 1.0 : 0.0 );
	}

	if( effect == 17 )      //Dissolve
	{
		//= mirrored
		float level = fract( t );
		float pass = mod( floor( t ), 2.0 );
		float on = Hash01( bulb ) < level ? 1.0 : 0.0;
		return vec2( mix( pass, 1.0 - pass, on ) * 0.5, 1.0 );
	}

	if( effect == 18 )      //Colorwaves
	{
		//= mirrored
		float position = base + 0.30 * sin( kTau * ( s * 1.7 - t * 0.6 ) ) + t * 0.25;
		float bright = 0.55 + 0.45 * sin( kTau * ( s * 2.3 + t * 0.4 ) );
		return vec2( position, bright );
	}

	if( effect == 19 )      //Fairy Lights
	{
		//= mirrored
		float r = Hash01( bulb );
		float g = Hash01( bulb ^ 0x51ED2701u );
		float ph = fract( t * ( 0.12 + r * 0.20 ) + r );
		float v = smoothstep( 0.0, 0.18, ph ) * ( 1.0 - smoothstep( 0.32, 0.62, ph ) );
		float taking = g < ( 0.10 + intensity * 0.85 ) ? 1.0 : 0.0;
		return vec2( base + r * 0.35, v * taking );
	}

	return vec2( 0.0, 0.0 );
}
)";

//---------------------------------------------------------------------------
// The rest of the light pass.
//---------------------------------------------------------------------------
const char* const kLightMain = R"(
//---------------------------------------------------------------------------
// Colour.
//---------------------------------------------------------------------------
vec3 paletteColour( float position )
{
	position = fract( position );

	int index = int( PaletteIndex + 0.5 );
	if( index == 0 )
		return Colour1;
	if( index == 1 )
		return mix( Colour1, Colour2, position );

	//texelFetch rather than texture(). One texture has one filter for both
	//axes, and bilinear on this one would blend a palette into the palette
	//below it along the way -- so the interpolation along the gradient is done
	//here, from two exact fetches, and the sampler never filters anything.
	float p = position * float( kPaletteSize - 1 );
	float i0 = floor( p );
	int a = int( i0 );
	int b = min( a + 1, kPaletteSize - 1 );

	vec3 ca = texelFetch( PaletteTexture, ivec2( a, index ), 0 ).rgb;
	vec3 cb = texelFetch( PaletteTexture, ivec2( b, index ), 0 ).rgb;
	return mix( ca, cb, p - i0 );
}

//---------------------------------------------------------------------------
// Where along the strip this pixel is.
//---------------------------------------------------------------------------
float stripCoordinate( vec2 p )
{
	//`wiring`, not `layout`. `layout` is a GLSL keyword, and using it here
	//fails to compile with nothing but "syntax error" and a line number -- which
	//in a shader assembled from three strings is a line number in a file that
	//does not exist. The uniform above keeps its capital L because the reserved
	//word is case-sensitive.
	int wiring = int( Layout + 0.5 );

	float angle = atan( p.y, p.x ) / kTau + 0.5;
	float radius = length( p );

	if( wiring == 1 )   //Angle -- once round the artwork is one strip
		return angle;

	if( wiring == 2 )   //Linear -- a plain projection, which is a wipe
	{
		float a = LayoutAngle * kTau;
		return dot( p, vec2( cos( a ), sin( a ) ) ) * 0.5 + 0.5;
	}

	if( wiring == 3 )   //Radial -- rings out from the middle
		return radius;

	//Spiral (0), and the geometry Random (4) is scrambled from.
	//
	//This is the default because it is what a string of lights actually does
	//when someone puts it on a tree: it goes round, and it climbs. An angle
	//alone gives a chase that runs round the outline like a clock hand, which
	//is the marquee look rather than the tree look.
	return angle + radius * Turns;
}

void main()
{
	//Thickness is a dilation done with the mip chain that is already there.
	//Blurring a thin ridge lowers its peak in proportion to how much it spread,
	//so reading it blurred and then restoring the peak widens the base without
	//a second buffer or a single extra tap. The gain is exactly 1.0 at zero, so
	//the control is a true identity at the bottom of its travel rather than
	//nearly one.
	float mask = textureLod( StableTexture, uv, Thickness ).a;
	mask = clamp( mask * ( 1.0 + Thickness * 2.0 ), 0.0, 1.0 );

	//The middle of the artwork, not the middle of the frame. Reduced out of
	//the stable buffer's mip chain in one fetch: the top level is the average
	//of (x*mask, y*mask, mask) over the picture, and the first two divided by
	//the third are the centroid. A frame with no edges in it divides by zero,
	//so it falls back to the centre.
	vec4 moments = textureLod( StableTexture, vec2( 0.5 ), CentroidLod );
	vec2 centre = moments.a > 1e-4 ? moments.gb / moments.a : vec2( 0.5 );

	vec2 p = ( uv - centre ) * vec2( Aspect, 1.0 );

	//Unwrapped: the coordinate is allowed to run past 1 and is only folded back
	//where it is used. The wrapping has to happen after the derivative below,
	//not before it.
	float sRaw = stripCoordinate( p );
	if( Reverse > 0.5 )
		sRaw = -sRaw;

	float bulbF = sRaw * BulbCount;

	//How fast the strip runs here, in lamps per pixel.
	//
	//This is what makes a lamp a fixed size *on screen* rather than a fixed
	//fraction of its own spacing, and it is the difference between lamps and
	//streaks. The field's gradient varies enormously across a frame -- a spiral
	//about the centroid turns fast near the middle and slowly at the edges --
	//so a window measured in strip units is a dot in one place and a long smear
	//in another, and Lamp Size means two different things in two parts of the
	//same picture. Measured in pixels instead, a lamp is a lamp everywhere.
	//
	//The jump removal matters. `angle` has a branch cut where atan2 wraps, and
	//across that one line the derivative reads as a whole strip per pixel --
	//which would put a hard bright seam along it. A discontinuity of exactly
	//one full strip is the cut and not a gradient, so subtract it off.
	vec2 slope = vec2( dFdx( bulbF ), dFdy( bulbF ) );
	slope -= BulbCount * round( slope / BulbCount );
	float lampsPerPixel = max( length( slope ), 1e-4 );

	//Quantise to a lamp. Every pixel of one lamp has to pass the same s to the
	//effect, or the two halves of a bulb disagree about what colour it is.
	float cell = floor( bulbF );
	float wrapped = cell - floor( cell / BulbCount ) * BulbCount;
	uint bulb = uint( int( wrapped ) );
	float sBulb = fract( ( cell + 0.5 ) / BulbCount );

	//Random layout: keep every lamp where it is and shuffle its place in the
	//running order, which is a strip wired in an arbitrary sequence rather
	//than a strip laid out at random.
	if( int( Layout + 0.5 ) == 4 )
		sBulb = Hash01( bulb );

	vec2 result = evaluate( int( EffectIndex + 0.5 ), sBulb, bulb, Time, Intensity, Spread );

	//Audio: a per-lamp brightness gate from the host's FFT, laid along the
	//strip's running order with the low frequencies at the start. Orthogonal
	//to the pattern on purpose -- Solid with Audio Level up is a spectrum
	//analyser hung along the outline, and every other pattern ducks and
	//swells where its own slice of the spectrum does. Deliberately NOT inside
	//the effect library: the library is mirrored in Effects.cpp and proved
	//against it, and the spectrum is host state the harness cannot mirror.
	float band = Audio[ clamp( int( sBulb * 64.0 ), 0, 63 ) ];
	result.y *= mix( 1.0, band, AudioLevel );

	//The lamp itself: distance from its centre, in pixels, against a radius in
	//pixels. Where the radius exceeds half the spacing the lamps run into one
	//another and the strip becomes a continuous rope -- the neon-sign look,
	//from the same control rather than from a second mode.
	float dCentre = abs( fract( bulbF ) - 0.5 ) / lampsPerPixel;
	float lamp = 1.0 - smoothstep( BulbSize * 0.55, BulbSize, dCentre );

	vec3 colour = paletteColour( result.x );

	//Desaturate towards the lamp's own luminance, so Saturation at zero gives
	//white bulbs rather than grey ones.
	float y = dot( colour, vec3( 0.2126, 0.7152, 0.0722 ) );
	colour = mix( vec3( y ), colour, Saturation );

	//Source Tint multiplies by the artwork underneath, so a logo can light its
	//own outline in its own colours. Un-premultiplied first: the copy is
	//premultiplied and multiplying by it directly would darken the tint
	//wherever the artwork happens to be soft.
	if( SourceTint > 0.0 )
	{
		vec4 src = texture( CopyTexture, uv );
		vec3 straight = src.a > 0.0031 ? src.rgb / src.a : src.rgb;
		colour = mix( colour, colour * straight, SourceTint );
	}

	float amount = clamp( result.y, 0.0, 1.0 ) * lamp * mask * Brightness;

	//Premultiplied out, matching everything else in the chain.
	fragColor = vec4( colour * amount, amount );
}
)";

//---------------------------------------------------------------------------
// Pass 5: blur. Run twice, once per axis.
//---------------------------------------------------------------------------
const char* const kBlurShader = R"(#version 410 core

uniform sampler2D SourceTexture;
uniform vec2 Direction;  //one tap step, in picture space. Zero on the other axis.
uniform float SourceLod; //mip level to read at. Non-zero only on the first pass.

in vec2 uv;
out vec4 fragColor;

void main()
{
	//A nine-tap Gaussian folded into five fetches. The offsets are not texel
	//centres: each fetch sits between two texels so that GL_LINEAR returns
	//their weighted average, which is why this needs Sampling::Linear and
	//would silently become a five-tap box on a Nearest buffer.
	const float offsets[ 3 ] = float[]( 0.0, 1.3846153846, 3.2307692308 );
	const float weights[ 3 ] = float[]( 0.2270270270, 0.3162162162, 0.0702702703 );

	//The level matters on the first pass and only there.
	//
	//That pass reads the full-resolution light buffer while drawing into a
	//quarter-size glow buffer, so five point samples per output texel are
	//point-sampling a picture four times finer than the target -- which is not
	//a blur, it is an aliased downsample. Lamps are a few pixels across, so
	//each one is either caught or missed depending on where it happens to sit,
	//and the miss pattern beats against the lamp spacing. It shows up as
	//straight streaks running across the frame, far longer than the blur's own
	//radius, which is what makes it look like anything other than a sampling
	//problem. Reading the pre-filtered level costs nothing and removes it.
	vec4 sum = textureLod( SourceTexture, uv, SourceLod ) * weights[ 0 ];
	for( int i = 1; i < 3; ++i )
	{
		sum += textureLod( SourceTexture, uv + Direction * offsets[ i ], SourceLod ) * weights[ i ];
		sum += textureLod( SourceTexture, uv - Direction * offsets[ i ], SourceLod ) * weights[ i ];
	}

	fragColor = sum;
}
)";

//---------------------------------------------------------------------------
// Pass 6: composite.
//---------------------------------------------------------------------------
const char* const kCompositeShader = R"(#version 410 core

uniform sampler2D CopyTexture;
uniform sampler2D LightTexture;
uniform sampler2D GlowTexture;
uniform sampler2D StableTexture;

uniform float Background;  //0 black, 1 source, 2 dimmed source, 3 transparent, 4 edges
uniform float Dim;
uniform float Glow;
uniform float MixAmount;

in vec2 uv;
out vec4 fragColor;

void main()
{
	vec4 source = texture( CopyTexture, uv );
	vec4 light = texture( LightTexture, uv );
	vec4 glow = texture( GlowTexture, uv );

	//The glow is added, not blended. A bulb's halo is light arriving on top of
	//whatever is already there, and alpha-blending it would have the halo
	//*hide* the artwork it is supposed to be sitting on.
	vec4 lit = light + glow * Glow;

	int mode = int( Background + 0.5 );

	vec4 result;
	if( mode == 4 )
	{
		//The edge mask on its own, in white. Not a look -- it is how
		//Sensitivity, Detail and Thickness are actually set, because judging a
		//threshold through a layer of lamps and glow is guesswork.
		float mask = texture( StableTexture, uv ).a;
		result = vec4( vec3( mask ), 1.0 );
	}
	else if( mode == 3 )
	{
		//Premultiplied already, so this is the plugin's own output over
		//nothing, ready for the layer below to show through.
		result = lit;
	}
	else
	{
		vec4 back = source;
		if( mode == 0 )
			back = vec4( 0.0, 0.0, 0.0, 1.0 );
		else if( mode == 2 )
			back = vec4( source.rgb * Dim, source.a );

		result = vec4( back.rgb + lit.rgb, clamp( back.a + lit.a, 0.0, 1.0 ) );
	}

	fragColor = mix( source, result, MixAmount );
}
)";

//---------------------------------------------------------------------------
// Assembly.
//---------------------------------------------------------------------------
const char* const kEffectLibrary = kEffectLibrarySource;

std::string LightShaderSource()
{
	return std::string( kLightPreamble ) + kEffectLibrarySource + kLightMain;
}

std::string EffectProbeShaderSource()
{
	//One lamp per pixel across a strip-wide, one-pixel-tall target, with
	//`evaluate()`'s two outputs written straight to red and green. No palette,
	//no mask, no lamp shape: those are the light pass's business and folding
	//any of them in here would mean a disagreement could not be attributed.
	//
	//The target is RGBA32F, so what comes back is the shader's own float and
	//not a quantised version of it -- an 8-bit readback would agree to within
	//1/255 with almost any drift and call it a pass.
	static const char* const probeMain = R"(
uniform float EffectIndex;
uniform float BulbCount;
uniform float TimeValue;
uniform float Intensity;
uniform float Spread;

out vec4 fragColor;

void main()
{
	uint bulb = uint( int( gl_FragCoord.x - 0.5 ) );
	float s = ( float( bulb ) + 0.5 ) / BulbCount;
	vec2 r = evaluate( int( EffectIndex + 0.5 ), s, bulb, TimeValue, Intensity, Spread );
	fragColor = vec4( r.x, r.y, 0.0, 1.0 );
}
)";

	return std::string( "#version 410 core\n" ) + kEffectLibrarySource + probeMain;
}

} // namespace tinsel
