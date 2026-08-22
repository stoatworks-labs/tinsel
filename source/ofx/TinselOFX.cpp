/// The OpenFX build of Tinsel, for DaVinci Resolve, Vegas, Nuke, Natron and
/// other OFX hosts.
///
/// The effect library, the palettes and the parameter conversions all live
/// once — Effects.cpp, Palette.cpp, Controls.cpp — and this file links them.
/// What is mirrored here is the six GPU passes of Shaders.cpp, collapsed onto
/// the CPU. When editing a pass in Shaders.cpp, edit the matching phase here.
///
/// Two deliberate departures from the FFGL build, both forced by OFX's time
/// model (frames render in any order, alone, and concurrently):
///
/// - **Phase is time * speed**, not an integrated rate. FFGL integrates so a
///   live Speed nudge does not rescale history; an OFX host keyframes Speed
///   instead, and a deterministic frame matters more than a smooth nudge.
/// - **The temporal filter is reconstructed, not carried.** The stabilise
///   pass's asymmetric IIR reads its own previous output in FFGL. Here the
///   same filter is re-run over a short window of previous frames fetched
///   through OFX temporal clip access — the release rate bounds how far back
///   the history matters, so the window is exact for practical purposes and
///   capped at kMaxHistory frames for pathological settings.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

#include "ofxsImageEffect.h"
#include "ofxsProcessing.h"

// After the OFX Support headers, which is where the OFX types come from.
#include "StoatworksAboutOFX.h"

#include "../Controls.h"
#include "../Effects.h"
#include "../Palette.h"
#include "../Presets.h"

namespace
{
constexpr const char* kPluginIdentifier = "com.stoatworks.tinsel";
constexpr const char* kPluginName       = "Tinsel";
constexpr const char* kPluginGrouping   = "Stoatworks";
constexpr const char* kPluginDescription =
	"Finds the outlines in the picture and lights them like a string of "
	"LEDs: chases, comets, twinkles, fire, a scanner, a wipe. An effect "
	"never chooses a colour — it says where in the palette a lamp sits and "
	"how bright it is, which is how an LED controller is built and why "
	"twenty patterns and sixteen palettes are worth more than thirty-six of "
	"either.\n\n"
	"https://stoatworks-labs.com";

/// The most previous frames the stabilise reconstruction will fetch. Twelve
/// frames at the default release rate leaves a residual under 2%; the very
/// top of the Stability range is truncated harder, which shows as an edge
/// that has been gone a while dropping out slightly earlier than in FFGL.
constexpr int kMaxHistory = 12;

constexpr const char* kParamSource      = "detectOn";
constexpr const char* kParamSensitivity = "sensitivity";
constexpr const char* kParamSoftness    = "softness";
constexpr const char* kParamDetail      = "detail";
constexpr const char* kParamThickness   = "thickness";
constexpr const char* kParamStability   = "stability";
constexpr const char* kParamLayout      = "layout";
constexpr const char* kParamTurns       = "turns";
constexpr const char* kParamLayoutAngle = "direction";
constexpr const char* kParamDensity     = "lamps";
constexpr const char* kParamBulbSize    = "lampSize";
constexpr const char* kParamReverse     = "reverse";
constexpr const char* kParamEffect      = "pattern";
constexpr const char* kParamSpeed       = "speed";
constexpr const char* kParamIntensity   = "intensity";
constexpr const char* kParamPalette     = "palette";
constexpr const char* kParamSpread      = "spread";
constexpr const char* kParamColour1     = "colour1";
constexpr const char* kParamColour2     = "colour2";
constexpr const char* kParamSaturation  = "saturation";
constexpr const char* kParamBrightness  = "brightness";
constexpr const char* kParamSourceTint  = "sourceTint";
constexpr const char* kParamGlow        = "glow";
constexpr const char* kParamGlowSize    = "glowSize";
constexpr const char* kParamBackground  = "background";
constexpr const char* kParamDim         = "dim";
constexpr const char* kParamMix         = "mix";
constexpr const char* kParamPreset      = "preset";

const char* const kSourceNames[]     = { "Luma", "Alpha", "Chroma", "Luma or Alpha" };
const char* const kLayoutNames[]     = { "Spiral", "Angle", "Linear", "Radial", "Random" };
const char* const kBackgroundNames[] = { "Black", "Source", "Dimmed Source", "Transparent", "Edges" };

constexpr float kTau = 6.283185307179586f;

//---------------------------------------------------------------------------
// Small image types: single-channel and RGBA float planes with box-filtered
// mip chains and the sampling the GPU passes relied on.
//---------------------------------------------------------------------------
struct Plane
{
	int w = 0, h = 0;
	std::vector<float> v;

	void resize( int width, int height )
	{
		w = width;
		h = height;
		v.assign( size_t( w ) * h, 0.0f );
	}
	float at( int x, int y ) const
	{
		x = std::clamp( x, 0, w - 1 );
		y = std::clamp( y, 0, h - 1 );
		return v[ size_t( y ) * w + x ];
	}
};

struct Plane4
{
	int w = 0, h = 0;
	std::vector<float> v; //!< RGBA interleaved

