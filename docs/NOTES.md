# Notes

Working notes for this repo: status, decisions, and the traps that have actually bitten.
Migrated out of Claude Code's memory on 2026-08-24, so they are written in the first
person and dated by when each thing was learned — that date is usually the useful part.

Cross-cutting notes that are not specific to this repo live in
[fleet-notes](https://github.com/stoatworks-labs/fleet-notes).

*tinsel — FFGL effect lighting detected outlines as an LED string, 20 WLED-style patterns x 16 palettes. v0.1 is a scalar field, NOT contour tracing; that is v0.2*

**tinsel** — `~/Projects/tinsel`, FFGL 2.1 effect for Resolume. Edge-detects
outlines and lights them like a string of LEDs. **PUBLIC MIT at
`stoatworks-labs/tinsel`**, shipped 2026-08-03. **v0.1.0 released** (4 assets),
CI green on macOS universal + Windows x64 + NSIS installer, **website page +
guide LIVE**, **YouTube `-TGCxAFDMYw`** (50.8s), Instagram reel published. All
four homes audited in agreement.

**The one idea:** an effect never chooses a colour — it returns *where in the
palette* a lamp sits and *how bright* it is, and the palette does the rest. So
20 patterns x 16 palettes, not 36 controls. That also makes an effect a pure
function of (lamp, time), which is what lets the library exist twice — C++ in
`Effects.cpp`, GLSL in `kEffectLibrary` — and be checked against itself.

**The honest limit of v0.1, chosen deliberately by the user:** the strip
coordinate is a **scalar field** (spiral about the artwork's centroid),
quantised into lamps. It is **not arc length and does not trace the contour**.
A chase climbs the shape like a string wound round a tree; it does *not* follow
an `S` round its own curve, and separate parts of a shape are not wired in
series. Tracing is v0.2 and replaces `stripCoordinate()` only. The user picked
"GPU field now, tracing later" plus "anything, video first" — note those two
partly conflict, because frame-to-frame lamp *identity* matching is inherently
tracing work; v0.1 delivers a temporally stabilised edge *field* instead.

**Verified** (M4 Max, macOS 26.4): 527,100 GLSL-vs-C++ comparisons, 0
disagreements; all 31 params measurably live; universal binary exports
`plugMain`; Windows x64 green in CI; **0.55 ms/frame at 1080p, 2.36 at 4K**
(14% of a 60fps frame). **Never loaded into Resolume**, and never loaded into
Resolve.

**The OpenFX build ships for Linux** as of 2026-08-25 (AlmaLinux 8 container
for glibc 2.28, Resolve's Rocky 8.6 floor; proven by `dlopen` on Rocky 8 rather
than argued). Tinsel was one of four plugins the load test caught failing with
`undefined symbol: pthread_create` -- pthreads are still in libpthread at glibc
2.28, and the OFX target linked none. It compiled, linked, exported
`OfxGetPlugin` and passed a glibc-version check first.

Siblings and shared patterns: [asciify](https://github.com/stoatworks-labs/asciify/blob/main/docs/NOTES.md) (`asciify`) style repo layout,
[old cathode](https://github.com/stoatworks-labs/old-cathode/blob/main/docs/NOTES.md) (`old-cathode`), [porthole](https://github.com/stoatworks-labs/porthole/blob/main/docs/NOTES.md) (`porthole`), [ffgl sdk bugs](https://github.com/stoatworks-labs/fleet-notes/blob/main/notes/reference_ffgl_sdk_bugs.md).
New traps found here are in [ffgl sdk bugs](https://github.com/stoatworks-labs/fleet-notes/blob/main/notes/reference_ffgl_sdk_bugs.md).
