# demo/ — the browser demo

Live at **https://tinsel-demo.stoatworks-labs.com**, linked from the
[project page](https://stoatworks-labs.com/software/tinsel/) and from the
[video plugins page](https://stoatworks-labs.com/video-plugins/).

**This is not the plugin.** It is the GLSL from [`source/Shaders.cpp`](../source/Shaders.cpp),
copied across unedited and run in WebGL2 over clips generated in the page, with
the parameters the plugin's constructor declares. The page says so in a banner,
and lists what it does not reproduce at the foot.

All six passes are here in the order `ProcessOpenGL` runs them — copy, edge,
stabilise, light, glow ×4, composite — including the ping-ponged history buffer,
so Stability behaves as it does in the host rather than being flattened out.

## Editing it

- `plugin.js` — this plugin's parameters and its shaders. **When a shader in
  `source/Shaders.cpp` changes, change it here too.** The two copies exist
  because the demo cannot include a C++ file; nothing enforces that they agree.
  The light pass is assembled from the same three pieces `LightShaderSource()`
  concatenates, so the effect library sits in one string here as it does there.
- `vendor/` — the shared kit, vendored from `stoatworks-backend/resolume-demo/`.
  **Do not edit these.** Fix the master and re-run `./sync.sh`; `./sync.sh
  --check` reports drift.

`Controls.cpp`, `Palette.cpp` and `Presets.h` are ported into `plugin.js` rather
than re-derived, so the number beside each slider is the plugin's own conversion
and a preset is the plugin's own table.

## What the page cannot have

- **No host FFT.** The Audio group's spectrum is zeros, which is what Resolume
  sends with nothing routed, so Audio Level only dims.
- **No transport.** Beat and Bar run off a 120 BPM clock generated in the page —
  the tempo the plugin itself falls back to when a host reports none.
- **Frame rate matters here.** The stabilise pass feeds back through the previous
  frame, so a throttled background tab changes what Stability's few frames of
  persistence mean.

## Deploying

From the repo root:

```bash
cf-run npx wrangler deploy
```

There is no build to run first — but check `git status` before deploying,
because a parallel session sharing this checkout can have staged its own work
into `demo/`.

Verify **by content, never by status code**. A wrong page returns a cheerful
200; only the title and the banner tell you which page is live:

```bash
curl -s 'https://tinsel-demo.stoatworks-labs.com/?cb=1' | grep -o '<title>[^<]*'
```
