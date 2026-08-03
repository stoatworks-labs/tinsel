# tinsel

Edge-detected outlines lit as a string of LEDs, as an FFGL effect for Resolume
Arena/Avenue. C++/GLSL, CMake MODULE → universal `.bundle` (macOS) + Windows
`.dll`. Public MIT repo.

Read `AGENTS.md` before changing the effect library, the strip coordinate or the
temporal filter.

## Commands (CMake)
- Configure: `cmake -B build -DCMAKE_BUILD_TYPE=Release`
- Fast dev build: add `-DCMAKE_OSX_ARCHITECTURES=arm64`
- Universal (what ships): `cmake -B build-universal -DCMAKE_BUILD_TYPE=Release`
- Build: `cmake --build build`
- Install to Resolume: `cmake --install build`
- Render a frame offline: `./build/tinseltest --out /tmp/frame.png`
- List parameters: `./build/tinseltest --list`
- Look at the palettes: `./build/tinseltest --palettes /tmp/palettes.png`
- Just the test card: `./build/tinseltest --card /tmp/card.png`
- Set a control: `--set "Pattern=7" --set "Palette=4"` (repeatable, by display name)
- Put real footage through the real shaders (for the project video):
  `ffmpeg … -f rawvideo -pix_fmt rgba - | ./build/tinseltest --pipe --width W --height H [--script cues.txt] | ffmpeg …`

## Verify
- Everything: `tools/verify.sh`
- GLSL effects vs C++ effects: `./build/tinseltest --effects`
- No dead controls: `python3 tools/sweep.py`
- Render cost: `./build/tinseltest --bench` (0.55 ms/frame at 1080p, 2.36 at 4K)
- Universal + exports: `lipo -archs build-universal/Tinsel.bundle/Contents/MacOS/Tinsel`
  and `nm -gU … | grep _plugMain`

## Notes
- **An effect never chooses a colour.** It returns where in the palette a lamp
  sits and how bright it is; the palette does the rest. That split is why 20
  patterns × 16 palettes is worth more than 36 controls.
- The effect library exists **twice** — `Effects.cpp` and `kEffectLibrary` in
  `Shaders.cpp`. Every mirrored line is marked `//= mirrored`. Change one,
  change both, run `--effects`. It is not decorative: it has already caught a
  real GPU-only defect.
- The GLSL effects are a **fragment** (no `#version`, no `main`).
  `LightShaderSource()` and `EffectProbeShaderSource()` assemble the light pass
  and the test probe around the *same* string, so the test runs what the plugin
  runs.
- **`ScopedFBOBinding` does not restore the viewport.** Capture the host
  viewport at the top of `ProcessOpenGL` and restore it before the composite.
- **`layout` is a GLSL keyword**, as are `flat`, `active`, `filter`, `input`,
  `output`, `sample`, `common`. Shader errors surface only at runtime, in the
  diagnostics log, as "the effect does nothing".
- Never use a float `mod` for a lamp index — it returns the spacing instead of
  zero at exact multiples, on the GPU only. Unsigned integers on both sides.
- Randomness is an integer PCG hash, never `fract(sin(x)*…)`.
- Lamp size is in **pixels**, divided by the field's screen-space gradient. A
  lamp measured in strip units is a streak wherever the field runs slowly.
- The temporal filter is **asymmetric** — fast attack, slow release. Symmetric
  trades flicker for lag, which is worse.
- All host parameters are 0..1 and mapped in `Controls.cpp`. `SetParamInfo`
  clamps a standard default into 0..1 before `SetParamRange` can widen it.
- `FFGLScopedFBOBinding.h` is not in `FFGLSDK.h`; include it by hand.
- macOS build must be universal (arm64 + x86_64). Verify with `lipo`, never the
  build log.
- The harness drives `SetTime` on a synthetic 60fps clock. Without it no time
  passes offline and nothing is reproducible.
- Public repo. "Commit" = commit **and** push.

## Diagnostics

`source/Diag.{h,cpp}` — log file only, no crash handler (this runs inside
Resolume), no bundle command. It exists for the one failure that actually
happens: a shader that will not compile, which otherwise looks like "the effect
does nothing" with no message anywhere. With six passes it records which one,
and logs the GL vendor/renderer/version next to it.

    ~/Library/Logs/tinsel/tinsel.YYYY-MM-DD.log
