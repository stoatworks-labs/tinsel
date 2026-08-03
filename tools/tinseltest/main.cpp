/**
    tinseltest -- render Tinsel offline, and check what its lamps are doing.

    What colour a lamp is at a given moment is a fact. Not "looks about right":
    the effect library is a pure function of (lamp, time) and it exists twice,
    once in `Effects.cpp` for this harness to call and once in GLSL for the GPU
    to run. `--effects` runs both and compares them.

    The trick that makes that possible is in `Shaders.h`: the GLSL effects are a
    *fragment* rather than a shader, and both the light pass and this harness's
    probe are assembled around the same string. So what is being checked is the
    text the plugin actually runs. A test that compiled its own copy of the
    effects would agree with itself perfectly and prove nothing.

        tinseltest --out /tmp/frame.png     a picture, on a test card
        tinseltest --list                   every parameter and its default
        tinseltest --effects                GLSL against C++, every effect
        tinseltest --palettes /tmp/p.png    the palette table, as a picture
        tinseltest --card /tmp/card.png     the test card on its own
        tinseltest --pipe                   raw frames in, raw frames out

    `--script` is a plain text file of `frame  Parameter Name  value` lines.
    Values are held before the first key and after the last, and linearly
    interpolated between. The format is identical to old-cathode's octest,
    porthole's phtest and resolume-scopes' sctest on purpose, so one build.py
    can film any of them.

    Note what interpolation means for an **option** parameter -- Pattern,
    Palette, Layout, Detect On, Background. Moving one produces the intermediate
    values on the way, so a change from Chase to Comet passes through Theater
    Chase and Running Lights. Key them one frame apart to cut, and give every
    such parameter a hold key at the END of each section it must not move in: a
    single key at the start of each section slides it continuously across the
    whole reel, and the result looks deliberate.

    `--pipe` takes the fleet's frame format so one script can film any of the
    FFGL plugins:

        ffmpeg -i in.mov -f rawvideo -pix_fmt rgba - \
          | tinseltest --pipe --width 1920 --height 1080 [--script cues.txt] \
          | ffmpeg -f rawvideo -pix_fmt rgba -s 1920x1080 -i - out.mov
*/

#include "Controls.h"
#include "Effects.h"
#include "Palette.h"
#include "Shaders.h"
#include "Tinsel.h"

#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <zlib.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <chrono>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

using namespace tinsel;

namespace
{
//---------------------------------------------------------------------------
/// How far the GLSL and the C++ are allowed to disagree.
///
/// Not zero, and it cannot be. The GLSL specification allows 3 units in the
/// last place for `exp` and gives `sin` no accuracy requirement at all outside
/// a limited range, so a comet's exponential falloff and a twinkle's sine will
/// differ from libm's in the last few bits on any driver. What this tolerance
/// is set to catch is a *drifted constant* -- a tail length of 0.45 against
/// 0.40, a rate of 6 against 8 -- which misses by percent, not by 1e-6.
///
/// Measured rather than guessed: across all 527,100 comparisons the largest
/// honest disagreement on this machine (M4 Max, macOS 26.4) is 3.45e-5, in
/// Chase at t=41 -- a smoothstep whose argument has lost a few bits to the size
/// of the time value. So this sits an order of magnitude above the observed
/// noise and three orders below any drift worth calling a bug.
///
/// It is deliberately *not* loose enough to swallow a wrong branch. The one
/// real defect this has caught so far -- Theater Chase computing its lamp index
/// with a float mod -- showed up as a difference of exactly 1.0.
constexpr float kEffectTolerance = 5e-4f;

//---------------------------------------------------------------------------
// A PNG writer. zlib ships with the OS, so this is a few chunk headers and a
// CRC rather than a dependency.
//---------------------------------------------------------------------------
void putU32( std::vector< unsigned char >& out, uint32_t value )
{
	out.push_back( static_cast< unsigned char >( value >> 24 ) );
	out.push_back( static_cast< unsigned char >( value >> 16 ) );
	out.push_back( static_cast< unsigned char >( value >> 8 ) );
	out.push_back( static_cast< unsigned char >( value ) );
}

void putChunk( std::vector< unsigned char >& out, const char* type, const std::vector< unsigned char >& data )
{
	putU32( out, static_cast< uint32_t >( data.size() ) );
	const size_t start = out.size();
	out.insert( out.end(), type, type + 4 );
	out.insert( out.end(), data.begin(), data.end() );
	uLong crc = crc32( 0L, Z_NULL, 0 );
	crc       = crc32( crc, out.data() + start, static_cast< uInt >( 4 + data.size() ) );
	putU32( out, static_cast< uint32_t >( crc ) );
}

bool writePng( const std::string& path, int width, int height, const std::vector< unsigned char >& rgba )
{
	std::vector< unsigned char > raw;
	raw.reserve( static_cast< size_t >( height ) * ( 1 + static_cast< size_t >( width ) * 4 ) );
	for( int y = 0; y < height; ++y )
	{
		raw.push_back( 0 );//filter: none
		const unsigned char* row = rgba.data() + static_cast< size_t >( y ) * width * 4;
		raw.insert( raw.end(), row, row + static_cast< size_t >( width ) * 4 );
	}

	uLongf compressedSize = compressBound( static_cast< uLong >( raw.size() ) );
	std::vector< unsigned char > compressed( compressedSize );
	if( compress2( compressed.data(), &compressedSize, raw.data(), static_cast< uLong >( raw.size() ), 6 ) != Z_OK )
		return false;
	compressed.resize( compressedSize );

	std::vector< unsigned char > png = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };

