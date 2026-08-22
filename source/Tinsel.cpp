#include "Tinsel.h"

#include "Controls.h"
#include "Diag.h"
#include "Effects.h"
#include "Palette.h"
#include "Shaders.h"

//FFGLSDK.h includes every other scoped binding and omits this one (SDK
//b1afaf9), so it has to be asked for by name. The symptom without it is an
//unknown-type error on ScopedFBOBinding and nothing else.
#include <ffglex/FFGLScopedFBOBinding.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <string>
#include <vector>

using namespace ffglex;
using namespace tinsel;

static CFFGLPluginInfo PluginInfo(
	PluginFactory< Tinsel >,                                  // Create method
	"TN01",                                                   // Plugin unique ID of maximum length 4.
	"Tinsel",                                                 // Plugin name
	2,                                                        // API major version number
	1,                                                        // API minor version number
	0,                                                        // Plugin major version number
	1,                                                        // Plugin minor version number
	FF_EFFECT,                                                // Plugin type
	"Lights the outlines in the clip like a string of LEDs",  // Plugin description
	"Tinsel FFGL effect"                                      // About
);

namespace
{
/// glGetString returns nullptr when there is no current context, and feeding
/// that to std::string is undefined behaviour. A logging call must never be the
/// thing that brings the host down.
std::string glStringOrUnknown( GLenum name )
{
	const GLubyte* value = glGetString( name );
	return value ? reinterpret_cast< const char* >( value ) : "unknown";
}

const char* const kSourceNames[]     = { "Luma", "Alpha", "Chroma", "Luma or Alpha" };
const char* const kLayoutNames[]     = { "Spiral", "Angle", "Linear", "Radial", "Random" };
const char* const kBackgroundNames[] = { "Black", "Source", "Dimmed Source", "Transparent", "Edges" };
const char* const kSyncNames[]       = { "Free", "Beat", "Bar" };

constexpr int kSourceCount     = 4;
constexpr int kLayoutCount     = 5;
constexpr int kBackgroundCount = 5;
constexpr int kSyncCount       = 3;

/// What Speed means. Free: cycles per second, integrated. Beat and Bar:
/// cycles per beat or per bar, locked to the host's transport.
enum SyncMode
{
	kSyncFree = 0,
	kSyncBeat = 1,
	kSyncBar  = 2,
};

/// Seconds of host time a single frame is allowed to advance the animation by.
/// The host's clock is not ours: it jumps when the composition is scrubbed,
/// when a clip is retriggered, and by however long the machine was asleep. An
/// unclamped delta turns any of those into every effect skipping forward by
/// minutes, which reads as the plugin having crashed and recovered.
constexpr double kMaxFrameDelta = 0.25;

/// Wall clock, for hosts that never call SetTime. Steady rather than system,
/// so nothing here moves when the machine's clock is corrected.
/// Frames that must agree before the host's clock unit is settled.
constexpr int kClockVotes = 4;

double wallSeconds()
{
	using namespace std::chrono;
	static const steady_clock::time_point start = steady_clock::now();
	return duration_cast< duration< double > >( steady_clock::now() - start ).count();
}

/// Case-insensitive name order, so a list sorts the way a reader expects it to
/// rather than the way a byte comparison does. Every name here happens to be
/// capitalised the same way today; that is not a thing to depend on.
bool nameLess( const char* a, const char* b )
{
	for( ; *a && *b; ++a, ++b )
	{
		const int ca = std::tolower( static_cast< unsigned char >( *a ) );
		const int cb = std::tolower( static_cast< unsigned char >( *b ) );
		if( ca != cb )
			return ca < cb;
	}
	return *b != '\0';
}
} // namespace