	void resize( int width, int height )
	{
		w = width;
		h = height;
		v.assign( size_t( w ) * h * 4, 0.0f );
	}
	const float* at( int x, int y ) const
	{
		x = std::clamp( x, 0, w - 1 );
		y = std::clamp( y, 0, h - 1 );
		return &v[ ( size_t( y ) * w + x ) * 4 ];
	}
	float* row( int y ) { return &v[ size_t( y ) * w * 4 ]; }
};

/// One box-halving. Non-power-of-two sizes floor, matching GL's mip sizing.
template<class P>
P halve( const P& src );

template<>
Plane halve( const Plane& src )
{
	Plane out;
	out.resize( std::max( 1, src.w / 2 ), std::max( 1, src.h / 2 ) );
	for( int y = 0; y < out.h; ++y )
		for( int x = 0; x < out.w; ++x )
			out.v[ size_t( y ) * out.w + x ] =
				0.25f * ( src.at( 2 * x, 2 * y ) + src.at( 2 * x + 1, 2 * y )
						  + src.at( 2 * x, 2 * y + 1 ) + src.at( 2 * x + 1, 2 * y + 1 ) );
	return out;
}

template<>
Plane4 halve( const Plane4& src )
{
	Plane4 out;
	out.resize( std::max( 1, src.w / 2 ), std::max( 1, src.h / 2 ) );
	for( int y = 0; y < out.h; ++y )
		for( int x = 0; x < out.w; ++x )
		{
			const float* a = src.at( 2 * x, 2 * y );
			const float* b = src.at( 2 * x + 1, 2 * y );
			const float* c = src.at( 2 * x, 2 * y + 1 );
			const float* d = src.at( 2 * x + 1, 2 * y + 1 );
			float* o       = &out.v[ ( size_t( y ) * out.w + x ) * 4 ];
			for( int k = 0; k < 4; ++k )
				o[ k ] = 0.25f * ( a[ k ] + b[ k ] + c[ k ] + d[ k ] );
		}
	return out;
}

template<class P>
std::vector<P> buildMips( P base )
{
	std::vector<P> mips;
	mips.push_back( std::move( base ) );
	while( mips.back().w > 1 || mips.back().h > 1 )
		mips.push_back( halve( mips.back() ) );
	return mips;
}

/// GL_LINEAR at one level, uv in 0..1 with texel centres at half-texel.
float bilinear( const Plane& p, float u, float v )
{
	const float fx = u * p.w - 0.5f;
	const float fy = v * p.h - 0.5f;
	const int x0   = int( std::floor( fx ) );
	const int y0   = int( std::floor( fy ) );
	const float ax = fx - x0;
	const float ay = fy - y0;

	const float top    = p.at( x0, y0 ) * ( 1 - ax ) + p.at( x0 + 1, y0 ) * ax;
	const float bottom = p.at( x0, y0 + 1 ) * ( 1 - ax ) + p.at( x0 + 1, y0 + 1 ) * ax;
	return top + ( bottom - top ) * ay;
}

void bilinear4( const Plane4& p, float u, float v, float out[ 4 ] )
{
	const float fx = u * p.w - 0.5f;
	const float fy = v * p.h - 0.5f;
	const int x0   = int( std::floor( fx ) );
	const int y0   = int( std::floor( fy ) );
	const float ax = fx - x0;
	const float ay = fy - y0;

	const float* p00 = p.at( x0, y0 );
	const float* p10 = p.at( x0 + 1, y0 );
	const float* p01 = p.at( x0, y0 + 1 );
	const float* p11 = p.at( x0 + 1, y0 + 1 );
	for( int k = 0; k < 4; ++k )
	{
		const float top    = p00[ k ] + ( p10[ k ] - p00[ k ] ) * ax;
		const float bottom = p01[ k ] + ( p11[ k ] - p01[ k ] ) * ax;
		out[ k ]           = top + ( bottom - top ) * ay;
	}
}

/// textureLod: trilinear across the chain.
float sampleLod( const std::vector<Plane>& mips, float u, float v, float lod )
{
	lod             = std::clamp( lod, 0.0f, float( mips.size() - 1 ) );
	const int level = int( lod );
	const float mix = lod - level;
	const float a   = bilinear( mips[ size_t( level ) ], u, v );
	if( mix <= 0.0f || level + 1 >= int( mips.size() ) )
		return a;
	const float b = bilinear( mips[ size_t( level ) + 1 ], u, v );
	return a + ( b - a ) * mix;
}

void sampleLod4( const std::vector<Plane4>& mips, float u, float v, float lod, float out[ 4 ] )
{
	lod             = std::clamp( lod, 0.0f, float( mips.size() - 1 ) );
	const int level = int( lod );
	const float mix = lod - level;
	bilinear4( mips[ size_t( level ) ], u, v, out );
	if( mix <= 0.0f || level + 1 >= int( mips.size() ) )
		return;
	float b[ 4 ];
	bilinear4( mips[ size_t( level ) + 1 ], u, v, b );
	for( int k = 0; k < 4; ++k )
		out[ k ] += ( b[ k ] - out[ k ] ) * mix;
}

float smoothstepf( float lo, float hi, float x )
{
	const float t = std::clamp( ( x - lo ) / ( hi - lo ), 0.0f, 1.0f );
	return t * t * ( 3.0f - 2.0f * t );
}

/// Run `body(y0, y1)` across the hardware threads.
void parallelRows( int height, const std::function<void( int, int )>& body )
{
	const int workers = std::max( 1u, std::thread::hardware_concurrency() );
	const int chunk   = std::max( 1, ( height + workers - 1 ) / workers );
	std::vector<std::thread> pool;
	for( int y0 = 0; y0 < height; y0 += chunk )
	{
		const int y1 = std::min( height, y0 + chunk );
		pool.emplace_back( [ =, &body ] { body( y0, y1 ); } );
	}
	for( std::thread& t : pool )
		t.join();
}

//---------------------------------------------------------------------------
// Everything one render computes before pixels are written.
//---------------------------------------------------------------------------
struct FrameSetup
{
	Plane4 copy;               //!< the picture, premultiplied
	Plane4 light;              //!< the lamps, premultiplied
	Plane mask;                //!< the stabilised, thresholded edge
	std::vector<Plane> maskMips;
	Plane4 glow;               //!< quarter size, blurred

	float glowGain = 0.0f;
	int background = 0;
	float dim      = 0.25f;
	float mixAmount = 1.0f;
};

class CompositeProcessorBase : public OFX::ImageProcessor
{
public:
	explicit CompositeProcessorBase( OFX::ImageEffect& effect ) :
		OFX::ImageProcessor( effect )
	{
	}

	void setSetup( const FrameSetup* v, bool premultipliedValue )
	{
		setup         = v;
		premultiplied = premultipliedValue;
	}

protected:
	const FrameSetup* setup = nullptr;
	bool premultiplied      = false;
};

template<class PIX, int nComponents, int maxValue>
class CompositeProcessor : public CompositeProcessorBase
{
public:
	explicit CompositeProcessor( OFX::ImageEffect& effect ) :
		CompositeProcessorBase( effect )
	{
	}

