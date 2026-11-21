# Performance on older webOS TVs

How to find out why a Flutter app is slow on an older TV, and what helped.
Numbers are from an LG C1 (webOS 6.5.3, 1080p UI, Mali GPU) running a real
app, a Jellyfin client, with a scripted remote-key scenario (rows down, along,
back up), and from `tests/compat_test`.

## The short version

1. **Leave the legacy defaults on.** The legacy package turns on Impeller and
   two window surfaces unless the app says otherwise (see "Renderer"). They
   halve GPU time per frame on webOS 6.
2. **Measure before changing code.** Build with `legacy_perf_probe` (below);
   it tells you whether frames are slow on the UI thread (*build*) or on the
   GPU (*raster*). On these TVs it is almost always raster.
3. **Make frames cheaper to draw**: no blurred shadows on many widgets, no
   `BackdropFilter`, few full-screen translucent layers, images sized to how
   they are shown.
4. **Check your caches work.** A cache that fails silently costs every start.

## Measuring

### legacy_perf_probe (in the app)

`packages/legacy_perf_probe` logs to `compat.log` in the app's folder on the
TV, only in builds made with `--dart-define=LEGACY_PERF_PROBE=true`:

- every frame whose build + raster time is over 20 ms, with both times;
- every 120 frames: average, 90th percentile and maximum of each;
- each remote key: time from the key to the end of the first frame drawn after it;
- each image load (one line in your `ImageProvider`): where the bytes came
  from, size, fetch and decode time.

```dart
void main() {
  WidgetsFlutterBinding.ensureInitialized();
  PerfProbe.start();
  runApp(const MyApp());
}
```

`tools/perf-summary.py compat.log` turns a log into one line:

```text
first frame 740 ms; build 2.3 ms, raster 8.8 ms (p90 11.4, max 197) over 1680 frames;
slow frames 43 (2.6%), over 50 ms 1; key to frame median 23 ms, p90 27, max 33 (31 keys);
images disk 49; decode median 31 ms
```

How to read it:

- **build** is your Dart code on the UI thread: `build`, layout, paint. If
  this is large, look at rebuilds (a `setState` high in the tree on every
  focus change), big synchronous work, or long lists built eagerly.
- **raster** is the GPU thread drawing the frame. At 60 frames per second a
  frame has 16.7 ms in total; on webOS 6 raster is where it goes.
- **key to frame** over ~200 ms usually means the key changed nothing (Down
  on the last row), so no frame followed; the summary leaves those out.

`print` does not reach `compat.log` on LG's engine (it goes to the system
log); the probe writes to stderr, which the legacy package redirects there.

### Shim switches (no app change)

Empty files in the app's folder on the TV (or in `webos/meta/` before
packaging):

| File | Output in `compat.log` |
| --- | --- |
| `compat-log-fps` | Every 120 animating frames: frame rate and intervals between presents (avg/p90/max), frames over 20 and 33 ms. Times presents only, so it does not slow the app. Gaps over 250 ms count as idle |
| `compat-log-swap` | GPU time per frame, time blocked in the swap, waits by thread. Adds a `glFinish` per frame: use it to find where time goes, not to measure frame rate |
| `compat-log-pmlog-all` | The engine's own messages, e.g. which renderer started (`Enable Impeller: true`) and the engine switches in use |

The shim always logs `first frame N ms after process start`.

### Driving the app

`tools/tv-key/tv-key` (built by `tools/build-shims.sh`) injects remote keys
on the TV, so a scenario can be repeated exactly:

```sh
scp tools/tv-key/tv-key root@$TV_HOST:/tmp/
ssh root@$TV_HOST '/tmp/tv-key -w 200 down down right right right left left up up'
```

Compare variants by changing one thing at a time and running the same
scenario after a fresh start.

## Renderer

LG's build renders with Skia unless `flutter-conf.json` enables Impeller.
The legacy package (`--legacy`) now applies these defaults when the app sets
nothing:

