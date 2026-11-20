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
| video_player (network MP4) | Works: play, pause, seek, 2x, resized window, Home/resume. Two players both play, but only the newest is visible (one video plane) |
| audioplayers (network MP3) | Works |
| webos_service_bridge (Luna calls + subscriptions), webos_app_manager, webos_display | Works |
| device_info_plus (`webosInfo`), package_info_plus, connectivity_plus | Works |
| url_launcher (opens the TV browser), webos_image_texture | Works |
| gamepads, flutter_keyboard_visibility, custom_mouse_cursor | Work (API calls; no gamepad attached) |
| flutter_secure_storage | Works via app-sandbox AES-GCM fallback (no system keystore on webOS 6; see Known issues) |
| video_player_drm | Clear HLS/DASH and PlayReady (licence URL) play; Widevine plays with the opt-in CDM proxy (software security level) |
| audioplayers_soloud | Works with plugins fork fixes (2026-11-18): WAV/MP3/Opus from asset, bytes, file and URL; pause, seek, 2x, volume/balance, completion, replay, three players mixed, low-latency retrigger. Output reaches the TV's ALSA hardware sink through PulseAudio. Open: in loop mode the reported position stays at the last seek target after wrapping (`tests/soloud_test`, 21/22) |
| Not tested | flutterfire |
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