	void multiThreadProcessImages( OfxRectI window ) override
	{
		const OfxRectI bounds = _dstImg->getBounds();
		const int outW        = bounds.x2 - bounds.x1;
		const int outH        = bounds.y2 - bounds.y1;
		const FrameSetup& s   = *setup;

		for( int y = window.y1; y < window.y2; ++y )
		{
			if( _effect.abort() )
				break;

			PIX* dstPix   = static_cast<PIX*>( _dstImg->getPixelAddress( window.x1, y ) );
			const float v = ( y - bounds.y1 + 0.5f ) / outH;
			const int py  = y - bounds.y1;

			for( int x = window.x1; x < window.x2; ++x, dstPix += nComponents )
			{
				const float u = ( x - bounds.x1 + 0.5f ) / outW;
				const int px  = x - bounds.x1;

				const float* source = s.copy.at( px, py );
				const float* light  = s.light.at( px, py );

				float glowPx[ 4 ];
				bilinear4( s.glow, u, v, glowPx );

				//The glow is added, not blended: a halo is light arriving on
				//top of whatever is already there.
				float lit[ 4 ];
				for( int k = 0; k < 4; ++k )
					lit[ k ] = light[ k ] + glowPx[ k ] * s.glowGain;

				float result[ 4 ];
				if( s.background == 4 )
				{
					const float m = s.mask.at( px, py );
					result[ 0 ] = result[ 1 ] = result[ 2 ] = m;
					result[ 3 ]                             = 1.0f;
				}
				else if( s.background == 3 )
				{
					for( int k = 0; k < 4; ++k )
						result[ k ] = lit[ k ];
				}
				else
				{
					float back[ 4 ] = { source[ 0 ], source[ 1 ], source[ 2 ], source[ 3 ] };
					if( s.background == 0 )
					{
						back[ 0 ] = back[ 1 ] = back[ 2 ] = 0.0f;
						back[ 3 ]                         = 1.0f;
					}
					else if( s.background == 2 )
					{
						back[ 0 ] *= s.dim;
						back[ 1 ] *= s.dim;
						back[ 2 ] *= s.dim;
					}
					result[ 0 ] = back[ 0 ] + lit[ 0 ];
					result[ 1 ] = back[ 1 ] + lit[ 1 ];
					result[ 2 ] = back[ 2 ] + lit[ 2 ];
					result[ 3 ] = std::clamp( back[ 3 ] + lit[ 3 ], 0.0f, 1.0f );
				}

				double r = source[ 0 ] + ( result[ 0 ] - source[ 0 ] ) * s.mixAmount;
				double g = source[ 1 ] + ( result[ 1 ] - source[ 1 ] ) * s.mixAmount;
				double b = source[ 2 ] + ( result[ 2 ] - source[ 2 ] ) * s.mixAmount;
				double a = source[ 3 ] + ( result[ 3 ] - source[ 3 ] ) * s.mixAmount;

				a = std::clamp( a, 0.0, 1.0 );
				r = std::min( std::clamp( r, 0.0, 1.0 ), a );
				g = std::min( std::clamp( g, 0.0, 1.0 ), a );
				b = std::min( std::clamp( b, 0.0, 1.0 ), a );

				if( !premultiplied && nComponents == 4 && a > 0.0 )
				{
					r /= a;
					g /= a;
					b /= a;
				}

				dstPix[ 0 ] = quantise( r );
				dstPix[ 1 ] = quantise( g );
				dstPix[ 2 ] = quantise( b );
				if( nComponents == 4 )
					dstPix[ 3 ] = quantise( a );
			}
		}
	}

private:
	static PIX quantise( double v )
	{
		if( maxValue == 1 )
			return PIX( v );

		v = std::clamp( v, 0.0, 1.0 );
		return PIX( v * maxValue + 0.5 );
	}
};

class TinselPlugin : public OFX::ImageEffect
{
public:
	explicit TinselPlugin( OfxImageEffectHandle handle ) :
		OFX::ImageEffect( handle )
	{
		dstClip = fetchClip( kOfxImageEffectOutputClipName );
		srcClip = fetchClip( kOfxImageEffectSimpleSourceClipName );

		source      = fetchChoiceParam( kParamSource );
		sensitivity = fetchDoubleParam( kParamSensitivity );
		softness    = fetchDoubleParam( kParamSoftness );
		detail      = fetchDoubleParam( kParamDetail );
		thickness   = fetchDoubleParam( kParamThickness );
		stability   = fetchDoubleParam( kParamStability );
		layout      = fetchChoiceParam( kParamLayout );
		turns       = fetchDoubleParam( kParamTurns );
		layoutAngle = fetchDoubleParam( kParamLayoutAngle );
		density     = fetchDoubleParam( kParamDensity );
		bulbSize    = fetchDoubleParam( kParamBulbSize );
		reverse     = fetchBooleanParam( kParamReverse );
		effect      = fetchChoiceParam( kParamEffect );
		speed       = fetchDoubleParam( kParamSpeed );
		intensity   = fetchDoubleParam( kParamIntensity );
		palette     = fetchChoiceParam( kParamPalette );
		spread      = fetchDoubleParam( kParamSpread );
		colour1     = fetchRGBParam( kParamColour1 );
		colour2     = fetchRGBParam( kParamColour2 );
		saturation  = fetchDoubleParam( kParamSaturation );
		brightness  = fetchDoubleParam( kParamBrightness );
		sourceTint  = fetchDoubleParam( kParamSourceTint );
		glow        = fetchDoubleParam( kParamGlow );
		glowSize    = fetchDoubleParam( kParamGlowSize );
		background  = fetchChoiceParam( kParamBackground );
		dim         = fetchDoubleParam( kParamDim );
		mixParam    = fetchDoubleParam( kParamMix );
		preset      = fetchChoiceParam( kParamPreset );
	}

	void changedParam( const OFX::InstanceChangedArgs& args, const std::string& paramName ) override
	{
		// The About links open a browser and change nothing about the render.
		if( stoatworks::about::ofx::changedParam( args, paramName ) )
			return;

		using namespace tinsel::presets;

		if( paramName == kParamPreset )
		{
			int chosen = 0;
			preset->getValue( chosen );
			if( chosen <= 0 || chosen > kCount || applyingPreset )
				return;

			// The copy IS the preset — same table as the FFGL build, same 0..1
			// space. One edit block so undo takes the whole preset back at once.
			const Preset& p = kPresets[ chosen - 1 ];
			applyingPreset  = true;
			beginEditBlock( "Preset" );
			setChoice( layout, p.v[ kLayout ] );
			setDouble( turns, p.v[ kTurns ] );
			setDouble( layoutAngle, p.v[ kLayoutAngle ] );
			setDouble( density, p.v[ kDensity ] );
			setDouble( bulbSize, p.v[ kBulbSize ] );
			setBool( reverse, p.v[ kReverse ] );
			setChoice( effect, p.v[ kEffect ] );
			setDouble( speed, p.v[ kSpeed ] );
			setDouble( intensity, p.v[ kIntensity ] );
			setChoice( palette, p.v[ kPalette ] );
			setDouble( spread, p.v[ kSpread ] );
			setRGB( colour1, p.v[ kC1R ], p.v[ kC1G ], p.v[ kC1B ] );
			setRGB( colour2, p.v[ kC2R ], p.v[ kC2G ], p.v[ kC2B ] );
			setDouble( saturation, p.v[ kSaturation ] );
			setDouble( brightness, p.v[ kBrightness ] );
			setDouble( sourceTint, p.v[ kSourceTint ] );
			setDouble( glow, p.v[ kGlow ] );
			setDouble( glowSize, p.v[ kGlowSize ] );
			setChoice( background, p.v[ kBackground ] );
			setDouble( dim, p.v[ kDim ] );
			endEditBlock();
			applyingPreset = false;
			return;
		}

		// Editing a covered control while a preset is active hands control back
		// to the sliders. Judged by value, not by the change reason: hosts are
		// not consistent about reasons, but "still equal to the preset" is
		// unambiguous and also absorbs the host echoing our own setValues.
		if( applyingPreset || args.reason == OFX::eChangeTime )
			return;

		int active = 0;
		preset->getValue( active );
		if( active <= 0 || active > kCount )
			return;

		const Preset& p    = kPresets[ active - 1 ];
		const bool covered =
			( paramName == kParamLayout && choiceDiffers( layout, p.v[ kLayout ] ) ) ||
			( paramName == kParamTurns && doubleDiffers( turns, p.v[ kTurns ] ) ) ||
			( paramName == kParamLayoutAngle && doubleDiffers( layoutAngle, p.v[ kLayoutAngle ] ) ) ||
			( paramName == kParamDensity && doubleDiffers( density, p.v[ kDensity ] ) ) ||
			( paramName == kParamBulbSize && doubleDiffers( bulbSize, p.v[ kBulbSize ] ) ) ||
			( paramName == kParamReverse && boolDiffers( reverse, p.v[ kReverse ] ) ) ||
			( paramName == kParamEffect && choiceDiffers( effect, p.v[ kEffect ] ) ) ||
			( paramName == kParamSpeed && doubleDiffers( speed, p.v[ kSpeed ] ) ) ||
			( paramName == kParamIntensity && doubleDiffers( intensity, p.v[ kIntensity ] ) ) ||
			( paramName == kParamPalette && choiceDiffers( palette, p.v[ kPalette ] ) ) ||
			( paramName == kParamSpread && doubleDiffers( spread, p.v[ kSpread ] ) ) ||
			( paramName == kParamColour1 && rgbDiffers( colour1, p.v[ kC1R ], p.v[ kC1G ], p.v[ kC1B ] ) ) ||
			( paramName == kParamColour2 && rgbDiffers( colour2, p.v[ kC2R ], p.v[ kC2G ], p.v[ kC2B ] ) ) ||
			( paramName == kParamSaturation && doubleDiffers( saturation, p.v[ kSaturation ] ) ) ||
			( paramName == kParamBrightness && doubleDiffers( brightness, p.v[ kBrightness ] ) ) ||
			( paramName == kParamSourceTint && doubleDiffers( sourceTint, p.v[ kSourceTint ] ) ) ||
			( paramName == kParamGlow && doubleDiffers( glow, p.v[ kGlow ] ) ) ||
			( paramName == kParamGlowSize && doubleDiffers( glowSize, p.v[ kGlowSize ] ) ) ||
			( paramName == kParamBackground && choiceDiffers( background, p.v[ kBackground ] ) ) ||
			( paramName == kParamDim && doubleDiffers( dim, p.v[ kDim ] ) );

		if( covered )
		{
			applyingPreset = true;
			preset->setValue( 0 );
			applyingPreset = false;
		}
	}

