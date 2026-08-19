# Tinsel

> **AI-assisted project.** This codebase was created with [Claude](https://claude.com/claude-code)
> (Anthropic), directed and reviewed by a human author. The pattern library is
> verified numerically by an offline harness that drives the real plugin class in
> a headless GL context: it renders the shader at one pixel per lamp and compares
> the readback against an independent C++ implementation — **527,100 comparisons
> with zero disagreements** (see [Status](#status)). Both the macOS universal
> bundle and the Windows x64 DLL build in CI. It has **never been loaded into
> Resolume** — only compiled, rendered and measured offline. Check it in your own
> rig before trusting it in a show.

Finds the outlines in a clip and lights them like a string of LEDs, as an FFGL
effect for [Resolume](https://resolume.com) Arena and Avenue. Point it at a logo
and it becomes a neon sign, a theatre marquee, or a Christmas tree.

**Video:** [What it does, in 50 seconds](https://www.youtube.com/watch?v=-TGCxAFDMYw)

![Chasing lamps round the outlines in red and green](docs/hero.jpg)

<sub>The repo's test card with a chase running round the detected outlines.
Rendered by `tinseltest`, the offline harness.</sub>

## An effect never chooses a colour

That is the whole design, and everything else follows from it.

A pattern answers two questions about one lamp — **where in the palette** it
sits, and **how bright** it is. The palette turns the first of those into RGB.
It is how an LED controller is built, and it is why twenty patterns and sixteen
palettes are worth more than thirty-six of either: Comet on **Christmas** is a
red-and-green chaser, Comet on **Frost** is a shooting star, and neither is the
other wearing a hat.

It also means a pattern is a pure function of *(lamp, time)* — no state, no
history — which is what makes it testable. The library exists twice, once in
C++ and once in GLSL, and the harness proves they agree.

**Twenty patterns:** Solid, Gradient, Rainbow, Colour Loop, Chase, Theater
Chase, Running Lights, Comet, Meteor, Larson Scanner, Colour Wipe, Twinkle,
Sparkle, Glitter, Fire Flicker, Breathe, Strobe, Dissolve, Colorwaves, Fairy
Lights.

**Sixteen palettes:** Colour 1, Colour 1 > 2, Rainbow, Party, Christmas, Candy
Cane, Warm White, Frost, Fire, Ocean, Forest, Sunset, Cyberpunk, Gold, Magenta,
Mono.

![The sixteen palettes](docs/palettes.png)

<sub>The baked palette table. The top two rows are black because they are drawn
from the Colour 1 and Colour 2 controls rather than from the table.</sub>

The palettes are **authored here, not imported.** WLED is EUPL-1.2 and FastLED's
gradient palettes carry cpt-city's own terms; neither is MIT and this repo is.
Nothing was copied from either — the patterns are implemented from a description
of what each one does.

## Lamps or rope, from one control

![A rainbow running round a continuous neon rope](docs/neon.jpg)

Lamp Size is a radius **in pixels**. Wind it up past half the lamp spacing and
the lamps run into each other and the strip becomes a continuous rope — the
neon-sign look, from the same control rather than a second mode.

![Warm white lamps twinkling](docs/twinkle.jpg)

Wind it down and you get discrete bulbs with gaps between them.

## Finding the outline

![The edge mask on its own](docs/edges.jpg)

**Detect On** picks what "different" means between two pixels, and the choice
matters more than the operator does:

- **Luma** — ordinary brightness edges.
- **Alpha** — for a logo delivered with transparency, which already has a
  perfect outline in its alpha channel.
- **Chroma** — the boundary between two colours of *equal brightness*, which a
  luma edge detector is completely blind to and which brand artwork is full of.
- **Luma or Alpha** — both, for artwork that could arrive either way.

**Detail** runs the operator at a coarser scale, so it finds the shape of a logo
rather than the texture inside it. **Stability** is a temporal filter for
footage — deliberately asymmetric, rising almost instantly and falling slowly,
because what goes wrong on video is not that edges move but that they drop out
for a frame and the lamp blinks.

**Background → Edges** shows the mask on its own, which is how Sensitivity,
Detail and Thickness are actually set — judging a threshold through a layer of
lamps and glow is guesswork.

## What this release does not do

The strip coordinate is a **field**, not an arc length. The plugin computes a
scalar for every pixel — a spiral about the artwork's centroid by default —
quantises it into lamps, and runs the patterns on that. It does not trace the
contour.

So a chase **climbs the shape the way a string wound round a tree climbs it**,
which is the intended look and is faithful to what a real strip does. But it is
**not** a chase that follows the letterform of an `S` round its own curve, and
separate parts of a shape are not wired in series.

Contour tracing is planned for v0.2. The controls are already the ones it needs.

## Build

Needs CMake and the Resolume FFGL SDK, which is a submodule.

```bash
git clone --recursive https://github.com/stoatworks-labs/tinsel
cd tinsel
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
cmake --install build    # → ~/Documents/Resolume Arena/Extra Effects
```

macOS builds universal (arm64 + x86_64) by default. Add
`-DCMAKE_OSX_ARCHITECTURES=arm64` for a faster development build.

A bundle you build yourself is unsigned, which is fine locally — quarantine only
applies to files that arrive from a browser. The released macOS builds are
Developer ID-signed and notarised, so those need nothing either.

<!-- downloads:start -->

## Download

**[v1.0.1](https://github.com/stoatworks-labs/tinsel/releases/tag/v1.0.1)** — prebuilt for macOS and Windows. Pick your platform:

<details>
<summary><b>macOS</b> — Universal (Apple Silicon + Intel)</summary>

| Build | Download | Size |
| --- | --- | --- |
| Universal (Apple Silicon + Intel) · .dmg disk image | [`tinsel-1.0.1-macos-universal.dmg`](https://github.com/stoatworks-labs/tinsel/releases/download/v1.0.1/tinsel-1.0.1-macos-universal.dmg) | 491 KB |
| Universal (Apple Silicon + Intel) · .zip archive | [`tinsel-macos-universal.zip`](https://github.com/stoatworks-labs/tinsel/releases/latest/download/tinsel-macos-universal.zip) | 208 KB |
| Universal (Apple Silicon + Intel) · .zip archive (OpenFX — Resolve, Vegas, Nuke) | [`tinsel-ofx-macos-universal.zip`](https://github.com/stoatworks-labs/tinsel/releases/latest/download/tinsel-ofx-macos-universal.zip) | 278 KB |

</details>

<details>
<summary><b>Windows</b> — x64</summary>

| Build | Download | Size |
| --- | --- | --- |
| x64 · .exe installer | [`tinsel-1.0.1-windows-x86_64-setup.exe`](https://github.com/stoatworks-labs/tinsel/releases/download/v1.0.1/tinsel-1.0.1-windows-x86_64-setup.exe) | 229 KB |
| x64 · .zip archive | [`tinsel-windows-x86_64.zip`](https://github.com/stoatworks-labs/tinsel/releases/latest/download/tinsel-windows-x86_64.zip) | 125 KB |
| x64 · .zip archive (OpenFX — Resolve, Vegas, Nuke) | [`tinsel-ofx-windows-x86_64.zip`](https://github.com/stoatworks-labs/tinsel/releases/latest/download/tinsel-ofx-windows-x86_64.zip) | 79 KB |

</details>

All builds, checksums and release notes: [github.com/stoatworks-labs/tinsel/releases](https://github.com/stoatworks-labs/tinsel/releases).

macOS builds are signed and notarised and open normally. The Windows builds are unsigned, so SmartScreen warns once.

<!-- downloads:end -->

## OpenFX — Resolve, Vegas, Nuke, Natron

The same effect also builds as an OpenFX plugin, so it runs in DaVinci Resolve
(Edit and Color pages, and Fusion), Vegas Pro, Nuke and Natron. The patterns,
palettes and controls are the same code the FFGL build runs; the temporal
stability filter is reconstructed from a short window of previous frames, so
any frame renders identically however the host reaches it.

Grab the `tinsel-ofx-*` zip for your platform from the release and copy
`Tinsel.ofx.bundle` into the standard OpenFX folder, then restart the host:

```
macOS    /Library/OFX/Plugins/
Windows  C:\Program Files\Common Files\OFX\Plugins\
```


## Status

Verified by measurement on an M4 Max, macOS 26.4:

| Check | Result |
| --- | --- |
| GLSL patterns vs C++ patterns | 20 patterns × 7 times × 5 intensities × 3 spreads × 251 lamps — **527,100 comparisons, 0 disagreements** |
| No dead controls | all **31** parameters measurably change the picture |
| Palette table | 16 rows, correct order, correct endpoints |
| macOS binary | universal (`x86_64 arm64`), exports `plugMain` |
| Windows x64 | builds green in CI, plus the NSIS installer |
| Render cost | 0.55 ms/frame at 1080p, 2.36 ms at 4K — 14% of a 60fps frame |

Run it yourself with `tools/verify.sh`.

**Not yet done:** never loaded into Resolume, never built on Linux. See
[AGENTS.md](AGENTS.md) for the full list of what is assumed rather than
measured, and for the traps.

<!-- attributions:start -->
This project is built on other people's work — see [ATTRIBUTIONS.md](ATTRIBUTIONS.md).
<!-- attributions:end -->

## Licence

MIT — see [LICENSE](LICENSE).