	std::vector< unsigned char > ihdr;
	putU32( ihdr, static_cast< uint32_t >( width ) );
	putU32( ihdr, static_cast< uint32_t >( height ) );
	ihdr.push_back( 8 );//bit depth
	ihdr.push_back( 6 );//truecolour with alpha
	ihdr.push_back( 0 );
	ihdr.push_back( 0 );
	ihdr.push_back( 0 );
	putChunk( png, "IHDR", ihdr );
	putChunk( png, "IDAT", compressed );
	putChunk( png, "IEND", {} );

	FILE* file = fopen( path.c_str(), "wb" );
	if( file == nullptr )
		return false;
	const size_t written = fwrite( png.data(), 1, png.size(), file );
	fclose( file );
	return written == png.size();
}

//---------------------------------------------------------------------------
// The test card.
//
// Not meant to look nice. It is built to exercise the edge detector's three
// source modes and nothing else, so each shape tests one thing:
//
//   - a filled disc:      a curved boundary at every angle, which is what
//                         shows whether the strip coordinate is continuous
//   - a hollow ring:      two concentric boundaries close together, which is
//                         where Thickness and Detail start merging edges
//   - a hard-edged bar:   straight boundaries on both axes, for aliasing
//   - two equal-luma
//     colour fields:      invisible to a luma Sobel and obvious to a chroma
//                         one, which is the whole reason Detect On exists
//   - a soft gradient:    no boundary anywhere, so anything the detector
//                         finds here is noise it has invented
//---------------------------------------------------------------------------
std::vector< unsigned char > buildCard( int width, int height )
{
	std::vector< unsigned char > image( static_cast< size_t >( width ) * height * 4, 0 );

	const float w = static_cast< float >( width );
	const float h = static_cast< float >( height );

	for( int y = 0; y < height; ++y )
	{
		for( int x = 0; x < width; ++x )
		{
			const float u = ( static_cast< float >( x ) + 0.5f ) / w;
			const float v = ( static_cast< float >( y ) + 0.5f ) / h;

			float r = 0.0f, g = 0.0f, b = 0.0f, a = 1.0f;

			//A soft horizontal gradient everywhere, as the noise floor.
			r = g = b = 0.06f + 0.10f * u;

			//Disc, left third.
			const float dx1 = ( u - 0.20f ) * ( w / h );
			const float dy1 = v - 0.5f;
			if( std::sqrt( dx1 * dx1 + dy1 * dy1 ) < 0.16f )
			{
				r = 0.95f;
				g = 0.95f;
				b = 0.95f;
			}

			//Ring, middle.
			const float dx2 = ( u - 0.50f ) * ( w / h );
			const float dy2 = v - 0.5f;
			const float d2  = std::sqrt( dx2 * dx2 + dy2 * dy2 );
			if( d2 < 0.18f && d2 > 0.12f )
			{
				r = 0.90f;
				g = 0.90f;
				b = 0.90f;
			}

			//Bar, right third.
			if( u > 0.72f && u < 0.88f && v > 0.20f && v < 0.80f )
			{
				r = 1.0f;
				g = 1.0f;
				b = 1.0f;
			}

			//Two fields of equal luminance, bottom left. A luma Sobel finds
			//nothing at all along the seam between them; a chroma one finds it
			//immediately. Computed against the same coefficients the shader
			//uses, so "equal" here means equal to the detector too.
			if( v < 0.16f && u < 0.40f )
			{
				//0.2126 R and 0.7152 G, arranged to the same luma.
				const bool right = u > 0.20f;
				r                = right ? 0.10f : 0.9333f;
				g                = right ? 0.2775f : 0.0f;
				b                = 0.0f;
			}

			image[ ( static_cast< size_t >( y ) * width + x ) * 4 + 0 ] =
				static_cast< unsigned char >( std::min( 255.0f, r * 255.0f ) );
			image[ ( static_cast< size_t >( y ) * width + x ) * 4 + 1 ] =
				static_cast< unsigned char >( std::min( 255.0f, g * 255.0f ) );
			image[ ( static_cast< size_t >( y ) * width + x ) * 4 + 2 ] =
				static_cast< unsigned char >( std::min( 255.0f, b * 255.0f ) );
			image[ ( static_cast< size_t >( y ) * width + x ) * 4 + 3 ] =
				static_cast< unsigned char >( a * 255.0f );
		}
	}

	return image;
}

/**
    The card with per-frame noise on it, which is the only way to test Stability.

    Stability filters the edge signal over time, so on a still picture it
    provably does nothing: the history and the current frame hold the same
    number, and every blend between them returns that number whatever the
    weight. `tools/sweep.py` reported it dead for exactly that reason, and it
    was right to.

    What it is *for* is footage, where a boundary grades through the threshold
    for a frame or two and the lamp on it blinks. This reproduces that: a
    per-pixel perturbation, reseeded every frame, which pushes marginal pixels
    back and forth across the threshold without moving any actual edge.

    Deterministic, and seeded from the frame number rather than from a running
    generator, so frame 40 of one run is frame 40 of the next and two settings
    can be compared at all.
*/
std::vector< unsigned char > addNoise( const std::vector< unsigned char >& card, int frame, float amount )
{
	std::vector< unsigned char > noisy = card;
	if( amount <= 0.0f )
		return noisy;

	const float scale = amount * 255.0f;
	for( size_t i = 0; i < noisy.size(); i += 4 )
	{
		//The same PCG the effects use, over pixel index and frame.
		const uint32_t seed = HashInt( static_cast< uint32_t >( i / 4 ) * 2654435761u
		                               ^ static_cast< uint32_t >( frame ) );
		const float jitter  = ( Hash01( seed ) - 0.5f ) * scale;

		for( int c = 0; c < 3; ++c )
		{
			const float value  = static_cast< float >( noisy[ i + c ] ) + jitter;
			noisy[ i + c ]     = static_cast< unsigned char >( std::min( 255.0f, std::max( 0.0f, value ) ) );
		}
	}
	return noisy;
}

