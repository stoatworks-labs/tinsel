/**
 * Tinsel — browser demo.
 *
 * The six shaders below are copied unedited from `source/Shaders.cpp`, light
 * pass included: `LIGHT_SHADER` is assembled from the same three pieces
 * `LightShaderSource()` concatenates, so the effect library in the middle is the
 * same text the plugin compiles.
 *
 * `Controls.cpp` (the 0..1 conversions), `Palette.cpp` (the stop lists and the
 * bake) and `Presets.h` are ported below rather than re-derived. The parameter
 * declarations, their names, groups, order, elements and defaults come from
 * `Tinsel.cpp`'s constructor.
 *
 * The one idea, before any of it: **an effect never chooses a colour.** It
 * returns where in the palette a lamp sits and how bright it is, and the palette
 * does the rest — which is why twenty patterns and sixteen palettes are worth
 * more than thirty-six of either, and why `evaluate()` can be one self-contained
 * function shared between the plugin, its harness and this page.
 */

import { mountDemo } from './vendor/demo.js';
import { Program, PassBuffer, bindTexture, mipLevels } from './vendor/gl.js';

//===========================================================================
// The shaders. Copied from source/Shaders.cpp.
//===========================================================================

const VERTEX_SHADER = `#version 410 core

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
`;

const COPY_SHADER = `#version 410 core

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
`;

const EDGE_SHADER = `#version 410 core

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
		//Written as \`max( luma * c.a, c.a )\` this is identically 1.0 for every
		//opaque pixel, so the mode found no edges at all on any clip without an
		//alpha channel -- which, being the default, meant the effect did
		//nothing at all out of the box. It cost 27 of 31 controls reading as
		//dead in \`tools/sweep.py\`, which is the only reason it was found: one
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
`;

const STABILISE_SHADER = `#version 410 core

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
`;

//---------------------------------------------------------------------------
// The light pass, in the three pieces LightShaderSource() concatenates.
//---------------------------------------------------------------------------

const LIGHT_PREAMBLE = `#version 410 core

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
`;

/**
 * The effect library. A fragment: no #version, no main, no uniforms, and no
 * dependency on anything outside itself — which is what lets the plugin, its
 * harness's probe and this page all compile the same text.
 *
 * `//= mirrored` marks every line with a counterpart in `Effects.cpp`. The
 * markers are kept here for the same reason they exist there: this is the file
 * that has to be changed when that one is.
 */
const EFFECT_LIBRARY = `
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
		//\`mod( float( bulb ) - slot, spacing )\` returns \`spacing\` rather than
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
`;

const LIGHT_MAIN = `
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
	//\`wiring\`, not \`layout\`. \`layout\` is a GLSL keyword, and using it here
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
	//The jump removal matters. \`angle\` has a branch cut where atan2 wraps, and
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
`;

/// The same concatenation `LightShaderSource()` does, for the same reason: one
/// copy of the effect library, compiled into whatever needs it.
const LIGHT_SHADER = LIGHT_PREAMBLE + EFFECT_LIBRARY + LIGHT_MAIN;

const BLUR_SHADER = `#version 410 core

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
`;

const COMPOSITE_SHADER = `#version 410 core

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
`;

//===========================================================================
// Ports of source/Controls.cpp
//
// Every numeric parameter the plugin declares is a plain 0..1 float, including
// the ones standing for a lamp count or a mip level: SetParamInfo clamps a
// standard default into 0..1 before a range can be attached, so a parameter
// declared in lamps cannot declare a default in lamps. These are the same
// conversions, so the number beside each slider is the plugin's own.
//===========================================================================

const clamp01 = (v) => Math.min(Math.max(v, 0), 1);
const lerp = (from, to, t) => from + (to - from) * clamp01(t);

/// Equal slider movements are equal *ratios*, for any quantity where the
/// question is "how many times more" rather than "how much more".
const geometric = (from, to, t) => from * Math.pow(to / from, clamp01(t));

const sensitivityFromParam = (v) => geometric(0.01, 1.0, v);
const softnessFromParam = (v) => lerp(0.05, 1.0, v);
const detailFromParam = (v) => lerp(0.0, 4.0, v);
const thicknessFromParam = (v) => lerp(0.0, 3.5, v);
const attackFromParam = (v) => lerp(1.0, 0.75, v);
const releaseFromParam = (v) => geometric(1.0, 0.02, v);
const bulbCountFromParam = (v) => Math.floor(geometric(8.0, 1000.0, v) + 0.5);
const bulbSizeFromParam = (v) => geometric(0.75, 40.0, v);
const turnsFromParam = (v) => lerp(0.0, 8.0, v);
const layoutAngleFromParam = (v) => clamp01(v);
const speedFromParam = (v) => lerp(0.0, 4.0, v);
const spreadFromParam = (v) => geometric(0.25, 8.0, v);
const brightnessFromParam = (v) => lerp(0.0, 2.0, v);
const saturationFromParam = (v) => lerp(0.0, 1.5, v);
const glowFromParam = (v) => lerp(0.0, 3.0, v);
const glowSizeFromParam = (v) => geometric(0.5, 8.0, v);