	void render( const OFX::RenderArguments& args ) override
	{
		std::unique_ptr<OFX::Image> dst( dstClip->fetchImage( args.time ) );
		std::unique_ptr<OFX::Image> src( srcClip->fetchImage( args.time ) );

		const bool premultiplied = srcClip->getPreMultiplication() == OFX::eImagePreMultiplied;

		const OFX::BitDepthEnum depth       = dst->getPixelDepth();
		const OFX::PixelComponentEnum comps = dst->getPixelComponents();

		if( comps != OFX::ePixelComponentRGBA && comps != OFX::ePixelComponentRGB )
			OFX::throwSuiteStatusException( kOfxStatErrUnsupported );

		FrameSetup setup;
		buildSetup( args, *src, premultiplied, setup );

		switch( depth )
		{
		case OFX::eBitDepthUByte:
			comps == OFX::ePixelComponentRGBA
				? runComposite<CompositeProcessor<unsigned char, 4, 255>>( args, dst.get(), setup, premultiplied )
				: runComposite<CompositeProcessor<unsigned char, 3, 255>>( args, dst.get(), setup, premultiplied );
			break;
		case OFX::eBitDepthUShort:
			comps == OFX::ePixelComponentRGBA
				? runComposite<CompositeProcessor<unsigned short, 4, 65535>>( args, dst.get(), setup, premultiplied )
				: runComposite<CompositeProcessor<unsigned short, 3, 65535>>( args, dst.get(), setup, premultiplied );
			break;
		case OFX::eBitDepthFloat:
			comps == OFX::ePixelComponentRGBA
				? runComposite<CompositeProcessor<float, 4, 1>>( args, dst.get(), setup, premultiplied )
				: runComposite<CompositeProcessor<float, 3, 1>>( args, dst.get(), setup, premultiplied );
			break;
		default:
			OFX::throwSuiteStatusException( kOfxStatErrUnsupported );
		}
	}

	/// The stabilise reconstruction needs a window of previous frames; tell
	/// the host so it can prefetch them.
	void getFramesNeeded( const OFX::FramesNeededArguments& args, OFX::FramesNeededSetter& frames ) override
	{
		const int window = historyWindow( args.time );
		OfxRangeD range;
		range.min = args.time - window;
		range.max = args.time;
		frames.setFramesNeeded( *srcClip, range );
	}

private:
	/// Frames of history the release rate makes visible, capped.
	int historyWindow( double t ) const
	{
		const float release = tinsel::ReleaseFromParam( float( stability->getValueAtTime( t ) ) );
		if( release >= 0.999f )
			return 0;
		//(1 - release)^k < 2% -> k
		const int k = int( std::ceil( std::log( 0.02 ) / std::log( 1.0 - release ) ) );
		return std::clamp( k, 0, kMaxHistory );
	}

	/// Read an OFX image into a premultiplied float plane.
	static void toPlane( OFX::Image& img, bool premultiplied, Plane4& out )
	{
		const OfxRectI b = img.getBounds();
		const int w      = b.x2 - b.x1;
		const int h      = b.y2 - b.y1;
		out.resize( w, h );

		const OFX::BitDepthEnum depth       = img.getPixelDepth();
		const OFX::PixelComponentEnum comps = img.getPixelComponents();
		const int n                         = comps == OFX::ePixelComponentRGBA ? 4 : 3;

		parallelRows( h, [ & ]( int y0, int y1 ) {
			for( int y = y0; y < y1; ++y )
			{
				float* row = out.row( y );
				for( int x = 0; x < w; ++x )
				{
					const void* pix = img.getPixelAddress( b.x1 + x, b.y1 + y );
					float r = 0, g = 0, bl = 0, a = 0;
					if( pix )
					{
						switch( depth )
						{
						case OFX::eBitDepthUByte:
						{
							const unsigned char* p = static_cast<const unsigned char*>( pix );
							r  = p[ 0 ] / 255.0f;
							g  = p[ 1 ] / 255.0f;
							bl = p[ 2 ] / 255.0f;
							a  = n == 4 ? p[ 3 ] / 255.0f : 1.0f;
							break;
						}
						case OFX::eBitDepthUShort:
						{
							const unsigned short* p = static_cast<const unsigned short*>( pix );
							r  = p[ 0 ] / 65535.0f;
							g  = p[ 1 ] / 65535.0f;
							bl = p[ 2 ] / 65535.0f;
							a  = n == 4 ? p[ 3 ] / 65535.0f : 1.0f;
							break;
						}
						default:
						{
							const float* p = static_cast<const float*>( pix );
							r  = p[ 0 ];
							g  = p[ 1 ];
							bl = p[ 2 ];
							a  = n == 4 ? p[ 3 ] : 1.0f;
							break;
						}
						}
						if( !premultiplied && n == 4 )
						{
							r *= a;
							g *= a;
							bl *= a;
						}
					}
					row[ x * 4 + 0 ] = r;
					row[ x * 4 + 1 ] = g;
					row[ x * 4 + 2 ] = bl;
					row[ x * 4 + 3 ] = a;
				}
			}
		} );
	}

	/// Pass 2: the Sobel, over a copy's mip chain. Mirrors kEdgeShader.
	static void edgePass( const std::vector<Plane4>& copyMips, int mode, float detailLod, Plane& out )
	{
		const int w = copyMips[ 0 ].w;
		const int h = copyMips[ 0 ].h;
		out.resize( w, h );

		auto channel = [ & ]( float u, float v ) -> float {
			float c[ 4 ];
			sampleLod4( copyMips, u, v, detailLod, c );

			if( mode == 1 )
				return c[ 3 ];

			if( mode == 2 )
			{
				const float y = 0.2126f * c[ 0 ] + 0.7152f * c[ 1 ] + 0.0722f * c[ 2 ];
				const float dr = c[ 0 ] - y, dg = c[ 1 ] - y, db = c[ 2 ] - y;
				return std::sqrt( dr * dr + dg * dg + db * db ) + y * 0.25f;
			}

			const float sr   = c[ 3 ] > 0.0031f ? c[ 0 ] / c[ 3 ] : c[ 0 ];
			const float sg   = c[ 3 ] > 0.0031f ? c[ 1 ] / c[ 3 ] : c[ 1 ];
			const float sb   = c[ 3 ] > 0.0031f ? c[ 2 ] / c[ 3 ] : c[ 2 ];
			const float luma = 0.2126f * sr + 0.7152f * sg + 0.0722f * sb;

			if( mode == 3 )
				return c[ 3 ] * ( 0.35f + 0.65f * luma );

			return luma;
		};

		const float stepX = std::exp2( detailLod ) / w;
		const float stepY = std::exp2( detailLod ) / h;

		parallelRows( h, [ & ]( int y0, int y1 ) {
			for( int y = y0; y < y1; ++y )
			{
				const float v = ( y + 0.5f ) / h;
				for( int x = 0; x < w; ++x )
				{
					const float u = ( x + 0.5f ) / w;

					const float tl = channel( u - stepX, v + stepY );
					const float tc = channel( u, v + stepY );
					const float tr = channel( u + stepX, v + stepY );
					const float ml = channel( u - stepX, v );
					const float mr = channel( u + stepX, v );
					const float bl = channel( u - stepX, v - stepY );
					const float bc = channel( u, v - stepY );
					const float br = channel( u + stepX, v - stepY );

					const float gx = ( tr + 2 * mr + br ) - ( tl + 2 * ml + bl );
					const float gy = ( tl + 2 * tc + tr ) - ( bl + 2 * bc + br );

					out.v[ size_t( y ) * w + x ] = std::sqrt( gx * gx + gy * gy ) * 0.25f;
				}
			}
		} );
	}