Tinsel::Tinsel()
{
	SetMinInputs( 1 );
	SetMaxInputs( 1 );

	//The host drives the animation where it can, so that rendering the same
	//frame twice gives the same picture twice and an export matches the
	//preview.
	SetTimeSupported( true );

	//---------------------------------------------------------------------
	// Defaults. SetParamInfof reads each one back out of GetFloatParameter, so
	// these assignments are what the host is told the defaults are.
	//
	// They add up to warm-white lamps twinkling round the outline over black,
	// because an effect that needs four sliders found before it does anything
	// recognisable is an effect nobody keeps. The null is Mix at zero.
	//---------------------------------------------------------------------
	params[ PT_SOURCE ]      = 3.0f;//Luma or Alpha -- right for artwork either way
	params[ PT_SENSITIVITY ] = 0.60f;
	params[ PT_SOFTNESS ]    = 0.35f;
	params[ PT_DETAIL ]      = 0.15f;
	params[ PT_THICKNESS ]   = 0.25f;
	params[ PT_STABILITY ]   = 0.35f;

	params[ PT_LAYOUT ]       = 0.0f;//Spiral
	//Half a turn, not two and a half.
	//
	//Turns decides the angle at which the strip's level sets cross the outline,
	//and that angle is what decides whether a lamp is a dot or a smear: a lamp
	//is a band across the field, so where the band lies along the outline
	//rather than across it, the lamp is drawn out into a streak. A tight spiral
	//meets a round logo at a shallow angle nearly everywhere. Wound loosely it
	//crosses close to square, which is what makes lamps read as lamps.
	params[ PT_TURNS ]        = 0.08f;
	params[ PT_LAYOUT_ANGLE ] = 0.0f;
	params[ PT_DENSITY ]      = 0.55f;
	params[ PT_BULB_SIZE ]    = 0.45f;
	params[ PT_REVERSE ]      = 0.0f;

	params[ PT_EFFECT ]    = static_cast< float >( Effect::Twinkle );
	params[ PT_SPEED ]     = 0.25f;
	params[ PT_INTENSITY ] = 0.50f;

	params[ PT_PALETTE ]     = static_cast< float >( Palette::WarmWhite );
	params[ PT_SPREAD ]      = 0.40f;
	params[ PT_C1_R ]        = 1.00f;
	params[ PT_C1_G ]        = 0.72f;
	params[ PT_C1_B ]        = 0.36f;
	params[ PT_C2_R ]        = 0.10f;
	params[ PT_C2_G ]        = 0.55f;
	params[ PT_C2_B ]        = 1.00f;
	params[ PT_SATURATION ]  = 0.667f;//1.0 after mapping
	params[ PT_BRIGHTNESS ]  = 0.50f; //1.0 after mapping
	params[ PT_SOURCE_TINT ] = 0.0f;

	params[ PT_GLOW ]       = 0.40f;
	params[ PT_GLOW_SIZE ]  = 0.35f;
	params[ PT_BACKGROUND ] = 0.0f;//Black
	params[ PT_DIM ]        = 0.25f;
	params[ PT_MIX ]        = 1.0f;

	params[ PT_PRESET ] = 0.0f;//Custom: the sliders are the truth
	params[ PT_SYNC ]   = 0.0f;//Free: v0.1.0's behaviour, and the harness's
	params[ PT_AUDIO_LEVEL ] = 0.0f;

	//---------------------------------------------------------------------
	// Declaration.
	//
	// Every numeric parameter is a plain 0..1 float even where it stands for a
	// lamp count or a mip level. SetParamInfo clamps an FF_TYPE_STANDARD
	// default into 0..1 *before* a range can be attached (SDK b1afaf9), so a
	// parameter declared in lamps cannot declare a default in lamps. The
	// conversions live in Controls.cpp.
	//
	// Option lists are declared in alphabetical order, and every entry keeps
	// the value it has always had. Those are two separate things in FFGL:
	// SetParamElementInfo takes an element's display slot and its stored value
	// as different arguments, and the spec is explicit that picking an option
	// gives the parameter "a value equal to that of the option's value" — the
	// slot is never stored. Everything on this side reads the value too;
	// params[] goes to the shader untouched. So the list can be re-sorted for
	// whoever has to find Meteor in it without a saved composition, a factory
	// preset or the harness changing meaning: the entry that has always stored
	// 8 still stores 8, it has just moved.
	//
	// Sorting is the operator's interest, not ours — twenty patterns and
	// sixteen palettes are otherwise in the order they were written in, which
	// is no order at all from outside this file. Reported as tinsel#4.
	//---------------------------------------------------------------------

	/// Declare an option's elements alphabetically, each keeping its value.
	/// The first `pinned` values stay at the top of the list in ordinal order:
	/// those are the entries whose position is the meaning rather than the
	/// name, and sorting them would say something untrue about them.
	auto declareOptions = [ this ]( unsigned int paramID, int count, int pinned, auto nameOf ) {
		std::vector< int > order( static_cast< size_t >( count ) );
		for( int i = 0; i < count; ++i )
			order[ i ] = i;

		std::stable_sort( order.begin() + pinned, order.end(),
		                  [ & ]( int a, int b ) { return nameLess( nameOf( a ), nameOf( b ) ); } );

		for( int slot = 0; slot < count; ++slot )
			SetParamElementInfo( paramID,
			                     static_cast< unsigned int >( slot ),
			                     nameOf( order[ slot ] ),
			                     static_cast< float >( order[ slot ] ) );
	};

	SetOptionParamInfo( PT_SOURCE, "Detect On", kSourceCount, params[ PT_SOURCE ] );
	declareOptions( PT_SOURCE, kSourceCount, 0, []( int v ) { return kSourceNames[ v ]; } );

	SetParamInfof( PT_SENSITIVITY, "Sensitivity", FF_TYPE_STANDARD );
	SetParamInfof( PT_SOFTNESS, "Softness", FF_TYPE_STANDARD );
	SetParamInfof( PT_DETAIL, "Detail", FF_TYPE_STANDARD );
	SetParamInfof( PT_THICKNESS, "Thickness", FF_TYPE_STANDARD );
	SetParamInfof( PT_STABILITY, "Stability", FF_TYPE_STANDARD );

	SetOptionParamInfo( PT_LAYOUT, "Layout", kLayoutCount, params[ PT_LAYOUT ] );
	declareOptions( PT_LAYOUT, kLayoutCount, 0, []( int v ) { return kLayoutNames[ v ]; } );

	SetParamInfof( PT_TURNS, "Turns", FF_TYPE_STANDARD );
	SetParamInfof( PT_LAYOUT_ANGLE, "Direction", FF_TYPE_STANDARD );
	SetParamInfof( PT_DENSITY, "Lamps", FF_TYPE_STANDARD );
	SetParamInfof( PT_BULB_SIZE, "Lamp Size", FF_TYPE_STANDARD );
	SetParamInfof( PT_REVERSE, "Reverse", FF_TYPE_BOOLEAN );

	SetOptionParamInfo( PT_EFFECT, "Pattern", static_cast< int >( Effect::Count ), params[ PT_EFFECT ] );
	declareOptions( PT_EFFECT, static_cast< int >( Effect::Count ), 0,
	                []( int v ) { return EffectName( static_cast< Effect >( v ) ); } );

	SetParamInfof( PT_SPEED, "Speed", FF_TYPE_STANDARD );
	SetParamInfof( PT_INTENSITY, "Intensity", FF_TYPE_STANDARD );

	//Colour 1 and Colour 1 > 2 are pinned to the top. They are the two that are
	//not baked palettes at all -- they are driven by the colour pickers below,
	//and reading them as the head of the list is what says so.
	SetOptionParamInfo( PT_PALETTE, "Palette", static_cast< int >( Palette::Count ), params[ PT_PALETTE ] );
	declareOptions( PT_PALETTE, static_cast< int >( Palette::Count ), 2,
	                []( int v ) { return PaletteName( static_cast< Palette >( v ) ); } );

	SetParamInfof( PT_SPREAD, "Spread", FF_TYPE_STANDARD );

	//Consecutive red/green/blue parameters are what a host needs to show a
	//colour swatch instead of three sliders, so the naming follows the SDK's
	//own convention rather than being tidied up.
	SetParamInfof( PT_C1_R, "Colour 1", FF_TYPE_RED );
	SetParamInfof( PT_C1_G, "Colour1_Green", FF_TYPE_GREEN );
	SetParamInfof( PT_C1_B, "Colour1_Blue", FF_TYPE_BLUE );
	SetParamInfof( PT_C2_R, "Colour 2", FF_TYPE_RED );
	SetParamInfof( PT_C2_G, "Colour2_Green", FF_TYPE_GREEN );
	SetParamInfof( PT_C2_B, "Colour2_Blue", FF_TYPE_BLUE );

	SetParamInfof( PT_SATURATION, "Saturation", FF_TYPE_STANDARD );
	SetParamInfof( PT_BRIGHTNESS, "Brightness", FF_TYPE_STANDARD );
	SetParamInfof( PT_SOURCE_TINT, "Source Tint", FF_TYPE_STANDARD );

	SetParamInfof( PT_GLOW, "Glow", FF_TYPE_STANDARD );
	SetParamInfof( PT_GLOW_SIZE, "Glow Size", FF_TYPE_STANDARD );

	SetOptionParamInfo( PT_BACKGROUND, "Background", kBackgroundCount, params[ PT_BACKGROUND ] );
	declareOptions( PT_BACKGROUND, kBackgroundCount, 0, []( int v ) { return kBackgroundNames[ v ]; } );

	SetParamInfof( PT_DIM, "Dim", FF_TYPE_STANDARD );
	SetParamInfof( PT_MIX, "Mix", FF_TYPE_STANDARD );

	// Factory presets. Element 0 is Custom; picking anything else copies that
	// preset's values into the covered parameters and raises value events so
	// the host re-reads the sliders. Editing a covered slider flips back to
	// Custom.
	//
	// Custom is pinned to the top and the presets sort below it. It is not a
	// preset — it is the statement that there isn't one — so a list that filed
	// it under C between Candy Wipe and Christmas Chase would be lying about
	// what it is. Its value stays 0, which is what applyPreset's 1-based index
	// and the sweep's Preset context both read.
	SetOptionParamInfo( PT_PRESET, "Preset", 1 + presets::kCount, params[ PT_PRESET ] );
	declareOptions( PT_PRESET, 1 + presets::kCount, 1, []( int v ) {
		return v == 0 ? "Custom" : presets::kPresets[ v - 1 ].name;
	} );

	// What Speed means: cycles per second (Free), or cycles per beat or bar,
	// phase-locked to Resolume's BPM clock.
	//
	// Deliberately not sorted, and not an oversight to tidy up later: Free,
	// Beat, Bar is a progression from no clock to the slowest division of one,
	// and the reader is following that rather than looking a name up. Sorted it
	// reads Bar, Beat, Free, which puts the two locked modes either side of the
	// unlocked one and says nothing true about any of them.
	SetOptionParamInfo( PT_SYNC, "Sync", kSyncCount, params[ PT_SYNC ] );
	for( int i = 0; i < kSyncCount; ++i )
		SetParamElementInfo( PT_SYNC, i, kSyncNames[ i ], static_cast< float >( i ) );

	// Audio. An FFT buffer: Resolume shows it as an audio-source picker and
	// writes one spectrum bin per element. Element defaults are zero on
	// purpose -- with no audio routed, Audio Level does nothing rather than
	// the lamps twitching to a phantom signal.
	SetBufferParamInfo( PT_AUDIO, "Audio", kAudioBins, FF_USAGE_FFT );
	for( int i = 0; i < kAudioBins; ++i )
		SetParamElementInfo( PT_AUDIO, i, "", 0.0f );

	SetParamInfof( PT_AUDIO_LEVEL, "Audio Level", FF_TYPE_STANDARD );

	//Thirty-one parameters is well past the point where an ungrouped list in
	//somebody else's inspector stops being readable.
	for( FFUInt32 i = PT_SOURCE; i <= PT_STABILITY; ++i )
		SetParamGroup( i, "Edge" );
	for( FFUInt32 i = PT_LAYOUT; i <= PT_REVERSE; ++i )
		SetParamGroup( i, "Strip" );
	for( FFUInt32 i = PT_EFFECT; i <= PT_INTENSITY; ++i )
		SetParamGroup( i, "Pattern" );
	for( FFUInt32 i = PT_PALETTE; i <= PT_SOURCE_TINT; ++i )
		SetParamGroup( i, "Colour" );
	for( FFUInt32 i = PT_GLOW; i <= PT_MIX; ++i )
		SetParamGroup( i, "Output" );
	SetParamGroup( PT_PRESET, "Preset" );
	// Not "Pattern", where it logically belongs: SetParamGroup collapses *runs*
	// of consecutive same-group ids, and PT_SYNC is appended, so it would show
	// as a second group also titled "Pattern" -- which reads as a bug.
	SetParamGroup( PT_SYNC, "Tempo" );
	SetParamGroup( PT_AUDIO, "Audio" );
	SetParamGroup( PT_AUDIO_LEVEL, "Audio" );

	// The About block. Declared inline rather than through a helper, because
	// SetParamInfo is protected on CFFGLPlugin and nothing outside the class
	// can call it.
	SetParamInfo( PT_ABOUT_FIRST, "About", FF_TYPE_TEXT, stoatworks::about::defaultText() );
	{
		FFUInt32 aboutId = PT_ABOUT_FIRST + 1;
		for( const auto& b : stoatworks::about::buttons() )
			SetParamInfo( aboutId++, b.label, FF_TYPE_EVENT, false );
	}
	for( FFUInt32 i = PT_ABOUT_FIRST; i < PT_COUNT; ++i )
		SetParamGroup( i, "About" );

	FFGLLog::LogToHost( "Created Tinsel effect" );

	diag::init();
}