Ubuntu 22.04/24.04/26.04 x86_64 host (WSL2 works) with LG's build packages and
Node.js 20 or newer (for the webOS CLI; Ubuntu's own `nodejs` is too old, use
[nvm](https://github.com/nvm-sh/nvm)):

```sh
sudo apt-get install -y git git-lfs curl unzip xz-utils file pkg-config cmake ninja-build clang \
  libgtk-3-dev python3 python3-venv openssh-client
```

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
#   -> build/webos/arm/release/ipk/<app-id>_<version>_arm.ipk (webOS 26)
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

- **Enable Impeller**, including for apps with `video_player` when they use
  the Webos-Flutter-Legacy-TV plugins fork (7a53a97 or later). All plugin,
  DRM-licence and input tests pass with it. A moved or resized video used to
  stay hidden (always with Impeller, about half the time with Skia): the
  placement sent while the video moves reaches the compositor while the area
  is still opaque. The fork places the video again once its hole is visible,
  and the shim schedules a frame after each placement; with both,
  `tests/video_test` showed the video in every stage in 3 of 3 Impeller runs.
  With Skia a stage's screenshot is still black now and then (about 1 in 10
  frames, any stage). That is the capture service, not the screen: in a run
  watched on the TV the video stayed visible while its screenshot was black,
  and the placement requests are identical in black and good runs.
- **`compat-dual-surface`** is opt-in. With Impeller, an external texture
  (a `webos_image_texture` image) is drawn on the window's own surface but
  not on the second one, although both share one GL context and full
  redraws make no difference (#3; cause not found). The shim therefore uses
  only the window's own surface while any external texture is registered,
  as it already does while media is loaded: the image then shows in 3 of 3
  runs, and apps without textures keep the speed-up (raster ~7 ms instead
  of ~17 ms).
- **Delivering vsync once a buffer is free** does not work instead of dual
  surface: LG's embedder leaves vsync to the engine's timer, and the buffer
  release arrives only with the vblank and is read only by the Mali driver
  inside `eglMakeCurrent`, so holding vsync just adds latency (#4).

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

## Hardware Widevine (L1) on older webOS (opt-in, experimental)

Add an empty `webos/meta/compat-widevine-l1` to the app (checked before
`compat-widevine-cdm`). Widevine DASH loads from `video_player_drm` (licence
URL or `licenseCallback`) then play through `libwv-l1.so`
(`compat/widevine-l1/`) at the hardware security level, the way LG's YouTube
app does:

1. the TV's Widevine CDM wrapper (`/usr/lib/libwvcdm_shared.so`) gets the
   licence and wraps each video sample for the secure video path (the video
   is decrypted in the TEE, never in the app);
2. the TV's player library (`StarfishMediaAPIs`) decodes it in the app's
   process; `libwv-l1` reads the DASH manifest and MP4 segments, feeds the
   samples and sends the plugin the events NDL would;
3. the player places its video with the video output service and asks for a
   redraw once the TV has applied it; the Wayland shim redraws whole frames
   while media is loaded, so the video widget's transparent hole reaches the
   screen.

Verified on webOS 6.5.3 (LG C1), picture on screen: 1080p and 4K H.264 and
HEVC with audio, licence by URL and by callback, seeking, audio track
switching, 3 minutes of 4K HEVC at 20 Mbit/s without stalls. Through the same
player with clear streams (public, and made by `tools/l1-test-media.sh`):
AV1, VP9, Dolby Vision Profile 8.1, HDR10, AC-3, E-AC-3 with Atmos, Opus, live
DASH, several periods, adaptive quality. Not yet checked on a TV: key
rotation, several keys across periods. Not supported: in-stream subtitles,
HLS, period changes in live streams. Details, debug options and status:
[compat/widevine-l1/README.md](compat/widevine-l1/README.md).

The same caution as for the software route applies: this uses the TV's CDM
outside the terms it ships under.

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
| 5 (5.3.1) | nothing (`NDL_MediaSetPlayer` shimmed: single player) | 15.1 | likely works; Widevine proxy needs the v15 CDM interface |
| 4 (4.4.2, 4.10) | nothing with `COMPAT_WEBOS4=1` (see below); without it `libsystemd.so.0`, `libcurl.so.4` and the `wl_webos_foreign` protocol | -, 14.0 | starts on paper; no video placement; untested on a TV |
| 3 (3.4, 3.9) | nothing with `COMPAT_WEBOS3=1` (see below); without it as webOS 4, plus older Luna C++, i18n and pbnjson APIs, Wayland < 1.10, no `libudev.so.1`/libwebp | - | starts on paper; no video placement; untested on a TV |
| 2 (2.2) | with `COMPAT_WEBOS3=1`: nothing for apps without media plugins. Media and image plugins use pbnjson's C++ API, and webOS 2's pbnjson 2.9 predates `JResult`, so its `JValue` has another layout: they need a newer pbnjson bundled (not done) | - | core app starts on paper; untested on a TV |
| 1 (1.2, 1.4) | with `COMPAT_WEBOS3=1` also older Luna, PmLog and Wayland APIs and more webOS protocols | - | highest risk |

```sh
tools/fw-compat-check.py --db <fw-symbols data dir> build/webos/arm/release/bundle/lib/*.so
```

`--require LO[:HI]` turns the report into a test: exit 1 unless every image
with a release from LO up to (not including) HI passes. CI builds every test
app and requires webOS 5 and newer for normal packages, 4.4 to 6 for
`COMPAT_WEBOS4=1` ones and 3 to 6 for `COMPAT_WEBOS3=1` ones. Pass the
bundled libraries from `lib/compat` too (not glibc or the C++ runtime): what
they export counts as available, and their own imports are checked.

### webOS 4 (opt-in, untested on a TV)

webOS 4 firmware lacks three things every app needs: `libsystemd.so.0` (LG's
media library runs an `sd_event` loop), `libcurl.so.4` (curl is there, but as
`libcurl.so.5`) and the `wl_webos_foreign` Wayland protocol, which LG's media
library references and which webOS 4's compositor does not offer. Package with
`COMPAT_WEBOS4=1` to add:

| Added to `lib/compat/` | Why |
| --- | --- |
| `libsystemd.so.0` | Debian's (`libsystemd0`, LGPL-2.1, fetched by `setup.sh`); needs only glibc and the firmware's `libcap.so.2` |
| `libopus.so.0` | Debian's (`libopus0`, BSD-3-Clause), only if a plugin uses it (audioplayers_soloud); needs only glibc |
| `libcurl.so.4` | `compat/fallback`: no code, only a dependency on the firmware's `libcurl.so.5` |
| `libwebos-foreign-compat.so` | `compat/webos4`: the `wl_webos_foreign` protocol descriptions, generated from webOS OSE's protocol file (Apache-2.0), so the app loads |

`NDL_MediaMuteDisplay` (also missing on webOS 4) is in the media shim for
every package: it calls the firmware's function where it exists.

```sh
COMPAT_WEBOS4=1 flutter-webos build webos --release --legacy
tools/fw-compat-check.py --db <data dir> --webos4 <stage>/<main> <stage>/lib/*.so <stage>/lib/compat/libsystemd.so.0 ...
```

The firmware check passes for webOS 4.4, 4.10, 5.3 and 6.4, for every test
app (CI checks this on each change: `.github/workflows/fw-compat.yml`). The package does
**not** suit webOS 7 and newer, which have no `libcurl.so.5`; build those
without the switch. Video placement cannot work on webOS 4 this way (no
exported windows); the rest of the app should. Bundled `libsystemd` (systemd
257) on a webOS 4 kernel is the main unknown until someone runs it on a TV.

### webOS 3 (opt-in, untested on a TV)

`COMPAT_WEBOS3=1` includes everything `COMPAT_WEBOS4=1` adds, plus:

| Added to `lib/compat/` | Why |
| --- | --- |
| `libluna-service2++.so.3` | Built from webOS OSE's luna-service2 (Apache-2.0, `compat/webos3/luna-service2pp`). webOS 3's copy has an older `LS::Call` (inline constructor and destructor, another reply context), which LG's embedder cannot use; OSE's matches LG's (checked in the embedder's machine code). No firmware library the app loads on webOS 3 uses this library |
| `libwebos3-compat.so` | `compat/webos3/webos3_compat.cpp`, a direct dependency of the app binary: `ResBundle` with the C++11 string ABI (accessibility labels stay untranslated), `LSErrorLogDefault`, `LSMessageIsConnected`, pbnjson's `JValue::asString` and `JGenerator::toString` (media plugins), `sqlite3_db_cacheflush`, `sqlite3_expanded_sql`, and the `wl_webos_xinput` protocol descriptions. Each forwards to the firmware's function where there is one |
| `libudev.so.1` | `compat/webos3/udev_compat.c`, only for the gamepads plugin: forwards to the firmware's `libudev.so.0` (webOS 3) or `libudev.so.1` (webOS 4+) |
| `libwebp.so.7`, `libwebpdemux.so.2`, `libsharpyuv.so.0` | Debian's (BSD-3-Clause, fetched by `setup.sh`), only for webos_image_texture |

The Wayland shim also covers libwayland older than 1.10 (webOS 3): it creates
proxies with the unversioned constructor and answers `wl_proxy_get_version`
from versions it records (the bind version after clamping, the parent's for
child objects). For libwayland older than 1.4 (webOS 2), which has no array
marshalling, it creates objects with `wl_proxy_create` and sends requests
through the varargs `wl_proxy_marshal` (every argument is one 32-bit word on
ARM32); for libwayland older than 1.2 it emulates the threaded read API LG's
embedder uses (`prepare_read` always succeeds, `read_events` runs
`wl_display_dispatch` after `poll` reported data). `libwebos3-compat` also
provides `LSRegisterApplicationService` (falls back to `LSRegister`) and
`g_unix_fd_source_new` (glib 2.34 on webOS 2). The switch file `compat-wl-track-versions` forces this path
on a newer TV, for testing.

```sh
COMPAT_WEBOS3=1 flutter-webos build webos --release --legacy
tools/fw-compat-check.py --db <data dir> --webos3 <stage>/<main> <stage>/lib/*.so <stage>/lib/compat/libwebos3-compat.so ...
```

On webOS 6.5.3 a `COMPAT_WEBOS3=1` plugins_test passes all checks with the
bundled libraries loaded, and input_test works with `compat-wl-track-versions`
(the webOS 3 Wayland path). The firmware check passes for webOS 3.4, 3.9, 4.4,
4.10, 5.3 and 6.4, for every test app, and for webOS 2.2 for the apps without media or image
plugins (compat_test, input_test) (CI). The package suits webOS 2-6 only (as
`COMPAT_WEBOS4=1`, not webOS 7+).
Like webOS 4, there is no video placement. Unknowns until it runs on a TV:
the 3.x kernels and GPU drivers, and whether webOS 3's compositor accepts
the shell and input protocols LG's embedder uses.

## Tools

The TV tools use SSH as root (rooted TV). Set `TV_HOST`, and `TV_SSH_KEY` if
the key is not `~/.ssh/lg_webos`.

| Tool | Purpose |
| --- | --- |
| `tools/make-compat-ipk.sh <app> [mode]` | Repackage a flutter-webos build for older webOS |
| `tools/check-compat-ipk.py [--webos4] [--webos3] <ipk>` | Check a legacy IPK's structure: loader, `DT_RPATH`, shims as direct dependencies, preload patch, webOS 4 and webOS 3 extras (run by the packager on every build) |
| `tools/fw-compat-check.py` | Check a build against firmware symbol data per webOS release (see above) |
| `tools/build-shims.sh` | Build the shims and `tv-key` (called by the packager) |
| `tools/dev-cycle.sh <app> [secs] [--luna]` | Package, install, run, print `compat.log` |
| `tools/webos-survey.sh` | Read-only survey of a TV's kernel, glibc, graphics and webOS libraries |
| `tools/tv-install.sh`, `tools/tv-shot.sh` | Install an IPK; take a screenshot |
| `tools/tv-key/tv-key.c` | Inject remote key presses into evdev |
| `tools/perf-variant.sh` | Run `tests/compat_test` with a resolution / Impeller setting |
| `tools/video-scenario.sh`, `tools/video-probe.sh` | Scripted video_player test; probe one URL |
| `tools/media-server.sh` | Download the test media once and serve it on the LAN (with HTTP Range, for seeking); build test apps with `--dart-define=MEDIA_BASE=http://<host>:8099`, or set `MEDIA_BASE` for `video-scenario.sh` |

Debug switches: an empty file in an app's `webos/meta/` turns on extra output
in `compat.log` (next to the installed binary):
`compat-log-globals` (Wayland globals), `compat-log-keys` (raw Wayland key
codes), `compat-back-as-key` (deliver Back as the embedder's key event instead
of `popRoute`), `compat-log-ndl` (raw NDL media events),
`compat-log-pmlog` (plugin PmLog messages), `compat-log-swap` (per 120
frames: GPU time, time blocked in the swap and between presents, waits over
2 ms by thread and caller, buffer releases; adds a `glFinish` per frame),
`compat-dual-surface` (alternate two EGL surfaces, see "Renderer and frame rate"),
`compat-log-exported` (video window placement requests and their regions),
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
  buffer. `compat-dual-surface` removes the wait (not while media is loaded
  or an external texture is registered; see "Renderer and frame rate"). Even with it, the
  four-large-tiles scene runs at ~54 fps with no slow raster frames; cause
  not found.
- **Moved or resized video**: fixed with the plugins fork (see "Renderer and
  frame rate"). `tests/video_test` screenshots are occasionally black although
  the video shows on screen (a capture-service artifact).
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
- **Streams with VP9/Opus tracks** in DASH fail to load through the media
  pipeline on webOS 6. Widevine streams with them play through the hardware
  route (`compat-widevine-l1`).
- **device_info_plus**: use `DeviceInfoWebOSPlugin().webosInfo`; the generic
  `DeviceInfoPlugin().deviceInfo` casts to `LinuxDeviceInfo` and throws.
- `PlatformDispatcher.displays.first.refreshRate` reports 16666 (the embedder
  passes the frame period in microseconds).
- Only webOS 6.5.3 has been tested. webOS 4 and webOS 3 support
  (`COMPAT_WEBOS4=1`, `COMPAT_WEBOS3=1`) is checked only against firmware
  symbol data. webOS 1 and 2 need more work (older Wayland, glib and Luna
  APIs; older kernels and GPU drivers).

## Licensing

The bundled glibc (LGPL, from Debian), with `COMPAT_WEBOS4=1` also libsystemd
(LGPL-2.1, from Debian), libopus (BSD-3-Clause, from Debian) and protocol code generated from webOS OSE (Apache-2.0,
see `compat/webos4/NOTICE`), with `COMPAT_WEBOS3=1` also luna-service2++
built from webOS OSE (Apache-2.0) and libwebp (BSD-3-Clause; see
`compat/webos3/NOTICE`), and the C++ runtime (GPL with the runtime
exception, from LG's NDK build of GCC) are open source; ship their licences
and offer the matching sources. LG's engine, embedder and NDK stay under LG's terms;
check those before distributing repackaged apps outside Developer Mode.