//===========================================================================
// Port of source/Palette.cpp
//
// The stop lists, verbatim. Two rules were followed when they were written and
// both survive the port: a palette must not go to black unless black is the
// point (Fire is the one deliberate exception, because its black end is what
// makes the embers work), and the stops sit where the eye puts the boundary
// rather than where the arithmetic does.
//===========================================================================

const PALETTE_SIZE = 256;

const RAINBOW = [
  [0.000, 1.00, 0.00, 0.00],
  [0.167, 1.00, 1.00, 0.00],
  [0.333, 0.00, 1.00, 0.00],
  [0.500, 0.00, 1.00, 1.00],
  [0.667, 0.00, 0.00, 1.00],
  [0.833, 1.00, 0.00, 1.00],
  [1.000, 1.00, 0.00, 0.00],
];

const PARTY = [
  [0.000, 0.34, 0.00, 0.66],
  [0.200, 0.78, 0.00, 0.35],
  [0.400, 1.00, 0.10, 0.00],
  [0.600, 1.00, 0.45, 0.00],
  [0.800, 1.00, 0.80, 0.05],
  [1.000, 0.34, 0.00, 0.66],
];

const CHRISTMAS = [
  [0.000, 0.90, 0.03, 0.03],
  [0.400, 0.90, 0.03, 0.03],
  [0.460, 1.00, 0.85, 0.60],
  [0.540, 1.00, 0.85, 0.60],
  [0.600, 0.02, 0.65, 0.10],
  [1.000, 0.02, 0.65, 0.10],
];

const CANDY_CANE = [
  [0.000, 0.95, 0.02, 0.06],
  [0.450, 0.95, 0.02, 0.06],
  [0.550, 1.00, 0.96, 0.92],
  [1.000, 1.00, 0.96, 0.92],
];

const WARM_WHITE = [
  [0.000, 1.00, 0.58, 0.18],
  [0.400, 1.00, 0.78, 0.46],
  [0.750, 1.00, 0.92, 0.78],
  [1.000, 1.00, 0.99, 0.95],
];

const FROST = [
  [0.000, 0.02, 0.10, 0.45],
  [0.350, 0.05, 0.45, 0.85],
  [0.700, 0.45, 0.85, 1.00],
  [1.000, 0.92, 0.99, 1.00],
];

const FIRE = [
  [0.000, 0.00, 0.00, 0.00],
  [0.180, 0.55, 0.02, 0.00],
  [0.450, 1.00, 0.16, 0.00],
  [0.720, 1.00, 0.60, 0.02],
  [1.000, 1.00, 0.98, 0.72],
];

const OCEAN = [
  [0.000, 0.00, 0.06, 0.28],
  [0.300, 0.00, 0.28, 0.52],
  [0.600, 0.02, 0.62, 0.60],
  [0.850, 0.35, 0.85, 0.72],
  [1.000, 0.85, 0.97, 0.92],
];

const FOREST = [
  [0.000, 0.01, 0.16, 0.04],
  [0.300, 0.05, 0.40, 0.06],
  [0.600, 0.30, 0.62, 0.05],
  [0.850, 0.58, 0.72, 0.12],
  [1.000, 0.28, 0.50, 0.08],
];

const SUNSET = [
  [0.000, 0.20, 0.03, 0.35],
  [0.250, 0.60, 0.06, 0.38],
  [0.500, 0.95, 0.24, 0.18],
  [0.750, 1.00, 0.55, 0.10],
  [1.000, 1.00, 0.85, 0.42],
];

const CYBERPUNK = [
  [0.000, 1.00, 0.03, 0.55],
  [0.350, 0.65, 0.05, 0.95],
  [0.650, 0.05, 0.55, 1.00],
  [0.850, 0.05, 1.00, 0.90],
  [1.000, 1.00, 0.03, 0.55],
];

const GOLD = [
  [0.000, 0.42, 0.18, 0.00],
  [0.350, 0.85, 0.48, 0.03],
  [0.700, 1.00, 0.78, 0.20],
  [1.000, 1.00, 0.95, 0.62],
];

const MAGENTA = [
  [0.000, 0.35, 0.00, 0.45],
  [0.500, 1.00, 0.05, 0.65],
  [1.000, 1.00, 0.72, 0.92],
];

const MONO = [
  [0.000, 1.00, 1.00, 1.00],
  [1.000, 1.00, 1.00, 1.00],
];

/// Rows 0 and 1 have no stop list: they are the two Colour parameters, and the
/// shader returns those directly rather than reading this texture.
const PALETTES = [
  [null, 'Colour 1'],
  [null, 'Colour 1 > 2'],
  [RAINBOW, 'Rainbow'],
  [PARTY, 'Party'],
  [CHRISTMAS, 'Christmas'],
  [CANDY_CANE, 'Candy Cane'],
  [WARM_WHITE, 'Warm White'],
  [FROST, 'Frost'],
  [FIRE, 'Fire'],
  [OCEAN, 'Ocean'],
  [FOREST, 'Forest'],
  [SUNSET, 'Sunset'],
  [CYBERPUNK, 'Cyberpunk'],
  [GOLD, 'Gold'],
  [MAGENTA, 'Magenta'],
  [MONO, 'Mono'],
];