| Setting | Legacy default | Turn off |
| --- | --- | --- |
| Impeller | on | `"engine-switches": { "enable-impeller": false }` in `webos/meta/flutter-conf.json` |
| Two window surfaces (`compat-dual-surface`) | on | empty file `webos/meta/compat-single-surface` |

Two surfaces remove a ~10 ms wait per frame in the Mali driver. The shim
goes back to one surface by itself while media is loaded or an external
texture is registered, so video and `webos_image_texture` keep working.

Jellyfin client, Home screen scenario. Present rate from `compat-log-fps`
(frames while something moves; 60 is the maximum):

| Renderer | Frames per second | Intervals over 20 ms |
| --- | --- | --- |
| Skia | 52.3 | 13.3% |
| Impeller, one surface | 48.9 | 26.7% |
| Skia, two surfaces | 54.1 | 11.4% |
| Impeller, two surfaces | 55.8 | 12.6% |

GPU time per frame from `legacy_perf_probe` (build time was ~2 ms in all):

| Renderer | Raster avg / p90 | Frames over 20 ms |
| --- | --- | --- |
| Skia | 17.5 / 22.1 ms | 22% |
| Impeller, two surfaces | 10.0 / 13.3 ms | 5.4% |
| Impeller, two surfaces, no blurred card shadow | 8.8 / 11.4 ms | 2.6% |

Movies grid (21 posters on screen): Skia 34.4 ms raster (about 29 frames per
second), Impeller with two surfaces and no blur 12.4 ms.

Impeller on its own is not faster here: frames land just over the 16.7 ms
budget, and the driver wait that two surfaces remove decides it.

### Shaders compiled on first use

With Impeller, LG's runner also passes `--impeller-lazy-shader-mode`:
shaders are compiled the first time they are needed, so the first scroll
after a start has one slow frame (191–264 ms in the Jellyfin client). The
build tool now lets an app turn it off:

```json
{ "engine-switches": { "enable-impeller": true, "impeller-lazy-shader-mode": false } }
```

That trades the one slow frame for a much slower start: first frame 3.9 s
instead of 0.8 s, worst first-scroll frame 62 ms instead of 264 ms. Keep the
default unless the first scroll matters more than the start.

### Damage regions (no gain)

`compat-egl-damage` advertises the Mali driver's partial-update extensions so
the embedder redraws only changed areas. It made no measurable difference
with either renderer in the Jellyfin client or the test scenes; it stays an
experiment.

## What made frames expensive

- **Blurred shadows.** A `BoxShadow` with `blurRadius` on every card (18 px,
  35% black) was the largest single cost after the renderer. On a dark
  background it was nearly invisible; removing it cut raster time by a
  further ~12% and slow frames by half. Focus rings drawn with
  `spreadRadius` and no blur are cheap.
- **Full-screen translucent layers.** Each gradient or `Opacity` over the
  whole screen is another full-screen pass on a GPU with little fill rate.
  Prefer one gradient to several, and opaque backgrounds where possible.
- **Images larger than shown.** Ask the server for the displayed size and use
  `ResizeImage` / `cacheWidth`, so decoding and uploading stay small. A
  1360-px backdrop took ~300 ms to decode (off the UI thread, but its upload
  shows up in raster).
- **`saveLayer` effects**: `BackdropFilter`, `ShaderMask`, `Opacity` on large
  subtrees, `ClipRRect` with antialiasing over many children. Use them on
  small areas only.

## What was fine

- **Build time**: ~2 ms per frame with a few hundred widgets on screen.
- **Startup**: first frame 0.7–0.8 s after the process starts, in every renderer mode.
- **Input**: 20–30 ms from a remote key to the drawn frame.

## Caches

`path_provider_webos` before 1.0.1 (plugins fork) threw on
`getApplicationCacheDirectory()`. Apps that catch the error and carry on
lose their disk cache without noticing; the Jellyfin client downloaded every
poster again on each start. Use the fork's current `path_provider`, or fall
back to `getApplicationSupportDirectory()`. Check with the probe's image
lines: `disk` on the second start, not `net`.
