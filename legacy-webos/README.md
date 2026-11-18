# Flutter on older webOS TVs (legacy-webos)

flutter-webos and its prebuilt engine target **webOS 26** only. This folder
repackages an ordinary `flutter-webos build webos` output so the same app runs
on **older webOS TV releases**, without rebuilding LG's engine or embedder.

Verified on an LG OLED65C1 running **webOS 6.5.3** (kernel 4.4, glibc 2.28,
Wayland 1.11, Mali-G51), launched normally through the application manager in
Developer Mode:

| Area | Result on webOS 6.5.3 |
| --- | --- |
| Rendering, Material UI | Works, 1920x1080 |
| Remote D-pad / OK | Works |
| Remote Back | Works: pops the current route; on the last route the app exits |
| DNS, HTTP, HTTPS (certificate verification) | Works |
| path_provider, shared_preferences, sqflite | Works; data survives reinstall |
| video_player (network MP4) | Works: play, pause, seek, 2x, resized window, two players, Home/resume |
| audioplayers (network MP3) | Works |
| webos_service_bridge (Luna calls + subscriptions), webos_app_manager, webos_display | Works |
| device_info_plus (`webosInfo`), package_info_plus, connectivity_plus | Works |
| url_launcher (opens the TV browser), webos_image_texture | Works |
| gamepads, flutter_keyboard_visibility, custom_mouse_cursor | Work (API calls; no gamepad attached) |
| flutter_secure_storage | Works via app-sandbox AES-GCM fallback (no system keystore on webOS 6; see Known issues) |
| video_player_drm | Clear HLS/DASH and PlayReady (licence URL) play; Widevine plays with the opt-in CDM proxy (software security level) |
| Not tested | audioplayers_soloud, flutterfire |
| Performance | 50-60 fps for lists and light UI with Impeller; heavy shadow/opacity grids are slow |

## How it works

`tools/make-compat-ipk.sh` takes the bundle flutter-webos built and fixes, in
order, everything that stopped it from starting on webOS 6:

| Problem on older webOS | Fix |
| --- | --- |
| LG's binaries need glibc 2.38; the NDK's own glibc needs kernel >= 5.15 (`statx`) | Bundle Debian armel glibc 2.41 (built for kernel >= 3.2) in `lib/compat/` |
| LG's binaries need `GLIBCXX_3.4.32` | Bundle the NDK's own libstdc++/libgcc_s/libatomic (GCC 13, ARMv7); Debian's armel libstdc++ targets ARMv5TE and lacks parts of `std::future` |
| The app must be the `main` executable, or webOS denies its Luna bus permissions | No wrapper script: patch the binary's ELF interpreter to the bundled loader |
| The devmode jailer sets `LD_LIBRARY_PATH`, which overrides `RUNPATH` | Use `DT_RPATH` |
| System libraries would pull in the TV's old libpthread/libdl/librt | Bundled ones added as direct `NEEDED` entries so they load first |
| Firmware preloads `/lib/libSegFault.so` (old glibc) into every process | Bundled loader patched to skip `/etc/ld.so.preload` |
| `wl_proxy_marshal_flags` missing (Wayland < 1.20) | `compat/wayland-shim` implements it on the 1.11 API |
| `wl_webos_shell` v2 requested, compositor offers v1 | Shim records advertised versions and clamps binds |
| Back key never delivered to the app | Shim sets `_WEBOS_ACCESS_POLICY_KEYS_BACK` on the shell surface |
| The embedder turns the remote's Back (evdev 412) into an Escape key, which Flutter does not treat as navigation | Shim sends `popRoute` on `flutter/navigation` instead, like Android, using the engine handle captured from `FlutterEngineGetProcAddresses` |
| Some firmware has only the vendor `libwayland-egl.so.0`, other only `.so.1` | Bundled `libwayland-egl.so.1` (`compat/wayland-egl-forward`) forwards to whichever the TV has |
| The Mali driver gives each window two buffers and the compositor returns the shown one only once the next is on screen, so `eglMakeCurrent` blocks ~10 ms per frame, leaving ~7 ms to render | Opt-in (`compat-dual-surface`): the shim gives the window a second EGL surface on the same Wayland surface and alternates frames (four buffers), converting Mali's per-surface buffer age for the embedder's partial redraw, and uses the window's own surface while media is loaded. See "Renderer and frame rate" for the known issue |
| NDL has no `NDL_MediaInitWithWindowId`; old NDL takes its window from SDL, even for audio | `compat/ndl-sdl-shim`: a stand-in `libSDL2-2.0.so.0` that hands NDL Flutter's exported window, or a placeholder for audio-only players (added when any bundled library links `libNDL_media`) |
| `drm.licenseUrl` in the load request (webOS 26) is ignored; old releases use a DRM client (`com.webos.service.drm`) | NDL shim translates it: `NDL_MediaDRMLoad`, `NDL_MediaDRMSendMessage` with a `PlayReadyInitiator` (`LA_URL`), then loads with `drm.clientId`; unloads the client at exit |
| `NDL_MediaSetDrmOperation` missing (video_player_drm, `-z now`) | NDL shim exports it: licence responses for the opt-in Widevine proxy, failure otherwise |
| libwebp < 1.1 lacks `WebPMalloc` (webos_image_texture, linked `-z now`, so the app would not start) | Wayland shim also exports `WebPMalloc` |