	void buildSetup( const OFX::RenderArguments& args, OFX::Image& src, bool premultiplied, FrameSetup& setup )
	{
		using namespace tinsel;

		const double t = args.time;

		double fps = dstClip->getFrameRate();
		if( !( fps > 0.0 ) )
			fps = 24.0;
		const float seconds = float( t / fps );

		//--- pass 1: copy ---------------------------------------------------
		toPlane( src, premultiplied, setup.copy );
		const int w = setup.copy.w;
		const int h = setup.copy.h;
		std::vector<Plane4> copyMips = buildMips( setup.copy );

		//--- pass 2 + 3: edge, stabilised over the reconstruction window ----
		int sourceMode = 3;
		source->getValueAtTime( t, sourceMode );
		const float detailLod = DetailFromParam( float( detail->getValueAtTime( t ) ) );

		const float attack  = AttackFromParam( float( stability->getValueAtTime( t ) ) );
		const float release = ReleaseFromParam( float( stability->getValueAtTime( t ) ) );
		const int window    = historyWindow( t );

		Plane stable;
		bool haveStable = false;

		for( int k = window; k >= 1; --k )
		{
			std::unique_ptr<OFX::Image> past( srcClip->fetchImage( t - k ) );
			if( !past )
				continue;

			Plane4 pastCopy;
			toPlane( *past, premultiplied, pastCopy );
			if( pastCopy.w != w || pastCopy.h != h )
				continue;

			std::vector<Plane4> pastMips = buildMips( std::move( pastCopy ) );
			Plane pastEdge;
			edgePass( pastMips, sourceMode, detailLod, pastEdge );

			if( !haveStable )
			{
				stable     = std::move( pastEdge );
				haveStable = true;
				continue;
			}

			for( size_t i = 0; i < stable.v.size(); ++i )
			{
				const float current = pastEdge.v[ i ];
				const float blend   = current > stable.v[ i ] ? attack : release;
				stable.v[ i ] += ( current - stable.v[ i ] ) * blend;
			}
		}

		Plane edgeNow;
		edgePass( copyMips, sourceMode, detailLod, edgeNow );

		if( !haveStable )
			stable = std::move( edgeNow );
		else
			for( size_t i = 0; i < stable.v.size(); ++i )
			{
				const float current = edgeNow.v[ i ];
				const float blend   = current > stable.v[ i ] ? attack : release;
				stable.v[ i ] += ( current - stable.v[ i ] ) * blend;
			}

		//Threshold with the soft shoulder, and the centroid moments.
		const float sens = SensitivityFromParam( float( sensitivity->getValueAtTime( t ) ) );
		const float soft = SoftnessFromParam( float( softness->getValueAtTime( t ) ) );
		const float lower = sens * ( 1.0f - soft );
		const float upper = std::max( sens * ( 1.0f + soft ), lower + 1e-5f );

		setup.mask.resize( w, h );
		double momX = 0.0, momY = 0.0, momA = 0.0;
		for( int y = 0; y < h; ++y )
		{
			const float v = ( y + 0.5f ) / h;
			for( int x = 0; x < w; ++x )
			{
				const float m = smoothstepf( lower, upper, stable.v[ size_t( y ) * w + x ] );
				setup.mask.v[ size_t( y ) * w + x ] = m;
				momX += ( x + 0.5 ) / w * m;
				momY += v * m;
				momA += m;
			}
		}
		const float centreX = momA > 1e-4 ? float( momX / momA ) : 0.5f;
		const float centreY = momA > 1e-4 ? float( momY / momA ) : 0.5f;

		setup.maskMips = buildMips( setup.mask );

		//--- pass 4: light ---------------------------------------------------
		int layoutMode = 0, effectIndex = 0, paletteIndex = int( Palette::WarmWhite );
		layout->getValueAtTime( t, layoutMode );
		effect->getValueAtTime( t, effectIndex );
		palette->getValueAtTime( t, paletteIndex );

		const float aspect      = float( w ) / std::max( 1, h );
		const float thicknessLod = ThicknessFromParam( float( thickness->getValueAtTime( t ) ) );
		const float layoutTurn  = LayoutAngleFromParam( float( layoutAngle->getValueAtTime( t ) ) );
		const float turnsValue  = TurnsFromParam( float( turns->getValueAtTime( t ) ) );
		const float bulbCount   = BulbCountFromParam( float( density->getValueAtTime( t ) ) );
		const float bulbRadius  = BulbSizeFromParam( float( bulbSize->getValueAtTime( t ) ) );
		const bool reversed     = reverse->getValueAtTime( t );
		const float phase       = seconds * SpeedFromParam( float( speed->getValueAtTime( t ) ) );
		const float intensityV  = float( intensity->getValueAtTime( t ) );
		const float spreadV     = SpreadFromParam( float( spread->getValueAtTime( t ) ) );
		const float satV        = SaturationFromParam( float( saturation->getValueAtTime( t ) ) );
		const float brightV     = BrightnessFromParam( float( brightness->getValueAtTime( t ) ) );
		const float tintV       = float( sourceTint->getValueAtTime( t ) );

		double c1r, c1g, c1b, c2r, c2g, c2b;
		colour1->getValueAtTime( t, c1r, c1g, c1b );
		colour2->getValueAtTime( t, c2r, c2g, c2b );
		const Rgb col1{ float( c1r ), float( c1g ), float( c1b ) };
		const Rgb col2{ float( c2r ), float( c2g ), float( c2b ) };

		auto stripCoordinate = [ & ]( float px, float py ) -> float {
			const float angle  = std::atan2( py, px ) / kTau + 0.5f;
			const float radius = std::sqrt( px * px + py * py );

			if( layoutMode == 1 )
				return angle;
			if( layoutMode == 2 )
			{
				const float a = layoutTurn * kTau;
				return ( px * std::cos( a ) + py * std::sin( a ) ) * 0.5f + 0.5f;
			}
			if( layoutMode == 3 )
				return radius;
			return angle + radius * turnsValue;
		};

		auto bulbAt = [ & ]( float u, float v ) -> float {
			const float px = ( u - centreX ) * aspect;
			const float py = v - centreY;
			float s        = stripCoordinate( px, py );
			if( reversed )
				s = -s;
			return s * bulbCount;
		};

		setup.light.resize( w, h );

		parallelRows( h, [ & ]( int y0, int y1 ) {
			for( int y = y0; y < y1; ++y )
			{
				const float v = ( y + 0.5f ) / h;
				float* row    = setup.light.row( y );

				for( int x = 0; x < w; ++x )
				{
					const float u = ( x + 0.5f ) / w;

					float maskValue = sampleLod( setup.maskMips, u, v, thicknessLod );
					maskValue       = std::clamp( maskValue * ( 1.0f + thicknessLod * 2.0f ), 0.0f, 1.0f );

					float* o = &row[ size_t( x ) * 4 ];
					if( maskValue <= 0.0f )
					{
						o[ 0 ] = o[ 1 ] = o[ 2 ] = o[ 3 ] = 0.0f;
						continue;
					}

					const float bulbF = bulbAt( u, v );

					//dFdx/dFdy, as one-pixel finite differences with the same
					//branch-cut removal.
					float dx = bulbAt( u + 1.0f / w, v ) - bulbF;
					float dy = bulbAt( u, v + 1.0f / h ) - bulbF;
					dx -= bulbCount * std::round( dx / bulbCount );
					dy -= bulbCount * std::round( dy / bulbCount );
					const float lampsPerPixel = std::max( std::sqrt( dx * dx + dy * dy ), 1e-4f );

					const float cell    = std::floor( bulbF );
					const float wrapped = cell - std::floor( cell / bulbCount ) * bulbCount;
					const uint32_t bulb = uint32_t( int( wrapped ) );
					float sBulb         = ( cell + 0.5f ) / bulbCount;
					sBulb -= std::floor( sBulb );

					if( layoutMode == 4 )
						sBulb = Hash01( bulb );

					const Bulb result = Evaluate( Effect( effectIndex ), sBulb, bulb, phase, intensityV, spreadV );

					const float dCentre = std::abs( bulbF - cell - 0.5f ) / lampsPerPixel;
					const float lamp    = 1.0f - smoothstepf( bulbRadius * 0.55f, bulbRadius, dCentre );
					if( lamp <= 0.0f )
					{
						o[ 0 ] = o[ 1 ] = o[ 2 ] = o[ 3 ] = 0.0f;
						continue;
					}

					float position = result.position - std::floor( result.position );
					Rgb colour     = PaletteLookup( Palette( paletteIndex ), position, col1, col2 );

					const float yLuma = 0.2126f * colour.r + 0.7152f * colour.g + 0.0722f * colour.b;
					colour.r = yLuma + ( colour.r - yLuma ) * satV;
					colour.g = yLuma + ( colour.g - yLuma ) * satV;
					colour.b = yLuma + ( colour.b - yLuma ) * satV;

					if( tintV > 0.0f )
					{
						const float* srcPx = setup.copy.at( x, y );
						const float sr     = srcPx[ 3 ] > 0.0031f ? srcPx[ 0 ] / srcPx[ 3 ] : srcPx[ 0 ];
						const float sg     = srcPx[ 3 ] > 0.0031f ? srcPx[ 1 ] / srcPx[ 3 ] : srcPx[ 1 ];
						const float sb     = srcPx[ 3 ] > 0.0031f ? srcPx[ 2 ] / srcPx[ 3 ] : srcPx[ 2 ];
						colour.r += ( colour.r * sr - colour.r ) * tintV;
						colour.g += ( colour.g * sg - colour.g ) * tintV;
						colour.b += ( colour.b * sb - colour.b ) * tintV;
					}

					const float amount =
						std::clamp( result.brightness, 0.0f, 1.0f ) * lamp * maskValue * brightV;

					o[ 0 ] = colour.r * amount;
					o[ 1 ] = colour.g * amount;
					o[ 2 ] = colour.b * amount;
					o[ 3 ] = amount;
				}
			}
		} );

		//--- pass 5: glow ------------------------------------------------------
		const int gw = std::max( 1, w / 4 );
		const int gh = std::max( 1, h / 4 );

		std::vector<Plane4> lightMips = buildMips( setup.light );
		const float downsampleLod     = std::log2( std::max( 1.0f, float( w ) / gw ) );
		const float glowSizeV         = GlowSizeFromParam( float( glowSize->getValueAtTime( t ) ) );
		const float stepX             = glowSizeV * 0.001f * float( w ) / gw;
		const float stepY             = glowSizeV * 0.001f * float( h ) / gh;

		Plane4 ping, pong;
		ping.resize( gw, gh );
		pong.resize( gw, gh );

		//The 9-tap Gaussian the GPU folds into five linear fetches.
		const float offsets[ 3 ] = { 0.0f, 1.3846153846f, 3.2307692308f };
		const float weights[ 3 ] = { 0.2270270270f, 0.3162162162f, 0.0702702703f };

		auto blur = [ & ]( const std::vector<Plane4>* fromMips, const Plane4* from, Plane4& to,
						   float dirX, float dirY, float lod ) {
			parallelRows( gh, [ & ]( int y0, int y1 ) {
				for( int y = y0; y < y1; ++y )
				{
					const float v = ( y + 0.5f ) / gh;
					float* row    = to.row( y );
					for( int x = 0; x < gw; ++x )
					{
						const float u = ( x + 0.5f ) / gw;
						float sum[ 4 ] = { 0, 0, 0, 0 };
						for( int i = 0; i < 3; ++i )
						{
							for( int sign = ( i == 0 ? 0 : -1 ); sign <= 1; sign += 2 )
							{
								const float su = u + dirX * offsets[ i ] * sign;
								const float sv = v + dirY * offsets[ i ] * sign;
								float c[ 4 ];
								if( fromMips )
									sampleLod4( *fromMips, su, sv, lod, c );
								else
									bilinear4( *from, su, sv, c );
								for( int k = 0; k < 4; ++k )
									sum[ k ] += c[ k ] * weights[ i ];
								if( i == 0 )
									break;
							}
						}
						float* o = &row[ size_t( x ) * 4 ];
						for( int k = 0; k < 4; ++k )
							o[ k ] = sum[ k ];
					}
				}
			} );
		};

		//The same four stages as the GPU: a tight pair, then a 1.8x wide pair.
		//Direction is one tap step in the glow buffer's own uv.
		blur( &lightMips, nullptr, ping, stepX / gw, 0.0f, downsampleLod );
		blur( nullptr, &ping, pong, 0.0f, stepY / gh, 0.0f );
		blur( nullptr, &pong, ping, stepX * 1.8f / gw, 0.0f, 0.0f );
		blur( nullptr, &ping, pong, 0.0f, stepY * 1.8f / gh, 0.0f );
		setup.glow = std::move( pong );

		//--- pass 6 inputs -----------------------------------------------------
		setup.glowGain = GlowFromParam( float( glow->getValueAtTime( t ) ) );
		background->getValueAtTime( t, setup.background );
		setup.dim       = float( dim->getValueAtTime( t ) );
		setup.mixAmount = float( mixParam->getValueAtTime( t ) );
	}