//---------------------------------------------------------------------------
// GL plumbing.
//---------------------------------------------------------------------------
CGLContextObj createContext()
{
	//Accelerated first; fall back so the harness still runs somewhere without a
	//GPU, where it will at least prove the shaders compile.
	const CGLPixelFormatAttribute accelerated[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAAccelerated,
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};
	const CGLPixelFormatAttribute software[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};

	CGLPixelFormatObj format = nullptr;
	GLint formatCount        = 0;
	if( CGLChoosePixelFormat( accelerated, &format, &formatCount ) != kCGLNoError || format == nullptr )
	{
		if( CGLChoosePixelFormat( software, &format, &formatCount ) != kCGLNoError || format == nullptr )
			return nullptr;
	}

	CGLContextObj context = nullptr;
	const CGLError error  = CGLCreateContext( format, nullptr, &context );
	CGLDestroyPixelFormat( format );
	if( error != kCGLNoError )
		return nullptr;

	CGLSetCurrentContext( context );
	return context;
}

GLuint makeTexture( int width, int height, const unsigned char* pixels )
{
	GLuint texture = 0;
	glGenTextures( 1, &texture );
	glBindTexture( GL_TEXTURE_2D, texture );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, 0 );
	return texture;
}

GLuint makeFramebuffer( GLuint texture )
{
	GLuint fbo = 0;
	glGenFramebuffers( 1, &fbo );
	glBindFramebuffer( GL_FRAMEBUFFER, fbo );
	glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0 );
	return fbo;
}

std::vector< unsigned char > flipRows( const std::vector< unsigned char >& image, int width, int height )
{
	std::vector< unsigned char > flipped( image.size() );
	const size_t stride = static_cast< size_t >( width ) * 4;
	for( int y = 0; y < height; ++y )
		std::memcpy( flipped.data() + static_cast< size_t >( y ) * stride,
		             image.data() + static_cast< size_t >( height - 1 - y ) * stride, stride );
	return flipped;
}

std::vector< unsigned char > readBackRaw( GLuint fbo, int width, int height )
{
	std::vector< unsigned char > pixels( static_cast< size_t >( width ) * height * 4 );
	glBindFramebuffer( GL_FRAMEBUFFER, fbo );
	glPixelStorei( GL_PACK_ALIGNMENT, 1 );
	glReadPixels( 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data() );
	return pixels;
}

//---------------------------------------------------------------------------
// Shader compilation, for the probe. The plugin uses ffglex for this; the
// probe cannot, because ffglex::FFGLShader insists on a vertex shader with the
// SDK's attribute layout and its own uniform helpers, and the probe needs
// neither.
//---------------------------------------------------------------------------
GLuint compileStage( GLenum type, const std::string& source, std::string& error )
{
	const GLuint shader   = glCreateShader( type );
	const char* const ptr = source.c_str();
	glShaderSource( shader, 1, &ptr, nullptr );
	glCompileShader( shader );

	GLint compiled = GL_FALSE;
	glGetShaderiv( shader, GL_COMPILE_STATUS, &compiled );
	if( compiled == GL_TRUE )
		return shader;

	GLint length = 0;
	glGetShaderiv( shader, GL_INFO_LOG_LENGTH, &length );
	std::string log( static_cast< size_t >( std::max( length, 1 ) ), '\0' );
	glGetShaderInfoLog( shader, length, nullptr, log.data() );
	error = log;
	glDeleteShader( shader );
	return 0;
}

GLuint buildProbeProgram( std::string& error )
{
	static const char* const vertexSource = R"(#version 410 core
layout( location = 0 ) in vec4 vPosition;
void main() { gl_Position = vPosition; }
)";

	const GLuint vertex = compileStage( GL_VERTEX_SHADER, vertexSource, error );
	if( vertex == 0 )
		return 0;

	const GLuint fragment = compileStage( GL_FRAGMENT_SHADER, EffectProbeShaderSource(), error );
	if( fragment == 0 )
	{
		glDeleteShader( vertex );
		return 0;
	}

	const GLuint program = glCreateProgram();
	glAttachShader( program, vertex );
	glAttachShader( program, fragment );
	glLinkProgram( program );
	glDeleteShader( vertex );
	glDeleteShader( fragment );

	GLint linked = GL_FALSE;
	glGetProgramiv( program, GL_LINK_STATUS, &linked );
	if( linked == GL_TRUE )
		return program;

	GLint length = 0;
	glGetProgramiv( program, GL_INFO_LOG_LENGTH, &length );
	std::string log( static_cast< size_t >( std::max( length, 1 ) ), '\0' );
	glGetProgramInfoLog( program, length, nullptr, log.data() );
	error = log;
	glDeleteProgram( program );
	return 0;
}

