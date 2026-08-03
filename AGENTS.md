# tinsel — orientation for another LLM (or a newcomer)

**What it is:** an FFGL 2.1 effect for Resolume Arena/Avenue that finds the
outlines in a clip and lights them like a string of LEDs. C++17 + GLSL 4.10,
CMake, universal macOS `.bundle` and a Windows `.dll`. Public, MIT,
`github.com/stoatworks-labs/tinsel`.

`CLAUDE.md` is the command reference — build, install, verify. This file is the
*why*: read it before touching the effect library, the strip coordinate, or the
temporal filter.

---

## The one idea

**An effect never chooses a colour.** It answers two questions about one lamp —
*where in the palette* it sits, and *how bright* it is — and the palette turns
the first of those into RGB.

That is how an LED controller is built, and almost everything below falls out of
it rather than having been arranged.

- **Twenty effects and sixteen palettes are worth more than thirty-six of
  either.** Comet on Christmas is a red-and-green chaser; Comet on Frost is a
  shooting star. Neither is the other wearing a hat.
- **An effect is a pure function of (lamp, time).** No state, no buffer, no
  history. That is what lets the whole library exist twice — once in
  `Effects.cpp` for the harness to measure, once in GLSL for the GPU to run —
  and lets `tinseltest --effects` prove the two agree.
- **Adding a palette is data.** `Palette.cpp` is a list of gradient stops.
  Adding an effect is one function and one `if` in the shader.

The palettes are **authored here, not imported**. WLED is EUPL-1.2 and FastLED's
gradient palettes carry cpt-city's terms; neither is MIT and this repo is.
Nothing was copied from either. The effects are implemented from a description
of what the pattern does, which is why the names are generic and the constants
are ours.

### What falls out, and what does not

The thing that does *not* fall out — and the honest limit of this release — is
**arc length**.

A real LED string has an ordering: lamp 37 is next to lamp 38, wherever the wire
happens to run. Every interesting effect depends on that. Here the ordering
comes from a **scalar field** — a spiral about the artwork's centroid, by
default — sampled per pixel and quantised. It is not the outline's arc length.

So:

- **A chase climbs the shape the way a string wound round a tree climbs it.**
  That is the intended look and it is faithful to what a real strip does.
- **A chase does not follow the letterform of an `S` round its own curve.**
  Nothing here knows the outline is a curve.
- **Separate parts of a shape are not wired in series.** The dot of an `i` and
  its stem share a coordinate because they are at similar angles from the
  centroid, not because a wire runs between them.

Contour tracing is v0.2, and the parameters are already the ones it needs: it
replaces `stripCoordinate()` and nothing else moves.

---

## The traps

Ordered by how much time they will cost you.

**`ScopedFBOBinding` does not restore the viewport.** It restores the
framebuffer binding and only that (SDK `b1afaf9`, `FFGLScopedFBOBinding.cpp`).
So every pass's `ResizeViewPort()` leaks into the pass after it, and the
composite — which draws to the host's framebuffer and so has no buffer of its
own to size itself from — inherits whatever the last pass left. Here that was
the quarter-size glow buffer. `ProcessOpenGL` captures the host viewport up
front and restores it before the composite for that reason.

It is worth knowing what this looks like, because it does not look like a
viewport bug: the effect renders correctly into the bottom-left quarter of the
frame and leaves the rest untouched — and in any viewer that shows transparency
as white, that reads as *the effect having blown out to solid white* with a
small correct picture in the corner. An hour went into the edge detector before
the actual cause was found.

**`layout` is a GLSL keyword.** So are `flat`, `active`, `filter`, `input`,
`output`, `sample` and `common`. Using one as a variable fails with nothing but
`syntax error` and a line number — and because the light pass is assembled from
three strings at runtime, that line number is in a file which does not exist.
The uniform is `Layout` with a capital L, and the local is `wiring`.

**A float `mod` is not safe for a lamp index.** Theater Chase was written the
obvious way, `mod( float( bulb ) - slot, spacing )`. GLSL defines `mod` as
`x - y * floor( x / y )`; where the subtraction lands on an exact multiple of
the spacing, the division can round a hair below the integer, `floor` takes it
down a whole step, and the result comes back as `spacing` rather than zero — so
the lamp that should be the brightest in the pattern is the one that goes out.
It does not reproduce on the CPU. `tinseltest --effects` found it as 60
disagreements out of 527,100, every one of them a full 0-against-1, all in that
one effect. The fix is unsigned integer arithmetic on both sides.

**Randomness must be integer.** `fract( sin( x ) * 43758.5453 )` depends on the
driver's `sin`, so two GPUs disagree about which lamps are lit and the C++
mirror cannot agree with either. `HashInt` is a PCG output mix, exact in 32 bits,
identical on both sides.