	template<class Processor>
	void runComposite( const OFX::RenderArguments& args, OFX::Image* dst, const FrameSetup& setup,
					   bool premultiplied )
	{
		Processor processor( *this );
		processor.setDstImg( dst );
		processor.setSetup( &setup, premultiplied );
		processor.setRenderWindow( args.renderWindow );
		processor.process();
	}

	OFX::Clip* dstClip = nullptr;
	OFX::Clip* srcClip = nullptr;

	OFX::ChoiceParam* source       = nullptr;
	OFX::DoubleParam* sensitivity  = nullptr;
	OFX::DoubleParam* softness     = nullptr;
	OFX::DoubleParam* detail       = nullptr;
	OFX::DoubleParam* thickness    = nullptr;
	OFX::DoubleParam* stability    = nullptr;
	OFX::ChoiceParam* layout       = nullptr;
	OFX::DoubleParam* turns        = nullptr;
	OFX::DoubleParam* layoutAngle  = nullptr;
	OFX::DoubleParam* density      = nullptr;
	OFX::DoubleParam* bulbSize     = nullptr;
	OFX::BooleanParam* reverse     = nullptr;
	OFX::ChoiceParam* effect       = nullptr;
	OFX::DoubleParam* speed        = nullptr;
	OFX::DoubleParam* intensity    = nullptr;
	OFX::ChoiceParam* palette      = nullptr;
	OFX::DoubleParam* spread       = nullptr;
	OFX::RGBParam* colour1         = nullptr;
	OFX::RGBParam* colour2         = nullptr;
	OFX::DoubleParam* saturation   = nullptr;
	OFX::DoubleParam* brightness   = nullptr;
	OFX::DoubleParam* sourceTint   = nullptr;
	OFX::DoubleParam* glow         = nullptr;
	OFX::DoubleParam* glowSize     = nullptr;
	OFX::ChoiceParam* background   = nullptr;
	OFX::ChoiceParam* preset       = nullptr;