//---------------------------------------------------------------------------
// Parameters by display name, so the automation reads as English.
//---------------------------------------------------------------------------
struct NamedParameter
{
	std::string name;
	unsigned int index;
	float value;
};

std::vector< NamedParameter > listParameters( Tinsel& plugin )
{
	std::vector< NamedParameter > list;
	for( unsigned int i = 0; i < Tinsel::PT_COUNT; ++i )
	{
		const char* const name = plugin.GetParamName( i );
		list.push_back( NamedParameter { name ? name : "?", i, plugin.GetFloatParameter( i ) } );
	}
	return list;
}

bool applySetting( Tinsel& plugin, const std::string& assignment, std::string& error )
{
	const size_t equals = assignment.find( '=' );
	if( equals == std::string::npos )
	{
		error = "expected Name=Value";
		return false;
	}

	const std::string name  = assignment.substr( 0, equals );
	const std::string value = assignment.substr( equals + 1 );

	for( const NamedParameter& parameter : listParameters( plugin ) )
	{
		if( parameter.name != name )
			continue;
		plugin.SetFloatParameter( parameter.index, std::strtof( value.c_str(), nullptr ) );
		return true;
	}

	error = "no parameter called '" + name + "'";
	return false;
}

//---------------------------------------------------------------------------
// --effects
//---------------------------------------------------------------------------
int runEffectCheck()
{
	std::string error;
	const GLuint program = buildProbeProgram( error );
	if( program == 0 )
	{
		std::fprintf( stderr, "the probe shader would not build:\n%s\n", error.c_str() );
		return 1;
	}

	//The lamp count is deliberately not round. A power of two would let a
	//quantisation bug in `s` land on exact binary fractions on both sides and
	//cancel out; 251 is prime and every lamp coordinate is a repeating
	//fraction.
	constexpr int kBulbs = 251;

	GLuint texture = 0;
	glGenTextures( 1, &texture );
	glBindTexture( GL_TEXTURE_2D, texture );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA32F, kBulbs, 1, 0, GL_RGBA, GL_FLOAT, nullptr );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	glBindTexture( GL_TEXTURE_2D, 0 );

	const GLuint fbo = makeFramebuffer( texture );
	if( glCheckFramebufferStatus( GL_FRAMEBUFFER ) != GL_FRAMEBUFFER_COMPLETE )
	{
		std::fprintf( stderr, "could not make a float target for the probe\n" );
		return 1;
	}

	//A full-screen triangle, which needs no index buffer and no UVs.
	GLuint vao = 0, vbo = 0;
	glGenVertexArrays( 1, &vao );
	glBindVertexArray( vao );
	const float triangle[] = { -1.0f, -1.0f, 3.0f, -1.0f, -1.0f, 3.0f };
	glGenBuffers( 1, &vbo );
	glBindBuffer( GL_ARRAY_BUFFER, vbo );
	glBufferData( GL_ARRAY_BUFFER, sizeof( triangle ), triangle, GL_STATIC_DRAW );
	glEnableVertexAttribArray( 0 );
	glVertexAttribPointer( 0, 2, GL_FLOAT, GL_FALSE, 0, nullptr );

	//Times, intensities and spreads chosen to land away from every effect's
	//own boundaries as well as on them: 0 is the cold start, 0.5 and 1.0 sit
	//exactly on a wipe's turnover and a strobe's edge, and the rest are
	//arbitrary. An effect that only agrees at nice numbers is not agreeing.
	const float times[]       = { 0.0f, 0.137f, 0.5f, 1.0f, 2.718f, 7.391f, 41.0f };
	const float intensities[] = { 0.0f, 0.23f, 0.5f, 0.77f, 1.0f };
	const float spreads[]     = { 0.25f, 1.0f, 3.7f };

	int checks       = 0;
	int disagreements = 0;
	float worst      = 0.0f;
	std::string worstWhere;

	std::vector< float > readback( static_cast< size_t >( kBulbs ) * 4 );

	glUseProgram( program );
	glBindFramebuffer( GL_FRAMEBUFFER, fbo );
	glViewport( 0, 0, kBulbs, 1 );

	for( int e = 0; e < static_cast< int >( Effect::Count ); ++e )
	{
		for( float t : times )
		{
			for( float intensity : intensities )
			{
				for( float spread : spreads )
				{
					glUniform1f( glGetUniformLocation( program, "EffectIndex" ), static_cast< float >( e ) );
					glUniform1f( glGetUniformLocation( program, "BulbCount" ), static_cast< float >( kBulbs ) );
					glUniform1f( glGetUniformLocation( program, "TimeValue" ), t );
					glUniform1f( glGetUniformLocation( program, "Intensity" ), intensity );
					glUniform1f( glGetUniformLocation( program, "Spread" ), spread );

					glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
					glClear( GL_COLOR_BUFFER_BIT );
					glDrawArrays( GL_TRIANGLES, 0, 3 );

					glReadPixels( 0, 0, kBulbs, 1, GL_RGBA, GL_FLOAT, readback.data() );

					for( int bulb = 0; bulb < kBulbs; ++bulb )
					{
						const float s = ( static_cast< float >( bulb ) + 0.5f ) / static_cast< float >( kBulbs );
						const Bulb expected = Evaluate( static_cast< Effect >( e ), s,
						                                static_cast< uint32_t >( bulb ), t, intensity, spread );

						const float gotPosition   = readback[ static_cast< size_t >( bulb ) * 4 + 0 ];
						const float gotBrightness = readback[ static_cast< size_t >( bulb ) * 4 + 1 ];

						const float dp = std::fabs( gotPosition - expected.position );
						const float db = std::fabs( gotBrightness - expected.brightness );
						const float d  = std::max( dp, db );

						++checks;
						if( d > worst )
						{
							worst = d;
							char where[ 256 ];
							std::snprintf( where, sizeof( where ),
							               "%s lamp %d t=%g intensity=%g spread=%g",
							               EffectName( static_cast< Effect >( e ) ), bulb, t, intensity, spread );
							worstWhere = where;
						}

						if( d <= kEffectTolerance )
							continue;

						++disagreements;
						if( disagreements <= 12 )
						{
							std::printf( "  %-16s lamp %3d t=%-8g i=%-5g sp=%-5g  "
							             "position %.6f vs %.6f   brightness %.6f vs %.6f\n",
							             EffectName( static_cast< Effect >( e ) ), bulb, t, intensity, spread,
							             gotPosition, expected.position, gotBrightness, expected.brightness );
						}
					}
				}
			}
		}
	}

	glDeleteBuffers( 1, &vbo );
	glDeleteVertexArrays( 1, &vao );
	glDeleteFramebuffers( 1, &fbo );
	glDeleteTextures( 1, &texture );
	glDeleteProgram( program );

	std::printf( "\n%d effects, %d comparisons, %d disagreements past %g\n",
	             static_cast< int >( Effect::Count ), checks, disagreements, kEffectTolerance );
	std::printf( "largest difference %.3g, at %s\n", worst, worstWhere.c_str() );

	if( disagreements > 12 )
		std::printf( "(%d more not shown)\n", disagreements - 12 );

	return disagreements == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --bench