const PALETTE_NAMES = PALETTES.map(([, name]) => name);
const FIRST_BAKED_PALETTE = 2;

/// Interpolate a stop list. The list is sorted and its first stop sits at 0, so
/// a position below the first stop cannot happen and is not handled.
function samplePalette(stops, position) {
  position = clamp01(position);

  for (let i = 1; i < stops.length; i += 1) {
    const previous = stops[i - 1];
    const current = stops[i];
    if (position > current[0]) continue;

    const span = current[0] - previous[0];
    // Two stops at the same position are how a hard edge is written. Guard the
    // divide rather than forbidding it.
    const t = span > 0 ? (position - previous[0]) / span : 0;
    return [
      previous[1] + (current[1] - previous[1]) * t,
      previous[2] + (current[2] - previous[2]) * t,
      previous[3] + (current[3] - previous[3]) * t,
    ];
  }

  const last = stops[stops.length - 1];
  return [last[1], last[2], last[3]];
}

/// BakePaletteTable(): 256 × 16 RGBA float, texelFetch only.
function bakePaletteTable() {
  const table = new Float32Array(PALETTES.length * PALETTE_SIZE * 4);

  for (let row = FIRST_BAKED_PALETTE; row < PALETTES.length; row += 1) {
    const stops = PALETTES[row][0];
    for (let x = 0; x < PALETTE_SIZE; x += 1) {
      // Texel centres, so entry 0 is position 0 and entry 255 is position 1 —
      // not 255/256, which would leave a seam where a wrapping palette meets
      // itself.
      const [r, g, b] = samplePalette(stops, x / (PALETTE_SIZE - 1));
      const offset = (row * PALETTE_SIZE + x) * 4;
      table[offset + 0] = r;
      table[offset + 1] = g;
      table[offset + 2] = b;
      table[offset + 3] = 1;
    }
  }

  return table;
}

//===========================================================================
// Declarations, from Tinsel.cpp's constructor.
//===========================================================================

const EFFECT_NAMES = [
  'Solid',
  'Gradient',
  'Rainbow',
  'Colour Loop',
  'Chase',
  'Theater Chase',
  'Running Lights',
  'Comet',
  'Meteor',
  'Larson Scanner',
  'Colour Wipe',
  'Twinkle',
  'Sparkle',
  'Glitter',
  'Fire Flicker',
  'Breathe',
  'Strobe',
  'Dissolve',
  'Colorwaves',
  'Fairy Lights',
];

const SOURCE_NAMES = ['Luma', 'Alpha', 'Chroma', 'Luma or Alpha'];
const LAYOUT_NAMES = ['Spiral', 'Angle', 'Linear', 'Radial', 'Random'];
const BACKGROUND_NAMES = ['Black', 'Source', 'Dimmed Source', 'Transparent', 'Edges'];
const SYNC_NAMES = ['Free', 'Beat', 'Bar'];

const pct = (v) => `${Math.round(v * 100)}%`;

//===========================================================================
// Port of source/Presets.h
//
// The plugin declares Preset as an option parameter whose elements are these,
// with element 0 "Custom" meaning the sliders are the truth. The page uses the
// kit's own preset menu instead of a dropdown that would sit in the panel
// pretending to be a parameter — but the values are the plugin's table, in the
// plugin's order, unedited. A preset covers layout, animation, colour and look;
// the Edge group and Mix are deliberately left alone, because sensitivity and
// thickness are tuned to the operator's own artwork.
//===========================================================================

/// The parameters a preset sets, in the fixed order `presets::Param` declares.
const PRESET_PARAM_IDS = [
  'layout', 'turns', 'direction', 'lamps', 'lampSize', 'reverse',
  'pattern', 'speed', 'intensity',
  'palette', 'spread',
  'c1r', 'c1g', 'c1b', 'c2r', 'c2g', 'c2b',
  'saturation', 'brightness', 'sourceTint',
  'glow', 'glowSize', 'background', 'dim',
];

