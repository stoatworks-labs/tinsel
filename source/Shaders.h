#pragma once

/**
    The passes, as GLSL source.

    Six fragment shaders and one vertex shader, in the order they run:

    1. **copy**      picture size. Resolves MaxUV once so no later pass has to
                     think about it, and carries a mip chain so the edge pass
                     can detect at a scale rather than at a pixel.
    2. **edge**      picture size. Sobel at a selectable scale on a selectable
                     channel. Outputs a raw gradient magnitude and nothing else.
    3. **stabilise** picture size, ping-ponged against itself. Asymmetric IIR
                     over time, then the threshold. Also writes the moments the
                     centroid is reduced from.
    4. **light**     picture size. The strip coordinate, the bulb, the effect,
                     the palette. Everything this plugin is about is here.
    5. **blur**      quarter size, run twice with `Direction` swapped. The glow.
    6. **composite** output size. Background mode, tint, mix.

    `kEffectLibrary` is a transcription of `Effects.cpp`. Every line with a
    counterpart there carries a `//= mirrored` marker in both files.

    **It is a fragment, not a shader, and that is the point.** The light pass is
    assembled from it at compile time by `LightShaderSource()`, and the
    harness's probe is assembled from the same string by
    `EffectProbeShaderSource()`. So `tinseltest --effects` runs the exact text
    the plugin runs -- one copy of the GLSL, checked against one copy of the
    C++. A test that compiled its own transcription of the effects would agree
    with itself perfectly and prove nothing.
*/

#include <string>

namespace tinsel
{

extern const char* const kVertexShader;
extern const char* const kCopyShader;
extern const char* const kEdgeShader;
extern const char* const kStabiliseShader;
extern const char* const kBlurShader;
extern const char* const kCompositeShader;

/// The effects, as GLSL. Not a complete shader: no `#version` and no `main`.
extern const char* const kEffectLibrary;

/// The light pass, assembled around kEffectLibrary.
std::string LightShaderSource();

/// One pixel per lamp, writing `evaluate()`'s two outputs to red and green so
/// they can be read straight back and compared against `Effects.cpp`. Built
/// from the same kEffectLibrary the light pass uses.
///
/// This exists only for `tinseltest --effects`, and lives here rather than in
/// the harness so that there is one place where the library is concatenated
/// into something runnable.
std::string EffectProbeShaderSource();

} // namespace tinsel