//---------------------------------------------------------------------------

/**
    Time one resolution.

    Two things make this a measurement rather than a number.

    **`glFinish()` on both sides.** GL calls queue; without forcing completion
    this times how fast the driver accepts commands, which on a deep pipeline is
    roughly how fast a `for` loop runs and has nothing to do with the GPU. The
    difference is not subtle -- an unsynchronised version of this reported
    tenths of a millisecond at 4K.

    **A warm-up that is thrown away.** The first frames pay for buffer
    allocation, shader specialisation and the mip chains, and the stabilise
    pass's history has to converge. Those costs are real but they are not the
    per-frame cost, and averaging them in makes a fast effect look slow in
    exactly the case -- a short run -- where somebody is most likely to measure.
*/
double benchAt( Tinsel& plugin, int width, int height, int frames, double fps )
{
	const std::vector< unsigned char > card = buildCard( width, height );
	const GLuint sourceTexture = makeTexture( width, height, card.data() );
	const GLuint outputTexture = makeTexture( width, height, nullptr );
	const GLuint outputFBO     = makeFramebuffer( outputTexture );

	FFGLTextureStruct inputStruct = {};
	inputStruct.Width = inputStruct.HardwareWidth = static_cast< FFUInt32 >( width );
	inputStruct.Height = inputStruct.HardwareHeight = static_cast< FFUInt32 >( height );
	inputStruct.Handle                              = sourceTexture;
	FFGLTextureStruct* inputs[ 1 ]                  = { &inputStruct };

	ProcessOpenGLStruct process = {};
	process.numInputTextures    = 1;
	process.inputTextures       = inputs;
	process.HostFBO             = outputFBO;

	auto renderOne = [ & ]( int frame ) {
		plugin.SetTime( static_cast< double >( frame ) / fps );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glViewport( 0, 0, width, height );
		plugin.ProcessOpenGL( &process );
	};

	const int warmup = 20;
	for( int frame = 0; frame < warmup; ++frame )
		renderOne( frame );
	glFinish();

	const auto start = std::chrono::steady_clock::now();
	for( int frame = 0; frame < frames; ++frame )
		renderOne( warmup + frame );
	glFinish();
	const auto end = std::chrono::steady_clock::now();

	glDeleteFramebuffers( 1, &outputFBO );
	glDeleteTextures( 1, &outputTexture );
	glDeleteTextures( 1, &sourceTexture );

	const double seconds = std::chrono::duration< double >( end - start ).count();
	return seconds * 1000.0 / static_cast< double >( frames );
}

int runBench( Tinsel& plugin, int frames, double fps )
{
	struct Size
	{
		const char* name;
		int width, height;
	};
	const Size sizes[] = {
		{ "1280x720  ", 1280, 720 },
		{ "1920x1080 ", 1920, 1080 },
		{ "2560x1440 ", 2560, 1440 },
		{ "3840x2160 ", 3840, 2160 },
	};

	std::printf( "%d frames each, after a 20-frame warm-up, glFinish both sides.\n\n", frames );
	std::printf( "resolution     ms/frame   equivalent fps   %% of a 60fps frame\n" );

	for( const Size& size : sizes )
	{
		const double ms = benchAt( plugin, size.width, size.height, frames, fps );
		std::printf( "%s    %7.3f       %8.0f            %5.1f%%\n",
		             size.name, ms, ms > 0.0 ? 1000.0 / ms : 0.0, ms / 16.667 * 100.0 );
	}

	std::printf( "\nTen passes: six at picture size (copy, edge, stabilise, light,\n"
	             "composite) and four at quarter size (the glow). The stabilise pass\n"
	             "feeds back into itself, so the cost is per frame whether or not\n"
	             "anything in the picture moved.\n" );
	return 0;
}