const PRESET_TABLE = [
  ['Warm Twinkle', [0, 0.08, 0.0, 0.55, 0.45, 0, 11, 0.25, 0.5, 6, 0.4,
    1.0, 0.72, 0.36, 0.1, 0.55, 1.0, 0.667, 0.5, 0.0, 0.4, 0.35, 0, 0.25]],
  ['Christmas Chase', [0, 0.08, 0.0, 0.6, 0.45, 0, 4, 0.35, 0.6, 4, 0.5,
    1.0, 0.72, 0.36, 0.1, 0.55, 1.0, 0.667, 0.5, 0.0, 0.5, 0.35, 0, 0.25]],
  ['Candy Wipe', [0, 0.08, 0.0, 0.6, 0.5, 0, 10, 0.3, 0.6, 5, 0.5,
    1.0, 0.72, 0.36, 0.1, 0.55, 1.0, 0.667, 0.5, 0.0, 0.45, 0.35, 0, 0.25]],
  ['Fire Flicker', [0, 0.08, 0.0, 0.55, 0.5, 0, 14, 0.3, 0.65, 8, 0.45,
    1.0, 0.72, 0.36, 0.1, 0.55, 1.0, 0.667, 0.5, 0.0, 0.6, 0.5, 0, 0.25]],
  ['Cyber Larson', [2, 0.08, 0.0, 0.55, 0.45, 0, 9, 0.45, 0.7, 12, 0.45,
    1.0, 0.72, 0.36, 0.1, 0.55, 1.0, 0.667, 0.5, 0.0, 0.55, 0.45, 0, 0.25]],
  ['Ocean Breathe', [0, 0.08, 0.0, 0.55, 0.45, 0, 15, 0.15, 0.55, 9, 0.6,
    1.0, 0.72, 0.36, 0.1, 0.55, 1.0, 0.667, 0.5, 0.0, 0.45, 0.4, 0, 0.25]],
  ['Party Meteor', [0, 0.08, 0.0, 0.6, 0.45, 0, 8, 0.5, 0.7, 3, 0.55,
    1.0, 0.72, 0.36, 0.1, 0.55, 1.0, 0.667, 0.5, 0.0, 0.5, 0.4, 0, 0.25]],
  ['Fairy Frost', [0, 0.08, 0.0, 0.5, 0.4, 0, 19, 0.2, 0.5, 7, 0.5,
    1.0, 0.72, 0.36, 0.1, 0.55, 1.0, 0.667, 0.5, 0.0, 0.45, 0.4, 0, 0.25]],
];

const PRESETS = Object.fromEntries(
  PRESET_TABLE.map(([name, values]) => [
    name,
    Object.fromEntries(values.map((value, i) => [PRESET_PARAM_IDS[i], value])),
  ]),
);

//===========================================================================
// The chain. Six passes, in the order ProcessOpenGL runs them.
//===========================================================================

/// Seconds of host time a single frame is allowed to advance the animation by.
/// The host's clock is not ours: it jumps when the composition is scrubbed and
/// by however long the machine was asleep, and an unclamped delta turns any of
/// those into the pattern skipping forward by minutes.
const MAX_FRAME_DELTA = 0.25;

/// The spectrum, all zeros. Resolume writes one bin per element from whatever
/// audio is routed to the plugin; a browser page has no host FFT, and the
/// plugin's own element defaults are zero for the same reason — nothing routed
/// must not become lamps twitching to a phantom signal.
const AUDIO_BINS = new Float32Array(64);