	// The preset table is plain floats; these give each param type its
	// reading of one. Option values are element indices, booleans are 0/1.
	static bool doubleDiffers( OFX::DoubleParam* p, float v )
	{
		double current = 0.0;
		p->getValue( current );
		return std::fabs( current - double( v ) ) > 1e-4;
	}
	static bool boolDiffers( OFX::BooleanParam* p, float v )
	{
		bool current = false;
		p->getValue( current );
		return current != ( v > 0.5f );
	}
	static bool choiceDiffers( OFX::ChoiceParam* p, float v )
	{
		int current = 0;
		p->getValue( current );
		return current != int( std::lround( v ) );
	}
	static bool rgbDiffers( OFX::RGBParam* p, float r, float g, float b )
	{
		double cr = 0.0, cg = 0.0, cb = 0.0;
		p->getValue( cr, cg, cb );
		return std::fabs( cr - double( r ) ) > 1e-4 || std::fabs( cg - double( g ) ) > 1e-4
			   || std::fabs( cb - double( b ) ) > 1e-4;
	}
	static void setDouble( OFX::DoubleParam* p, float v )
	{
		if( doubleDiffers( p, v ) )
			p->setValue( double( v ) );
	}
	static void setBool( OFX::BooleanParam* p, float v )
	{
		if( boolDiffers( p, v ) )
			p->setValue( v > 0.5f );
	}
	static void setChoice( OFX::ChoiceParam* p, float v )
	{
		if( choiceDiffers( p, v ) )
			p->setValue( int( std::lround( v ) ) );
	}
	static void setRGB( OFX::RGBParam* p, float r, float g, float b )
	{
		if( rgbDiffers( p, r, g, b ) )
			p->setValue( double( r ), double( g ), double( b ) );
	}

	/// True while our own setValues are in flight, so the resulting
	/// changedParam callbacks are not mistaken for the operator editing.
	bool applyingPreset = false;
	OFX::DoubleParam* dim          = nullptr;
	OFX::DoubleParam* mixParam     = nullptr;
};

OFX::DoubleParamDescriptor* defineSlider( OFX::ImageEffectDescriptor& desc, OFX::PageParamDescriptor* page,
										  const char* name, const char* label, const char* hint, double def )
{
	OFX::DoubleParamDescriptor* p = desc.defineDoubleParam( name );
	p->setLabels( label, label, label );
	p->setHint( hint );
	p->setRange( 0.0, 1.0 );
	p->setDisplayRange( 0.0, 1.0 );
	p->setDefault( def );
	page->addChild( *p );
	return p;
}

} // namespace

mDeclarePluginFactory( TinselPluginFactory, {}, {} );

void TinselPluginFactory::describe( OFX::ImageEffectDescriptor& desc )
{
	desc.setLabels( kPluginName, kPluginName, kPluginName );
	desc.setPluginGrouping( kPluginGrouping );
	desc.setPluginDescription( kPluginDescription );

	desc.addSupportedContext( OFX::eContextFilter );
	desc.addSupportedContext( OFX::eContextGeneral );

	desc.addSupportedBitDepth( OFX::eBitDepthUByte );
	desc.addSupportedBitDepth( OFX::eBitDepthUShort );
	desc.addSupportedBitDepth( OFX::eBitDepthFloat );

	// The centroid and the strip coordinate are whole-frame quantities, so no
	// tiles; the stabilise pass reads a window of previous source frames, so
	// temporal access is declared. Frames still render in any order.
	desc.setSupportsTiles( false );
	desc.setTemporalClipAccess( true );
	desc.setRenderThreadSafety( OFX::eRenderFullySafe );
	desc.setSupportsMultiResolution( true );
}