//---------------------------------------------------------------------------
// --palettes
//---------------------------------------------------------------------------
int writePalettes( const std::string& path )
{
	const std::vector< float > table = BakePaletteTable();

	//Twelve rows per palette so the strip is thick enough to judge by eye, and
	//the two colour-driven palettes are left black on purpose: they are not in
	//the table, and a picture that invented something for them would be
	//claiming the table holds more than it does.
	constexpr int kRowHeight = 12;
	const int rows           = static_cast< int >( Palette::Count );
	const int height         = rows * kRowHeight;

	std::vector< unsigned char > image( static_cast< size_t >( kPaletteSize ) * height * 4, 0 );
	for( int row = 0; row < rows; ++row )
	{
		for( int band = 0; band < kRowHeight; ++band )
		{
			const int y = row * kRowHeight + band;
			for( int x = 0; x < kPaletteSize; ++x )
			{
				const size_t from = ( static_cast< size_t >( row ) * kPaletteSize + x ) * 4;
				const size_t to   = ( static_cast< size_t >( y ) * kPaletteSize + x ) * 4;
				for( int c = 0; c < 3; ++c )
					image[ to + c ] = static_cast< unsigned char >(
						std::min( 255.0f, std::max( 0.0f, table[ from + c ] ) * 255.0f ) );
				image[ to + 3 ] = 255;
			}
		}
	}

	if( !writePng( path, kPaletteSize, height, image ) )
	{
		std::fprintf( stderr, "could not write %s\n", path.c_str() );
		return 1;
	}

	std::printf( "wrote %s -- %d palettes, top to bottom:\n", path.c_str(), rows );
	for( int row = 0; row < rows; ++row )
		std::printf( "  %2d  %s\n", row, PaletteName( static_cast< Palette >( row ) ) );

	return 0;
}

//---------------------------------------------------------------------------
void usage()
{
	std::printf(
		"tinseltest -- render and check the Tinsel LED-outline effect\n"
		"\n"
		"  --out PATH        render the test card through the plugin (default /tmp/tinsel.png)\n"
		"  --card PATH       write the test card alone, undecorated\n"
		"  --width N         width (default 1280)\n"
		"  --height N        height (default 720)\n"
		"  --frames N        frames to render before reading back (default 30)\n"
		"  --fps N           synthetic frame rate driving the animation (default 60)\n"
		"  --noise F         per-frame noise on the source, 0..1. What Stability is for.\n"
		"  --set \"Name=V\"    set a parameter by its display name, 0..1. Repeatable.\n"
		"  --list            print every parameter and its default, then exit\n"
		"  --effects         run the GLSL effect library against the C++ one\n"
		"  --bench           time ProcessOpenGL at 720p through 4K\n"
		"  --palettes PATH   write the palette table as a picture\n"
		"  --pipe            raw RGBA frames on stdin, raw RGBA frames on stdout\n"
		"  --script PATH     parameter cues for --pipe: 'frame Name=Value'\n"
		"  --help\n" );
}

//---------------------------------------------------------------------------
// --pipe cue sheet: one 'frame Name=Value' per line, applied when the frame
// number is reached. Same format as porthole, old-cathode and asciify, so one
// filming script drives any of them.
//---------------------------------------------------------------------------
using Track = std::vector< std::pair< int, float > >;

std::map< std::string, Track > loadScript( const std::string& path, std::string& error )
{
	std::map< std::string, Track > tracks;
	std::ifstream file( path );
	if( !file )
	{
		error = "cannot open " + path;
		return tracks;
	}

	std::string line;
	int lineNumber = 0;
	while( std::getline( file, line ) )
	{
		++lineNumber;
		const size_t hash = line.find( '#' );
		if( hash != std::string::npos )
			line.erase( hash );
		std::istringstream in( line );

		int frame = 0;
		if( !( in >> frame ) )
			continue;//blank or comment

		//The name is everything up to the last token, because parameters have
		//spaces in them ("Lamp Size") and the value never does.
		std::vector< std::string > words;
		std::string word;
		while( in >> word )
			words.push_back( word );
		if( words.size() < 2 )
		{
			error = path + ":" + std::to_string( lineNumber ) + ": expected `frame Parameter Name value`";
			return {};
		}

		const float value = std::strtof( words.back().c_str(), nullptr );
		words.pop_back();
		std::string name = words.front();
		for( size_t i = 1; i < words.size(); ++i )
			name += " " + words[ i ];

		tracks[ name ].emplace_back( frame, value );
	}

	for( auto& entry : tracks )
		std::sort( entry.second.begin(), entry.second.end() );
	return tracks;
}

float valueAt( const Track& track, int frame )
{
	if( track.empty() )
		return 0.0f;
	if( frame <= track.front().first )
		return track.front().second;
	if( frame >= track.back().first )
		return track.back().second;

	for( size_t i = 1; i < track.size(); ++i )
	{
		if( frame <= track[ i ].first )
		{
			const auto& a    = track[ i - 1 ];
			const auto& b    = track[ i ];
			const float span = static_cast< float >( b.first - a.first );
			const float t    = span > 0.0f ? ( static_cast< float >( frame - a.first ) / span ) : 1.0f;
			return a.second + ( b.second - a.second ) * t;
		}
	}
	return track.back().second;
}
} // namespace

