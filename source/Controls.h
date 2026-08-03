#pragma once

/**
    Host parameters are 0..1; these are what they mean.

    Every numeric parameter this plugin declares is a plain FF_TYPE_STANDARD
    float in 0..1, including the ones that stand for a lamp count or a mip
    level. That is not a style preference. `CFFGLPluginManager::SetParamInfo`
    clamps a standard default into 0..1 *before* returning, and `SetParamRange`
    can only be called afterwards because it finds the parameter by ID -- so a
    parameter declared in lamps cannot declare a default in lamps, and 120
    would silently become 1. The conversions live here instead, in one file
    that both the plugin and the test harness use, so there is only ever one
    answer to what a slider position means.

    Where a mapping is geometric rather than linear it is because the
    interesting range is at one end. That is worth stating once: a slider is a
    fixed number of pixels wide and spending half of them on a range nobody
    uses is the difference between a control that works and one that has a
    sweet spot at 0.03.
*/
namespace tinsel
{

/// 0.01 to 1.0, geometrically. This is the gradient magnitude at which an edge
/// is fully lit, and a clean black-to-white step measures 1.0 -- so anything
/// above about 0.4 finds only the hardest boundaries in the frame, and the
/// whole useful range for photographic footage sits below 0.15.
float SensitivityFromParam( float value );

/// 0.05 to 1.0, linear, as a fraction of the sensitivity. The width of the
/// shoulder either side of the threshold.
float SoftnessFromParam( float value );

/// 0 to 4, linear. The mip level the Sobel runs at: 0 finds every pixel of
/// sensor noise, 3 finds the shape of a logo and ignores its texture.
float DetailFromParam( float value );

/// 0 to 3.5, linear. A mip level again, used the other way round -- the mask
/// is read blurred and re-thresholded low, which dilates it.
float ThicknessFromParam( float value );

/// The two halves of the temporal filter, from one Stability control.
///
/// They are asymmetric and that is the entire point: rise nearly instantly so
/// a moving outline is not dragged behind the thing that owns it, fall slowly
/// so a boundary that grades through the threshold for a frame or two does not
/// blink. See the comment in the stabilise shader.
float AttackFromParam( float value );

/// 1.0 down to 0.02, geometrically: a full second of persistence at 60fps at
/// the top of the range.
float ReleaseFromParam( float value );

/// 8 to 1000 lamps, geometrically. Geometric because the difference between 20
/// lamps and 40 is a different look and the difference between 800 and 820 is
/// not.
float BulbCountFromParam( float value );

/// A lamp's radius **in pixels**, 0.75 to 40, geometrically.
///
/// In pixels and not in strip units, which is the whole reason lamps read as
/// lamps: the strip coordinate is a field whose gradient varies across the
/// frame, so a window measured as a fraction of a lamp's own spacing is a dot
/// where the field runs fast and a long streak where it runs slowly. The light
/// pass divides by the field's screen-space gradient to get here.
///
/// Where the radius grows past half the lamp spacing the lamps merge and the
/// strip becomes a continuous rope. That is the neon-sign look, and it is the
/// top of this control's travel rather than a second mode.
float BulbSizeFromParam( float value );

/// 0 to 8 turns of spiral across the artwork.
float TurnsFromParam( float value );

/// 0 to 1 turn, for the Linear layout's direction.
float LayoutAngleFromParam( float value );

/// 0 to 4 cycles per second, linear, with zero meaning frozen. Not bipolar:
/// direction is Reverse's job, because for a comet the two are not the same
/// thing -- negating time walks the comet backwards with its tail in front.
float SpeedFromParam( float value );

/// 0.25 to 8 palette cycles across the strip, geometrically.
float SpreadFromParam( float value );

/// 0 to 2. Over 1 on purpose: the glow is what a bulb looks like, and a bulb
/// that is not clipping does not look like a bulb.
float BrightnessFromParam( float value );

/// 0 to 1.5. Over 1 pushes past the palette's own saturation.
float SaturationFromParam( float value );

/// 0 to 3.
float GlowFromParam( float value );

/// 0.5 to 8, geometrically: the glow's radius as a fraction of the picture
/// width, in per-mille.
float GlowSizeFromParam( float value );

} // namespace tinsel