**`max( luma * a, a )` is identically 1.0 for an opaque pixel.** That was the
first implementation of the `Luma or Alpha` detect mode, and since the mode is
the *default*, the effect found no edges at all on any clip without an alpha
channel and did nothing out of the box. It surfaced as 27 of 31 controls reading
as dead in `tools/sweep.py` — which is the only reason it was found at all. One
dead control looks like a typo; twenty-seven looks like a pipeline, and points
at the one stage they all pass through.

**A lamp measured in strip units is a streak.** The strip coordinate is a field,
and its gradient varies enormously across a frame — a spiral about the centroid
turns fast near the middle and slowly at the edges. A lamp window that is a
fixed fraction of its own spacing is therefore a dot in one place and a long
smear in another, and `Lamp Size` means two different things in two parts of the
same picture. The light pass divides by the field's screen-space gradient so
that a lamp is a fixed size *in pixels*. This is the single biggest difference
between "glowing streaks" and "lamps".

Related, and the reason the default `Turns` is 0.64 and not 2.4: **`Turns`
decides the angle at which the strip's level sets cross the outline**, and that
angle decides whether a lamp is a dot or a smear. A lamp is a band across the
field; where the band lies *along* the outline instead of across it, the lamp is
drawn out. A tight spiral meets a round logo at a shallow angle nearly
everywhere.

**The temporal filter is asymmetric, and must stay that way.** A symmetric IIR
is a low-pass, and a low-pass on an edge signal trades flicker for lag: the
outline of anything moving arrives late and smeared behind it. What actually
goes wrong on footage is not that edges move, it is that they *drop out* — a
boundary grades through the threshold for one frame and the lamp blinks. So
`Attack` stays near 1.0 across the whole Stability range and only `Release`
slows down.

**`ffglex::FFGLFBO::Release()` leaks the colour texture.** It deletes the
framebuffer and the depth renderbuffer, then tests `depthBufferID` a second time
where it plainly meant `colorTextureID`. `PassBuffer::Destroy()` deletes it
first. It matters here rather than being pedantry: the glow buffers reallocate
whenever `Glow Size` changes, and an operator drags that.

**Every `ffglex::Scoped*` binding clears to 0 on scope exit — it does not
restore.** `FFGLFBO::Initialise` sizes its new colour texture under one of
those, so *allocating a buffer silently unbinds your input texture from the
active unit*. The symptom is the dangerous part: correct on every frame except
the one that allocates. Every `Ensure()` in `ProcessOpenGL` therefore happens
before anything binds a texture.

**A ranged parameter cannot have a ranged default.** `SetParamInfo` clamps an
`FF_TYPE_STANDARD` default into 0..1 *before* returning, and `SetParamRange` can
only be called afterwards. So every host parameter here is 0..1 and the
conversions live in `Controls.cpp`.

**`FFGLScopedFBOBinding.h` is not in the umbrella header.** `FFGLSDK.h` includes
every other scoped binding and omits that one. Include it by hand; the symptom
is an unknown-type error on `ScopedFBOBinding` and nothing else.

**The plugin registers itself from a file-scope constructor.** `CFFGLPluginInfo`
is never referenced by name, so in a **STATIC** archive the linker may drop the
whole translation unit, giving a bundle that loads, exports `plugMain`, and
reports that it contains no plugins. The core is an **OBJECT** library for that
reason. Verify with `nm -gU … | grep _plugMain` plus an actual host load.

**The harness needs a synthetic clock.** Left to the wall clock it renders a
hundred frames in a few milliseconds, so no time passes, every animation stays
on frame zero, and `Speed` measurably does nothing — which `sweep.py` duly
reported. Worse, what little time did pass was whatever the machine took, so no
two runs produced the same picture and nothing could be compared with anything.
`tinseltest` drives `SetTime` at a synthetic 60fps.

---

## Shape of the code

    source/Effects.{h,cpp}  the twenty patterns. Mirrored in GLSL. The plugin.
    source/Palette.{h,cpp}  sixteen palettes as gradient stops; bakes the table.
    source/Controls.*       0..1 host parameters to physical units.
    source/Shaders.cpp      the six passes. kEffectLibrary is shared with the
                            harness — see below.
    source/PassBuffer.*     FFGLFBO with the leak fixed and three sampling modes.
    source/Tinsel.*         the plugin: parameters, buffers, the passes.
    source/Diag.*           a log file, for the shader that will not compile.
    tools/tinseltest/       the offline harness.
    tools/sweep.py          no control is silently dead.
    tools/verify.sh         all of it.

Six passes:

1. **copy** — picture size, mipmapped. Resolves `MaxUV` once so no later pass
   has to think about it. The mip chain lets the edge pass detect at a *scale*
   rather than at a pixel, which is what `Detail` moves.
2. **edge** — picture size. Sobel at the selected scale on the selected channel.
   Outputs a raw gradient magnitude and nothing else.
3. **stabilise** — picture size, ping-ponged against its own previous output.
   The asymmetric filter, then the threshold. Also writes the first moments of
   the mask, so the centroid is a single `textureLod` off the top of its mip
   chain rather than a reduction anybody had to write.
