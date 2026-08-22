#pragma once

#include "PassBuffer.h"
#include "Presets.h"
#include "StoatworksAboutParams.h"

#include <FFGLSDK.h>

#include <array>
#include <string>

/// Spectrum bins in the Audio buffer parameter, and in the light shader's
/// `Audio[]` uniform. The two must agree, and the shader's is a literal.
constexpr int kAudioBins = 64;

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
	/// Clock test hook. The offline harness DECLARES its unit rather than
	/// leaving the calibration to infer one -- an absolute time handed over in
	/// a single frame is genuinely ambiguous, and an implicit unit is what let
	/// the millisecond bug through in the first place.
	void SetClockScaleForTest( double scale );

	Tinsel();

	//CFFGLPlugin
	FFResult InitGL( const FFGLViewportStruct* vp ) override;
	FFResult ProcessOpenGL( ProcessOpenGLStruct* pGL ) override;
	FFResult DeInitGL() override;

	FFResult SetFloatParameter( unsigned int index, float value ) override;
	float GetFloatParameter( unsigned int index ) override;

	/// Test hook: the parameter ids a preset covers, in presets::Param
	/// order. Handed out rather than copied into the harness, so a second
	/// list cannot go quietly out of step with this one.
	static const unsigned int* PresetParamIDsForTest( int& count );

	FFResult SetTime( double time ) override;

	char* GetTextParameter( unsigned int index ) override;

	/// Declared only so the About line can accept its own default.
	/// instantiateGL pushes every declared default back through the setters and
	/// deletes the whole instance if one fails, and CFFGLPlugin's
	/// SetTextParameter is a stub that returns exactly that failure.
	FFResult SetTextParameter( unsigned int index, const char* value ) override;

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

		//Preset. Declared after the real controls so their IDs — which a saved
		//composition refers to — do not shift under existing users.
		PT_PRESET,

		//Sync. Appended for the same reason: this arrived after v0.1.0 shipped,
		//and inserting it next to Speed — where it belongs — would renumber
		//every parameter after it in every saved composition.
		PT_SYNC,

		//Audio. Appended likewise. PT_AUDIO is an FFT buffer (FF_TYPE_BUFFER,
		//FF_USAGE_FFT): Resolume shows it as an audio-source picker and writes
		//one spectrum bin per element, low frequencies first.
		PT_AUDIO,
		PT_AUDIO_LEVEL,

		//About. FFGL has no window and cannot make one, so the name, the
		//version, the maker and the links are parameters the host draws with
		//everything else. Last in the enum, like PT_SYNC and PT_AUDIO before
		//it, so no saved composition's parameter ids shift.
		//See StoatworksAboutParams.h.
		PT_ABOUT_FIRST,
		PT_COUNT = PT_ABOUT_FIRST + stoatworks::about::kParamCount
	};

private:
	/// The ParamID each presets::Param drives, in presets::Param order. The
	/// preset table stays host-agnostic; this is the FFGL binding of it.
	static constexpr unsigned int kPresetParamIDs[ tinsel::presets::kParamCount ] = {
		PT_LAYOUT, PT_TURNS, PT_LAYOUT_ANGLE, PT_DENSITY, PT_BULB_SIZE, PT_REVERSE,
		PT_EFFECT, PT_SPEED, PT_INTENSITY, PT_PALETTE, PT_SPREAD,
		PT_C1_R, PT_C1_G, PT_C1_B, PT_C2_R, PT_C2_G, PT_C2_B,
		PT_SATURATION, PT_BRIGHTNESS, PT_SOURCE_TINT,
		PT_GLOW, PT_GLOW_SIZE, PT_BACKGROUND, PT_DIM
	};

	/// Copy a factory preset's values into params[] and raise value events so
	/// the host re-reads the sliders. `presetIndex` is 1-based; 0 is Custom.
	/// The active preset's value for `id`, or -1 when no preset is active or
	/// this one has no opinion about `id`. Preset values are all 0..1, so a
	/// negative is unambiguous.
	float presetValue( int presetIndex, unsigned int id ) const;

	/// True when this write is the HOST restating a value it still believes in
	/// rather than the operator moving anything -- in which case it must not
	/// reach params[] and must not disturb the preset.
	bool hostIsRestatingItself( unsigned int index, float value );

	/// Record the defaults as the host's opening position, once, before
	/// anything has had a chance to move them.
	void seedHostValues();

	void applyPreset( int presetIndex );

	/// What the HOST last sent for each parameter, which is not the same thing
	/// as what the plugin is rendering with.
	///
	/// FFGL's host owns parameter state. It pushes its own values back down
	/// whenever it likes, and nothing obliges it to act on the value events
	/// applyPreset raises -- Resolume does not. So a preset that writes params[]
	/// and trusts the host to follow is relying on behaviour the specification
	/// never promised, and when the host instead restates the values it still
	/// believes in, the rule that a covered parameter changing means the
	/// operator has taken over fires on the host's own echo and drops straight
	/// back to Custom. Reported against vertigo as its issue #2; the same
	/// pattern had been copied into all seven plugins.
	///
	/// Keeping the host's own last word separately is what tells the two apart.
	float hostValues[ PT_COUNT ] = {};
	bool hostValuesSeeded        = false;

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
	//
	// That holds for Sync = Free only. In Beat and Bar mode the phase is
	// *absolute* -- recovered from the host's tempo and bar position each
	// frame -- because the entire point of those modes is that a cycle
	// boundary lands on the grid, and an integrated phase drifts off it.
	//---------------------------------------------------------------------
	double hostTime     = -1.0;
	double lastHostTime = -1.0;
	double phase        = 0.0;

	//---------------------------------------------------------------------
	// Host clock units.
	//
	// The FFGL header never says what unit SetTime is in, and hosts disagree:
	// Resolume hands over MILLISECONDS (measured live: 20.0 per frame at its
	// 50 fps, and the SDK's own Particles sample divides by 1000), while the
	// offline harness -- and any host following the header's silence -- sends
	// seconds. Decide from the first plausible frame delta and stick:
	// 0.001..0.5 is a seconds-host frame, 2..500 is a milliseconds-host
	// frame, anything else is a stall or a scrub and keeps waiting.
	//---------------------------------------------------------------------
	double clockScale  = 0.0;///< 0 until decided; then 1.0 or 0.001
	double lastWallTime = -1.0;
	double wallStart    = -1.0;
	int secondsVotes    = 0;
	int millisVotes     = 0;
	double lastRawTime = -1.0;

	/// Counts frames so the sixtieth can log what the host's clock actually
	/// looks like. One line, once, in the diag log.
	int clockFrames = 0;

	//---------------------------------------------------------------------
	// Audio.
	//
	// The host writes one spectrum bin per element of PT_AUDIO; UpdateAudio
	// runs them through an attack/release filter into `audioLevel`, and the
	// light pass reads that as a uniform array -- a per-lamp brightness gate
	// laid along the strip, orthogonal to whatever pattern is running.
	//---------------------------------------------------------------------
	void UpdateAudio();

	std::array< float, kAudioBins > audioLevel = {};
	double audioClock = -1.0;

	/// Set when the history buffers hold nothing worth blending against -- the
	/// first frame, and any frame after a resize. Without it the first
	/// stabilised frame blends the new picture against the last clip's
	/// outlines, which lingers for as long as Stability is set to hold things.
	bool historyValid = false;

	float params[ PT_COUNT ] = {};

	/// GetTextParameter hands the host a bare pointer, so the string has to
	/// outlive the call.
	std::string aboutText;
};