//---------------------------------------------------------------------------
int main( int argc, char** argv )
{
	std::string outPath     = "/tmp/tinsel.png";
	std::string cardPath;
	std::string palettePath;
	std::string scriptPath;
	int width               = 1280;
	int height              = 720;
	int frames              = 30;
	double fps              = 60.0;
	float noise             = 0.0f;
	bool wantList           = false;
	bool wantEffects        = false;
	bool wantBench          = false;
	bool wantPipe           = false;
	std::vector< std::string > settings;

	for( int i = 1; i < argc; ++i )
	{
		const std::string argument = argv[ i ];
		const bool hasNext         = i + 1 < argc;

		if( argument == "--help" )
		{
			usage();
			return 0;
		}
		else if( argument == "--out" && hasNext )
			outPath = argv[ ++i ];
		else if( argument == "--card" && hasNext )
			cardPath = argv[ ++i ];
		else if( argument == "--palettes" && hasNext )
			palettePath = argv[ ++i ];
		else if( argument == "--script" && hasNext )
			scriptPath = argv[ ++i ];
		else if( argument == "--width" && hasNext )
			width = std::atoi( argv[ ++i ] );
		else if( argument == "--height" && hasNext )
			height = std::atoi( argv[ ++i ] );
		else if( argument == "--frames" && hasNext )
			frames = std::atoi( argv[ ++i ] );
		else if( argument == "--fps" && hasNext )
			fps = std::strtod( argv[ ++i ], nullptr );
		else if( argument == "--noise" && hasNext )
			noise = std::strtof( argv[ ++i ], nullptr );
		else if( argument == "--set" && hasNext )
			settings.push_back( argv[ ++i ] );
		else if( argument == "--list" )
			wantList = true;
		else if( argument == "--effects" )
			wantEffects = true;
		else if( argument == "--bench" )
			wantBench = true;
		else if( argument == "--pipe" )
			wantPipe = true;
		else
		{
			std::fprintf( stderr, "unknown argument: %s\n", argument.c_str() );
			usage();
			return 2;
		}
	}

	if( width <= 0 || height <= 0 || frames <= 0 || fps <= 0.0 )
	{
		std::fprintf( stderr, "width, height, frames and fps must all be positive\n" );
		return 2;
	}

	//The palette table is pure C++ and needs no context, so it is answered
	//before one is made -- which also means it still works on a machine where
	//creating a GL context fails.
	if( !palettePath.empty() )
		return writePalettes( palettePath );

	if( !cardPath.empty() )
	{
		const std::vector< unsigned char > card = buildCard( width, height );
		if( !writePng( cardPath, width, height, card ) )
		{
			std::fprintf( stderr, "could not write %s\n", cardPath.c_str() );
			return 1;
		}
		std::printf( "wrote %s\n", cardPath.c_str() );
		return 0;
	}

	CGLContextObj context = createContext();
	if( context == nullptr )
	{
		std::fprintf( stderr, "could not create an OpenGL context\n" );
		return 1;
	}

	if( wantEffects )
	{
		const int result = runEffectCheck();
		CGLSetCurrentContext( nullptr );
		CGLDestroyContext( context );
		return result;
	}

	Tinsel plugin;

	for( const std::string& setting : settings )
	{
		std::string error;
		if( applySetting( plugin, setting, error ) )
			continue;
		std::fprintf( stderr, "--set %s: %s\n", setting.c_str(), error.c_str() );
		return 2;
	}

	if( wantList )
	{
		std::printf( "%-3s %-16s %s\n", "id", "name", "default" );
		for( const NamedParameter& parameter : listParameters( plugin ) )
			std::printf( "%-3u %-16s %.4f\n", parameter.index, parameter.name.c_str(), parameter.value );
		CGLSetCurrentContext( nullptr );
		CGLDestroyContext( context );
		return 0;
	}

	if( wantBench )
	{
		FFGLViewportStruct benchViewport = {};
		benchViewport.width  = static_cast< FFUInt32 >( width );
		benchViewport.height = static_cast< FFUInt32 >( height );
		if( plugin.InitGL( &benchViewport ) != FF_SUCCESS )
		{
			std::fprintf( stderr, "InitGL failed -- see the diagnostics log for which shader\n" );
			return 1;
		}
		const int result = runBench( plugin, frames, fps );
		plugin.DeInitGL();
		CGLSetCurrentContext( nullptr );
		CGLDestroyContext( context );
		return result;
	}

	FFGLViewportStruct viewport = {};
	viewport.x                  = 0;
	viewport.y                  = 0;
	viewport.width              = static_cast< FFUInt32 >( width );
	viewport.height             = static_cast< FFUInt32 >( height );

	if( plugin.InitGL( &viewport ) != FF_SUCCESS )
	{
		std::fprintf( stderr, "InitGL failed -- see the diagnostics log for which shader\n" );
		return 1;
	}

	const std::vector< unsigned char > card = buildCard( width, height );
	GLuint sourceTexture                    = makeTexture( width, height, card.data() );
	GLuint outputTexture                    = makeTexture( width, height, nullptr );
	const GLuint outputFBO                  = makeFramebuffer( outputTexture );

	FFGLTextureStruct inputStruct = {};
	inputStruct.Width = inputStruct.HardwareWidth = static_cast< FFUInt32 >( width );
	inputStruct.Height = inputStruct.HardwareHeight = static_cast< FFUInt32 >( height );
	inputStruct.Handle                              = sourceTexture;
	FFGLTextureStruct* inputs[ 1 ]                  = { &inputStruct };

	ProcessOpenGLStruct process = {};
	process.numInputTextures    = 1;
	process.inputTextures       = inputs;
	process.HostFBO             = outputFBO;

	if( wantPipe )
	{
		//Raw RGBA in, raw RGBA out, one frame at a time.
		//Resolve the script's parameter names to indices once, up front, and
		//refuse to run on a name that is not a parameter. A misspelled name that
		//silently did nothing would produce a take that looks deliberate and is
		//wrong -- the reel would simply hold whatever the default was, with a
		//caption over it describing the control that never moved.
		std::map< unsigned int, Track > automation;
		if( !scriptPath.empty() )
		{
			std::string error;
			const std::map< std::string, Track > tracks = loadScript( scriptPath, error );
			if( !error.empty() )
			{
				std::fprintf( stderr, "%s\n", error.c_str() );
				return 2;
			}

			const std::vector< NamedParameter > known = listParameters( plugin );
			for( const auto& entry : tracks )
			{
				bool found = false;
				for( const NamedParameter& parameter : known )
				{
					if( parameter.name != entry.first )
						continue;
					automation[ parameter.index ] = entry.second;
					found                         = true;
					break;
				}
				if( !found )
				{
					std::fprintf( stderr,
					              "script names '%s', which is not a parameter (try --list)\n",
					              entry.first.c_str() );
					return 2;
				}
			}
		}

		std::vector< unsigned char > frame( static_cast< size_t >( width ) * height * 4 );

		for( int index = 0;; ++index )
		{
			size_t filled = 0;
			while( filled < frame.size() )
			{
				const ssize_t got = read( STDIN_FILENO, frame.data() + filled, frame.size() - filled );
				if( got <= 0 )
					break;
				filled += static_cast< size_t >( got );
			}
			if( filled < frame.size() )
				break;

			for( const auto& track : automation )
				plugin.SetFloatParameter( track.first, valueAt( track.second, index ) );

			//Same synthetic clock as the still path, so a filmed sequence
			//advances at the frame rate it will be played back at rather than
			//at whatever rate the pipe happens to deliver -- a stall in ffmpeg
			//must not show up as the effect speeding up afterwards.
			plugin.SetTime( static_cast< double >( index ) / fps );

			//The card is flipped on the way in because a raw frame arrives top
			//row first and GL wants bottom row first.
			const std::vector< unsigned char > flipped = flipRows( frame, width, height );
			glBindTexture( GL_TEXTURE_2D, sourceTexture );
			glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, flipped.data() );
			glBindTexture( GL_TEXTURE_2D, 0 );

			glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
			glViewport( 0, 0, width, height );
			glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
			glClear( GL_COLOR_BUFFER_BIT );
			if( plugin.ProcessOpenGL( &process ) != FF_SUCCESS )
				break;

			const std::vector< unsigned char > out = flipRows( readBackRaw( outputFBO, width, height ), width, height );
			size_t written                         = 0;
			while( written < out.size() )
			{
				const ssize_t put = write( STDOUT_FILENO, out.data() + written, out.size() - written );
				if( put <= 0 )
					break;
				written += static_cast< size_t >( put );
			}
		}

		plugin.DeInitGL();
		CGLSetCurrentContext( nullptr );
		CGLDestroyContext( context );
		return 0;
	}

	//A still. Several frames, not one: the temporal filter needs a few to
	//settle, and a picture taken on frame zero is a picture of the history
	//buffer being empty rather than of the effect.
	for( int frame = 0; frame < frames; ++frame )
	{
		//A synthetic clock, and it has to be synthetic.
		//
		//Left to the wall clock the harness renders a hundred frames in a few
		//milliseconds, so no time passes, every animation stays on frame zero
		//and Speed measurably does nothing -- which is exactly what
		//tools/sweep.py reported. Worse, what little time *did* pass was
		//whatever the machine happened to take, so no two runs produced the
		//same picture and nothing here could be compared against anything.
		plugin.SetTime( static_cast< double >( frame ) / fps );

		if( noise > 0.0f )
		{
			const std::vector< unsigned char > noisy = addNoise( card, frame, noise );
			glBindTexture( GL_TEXTURE_2D, sourceTexture );
			glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, noisy.data() );
			glBindTexture( GL_TEXTURE_2D, 0 );
		}

		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glViewport( 0, 0, width, height );
		glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
		glClear( GL_COLOR_BUFFER_BIT );
		if( plugin.ProcessOpenGL( &process ) != FF_SUCCESS )
		{
			std::fprintf( stderr, "ProcessOpenGL failed on frame %d\n", frame );
			return 1;
		}
	}

	const std::vector< unsigned char > image = flipRows( readBackRaw( outputFBO, width, height ), width, height );
	if( !writePng( outPath, width, height, image ) )
	{
		std::fprintf( stderr, "could not write %s\n", outPath.c_str() );
		return 1;
	}

	std::printf( "wrote %s (%dx%d, %d frames)\n", outPath.c_str(), width, height, frames );

	plugin.DeInitGL();
	glDeleteFramebuffers( 1, &outputFBO );
	glDeleteTextures( 1, &outputTexture );
	glDeleteTextures( 1, &sourceTexture );
	CGLSetCurrentContext( nullptr );
	CGLDestroyContext( context );
	return 0;
}