4. **light** — picture size. Strip coordinate, lamp, effect, palette.
5. **blur** — quarter size, run four times: two axes, twice, the second pair
   wider. Summing two Gaussians of different widths is what gives a lamp a tight
   core and a wide falloff instead of one soft blob.
6. **composite** — output size. Background mode, tint, mix.

### The GLSL effects are a fragment, not a shader

`kEffectLibrary` has no `#version` and no `main`. `LightShaderSource()` assembles
the light pass around it and `EffectProbeShaderSource()` assembles the harness's
probe around the same string, so `--effects` runs *the text the plugin runs*. A
test that compiled its own transcription of the effects would agree with itself
perfectly and prove nothing.

---

## What is genuinely verified, and what is assumed

**Verified, by measurement, on this machine (M4 Max, macOS 26.4):**

- **The GPU computes the effects the C++ predicts.** 20 effects × 7 times × 5
  intensities × 3 spreads × 251 lamps = **527,100 comparisons, zero
  disagreements** past 5e-4. The largest honest difference is 3.45e-5, in Chase
  at t=41, which is a `smoothstep` whose argument has lost a few bits to the
  size of the time value. The tolerance exists because the GLSL spec allows 3
  ulp for `exp` and gives `sin` no accuracy requirement at all — it is not loose
  enough to hide a wrong branch, and the one real defect it has caught showed up
  as a difference of exactly 1.0.
- **No dead controls.** All **31** parameters measurably change the picture,
  including the conditional ones, each with the context that makes it mean
  anything (`tools/sweep.py`). `Stability` needs `--noise`, because on a still
  picture it provably does nothing: history and current hold the same number and
  every blend between them returns it.
- **The palette table bakes correctly and in order.** Sixteen rows, the two
  colour-driven palettes correctly left unbaked, Christmas red→cream→green, Mono
  pure white end to end.
- **The build is universal and exports `plugMain`** — `lipo` reports
  `x86_64 arm64`, `nm -gU` finds `_plugMain`.
- **The edge detector does what it claims per mode.** On the test card, the seam
  between two fields of equal luminance is invisible to `Luma` and found by
  `Chroma`, which is the entire reason `Detect On` exists.
- **The render cost**, by `tinseltest --bench` (120 frames each, after a
  20-frame warm-up, `glFinish` on both sides — without which this times how fast
  the driver accepts commands rather than how fast the GPU runs them):

  | | ms/frame | % of a 60fps frame |
  | --- | --- | --- |
  | 1280×720 | 0.348 | 2.1% |
  | 1920×1080 | 0.552 | 3.3% |
  | 2560×1440 | 0.995 | 6.0% |
  | 3840×2160 | 2.358 | 14.1% |

  So it is comfortably real-time but it is not free: at 4K it takes a seventh of
  the frame, and an operator stacking four effects on a layer will feel that.
  The cost is **per frame whether or not anything moved** — the stabilise pass
  feeds back into itself, so there is no still-image fast path and adding one
  would mean detecting that the input has not changed, which costs a comparison
  over the whole frame.
- **Windows x64 compiles**, proven by a `workflow_dispatch` run that built the
  macOS universal bundle, the Windows `.dll` and the NSIS installer green before
  anything was tagged. Dispatching that workflow is the cheap way to test an
  FFGL build without publishing: the release job is gated on `refs/tags/v*`, so
  a manual run builds and skips publication. The macOS job also asserts `lipo`
  reports both architectures and `nm` finds `_plugMain`, because neither failure
  is visible in a build log.

**Assumed, or not yet done:**

- **Never loaded into Resolume.** Everything here was compiled, rendered and
  measured offline against the real plugin class in a headless GL context.
  Nothing has driven the host. How the parameters *present* — whether the
  Colour 1/2 triples show as swatches, whether five groups read sensibly in the
  inspector, whether 31 controls is too many in practice — is untested.
- **Never built on Linux.** There is no CI job for it and nothing has tried. The
  code uses nothing platform-specific outside `Diag.cpp`, but that is an
  argument rather than a build.
- ~~Nothing timed.~~ **Measured** — see below.
- **The `Stability` range is judged by eye.** The mapping tops out at a time
  constant of about fifty frames because that is roughly where a lamp hanging on
  after its edge has gone starts reading as a ghost. Where exactly that crossover
  sits has not been measured on real footage.
- **No contour tracing.** See "The one idea" above — this is the honest limit of
  v0.1, not an oversight.

---

## Siblings

The CMake MODULE + FFGL-submodule pattern, the `Diag` logger, `PassBuffer`, the
offline-harness shape and `sweep.py` all come from **porthole**, **old-cathode**
and **asciify**, which are the other FFGL effects in the fleet. The `--pipe`
frame format and the `--script` cue file are identical across all of them on
purpose, so one build script can film any of them.