void TinselPluginFactory::describeInContext( OFX::ImageEffectDescriptor& desc, OFX::ContextEnum )
{
	using namespace tinsel;

	OFX::ClipDescriptor* srcClip = desc.defineClip( kOfxImageEffectSimpleSourceClipName );
	srcClip->addSupportedComponent( OFX::ePixelComponentRGBA );
	srcClip->addSupportedComponent( OFX::ePixelComponentRGB );
	srcClip->setSupportsTiles( false );
	srcClip->setTemporalClipAccess( true );

	OFX::ClipDescriptor* dstClip = desc.defineClip( kOfxImageEffectOutputClipName );
	dstClip->addSupportedComponent( OFX::ePixelComponentRGBA );
	dstClip->addSupportedComponent( OFX::ePixelComponentRGB );
	dstClip->setSupportsTiles( false );

	OFX::PageParamDescriptor* page = desc.definePageParam( "Controls" );

	// Factory presets, from the same table the FFGL build reads (Presets.h).
	// Custom is not a preset: it means the sliders are the truth.
	OFX::ChoiceParamDescriptor* presetParam = desc.defineChoiceParam( kParamPreset );
	presetParam->setLabels( "Preset", "Preset", "Preset" );
	presetParam->setHint( "Named string-light looks. Picking one sets the covered controls; "
	                      "editing any of them afterwards falls back to Custom. The Edge "
	                      "group is left alone — that tuning belongs to your artwork." );
	presetParam->appendOption( "Custom" );
	for( int i = 0; i < tinsel::presets::kCount; ++i )
		presetParam->appendOption( tinsel::presets::kPresets[ i ].name );
	presetParam->setDefault( 0 );
	presetParam->setIsPersistant( true );
	presetParam->setEvaluateOnChange( false );//the copied values re-render; the label itself does not
	presetParam->setAnimates( false );
	page->addChild( *presetParam );

	OFX::GroupParamDescriptor* edge = desc.defineGroupParam( "Edge" );
	edge->setLabels( "Edge", "Edge", "Edge" );

	OFX::ChoiceParamDescriptor* sourceParam = desc.defineChoiceParam( kParamSource );
	sourceParam->setLabels( "Detect On", "Detect On", "Detect On" );
	sourceParam->setHint( "What \"different\" means between two pixels. Artwork with alpha has a "
						  "perfect edge in the alpha channel already." );
	for( const char* name : kSourceNames )
		sourceParam->appendOption( name );
	sourceParam->setDefault( 3 );
	sourceParam->setParent( *edge );
	page->addChild( *sourceParam );

	defineSlider( desc, page, kParamSensitivity, "Sensitivity",
				  "Gradient magnitude at which a lamp is fully lit; a clean step is 1.0.", 0.60 )
		->setParent( *edge );
	defineSlider( desc, page, kParamSoftness, "Softness", "Width of the threshold's shoulder.", 0.35 )
		->setParent( *edge );
	defineSlider( desc, page, kParamDetail, "Detail",
				  "Scale the edges are found at: 0 finds sensor noise, high finds the shape of a logo.", 0.15 )
		->setParent( *edge );
	defineSlider( desc, page, kParamThickness, "Thickness", "Dilates the outline.", 0.25 )
		->setParent( *edge );
	defineSlider( desc, page, kParamStability, "Stability",
				  "Asymmetric temporal filter: edges appear at once and are given a few frames "
				  "to come back when they vanish. Reconstructed from previous frames here, so "
				  "high values cost render time.",
				  0.35 )
		->setParent( *edge );

	OFX::GroupParamDescriptor* strip = desc.defineGroupParam( "Strip" );
	strip->setLabels( "Strip", "Strip", "Strip" );

	OFX::ChoiceParamDescriptor* layoutParam = desc.defineChoiceParam( kParamLayout );
	layoutParam->setLabels( "Layout", "Layout", "Layout" );
	layoutParam->setHint( "How the string is wound over the artwork. Spiral is what a string on a "
						  "tree actually does: it goes round, and it climbs." );
	for( const char* name : kLayoutNames )
		layoutParam->appendOption( name );
	layoutParam->setDefault( 0 );
	layoutParam->setParent( *strip );
	page->addChild( *layoutParam );

	defineSlider( desc, page, kParamTurns, "Turns", "Turns of spiral across the artwork.", 0.08 )
		->setParent( *strip );
	defineSlider( desc, page, kParamLayoutAngle, "Direction", "The Linear layout's direction.", 0.0 )
		->setParent( *strip );
	defineSlider( desc, page, kParamDensity, "Lamps", "8 to 1000 lamps, geometrically.", 0.55 )
		->setParent( *strip );
	defineSlider( desc, page, kParamBulbSize, "Lamp Size",
				  "A lamp's radius in pixels. Past half the spacing the lamps merge into a rope — "
				  "the neon look.",
				  0.45 )
		->setParent( *strip );

	OFX::BooleanParamDescriptor* reverseParam = desc.defineBooleanParam( kParamReverse );
	reverseParam->setLabels( "Reverse", "Reverse", "Reverse" );
	reverseParam->setHint( "Run the strip the other way." );
	reverseParam->setDefault( false );
	reverseParam->setParent( *strip );
	page->addChild( *reverseParam );

	OFX::GroupParamDescriptor* effectGroup = desc.defineGroupParam( "Effect" );
	effectGroup->setLabels( "Effect", "Effect", "Effect" );

	OFX::ChoiceParamDescriptor* effectParam = desc.defineChoiceParam( kParamEffect );
	effectParam->setLabels( "Pattern", "Pattern", "Pattern" );
	for( int i = 0; i < int( Effect::Count ); ++i )
		effectParam->appendOption( EffectName( Effect( i ) ) );
	effectParam->setDefault( int( Effect::Twinkle ) );
	effectParam->setParent( *effectGroup );
	page->addChild( *effectParam );

	defineSlider( desc, page, kParamSpeed, "Speed", "0 to 4 cycles per second; 0 is frozen.", 0.25 )
		->setParent( *effectGroup );
	defineSlider( desc, page, kParamIntensity, "Intensity",
				  "The pattern's second knob: tail length for Comet, lamps lit for Twinkle, duty "
				  "cycle for Strobe.",
				  0.50 )
		->setParent( *effectGroup );

	OFX::GroupParamDescriptor* colour = desc.defineGroupParam( "Colour" );
	colour->setLabels( "Colour", "Colour", "Colour" );

	OFX::ChoiceParamDescriptor* paletteParam = desc.defineChoiceParam( kParamPalette );
	paletteParam->setLabels( "Palette", "Palette", "Palette" );
	for( int i = 0; i < int( Palette::Count ); ++i )
		paletteParam->appendOption( PaletteName( Palette( i ) ) );
	paletteParam->setDefault( int( Palette::WarmWhite ) );
	paletteParam->setParent( *colour );
	page->addChild( *paletteParam );

	defineSlider( desc, page, kParamSpread, "Spread", "Palette cycles across the strip.", 0.40 )
		->setParent( *colour );

	OFX::RGBParamDescriptor* c1 = desc.defineRGBParam( kParamColour1 );
	c1->setLabels( "Colour 1", "Colour 1", "Colour 1" );
	c1->setDefault( 1.00, 0.72, 0.36 );
	c1->setParent( *colour );
	page->addChild( *c1 );

	OFX::RGBParamDescriptor* c2 = desc.defineRGBParam( kParamColour2 );
	c2->setLabels( "Colour 2", "Colour 2", "Colour 2" );
	c2->setDefault( 0.10, 0.55, 1.00 );
	c2->setParent( *colour );
	page->addChild( *c2 );

	defineSlider( desc, page, kParamSaturation, "Saturation", "0 gives white bulbs, over 1 pushes past the palette.", 0.667 )
		->setParent( *colour );
	defineSlider( desc, page, kParamBrightness, "Brightness", "Over 1 on purpose: a bulb that is not clipping does not look like a bulb.", 0.50 )
		->setParent( *colour );
	defineSlider( desc, page, kParamSourceTint, "Source Tint",
				  "Multiplies by the artwork underneath, so a logo lights its own outline in its own colours.", 0.0 )
		->setParent( *colour );

	OFX::GroupParamDescriptor* output = desc.defineGroupParam( "Output" );
	output->setLabels( "Output", "Output", "Output" );

	defineSlider( desc, page, kParamGlow, "Glow", "", 0.40 )->setParent( *output );
	defineSlider( desc, page, kParamGlowSize, "Glow Size", "", 0.35 )->setParent( *output );

	OFX::ChoiceParamDescriptor* backgroundParam = desc.defineChoiceParam( kParamBackground );
	backgroundParam->setLabels( "Background", "Background", "Background" );
	backgroundParam->setHint( "Edges shows the raw mask — it is how Sensitivity, Detail and "
							  "Thickness are actually set." );
	for( const char* name : kBackgroundNames )
		backgroundParam->appendOption( name );
	backgroundParam->setDefault( 0 );
	backgroundParam->setParent( *output );
	page->addChild( *backgroundParam );

	defineSlider( desc, page, kParamDim, "Dim", "For the Dimmed Source background.", 0.25 )
		->setParent( *output );
	defineSlider( desc, page, kParamMix, "Mix", "Dry/wet with the untouched clip.", 1.0 )
		->setParent( *output );

	// The Stoatworks About block: a read-only credit line and one push button
	// per link, in a group that starts folded. Last, so it sits under the
	// effect's own controls.
	stoatworks::about::ofx::describe( desc, page );
}

OFX::ImageEffect* TinselPluginFactory::createInstance( OfxImageEffectHandle handle, OFX::ContextEnum )
{
	return new TinselPlugin( handle );
}

void OFX::Plugin::getPluginIDs( OFX::PluginFactoryArray& ids )
{
	// Deliberately leaked: a by-value static would register an exit-time
	// destructor inside this module, and a host that dlclose()s the bundle
	// before process exit then jumps through a dangling pointer.
	static TinselPluginFactory* factory =
		new TinselPluginFactory( kPluginIdentifier, PLUGIN_VERSION_MAJOR, PLUGIN_VERSION_MINOR );
	ids.push_back( factory );
}