//---------------------------------------------------------------------------
bool Tinsel::UploadPalettes()
{
	const std::vector< float > table = BakePaletteTable();

	glGenTextures( 1, &paletteTexture );
	if( paletteTexture == 0 )
		return false;

	Scoped2DTextureBinding binding( paletteTexture );

	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA32F,
	              kPaletteSize, static_cast< GLsizei >( Palette::Count ), 0,
	              GL_RGBA, GL_FLOAT, table.data() );

	//Addressed by texelFetch only. Nothing may be interpolated -- one texture
	//has one filter for both axes, and bilinear here would blend each palette
	//into the one below it.
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );

	return true;
}

//---------------------------------------------------------------------------
FFResult Tinsel::InitGL( const FFGLViewportStruct* vp )
{
	//The GL strings first, and unconditionally: when a shader will not compile
	//it is almost always the driver or the GL version, and knowing which
	//machine reported what is most of the diagnosis.
	diag::info( std::string( "GL vendor=" ) + glStringOrUnknown( GL_VENDOR )
	            + " renderer=" + glStringOrUnknown( GL_RENDERER )
	            + " version=" + glStringOrUnknown( GL_VERSION ) );

	//The light pass is assembled rather than written out, because the effect
	//library in the middle of it is shared verbatim with the harness's probe.
	//Held in a local so the pointer handed to Compile outlives the call.
	const std::string lightSource = LightShaderSource();

	struct
	{
		FFGLShader* shader;
		const char* fragment;
		const char* name;
	} const stages[] = {
		{ &copyShader, kCopyShader, "copy" },
		{ &edgeShader, kEdgeShader, "edge" },
		{ &stabiliseShader, kStabiliseShader, "stabilise" },
		{ &lightShader, lightSource.c_str(), "light" },
		{ &blurShader, kBlurShader, "blur" },
		{ &compositeShader, kCompositeShader, "composite" },
	};

	for( const auto& stage : stages )
	{
		if( stage.shader->Compile( kVertexShader, stage.fragment ) )
			continue;

		//Returning FF_FAIL here is invisible to the operator: the effect simply
		//does nothing in Resolume, with no message anywhere. These two lines
		//are the only record of which pass it was.
		diag::error( std::string( "the " ) + stage.name
		             + " shader failed to compile - the effect will do nothing" );
		FFGLLog::LogToHost( "Tinsel: shader failed to compile" );
		DeInitGL();
		return FF_FAIL;
	}

	if( !quad.Initialise() )
	{
		diag::error( "quad geometry failed to initialise" );
		FFGLLog::LogToHost( "Tinsel: quad geometry failed to initialise" );
		DeInitGL();
		return FF_FAIL;
	}

	if( !UploadPalettes() )
	{
		diag::error( "could not upload the palette table" );
		FFGLLog::LogToHost( "Tinsel: could not upload the palette table" );
		DeInitGL();
		return FF_FAIL;
	}

	historyValid = false;

	diag::info( "initialised, " + std::to_string( static_cast< int >( Effect::Count ) ) + " patterns and "
	            + std::to_string( static_cast< int >( Palette::Count ) ) + " palettes" );

	//Use base-class init as the success result so it retains the viewport.
	return CFFGLPlugin::InitGL( vp );
}