class TinselRenderer {
  constructor(gl, quad) {
    this.gl = gl;
    this.quad = quad;

    this.copy = new Program(gl, VERTEX_SHADER, COPY_SHADER, 'copy');
    this.edge = new Program(gl, VERTEX_SHADER, EDGE_SHADER, 'edge');
    this.stabilise = new Program(gl, VERTEX_SHADER, STABILISE_SHADER, 'stabilise');
    this.light = new Program(gl, VERTEX_SHADER, LIGHT_SHADER, 'light');
    this.blur = new Program(gl, VERTEX_SHADER, BLUR_SHADER, 'blur');
    this.composite = new Program(gl, VERTEX_SHADER, COMPOSITE_SHADER, 'composite');

    // Sampling matches the plugin's PassBuffer flags: mipmapped where a pass
    // reads a level other than zero, linear everywhere else.
    this.copyBuffer = new PassBuffer(gl, { mip: true });
    this.edgeBuffer = new PassBuffer(gl);
    this.stableBuffer = [new PassBuffer(gl, { mip: true }), new PassBuffer(gl, { mip: true })];
    this.lightBuffer = new PassBuffer(gl, { mip: true });
    this.glowBuffer = [new PassBuffer(gl), new PassBuffer(gl)];

    this.stableCurrent = 0;
    this.historyValid = false;

    // The clock, integrated the way the plugin integrates it.
    this.phase = 0;
    this.lastTime = -1;

    // What the edge passes were last told to look for. Changing either
    // invalidates the history: the numbers being blended stop measuring the
    // same thing, and without this, turning Detail up with Stability high
    // leaves the old scale's outlines decaying under the new ones.
    this.lastDetail = null;
    this.lastSource = null;

    this.paletteTexture = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, this.paletteTexture);
    gl.texImage2D(
      gl.TEXTURE_2D, 0, gl.RGBA32F, PALETTE_SIZE, PALETTES.length, 0,
      gl.RGBA, gl.FLOAT, bakePaletteTable(),
    );
    // texelFetch only. One texture has one filter for both axes, and bilinear
    // here would blend each palette into the one below it.
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    gl.bindTexture(gl.TEXTURE_2D, null);
  }

  /**
   * Free integrates the rate, exactly as the plugin does, and never rescales
   * the history. Beat and Bar take an absolute phase off a transport instead —
   * the point of those modes is that a cycle boundary lands on the host's grid,
   * and an integrated phase drifts off it.
   */
  advanceClock(params, time) {
    const speed = speedFromParam(params.get('speed'));
    const sync = params.option('sync');

    if (sync === 1 || sync === 2) {
      // The host hands the plugin a tempo and a position within the current
      // bar. This page has no transport, so it generates one at 120 BPM — the
      // same tempo the plugin falls back to when the host reports none — and
      // the bar number comes straight off the page's own clock.
      const barSeconds = 240.0 / 120.0;
      const bars = time / barSeconds;
      this.phase = (sync === 1 ? bars * 4.0 : bars) * speed;
    } else if (this.lastTime >= 0) {
      // Restart sets the page's clock back to zero. The plugin would clamp that
      // to a zero delta and carry on from wherever it had got to; here it means
      // the visitor asked for the top, so the phase goes back with it.
      if (time < this.lastTime) {
        this.phase = 0;
      } else {
        this.phase += Math.min(time - this.lastTime, MAX_FRAME_DELTA) * speed;
      }
    }

    this.lastTime = time;
  }

  render({ input, params, width, height, time }) {
    const gl = this.gl;
    const quad = this.quad;

    this.advanceClock(params, time);

    const detail = detailFromParam(params.get('detail'));
    const source = params.option('detectOn');
    if (detail !== this.lastDetail || source !== this.lastSource) {
      this.historyValid = false;
      this.lastDetail = detail;
      this.lastSource = source;
    }

    const glowSize = glowSizeFromParam(params.get('glowSize'));
    const glowWidth = Math.max(16, Math.floor(width / 4));
    const glowHeight = Math.max(16, Math.floor(height / 4));

    // Every ensure() before anything binds a texture, as in the plugin and for
    // the same reason: allocating a buffer disturbs the bindings around it.
    this.copyBuffer.ensure(width, height, gl.RGBA16F);
    this.edgeBuffer.ensure(width, height, gl.RGBA16F);
    this.stableBuffer[0].ensure(width, height, gl.RGBA16F);
    this.stableBuffer[1].ensure(width, height, gl.RGBA16F);
    this.lightBuffer.ensure(width, height, gl.RGBA16F);
    this.glowBuffer[0].ensure(glowWidth, glowHeight, gl.RGBA16F);
    this.glowBuffer[1].ensure(glowWidth, glowHeight, gl.RGBA16F);

    gl.disable(gl.BLEND);

    //---------------------------------------------------------------------
    // 1. The picture, into a texture of ours, with a mip chain on it.
    //---------------------------------------------------------------------
    this.copyBuffer.bind();
    this.copy.use();
    bindTexture(gl, 0, input.texture);
    // setSampler, not set. The plugin writes `Set( "InputTexture", 0 )` and the
    // SDK picks glUniform1i from the C++ type; here a bare 0 is a float, and
    // glUniform1f on a sampler is rejected — leaving every sampler on unit 0,
    // which renders a plausible picture built entirely from the wrong texture.
    this.copy.setSampler('InputTexture', 0);
    // The kit hands over a texture the picture exactly fills, so MaxUV is the
    // whole of it. In Resolume it is whatever GetMaxGLTexCoords reports.
    this.copy.set('MaxUV', 1, 1);
    this.copy.set('HalfTexel', 0.5 / width, 0.5 / height);
    quad.draw();
    this.copyBuffer.generateMipmap();

    //---------------------------------------------------------------------
    // 2. Edge.
    //---------------------------------------------------------------------
    this.edgeBuffer.bind();
    this.edge.use();
    bindTexture(gl, 0, this.copyBuffer.texture);
    this.edge.setSampler('CopyTexture', 0);
    this.edge.set('TexelSize', 1 / width, 1 / height);
    this.edge.set('Detail', detail);
    this.edge.set('SourceMode', source);
    quad.draw();

    //---------------------------------------------------------------------
    // 3. Stabilise, ping-ponged against the previous frame's result.
    //---------------------------------------------------------------------
    const history = this.stableCurrent;
    const target = 1 - this.stableCurrent;

    this.stableBuffer[target].bind();
    this.stabilise.use();
    bindTexture(gl, 0, this.edgeBuffer.texture);
    bindTexture(gl, 1, this.stableBuffer[history].texture);
    this.stabilise.setSampler('EdgeTexture', 0);
    this.stabilise.setSampler('HistoryTexture', 1);
    this.stabilise.set('Attack', attackFromParam(params.get('stability')));
    this.stabilise.set('Release', releaseFromParam(params.get('stability')));
    this.stabilise.set('Sensitivity', sensitivityFromParam(params.get('sensitivity')));
    this.stabilise.set('Softness', softnessFromParam(params.get('softness')));
    this.stabilise.set('Reset', this.historyValid ? 0 : 1);
    quad.draw();

    this.stableCurrent = target;
    this.historyValid = true;
    this.stableBuffer[target].generateMipmap();

    //---------------------------------------------------------------------
    // 4. Light. The plugin.
    //---------------------------------------------------------------------
    this.lightBuffer.bind();
    this.light.use();
    bindTexture(gl, 0, this.stableBuffer[target].texture);
    bindTexture(gl, 1, this.copyBuffer.texture);
    bindTexture(gl, 2, this.paletteTexture);
    this.light.setSampler('StableTexture', 0);
    this.light.setSampler('CopyTexture', 1);
    this.light.setSampler('PaletteTexture', 2);

    this.light.set('Aspect', width / height);
    this.light.set('CentroidLod', mipLevels(width, height) - 1);
    this.light.set('Thickness', thicknessFromParam(params.get('thickness')));

    this.light.set('Layout', params.option('layout'));
    this.light.set('LayoutAngle', layoutAngleFromParam(params.get('direction')));
    this.light.set('Turns', turnsFromParam(params.get('turns')));
    this.light.set('BulbCount', bulbCountFromParam(params.get('lamps')));
    this.light.set('BulbSize', bulbSizeFromParam(params.get('lampSize')));
    this.light.set('Reverse', params.get('reverse'));

    this.light.set('EffectIndex', params.option('pattern'));
    this.light.set('Time', this.phase);
    this.light.set('Intensity', params.get('intensity'));
    this.light.set('Spread', spreadFromParam(params.get('spread')));

    this.light.set('PaletteIndex', params.option('palette'));
    this.light.set('PaletteCount', PALETTES.length);
    this.light.set('Colour1', params.get('c1r'), params.get('c1g'), params.get('c1b'));
    this.light.set('Colour2', params.get('c2r'), params.get('c2g'), params.get('c2b'));
    this.light.set('Saturation', saturationFromParam(params.get('saturation')));
    this.light.set('Brightness', brightnessFromParam(params.get('brightness')));
    this.light.set('SourceTint', params.get('sourceTint'));

    this.light.set('AudioLevel', params.get('audioLevel'));
    this.light.setArray('Audio', AUDIO_BINS);
    quad.draw();

    // The glow's first pass reads this while drawing into a buffer a quarter
    // the size, so it needs a pre-filtered level rather than five point samples
    // of a picture four times finer than its target.
    this.lightBuffer.generateMipmap();

    //---------------------------------------------------------------------
    // 5. Glow. Two separable passes, run twice: summing two Gaussians of
    //    different widths is what gives a bulb a tight core and a wide falloff
    //    instead of one soft blob.
    //---------------------------------------------------------------------
    const downsampleLod = Math.log2(Math.max(1, width / glowWidth));
    const stepX = glowSize * 0.001 * width / glowWidth;
    const stepY = glowSize * 0.001 * height / glowHeight;

    // The wide pair is 1.8× and not 2.5×: past that the five fetches start
    // showing as separate ghosts of the picture rather than a smooth falloff.
    const stages = [
      { from: -1, to: 0, x: stepX, y: 0, lod: downsampleLod },
      { from: 0, to: 1, x: 0, y: stepY, lod: 0 },
      { from: 1, to: 0, x: stepX * 1.8, y: 0, lod: 0 },
      { from: 0, to: 1, x: 0, y: stepY * 1.8, lod: 0 },
    ];

    this.blur.use();
    for (const stage of stages) {
      this.glowBuffer[stage.to].bind();
      bindTexture(gl, 0, stage.from < 0 ? this.lightBuffer.texture : this.glowBuffer[stage.from].texture);
      this.blur.setSampler('SourceTexture', 0);
      this.blur.set('Direction', stage.x, stage.y);
      this.blur.set('SourceLod', stage.lod);
      quad.draw();
    }

    //---------------------------------------------------------------------
    // 6. Composite, straight to the canvas.
    //
    // Back to the output framebuffer and its viewport by hand. The plugin has
    // the same line for a sharper reason: ScopedFBOBinding restores the
    // framebuffer binding and *only* that, so without it the composite
    // inherits the quarter-size glow buffer's viewport and the effect renders
    // into the bottom-left quarter of the frame.
    //---------------------------------------------------------------------
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    gl.viewport(0, 0, width, height);

    this.composite.use();
    bindTexture(gl, 0, this.copyBuffer.texture);
    bindTexture(gl, 1, this.lightBuffer.texture);
    bindTexture(gl, 2, this.glowBuffer[1].texture);
    bindTexture(gl, 3, this.stableBuffer[target].texture);
    this.composite.setSampler('CopyTexture', 0);
    this.composite.setSampler('LightTexture', 1);
    this.composite.setSampler('GlowTexture', 2);
    this.composite.setSampler('StableTexture', 3);

    this.composite.set('Background', params.option('background'));
    this.composite.set('Dim', params.get('dim'));
    this.composite.set('Glow', glowFromParam(params.get('glow')));
    this.composite.set('MixAmount', params.get('mix'));
    quad.draw();
  }
}