The video_player fixes that are not webOS-6 specific (loop position,
position after Home, volume error at startup) are in
[Webos-Flutter-Legacy-TV/plugins](https://github.com/Webos-Flutter-Legacy-TV/plugins).

## Setup

Linux x86_64 host, with `curl`, `xz`, `dpkg-deb`, `python3`, `npm`, `cmake`.

```sh
cd legacy-webos
WEBOS_NDK_ACCEPT_EULA=1 ./setup.sh   # fetches toolchains/ and downloads/ (git-ignored)
. ./env.sh                           # flutter-webos, ares, patchelf on PATH
```

`setup.sh` downloads the webosbrew/buildroot-nc4 toolchain (builds the shims
against glibc 2.12, so they load on every release), the Debian armel runtime
(checksums verified against the Debian archive), patchelf, the webOS CLI and,
if you accept its licence, the LG webOS NDK. Nothing is installed system-wide.

## Build and run an app on an older TV

```sh
cd my_app && flutter-webos build webos --release --legacy
#   -> build/webos/arm/release/...ipk                         (webOS 26)
#   -> build/webos-compat/release/<app-id>_<version>_arm.ipk  (older webOS)
```

`--legacy` runs `tools/make-compat-ipk.sh` after the normal build; the script
can also be run by hand on an existing build (`make-compat-ipk.sh . release`).
[docs/PORTING-GUIDE.md](docs/PORTING-GUIDE.md) covers plugins, configuration
and testing for an existing app.

Install it like any IPK (`ares-install`, or `tools/tv-install.sh` on a rooted TV).

### Renderer and frame rate

LG's build renders with Skia unless `webos/meta/flutter-conf.json` enables
Impeller:

```json
{ "engine-switches": { "enable-impeller": true } }
```

Measured on webOS 6.5.3 at 1080p (`tests/compat_test`, frames per second):

| Scene | Skia | Impeller | Impeller + `compat-dual-surface` |
| --- | --- | --- | --- |
| Four large animated tiles with opacity and shadows | 17 | 52 | 54 |
| Constant-speed list scroll | 58 | 61 | 61 |
| 64 animated tiles | 12 | 19 | 24 |

Raster time per frame with Impeller drops from ~17 ms to ~7 ms with
`compat-dual-surface`, leaving headroom for heavier frames.

- **Apps without `video_player`**: enable Impeller. All plugin, DRM-licence
  and input tests pass with it.
- **Apps with `video_player`**: stay on Skia. With Impeller the video stayed
  hidden behind the UI in the full-screen stage of `tests/video_test` in 3 of
  3 runs (fine with Skia in 4 of 4); the plugin's hole draws, but in some
  layouts it does not let the video show through.
- **`compat-dual-surface`** is opt-in: with Impeller, a change drawn just
  before the screen goes static can stay invisible (a `webos_image_texture`
  image did not appear in 5 of 5 runs). It does not happen with a single
  surface or when frames are slower, so it looks like a race exposed by the
  faster frames; not yet found.

## Widevine on older webOS (opt-in)

Add an empty `webos/meta/compat-widevine-cdm` to the app. When the app loads a
Widevine DASH stream (with `licenseServerUrl` or `licenseCallback`), the NDL
shim hands it to `libwv-compat.so` (`compat/widevine/`), which:

1. loads the TV's own Widevine CE CDM (`/usr/lib/libwidevine_ce_cdm_shared.so`),
   provisions the device once (certificate kept in the app's data folder) and
   gets the licence: from `licenseServerUrl`, or, with `licenseCallback`, by
   sending the challenge to the app as the `drmEncrypted` event webOS 26 NDL
   sends and taking the licence back from `NDL_MediaSetDrmOperation`;
2. runs a proxy on `127.0.0.1` that serves the manifest without
   `ContentProtection` and decrypts each requested byte range of the media
   files in place (CENC `cenc`/`cbcs`; boxes are renamed, never resized, so
   byte ranges stay valid);
3. gives the media pipeline the local URL, so it plays a clear stream.

Verified on webOS 6.5.3 with Google's Widevine test stream: licence by URL and
by `licenseCallback`, seeking (resumes after ~4 s, as with clear streams) and
adaptive switching from 240p to 720p (8 Mbit/s) with no decrypt failures.

Limits: software security level (L3), so services that require hardware
security cap quality or refuse; Widevine DASH only (encrypted HLS is not
handled). Using the TV's provisioned CDM this way is outside the terms LG and
Google ship it under, and the device certificate could be revoked. Use at your
own risk. `compat/widevine/out/wv-selftest` (built by `build-shims.sh`) checks
the CDM, provisioning and a licence round trip from inside an app's jail.

## Other webOS releases (paper check)

`tools/fw-compat-check.py` checks, against webosbrew's firmware symbol
inventory (one image per release), whether every system library and function
LG's engine, embedder and plugins use exists, after legacy-webos's bundling
and shims. It cannot see kernel, Wayland protocol or GPU driver versions, so
real TVs are still needed. Result (inventory 0.10.0, Oct 2026):

| webOS | Missing on that firmware | Widevine CDM | Outlook |
| --- | --- | --- | --- |
| 26 (11.2) | nothing | 17.1 | LG's own target |
| 22-25 (7.4-10.2) | nothing | 16.4 / 17.1 | should work as is (CDM 17 untested) |
| 6 (6.4) | nothing | 16.3 | verified on 6.5.3 |
| 5 (5.3.1) | `NDL_MediaSetPlayer` (shimmed: single player) | 15.1 | likely works; Widevine proxy needs the v15 CDM interface |
| 4 (4.4.2, 4.10) | `libsystemd.so.0` (embedder event loop), `libcurl.so.4` (image texture), `NDL_MediaMuteDisplay` | -, 14.0 | needs bundled libsystemd/libcurl and an NDL shim |
| 3 (3.4, 3.9) | as webOS 4, plus older Luna C++, i18n and pbnjson APIs, no libudev/libwebp | - | much more work |
| 1-2 | as webOS 3, plus no Luna C++ library | - | highest risk |

```sh
tools/fw-compat-check.py --db <fw-symbols data dir> build/webos/arm/release/bundle/lib/*.so
```

## Tools

The TV tools use SSH as root (rooted TV). Set `TV_HOST`, and `TV_SSH_KEY` if
the key is not `~/.ssh/lg_webos`.

| Tool | Purpose |
| --- | --- |
| `tools/make-compat-ipk.sh <app> [mode]` | Repackage a flutter-webos build for older webOS |
| `tools/build-shims.sh` | Build the shims and `tv-key` (called by the packager) |
| `tools/dev-cycle.sh <app> [secs] [--luna]` | Package, install, run, print `compat.log` |
| `tools/webos-survey.sh` | Read-only survey of a TV's kernel, glibc, graphics and webOS libraries |
| `tools/tv-install.sh`, `tools/tv-shot.sh` | Install an IPK; take a screenshot |
| `tools/tv-key/tv-key.c` | Inject remote key presses into evdev |
| `tools/perf-variant.sh` | Run `tests/compat_test` with a resolution / Impeller setting |
| `tools/video-scenario.sh`, `tools/video-probe.sh` | Scripted video_player test; probe one URL |

Debug switches: an empty file in an app's `webos/meta/` turns on extra output
in `compat.log` (next to the installed binary):
`compat-log-globals` (Wayland globals), `compat-log-keys` (raw Wayland key
codes), `compat-back-as-key` (deliver Back as the embedder's key event instead
of `popRoute`), `compat-log-ndl` (raw NDL media events),
`compat-log-pmlog` (plugin PmLog messages), `compat-log-swap` (per 120
frames: GPU time, time blocked in the swap and between presents, waits over
2 ms by thread and caller, buffer releases; adds a `glFinish` per frame),
`compat-dual-surface` (alternate two EGL surfaces, see "Renderer and frame rate"),
`compat-swap0` (EGL swap interval 0; no effect on webOS 6.5.3, whose Mali
configs only allow interval 0). In `tests/compat_test`, `compat-perf` adds
the frame-timing scenes (`tools/perf-variant.sh` sets it).

## Test apps

- `tests/compat_test`: environment, network, storage plugins, text layout and
  rendering performance; results in `compat_results.log`.
- `tests/plugins_test`: one check per remaining LG plugin (Luna bridge, app
  manager, display, device/package info, connectivity, secure storage,
  gamepads, keyboard visibility, mouse cursor, audioplayers, image texture,
  url_launcher last because it opens the browser).
- `tests/input_test`: logs every key event, system back (`didPopRoute`) and
  lifecycle change; starts on a second page so Back can be checked.
- `tests/video_test`: scripted video_player checks, using the patched plugin
  from [Webos-Flutter-Legacy-TV/plugins](https://github.com/Webos-Flutter-Legacy-TV/plugins).

## Known issues

- **Frame pacing**: the Mali driver blocks ~10 ms per frame waiting for a
  buffer. `compat-dual-surface` removes the wait but is opt-in because of an
  Impeller race (see "Renderer and frame rate"). Even with it, the
  four-large-tiles scene runs at ~54 fps with no slow raster frames; cause
  not found.
- **Impeller and video**: see "Renderer and frame rate"; video apps should
  stay on Skia.
- **video_test "small" stage**: the video was hidden in about half of the
  runs with either renderer (the 960x540 window after leaving full screen);
  not yet investigated.
- **Video**: after a seek, playback resumes 4-7 s later; switching 2x back to
  1x makes the reported position jump back a few seconds; with two players
  only the newest is visible; per-stream volume is not supported by LG's plugin.
- **flutter_secure_storage is not hardware-backed on webOS 6.** LG's embedder
  encrypts through `com.webos.service.keymanager3`, which webOS 6 lacks, and
  its `keymanager2` returns the plaintext (16 random bytes + data). The
  patched plugin in Webos-Flutter-Legacy-TV/plugins detects this and
  encrypts with AES-256-GCM using a key in the app's private data folder
  (mode 0600): safe from other apps, not from root on the TV.
- **Widevine (video_player_drm) needs the opt-in CDM proxy on webOS 6.** The
  URL pipeline cannot do Widevine Modular (LG supports it there only through
  MSE/EME with an in-app CDM; every load-option form ends in
  `552 Encrypted Content`). See "Widevine on older webOS" above.
- **Streams with VP9/Opus tracks** in DASH fail to load on webOS 6.
- **device_info_plus**: use `DeviceInfoWebOSPlugin().webosInfo`; the generic
  `DeviceInfoPlugin().deviceInfo` casts to `LinuxDeviceInfo` and throws.
- `PlatformDispatcher.displays.first.refreshRate` reports 16666 (the embedder
  passes the frame period in microseconds).
- Only webOS 6.5.3 has been tested. Releases older than webOS 4 may need more
  work (older kernels and GPU drivers).

## Licensing

The bundled glibc (LGPL, from Debian) and C++ runtime (GPL with the runtime
exception, from LG's NDK build of GCC) are open source; ship their licences
and offer the matching sources. LG's engine, embedder and NDK stay under LG's terms;
check those before distributing repackaged apps outside Developer Mode.