//---------------------------------------------------------------------------
FFResult Tinsel::ProcessOpenGL( ProcessOpenGLStruct* pGL )
{
	if( pGL->numInputTextures < 1 || pGL->inputTextures[ 0 ] == nullptr )
		return FF_FAIL;

	const FFGLTextureStruct& picture = *pGL->inputTextures[ 0 ];
	if( picture.Width == 0 || picture.Height == 0 )
		return FF_FAIL;

	const int pictureWidth  = static_cast< int >( picture.Width );
	const int pictureHeight = static_cast< int >( picture.Height );

	//The host's viewport, read before anything of ours changes it.
	//
	//`ScopedFBOBinding` restores the framebuffer binding and *only* the
	//framebuffer binding -- it does not touch the viewport (SDK b1afaf9,
	//FFGLScopedFBOBinding.cpp). So every pass's ResizeViewPort() leaks out into
	//the pass after it, and the composite, which draws to the host's own
	//framebuffer and so has no buffer of its own to size itself from, inherits
	//whatever the last pass left behind. Here that was the quarter-size glow
	//buffer, and the effect rendered into the bottom-left quarter of the frame
	//with the rest left transparent. Worth knowing that it does not look like a
	//viewport bug from the outside: in a viewer that shows transparency as
	//white, it reads as the effect having blown out to white with a small
	//correct picture in the corner.
	GLint hostViewport[ 4 ] = { 0, 0, 0, 0 };
	glGetIntegerv( GL_VIEWPORT, hostViewport );

	//---------------------------------------------------------------------
	// Time. In Free mode, integrate the rate; never rescale the history. See
	// Tinsel.h.
	//
	// In Beat and Bar mode the phase is absolute instead: the point of those
	// modes is that a cycle boundary lands on the host's grid, and an
	// integrated phase drifts off it. That makes a Speed change jump the
	// pattern -- which is correct there: the operator asked for a different
	// musical division, and the new one has to land on the grid too.
	//---------------------------------------------------------------------
	// Normalise the host's clock to seconds first -- see Tinsel.h: Resolume
	// sends milliseconds, the harness sends seconds, and the header says
	// nothing. Until the first plausible frame delta decides, assume seconds;
	// the decision lands within two frames and the delta clamp below absorbs
	// the one mis-scaled step a late decision could produce.
	// steady_clock says how much real time passed, the host says how much host
	// time passed, and the ratio names the unit outright -- 1 for seconds,
	// 1000 for milliseconds, and nothing plausible in between. This replaced a
	// guess made from the magnitude of a single frame delta, which had three
	// holes: a delta between 0.5 and 2.0 decided nothing, a burst of sub-0.5 ms
	// frames at load locked it to "seconds" for the session, and while
	// undecided it assumed seconds -- precisely the millisecond host's wrong
	// answer.
	const double wallNow = wallSeconds();
	if( wallStart < 0.0 )
		wallStart = wallNow;

	const double raw = hostTime;

	if( clockScale == 0.0 && raw >= 0.0 && lastRawTime >= 0.0 && lastWallTime >= 0.0 )
	{
		const double hostDelta = raw - lastRawTime;
		const double wallDelta = wallNow - lastWallTime;

		// A paused host, a looping clip or a stalled frame tells us nothing.
		if( hostDelta > 0.0 && wallDelta >= 0.0005 )
		{
			const double ratio = hostDelta / wallDelta;
			if( ratio > 0.1 && ratio < 10.0 )
				++secondsVotes;
			else if( ratio > 100.0 && ratio < 10000.0 )
				++millisVotes;

			// Several frames rather than one, so a single odd frame cannot
			// decide it alone.
			if( secondsVotes >= kClockVotes || millisVotes >= kClockVotes )
				clockScale = millisVotes > secondsVotes ? 0.001 : 1.0;
		}
	}

	if( raw >= 0.0 )
		lastRawTime = raw;
	lastWallTime = wallNow;

	// Until the unit is settled -- and for a host that never calls SetTime --
	// run on the real clock: wrong in origin but right in rate, where assuming
	// seconds would be a thousand times fast on Resolume.
	const double now = ( raw >= 0.0 && clockScale != 0.0 ) ? raw * clockScale
	                                                       : wallNow - wallStart;
	const int    sync = static_cast< int >( std::lround( params[ PT_SYNC ] ) );
	const double speed = static_cast< double >( SpeedFromParam( params[ PT_SPEED ] ) );

	if( sync == kSyncBeat || sync == kSyncBar )
	{
		// The host hands us a tempo and a position *within* the current bar,
		// never which bar it is. Recover a continuous bar number without
		// keeping state: the clock estimates how many bars have passed,
		// `barPhase` gives the exact position inside this one, and the whole
		// number reconciling them is round( estimate - barPhase ). Continuous
		// across the bar line -- as barPhase wraps 1 to 0 the rounded integer
		// steps up at the same moment -- and exact while the clock estimate is
		// within half a bar. (Same recovery as Orrery's Sync modes.)
		const double tempo      = bpm > 1.0f ? static_cast< double >( bpm ) : 120.0;
		const double barSeconds = 240.0 / tempo;//four beats to the bar
		const double estimate   = now / barSeconds;
		const double within     = std::clamp( static_cast< double >( barPhase ), 0.0, 1.0 );

		const double bars = within + std::round( estimate - within );

		phase = ( sync == kSyncBeat ? bars * 4.0 : bars ) * speed;
	}
	else if( lastHostTime >= 0.0 )
	{
		const double delta = std::clamp( now - lastHostTime, 0.0, kMaxFrameDelta );
		phase += delta * speed;
	}

	if( ++clockFrames == 60 )
		diag::info( "host clock at frame 60: raw=" + std::to_string( raw )
		            + " scale=" + std::to_string( clockScale )
		            + " seconds=" + std::to_string( now )
		            + " bpm=" + std::to_string( bpm )
		            + " barPhase=" + std::to_string( barPhase ) );

	lastHostTime = now;

	UpdateAudio();

	//---------------------------------------------------------------------
	// Buffers.
	//
	// Every Ensure() happens here, before anything binds a texture. That is
	// not tidiness: ffglex::FFGLFBO::Initialise sizes its new colour texture
	// under a ScopedTextureBinding, and every ffglex Scoped* binding *clears*
	// to 0 on scope exit rather than restoring what was there. Allocating a
	// buffer therefore unbinds the input texture from the active unit, and the
	// symptom is the dangerous part -- correct on every frame except the one
	// that allocates, so a control reads zero for a single frame after load and
	// once more each time a size drag reallocates.
	//---------------------------------------------------------------------
	const float glowSize  = GlowSizeFromParam( params[ PT_GLOW_SIZE ] );
	const int glowWidth   = std::max( 16, pictureWidth / 4 );
	const int glowHeight  = std::max( 16, pictureHeight / 4 );

	const bool allocated =
		copyBuffer.Ensure( pictureWidth, pictureHeight, GL_RGBA16F, PassBuffer::Sampling::Mipmapped )
		&& edgeBuffer.Ensure( pictureWidth, pictureHeight, GL_RGBA16F, PassBuffer::Sampling::Linear )
		&& stableBuffer[ 0 ].Ensure( pictureWidth, pictureHeight, GL_RGBA16F, PassBuffer::Sampling::Mipmapped )
		&& stableBuffer[ 1 ].Ensure( pictureWidth, pictureHeight, GL_RGBA16F, PassBuffer::Sampling::Mipmapped )
		&& lightBuffer.Ensure( pictureWidth, pictureHeight, GL_RGBA16F, PassBuffer::Sampling::Mipmapped )
		&& glowBuffer[ 0 ].Ensure( glowWidth, glowHeight, GL_RGBA16F, PassBuffer::Sampling::Linear )
		&& glowBuffer[ 1 ].Ensure( glowWidth, glowHeight, GL_RGBA16F, PassBuffer::Sampling::Linear );

	if( !allocated )
	{
		diag::error( "could not allocate the pass buffers" );
		return FF_FAIL;
	}

	//---------------------------------------------------------------------
	// 1. The picture, into a texture of ours, with a mip chain on it.
	//---------------------------------------------------------------------
	{
		ScopedFBOBinding fbo( copyBuffer.GetGLID(), ScopedFBOBinding::RB_REVERT );
		copyBuffer.ResizeViewPort();
		ScopedShaderBinding shader( copyShader.GetGLID() );
		ScopedSamplerActivation sampler( 0 );
		Scoped2DTextureBinding texture( picture.Handle );

		const FFGLTexCoords maxCoords = GetMaxGLTexCoords( picture );
		copyShader.Set( "InputTexture", 0 );
		copyShader.Set( "MaxUV", maxCoords.s, maxCoords.t );
		copyShader.Set( "HalfTexel",
		                0.5f / static_cast< float >( pictureWidth ),
		                0.5f / static_cast< float >( pictureHeight ) );
		quad.Draw();
	}
	copyBuffer.GenerateMipmaps();

	//---------------------------------------------------------------------
	// 2. Edge.
	//---------------------------------------------------------------------
	{
		ScopedFBOBinding fbo( edgeBuffer.GetGLID(), ScopedFBOBinding::RB_REVERT );
		edgeBuffer.ResizeViewPort();
		ScopedShaderBinding shader( edgeShader.GetGLID() );
		ScopedSamplerActivation sampler( 0 );
		Scoped2DTextureBinding texture( copyBuffer.TextureID() );

		edgeShader.Set( "CopyTexture", 0 );
		edgeShader.Set( "TexelSize",
		                1.0f / static_cast< float >( pictureWidth ),
		                1.0f / static_cast< float >( pictureHeight ) );
		edgeShader.Set( "Detail", DetailFromParam( params[ PT_DETAIL ] ) );
		edgeShader.Set( "SourceMode", params[ PT_SOURCE ] );
		quad.Draw();
	}

	//---------------------------------------------------------------------
	// 3. Stabilise, ping-ponged against the previous frame's result.
	//---------------------------------------------------------------------
	const int history = stableCurrent;
	const int target  = 1 - stableCurrent;
	{
		ScopedFBOBinding fbo( stableBuffer[ target ].GetGLID(), ScopedFBOBinding::RB_REVERT );
		stableBuffer[ target ].ResizeViewPort();
		ScopedShaderBinding shader( stabiliseShader.GetGLID() );

		ScopedSamplerActivation sampler0( 0 );
		Scoped2DTextureBinding edgeTexture( edgeBuffer.TextureID() );
		ScopedSamplerActivation sampler1( 1 );
		Scoped2DTextureBinding historyTexture( stableBuffer[ history ].TextureID() );

		stabiliseShader.Set( "EdgeTexture", 0 );
		stabiliseShader.Set( "HistoryTexture", 1 );
		stabiliseShader.Set( "Attack", AttackFromParam( params[ PT_STABILITY ] ) );
		stabiliseShader.Set( "Release", ReleaseFromParam( params[ PT_STABILITY ] ) );
		stabiliseShader.Set( "Sensitivity", SensitivityFromParam( params[ PT_SENSITIVITY ] ) );
		stabiliseShader.Set( "Softness", SoftnessFromParam( params[ PT_SOFTNESS ] ) );
		stabiliseShader.Set( "Reset", historyValid ? 0.0f : 1.0f );
		quad.Draw();
	}
	stableCurrent = target;
	historyValid  = true;
	stableBuffer[ target ].GenerateMipmaps();

	//---------------------------------------------------------------------
	// 4. Light. The plugin.
	//---------------------------------------------------------------------
	{
		ScopedFBOBinding fbo( lightBuffer.GetGLID(), ScopedFBOBinding::RB_REVERT );
		lightBuffer.ResizeViewPort();
		ScopedShaderBinding shader( lightShader.GetGLID() );

		ScopedSamplerActivation sampler0( 0 );
		Scoped2DTextureBinding stableTexture( stableBuffer[ target ].TextureID() );
		ScopedSamplerActivation sampler1( 1 );
		Scoped2DTextureBinding copyTexture( copyBuffer.TextureID() );
		ScopedSamplerActivation sampler2( 2 );
		Scoped2DTextureBinding paletteBinding( paletteTexture );

		lightShader.Set( "StableTexture", 0 );
		lightShader.Set( "CopyTexture", 1 );
		lightShader.Set( "PaletteTexture", 2 );

		lightShader.Set( "Aspect", static_cast< float >( pictureWidth ) / static_cast< float >( pictureHeight ) );
		lightShader.Set( "CentroidLod", stableBuffer[ target ].MaxMipLevel() );
		lightShader.Set( "Thickness", ThicknessFromParam( params[ PT_THICKNESS ] ) );

		lightShader.Set( "Layout", params[ PT_LAYOUT ] );
		lightShader.Set( "LayoutAngle", LayoutAngleFromParam( params[ PT_LAYOUT_ANGLE ] ) );
		lightShader.Set( "Turns", TurnsFromParam( params[ PT_TURNS ] ) );
		lightShader.Set( "BulbCount", BulbCountFromParam( params[ PT_DENSITY ] ) );
		lightShader.Set( "BulbSize", BulbSizeFromParam( params[ PT_BULB_SIZE ] ) );
		lightShader.Set( "Reverse", params[ PT_REVERSE ] );

		lightShader.Set( "EffectIndex", params[ PT_EFFECT ] );
		lightShader.Set( "Time", static_cast< float >( phase ) );
		lightShader.Set( "Intensity", params[ PT_INTENSITY ] );
		lightShader.Set( "Spread", SpreadFromParam( params[ PT_SPREAD ] ) );

		lightShader.Set( "PaletteIndex", params[ PT_PALETTE ] );
		lightShader.Set( "PaletteCount", static_cast< float >( Palette::Count ) );
		lightShader.Set( "Colour1", params[ PT_C1_R ], params[ PT_C1_G ], params[ PT_C1_B ] );
		lightShader.Set( "Colour2", params[ PT_C2_R ], params[ PT_C2_G ], params[ PT_C2_B ] );
		lightShader.Set( "Saturation", SaturationFromParam( params[ PT_SATURATION ] ) );
		lightShader.Set( "Brightness", BrightnessFromParam( params[ PT_BRIGHTNESS ] ) );
		lightShader.Set( "SourceTint", params[ PT_SOURCE_TINT ] );

		//FFGLShader::Set has no array overload, so the spectrum goes up raw.
		lightShader.Set( "AudioLevel", params[ PT_AUDIO_LEVEL ] );
		glUniform1fv( lightShader.FindUniform( "Audio" ), kAudioBins, audioLevel.data() );

		quad.Draw();
	}

	//The glow's first pass reads this while drawing into a buffer a quarter the
	//size, so it needs a pre-filtered level to read rather than five point
	//samples of a picture four times finer than its target.
	lightBuffer.GenerateMipmaps();

	//---------------------------------------------------------------------
	// 5. Glow. Two separable passes, run twice: the second pair is wider than
	//    the first, and summing two Gaussians of different widths is what gives
	//    a bulb a tight core and a wide falloff instead of one soft blob.
	//---------------------------------------------------------------------
	{
		//How much finer the light buffer is than the glow buffer, as a mip level.
		const float downsampleLod =
			std::log2( std::max( 1.0f, static_cast< float >( pictureWidth ) / static_cast< float >( glowWidth ) ) );

		const float stepX = glowSize * 0.001f * static_cast< float >( pictureWidth ) / static_cast< float >( glowWidth );
		const float stepY = glowSize * 0.001f * static_cast< float >( pictureHeight ) / static_cast< float >( glowHeight );

		//The wide pair is 1.8x and not 2.5x. A second iteration only reads as a
		//smooth falloff while the first one's blur is wide enough to cover the
		//gaps between the second one's taps; past that the five fetches start
		//showing as separate ghosts of the picture. 1.8 keeps the widest gap
		//inside the first pass's own radius.
		struct Stage
		{
			int from;    ///< -1 for the light buffer
			int to;
			float x, y;
			float lod;   ///< only the first pass reads anything but level 0
		};
		const Stage stages[] = {
			{ -1, 0, stepX, 0.0f, downsampleLod },
			{ 0, 1, 0.0f, stepY, 0.0f },
			{ 1, 0, stepX * 1.8f, 0.0f, 0.0f },
			{ 0, 1, 0.0f, stepY * 1.8f, 0.0f },
		};

		for( const Stage& stage : stages )
		{
			ScopedFBOBinding fbo( glowBuffer[ stage.to ].GetGLID(), ScopedFBOBinding::RB_REVERT );
			glowBuffer[ stage.to ].ResizeViewPort();
			ScopedShaderBinding shader( blurShader.GetGLID() );
			ScopedSamplerActivation sampler( 0 );
			Scoped2DTextureBinding texture( stage.from < 0 ? lightBuffer.TextureID()
			                                               : glowBuffer[ stage.from ].TextureID() );

			blurShader.Set( "SourceTexture", 0 );
			blurShader.Set( "Direction", stage.x, stage.y );
			blurShader.Set( "SourceLod", stage.lod );
			quad.Draw();
		}
	}

	//---------------------------------------------------------------------
	// 6. Composite, straight to the host's framebuffer.
	//---------------------------------------------------------------------
	{
		//Back to the host's viewport. See the note where it was captured.
		glViewport( hostViewport[ 0 ], hostViewport[ 1 ], hostViewport[ 2 ], hostViewport[ 3 ] );

		ScopedShaderBinding shader( compositeShader.GetGLID() );

		ScopedSamplerActivation sampler0( 0 );
		Scoped2DTextureBinding copyTexture( copyBuffer.TextureID() );
		ScopedSamplerActivation sampler1( 1 );
		Scoped2DTextureBinding lightTexture( lightBuffer.TextureID() );
		ScopedSamplerActivation sampler2( 2 );
		Scoped2DTextureBinding glowTexture( glowBuffer[ 1 ].TextureID() );
		ScopedSamplerActivation sampler3( 3 );
		Scoped2DTextureBinding stableTexture( stableBuffer[ target ].TextureID() );

		compositeShader.Set( "CopyTexture", 0 );
		compositeShader.Set( "LightTexture", 1 );
		compositeShader.Set( "GlowTexture", 2 );
		compositeShader.Set( "StableTexture", 3 );

		compositeShader.Set( "Background", params[ PT_BACKGROUND ] );
		compositeShader.Set( "Dim", params[ PT_DIM ] );
		compositeShader.Set( "Glow", GlowFromParam( params[ PT_GLOW ] ) );
		compositeShader.Set( "MixAmount", params[ PT_MIX ] );
		quad.Draw();
	}

	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
FFResult Tinsel::DeInitGL()
{
	copyShader.FreeGLResources();
	edgeShader.FreeGLResources();
	stabiliseShader.FreeGLResources();
	lightShader.FreeGLResources();
	blurShader.FreeGLResources();
	compositeShader.FreeGLResources();
	quad.Release();

	copyBuffer.Destroy();
	edgeBuffer.Destroy();
	stableBuffer[ 0 ].Destroy();
	stableBuffer[ 1 ].Destroy();
	lightBuffer.Destroy();
	glowBuffer[ 0 ].Destroy();
	glowBuffer[ 1 ].Destroy();

	if( paletteTexture != 0 )
	{
		glDeleteTextures( 1, &paletteTexture );
		paletteTexture = 0;
	}

	historyValid = false;

	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
FFResult Tinsel::SetFloatParameter( unsigned int index, float value )
{
	if( index >= PT_COUNT )
		return FF_FAIL;

	// The About buttons open a browser and store nothing, so they are handled
	// before the params[] write below -- there is no value to keep.
	if( index >= PT_ABOUT_FIRST )
		return stoatworks::about::handleParam( index - PT_ABOUT_FIRST, value ) ? FF_SUCCESS : FF_FAIL;

	if( index == PT_PRESET )
	{
		const int chosen = static_cast< int >( std::lround( value ) );
		if( chosen != static_cast< int >( std::lround( params[ PT_PRESET ] ) ) )
			applyPreset( chosen );
		return FF_SUCCESS;
	}

	const float previous = params[ index ];
	params[ index ]      = value;

	//Changing what an edge *is* invalidates the history, because the numbers
	//being blended are no longer measuring the same thing. Without this,
	//turning Detail up with Stability high leaves the old scale's outlines
	//decaying underneath the new ones for a second or so, which looks like the
	//control having two effects.
	if( index == PT_DETAIL || index == PT_SOURCE )
		historyValid = false;

	// A slider moved while a preset is active means the operator has taken
	// over: the dropdown falls back to Custom. The equality guard matters —
	// hosts that honour the value events echo the preset's own values straight
	// back through here, and that echo must not un-set the preset.
	const int active = static_cast< int >( std::lround( params[ PT_PRESET ] ) );
	if( active > 0 && std::fabs( value - previous ) > 1e-4f )
	{
		for( unsigned int id : kPresetParamIDs )
		{
			if( id == index )
			{
				params[ PT_PRESET ] = 0.0f;
				RaiseParamEvent( PT_PRESET, FF_EVENT_FLAG_VALUE );
				break;
			}
		}
	}

	return FF_SUCCESS;
}

void Tinsel::applyPreset( int presetIndex )
{
	params[ PT_PRESET ] = static_cast< float >( presetIndex );

	if( presetIndex <= 0 || presetIndex > presets::kCount )
		return;//Custom: the sliders keep whatever they said

	const presets::Preset& preset = presets::kPresets[ presetIndex - 1 ];
	for( int j = 0; j < presets::kParamCount; ++j )
	{
		const unsigned int id = kPresetParamIDs[ j ];
		if( std::fabs( params[ id ] - preset.v[ j ] ) <= 1e-6f )
			continue;

		// The copy is what changes the picture; the event only tells the host
		// to re-read the slider. A host that ignores it renders the preset
		// correctly and merely shows stale knobs.
		params[ id ] = preset.v[ j ];
		RaiseParamEvent( id, FF_EVENT_FLAG_VALUE );
	}
}

float Tinsel::GetFloatParameter( unsigned int index )
{
	if( index >= PT_COUNT )
		return 0.0f;

	return params[ index ];
}

//---------------------------------------------------------------------------
char* Tinsel::GetTextParameter( unsigned int index )
{
	if( index == PT_ABOUT_FIRST )
	{
		aboutText = stoatworks::about::textParam( 0 );
		return const_cast< char* >( aboutText.c_str() );
	}

	return CFFGLPlugin::GetTextParameter( index );
}

//---------------------------------------------------------------------------
FFResult Tinsel::SetTextParameter( unsigned int index, const char* value )
{
	// See the declaration: the base class fails, and a failed default deletes
	// the instance. The About line is display-only, so there is genuinely
	// nothing to store -- but it has to say so successfully.
	if( index == PT_ABOUT_FIRST )
		return FF_SUCCESS;

	return CFFGLPlugin::SetTextParameter( index, value );
}

FFResult Tinsel::SetTime( double time )
{
	hostTime = time;
	return FF_SUCCESS;
}

void Tinsel::UpdateAudio()
{
	const ParamInfo* info = FindParamInfo( PT_AUDIO );
	if( info == nullptr )
		return;

	// Frame delta for the release filter, off the same clock everything else
	// runs on -- lastHostTime is the normalised-to-seconds clock the time
	// block just advanced, so the ms-vs-seconds question is already settled.
	// First frame -- or a clock that has not moved -- snaps instead.
	const double now = lastHostTime;
	const double dt  = ( audioClock >= 0.0 && now > audioClock ) ? now - audioClock : 0.0;
	audioClock       = now;

	// Fast up, slow down -- the same asymmetry as the stabilise pass and for
	// the same reason: a flash that arrives a frame late reads as broken,
	// while one that takes ~150 ms to die away reads as intended.
	const float release = dt > 0.0 ? 1.0f - std::exp( static_cast< float >( -dt / 0.15 ) ) : 1.0f;

	const size_t bins = std::min( info->elements.size(), audioLevel.size() );
	for( size_t i = 0; i < bins; ++i )
	{
		// sqrt because bin magnitudes bunch near zero: a spectrum used raw
		// lights nothing but the kick drum's lamp.
		const float raw = std::sqrt( std::max( 0.0f, info->elements[ i ].value ) );

		if( raw >= audioLevel[ i ] )
			audioLevel[ i ] = raw;
		else
			audioLevel[ i ] += ( raw - audioLevel[ i ] ) * release;
	}
}

void Tinsel::SetClockScaleForTest( double scale )
{
	clockScale = scale;
}