//===========================================================================
// The page.
//===========================================================================

mountDemo({
  name: 'Tinsel',
  pluginId: 'TN01',
  tagline: 'Finds the outlines in the clip and hangs a string of lamps along them.',
  repo: 'https://github.com/stoatworks-labs/tinsel',
  page: 'https://stoatworks-labs.com/software/tinsel/',
  video: 'https://www.youtube.com/watch?v=-TGCxAFDMYw',
  needFloat: true,
  showBackdrop: true,
  presets: PRESETS,

  // Clips with something to hang lamps on. The geometry card first, because a
  // strip is only legible where the outline is: a photographic frame lights
  // every texture in it, which is the plugin working correctly and the worst
  // possible first impression of it.
  sources: ['grid', 'scene', 'alpha', 'spot', 'bars', 'detail', 'ramp'],

  differences: [
    'There is no host FFT in a browser page, so the spectrum the Audio group reads is all zeros — exactly what Resolume sends with no audio routed. Audio Level therefore only dims: the gate has nothing to open it.',
    'Beat and Bar lock to a 120 BPM transport generated in this page, which is the tempo the plugin falls back to when a host reports none. Resolume would supply its own, and the bar position with it.',
    'The plugin integrates the host clock and clamps a single frame to a quarter of a second so a scrub cannot skip the pattern forward by minutes. That is ported, with one departure: Restart here puts the phase back to zero rather than freezing, because on this page a backwards clock is the visitor asking for the top.',
    'The stabilise pass feeds back through the previous frame, so Stability is the one control on this page that depends on frame rate — a browser tab that throttles when it loses focus changes what a "few frames of persistence" means.',
    'Preset is an option parameter in the plugin, with Custom as element 0 and a slider edit dropping back to it. Here the same eight presets are in the panel header instead, from the plugin\'s own table.',
  ],

  params: [
    //---- Edge -------------------------------------------------------------
    {
      id: 'detectOn', name: 'Detect On', type: 'option', default: 3, group: 'Edge',
      elements: SOURCE_NAMES,
      hint: 'What "different" means between two pixels. Artwork delivered with alpha has a perfect edge in the alpha channel already; a luma Sobel over it throws away the only clean signal in the frame.',
    },
    {
      id: 'sensitivity', name: 'Sensitivity', type: 'standard', default: 0.6, group: 'Edge',
      display: (v) => sensitivityFromParam(v).toFixed(3),
      hint: 'The gradient magnitude at which a lamp is fully lit. A clean black-to-white step measures exactly 1.0, so the whole useful range for photographic footage sits below 0.15.',
    },
    {
      id: 'softness', name: 'Softness', type: 'standard', default: 0.35, group: 'Edge',
      display: (v) => pct(softnessFromParam(v)),
      hint: 'The width of the shoulder either side of the threshold, as a fraction of it.',
    },
    {
      id: 'detail', name: 'Detail', type: 'standard', default: 0.15, group: 'Edge',
      display: (v) => `mip ${detailFromParam(v).toFixed(2)}`,
      hint: 'The mip level the Sobel runs at: 0 finds every pixel of sensor noise, 3 finds the shape of a logo and ignores its texture.',
    },
    {
      id: 'thickness', name: 'Thickness', type: 'standard', default: 0.25, group: 'Edge',
      display: (v) => `mip ${thicknessFromParam(v).toFixed(2)}`,
      hint: 'A dilation done with the mip chain that is already there — the mask read blurred and its peak restored, which widens the base without a second buffer.',
    },
    {
      id: 'stability', name: 'Stability', type: 'standard', default: 0.35, group: 'Edge',
      display: (v) => `↑${attackFromParam(v).toFixed(2)} ↓${releaseFromParam(v).toFixed(2)}`,
      hint: 'One control, two rates, asymmetric on purpose: an edge that appears is believed at once, an edge that vanishes is given a few frames to come back. What goes wrong on footage is dropout, not movement.',
    },

    //---- Strip ------------------------------------------------------------
    {
      id: 'layout', name: 'Layout', type: 'option', default: 0, group: 'Strip',
      elements: LAYOUT_NAMES,
      hint: 'Spiral is the default because it is what a string of lights actually does on a tree: it goes round, and it climbs. Random keeps every lamp where it is and shuffles its place in the running order.',
    },
    {
      id: 'turns', name: 'Turns', type: 'standard', default: 0.08, group: 'Strip',
      display: (v) => turnsFromParam(v).toFixed(2),
      hint: 'Half a turn by default, not two and a half: a tight spiral meets a round outline at a shallow angle nearly everywhere, and a lamp lying along the outline rather than across it is drawn out into a streak.',
    },
    {
      id: 'direction', name: 'Direction', type: 'standard', default: 0.0, group: 'Strip',
      display: (v) => `${(layoutAngleFromParam(v) * 360).toFixed(0)}°`,
      hint: 'The Linear layout only.',
    },
    {
      id: 'lamps', name: 'Lamps', type: 'standard', default: 0.55, group: 'Strip',
      display: (v) => `${bulbCountFromParam(v)}`,
      hint: 'Geometric, because the difference between 20 lamps and 40 is a different look and the difference between 800 and 820 is not.',
    },
    {
      id: 'lampSize', name: 'Lamp Size', type: 'standard', default: 0.45, group: 'Strip',
      display: (v) => `${bulbSizeFromParam(v).toFixed(1)} px`,
      hint: 'A radius in pixels, not in strip units — which is the whole reason lamps read as lamps. Past half the spacing they merge and the strip becomes a continuous rope: the neon-sign look, from this control rather than a second mode.',
    },
    { id: 'reverse', name: 'Reverse', type: 'boolean', default: 0, group: 'Strip' },

    //---- Pattern ----------------------------------------------------------
    {
      id: 'pattern', name: 'Pattern', type: 'option', default: 11, group: 'Pattern',
      elements: EFFECT_NAMES,
      hint: 'A pattern never chooses a colour. It says where in the palette a lamp sits and how bright it is, which is how an LED controller is built.',
    },
    {
      id: 'speed', name: 'Speed', type: 'standard', default: 0.25, group: 'Pattern',
      display: (v) => `${speedFromParam(v).toFixed(2)} /s`,
      hint: 'Not bipolar: direction is Reverse\'s job, because for a comet the two are not the same thing — negating time walks it backwards with its tail in front.',
    },
    {
      id: 'intensity', name: 'Intensity', type: 'standard', default: 0.5, group: 'Pattern',
      display: pct,
      hint: 'The pattern\'s second knob, and what it means is the pattern\'s business: tail length for Comet, how many lamps are lit for Twinkle, duty cycle for Strobe.',
    },

    //---- Colour -----------------------------------------------------------
    {
      id: 'palette', name: 'Palette', type: 'option', default: 6, group: 'Colour',
      elements: PALETTE_NAMES,
      hint: 'The first two entries are the Colour parameters below; the rest are baked stop lists. Fire is the only one allowed to reach black, because embers are what it is for.',
    },
    {
      id: 'spread', name: 'Spread', type: 'standard', default: 0.4, group: 'Colour',
      display: (v) => `${spreadFromParam(v).toFixed(2)}×`,
      hint: 'How many times the palette is laid across the strip. Applied to colour only, never to the geometry, so turning it up recolours a chase without moving it.',
    },
    // FF_TYPE_RED/GREEN/BLUE on three consecutive parameters, which is what a
    // host needs to show one swatch instead of three sliders — the reason the
    // plugin names them the way it does rather than tidying them up.
    { id: 'c1r', name: 'Colour 1', type: 'colour', default: 1.0, group: 'Colour' },
    { id: 'c1g', name: 'Colour1_Green', type: 'colour', default: 0.72, group: 'Colour' },
    { id: 'c1b', name: 'Colour1_Blue', type: 'colour', default: 0.36, group: 'Colour' },
    { id: 'c2r', name: 'Colour 2', type: 'colour', default: 0.1, group: 'Colour' },
    { id: 'c2g', name: 'Colour2_Green', type: 'colour', default: 0.55, group: 'Colour' },
    { id: 'c2b', name: 'Colour2_Blue', type: 'colour', default: 1.0, group: 'Colour' },
    {
      id: 'saturation', name: 'Saturation', type: 'standard', default: 0.667, group: 'Colour',
      display: (v) => saturationFromParam(v).toFixed(2),
      hint: 'Towards the lamp\'s own luminance, so zero gives white bulbs rather than grey ones. Over 1 pushes past the palette\'s own saturation.',
    },
    {
      id: 'brightness', name: 'Brightness', type: 'standard', default: 0.5, group: 'Colour',
      display: (v) => brightnessFromParam(v).toFixed(2),
      hint: 'Goes to 2 on purpose: the glow is what a bulb looks like, and a bulb that is not clipping does not look like a bulb.',
    },
    {
      id: 'sourceTint', name: 'Source Tint', type: 'standard', default: 0.0, group: 'Colour',
      display: pct,
      hint: 'Multiplies the lamp by the artwork underneath, so a logo can light its own outline in its own colours.',
    },

    //---- Output -----------------------------------------------------------
    {
      id: 'glow', name: 'Glow', type: 'standard', default: 0.4, group: 'Output',
      display: (v) => glowFromParam(v).toFixed(2),
      hint: 'Added, not blended: a bulb\'s halo is light arriving on top of whatever is already there.',
    },
    {
      id: 'glowSize', name: 'Glow Size', type: 'standard', default: 0.35, group: 'Output',
      display: (v) => `${glowSizeFromParam(v).toFixed(2)}‰`,
      hint: 'As a fraction of the picture width, in per-mille.',
    },
    {
      id: 'background', name: 'Background', type: 'option', default: 0, group: 'Output',
      elements: BACKGROUND_NAMES,
      hint: 'Edges is not a look — it is how Sensitivity, Detail and Thickness are actually set, because judging a threshold through a layer of lamps and glow is guesswork.',
    },
    { id: 'dim', name: 'Dim', type: 'standard', default: 0.25, group: 'Output', display: pct },
    { id: 'mix', name: 'Mix', type: 'standard', default: 1.0, group: 'Output', display: pct },

    //---- Tempo ------------------------------------------------------------
    {
      id: 'sync', name: 'Sync', type: 'option', default: 0, group: 'Tempo',
      elements: SYNC_NAMES,
      hint: 'What Speed means: cycles per second, or cycles per beat or bar locked to the transport. See the note at the foot — this page generates its own at 120 BPM.',
    },

    //---- Audio ------------------------------------------------------------
    {
      id: 'audioLevel', name: 'Audio Level', type: 'standard', default: 0.0, group: 'Audio',
      display: pct,
      hint: 'A per-lamp gate from the host\'s FFT, with the low frequencies at the start of the strip. There is no host FFT here, so the spectrum is zeros and this only dims — see the note at the foot.',
    },
  ],

  createRenderer: (gl, quad) => new TinselRenderer(gl, quad),
});
