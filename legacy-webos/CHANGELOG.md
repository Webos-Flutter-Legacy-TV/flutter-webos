# Changelog: legacy-webos

Releases are tagged `legacy-vX.Y.Z` on both
[flutter-webos](https://github.com/Webos-Flutter-Legacy-TV/flutter-webos) and
[plugins](https://github.com/Webos-Flutter-Legacy-TV/plugins); use the same
tag on both.

## Unreleased

- Hardware (L1) Widevine on older webOS (opt-in `compat-widevine-l1`,
  experimental): `compat/widevine-l1` plays Widevine DASH through the TV's
  CDM wrapper and its StarfishMediaAPIs player on the secure video path, as
  LG's YouTube app does. Verified on webOS 6.5.3 with the picture on screen:
  1080p and 4K H.264 and HEVC with audio, licence by URL and by callback,
  seeking, audio track switching, 3 minutes of 4K HEVC without stalls; with
  clear test streams through the same player, AV1, VP9, Dolby Vision 8.1,
  HDR10, AC-3, E-AC-3 with Atmos, Opus, live DASH, several periods and
  adaptive quality. Key rotation is not yet checked on a TV.
- Removed the software Widevine route (`compat-widevine-cdm`, `libwv-compat`,
  `wv-selftest`): it decrypted media inside the app and served it clear on
  `127.0.0.1`, which works against the content protection. Widevine now
  plays only through the hardware route, where video is decrypted in the
  TV's secure hardware. Apps that still ship `compat-widevine-cdm` get a
  packaging warning and play Widevine through NDL (which refuses it on
  webOS 6). Also removed `l1-selftest`, an early probe that printed
  decrypted sample bytes.
- Wayland shim: while an L1 player is loaded, report EGL buffer age 0 so the
  embedder redraws whole frames (its partial redraw left the video widget's
  transparent hole in one buffer only); exports `compat_request_frame()` for
  the player to redraw after the TV applies the video area. NDL playback is
  unchanged: video_test 11/11 with the new shims.
- drm_test: L1 cases `widevine_uhd_h264`, `widevine_hevc`,
  `widevine_uhd_hevc`, `widevine_uhd_long`, `widevine_audio_switch`,
  `widevine_multiperiod` (Axinom's licence server currently refuses the
  published test token).

- webOS 3 (opt-in `COMPAT_WEBOS3=1`, untested on a TV): adds to the webOS 4
  extras a `libluna-service2++` built from webOS OSE (LG's embedder needs the
  newer `LS::Call`), `libwebos3-compat` (ResBundle with the C++11 string ABI,
  two Luna, two pbnjson and two SQLite functions, the `wl_webos_xinput`
  protocol), a `libudev.so.1` stand-in and Debian's libwebp where plugins use
  them. The firmware check passes for webOS 3.4 to 6.4 for every test app.
- Verified on webOS 6.5.3 after the webOS 3/2 changes: normal compat_test
  (14/14), input_test (keys, Back) and video_test (12/12 in 3 runs) with the
  new shim; plugins_test 15/15 both as a normal and as a `COMPAT_WEBOS3=1`
  package (the bundled luna-service2++, libwebos3-compat, libudev stand-in
  and libwebp loaded); input_test as a `COMPAT_WEBOS3=1` package with
  `compat-wl-track-versions` (the libwayland < 1.10 path): keys and Back work.
- webOS 2 (same `COMPAT_WEBOS3=1` package, on paper): apps without media or
  image plugins pass the firmware check for webOS 2.2. The Wayland shim
  marshals through `wl_proxy_create`/varargs `wl_proxy_marshal` on
  libwayland < 1.4 and emulates `prepare_read`/`read_events`/`cancel_read` on
  < 1.2; `libwebos3-compat` adds `LSRegisterApplicationService` (falls back to
  `LSRegister`) and `g_unix_fd_source_new`. The shim's timing wrappers no
  longer call a missing function. Media and image plugins are not covered on
  webOS 2: its pbnjson 2.9 has no `JResult`, so every `JValue` the plugins
  use has another layout there; that needs a bundled newer pbnjson.
- Wayland shim: works with libwayland < 1.10 (webOS 3), which has no
  `wl_proxy_get_version` or versioned constructor; it records proxy versions
  itself there. Found because the firmware check now also checks the bundled
  libraries. Unchanged behaviour on webOS 4+ (normal compat_test on 6.5.3).
- `fw-compat-check.py`: `--webos3`; symbols exported by any ELF given count as
  available, so passing `lib/compat` libraries checks their imports too (CI
  does). `check-compat-ipk.py`: `--webos3`.
- video_player, video_player_drm (plugins a6310ed): the video widget state is
  disposed (it kept a post-frame callback and move timer after layout changes).
- webOS 4 (opt-in `COMPAT_WEBOS4=1`, untested on a TV): bundles Debian's
  libsystemd, a `libcurl.so.4` forwarder to the firmware's `libcurl.so.5`,
  and the `wl_webos_foreign` protocol descriptions; the firmware check passes
  for webOS 4.4, 4.10, 5.3 and 6.4 (#2).
- webOS 4: also bundles Debian's libopus where a plugin uses it
  (audioplayers_soloud); found by the new firmware CI.
- audioplayers_soloud (plugins): position no longer sticks at the seek target
  after a loop wraps (plugins#1; untested on a TV).
- `tools/check-compat-ipk.py`: checks every legacy IPK's structure (ELF
  interpreter, `DT_RPATH`, shims as direct dependencies, preload patch, webOS 4
  extras only with the switch); the packager runs it on each build.
- `fw-compat-check.py --require LO[:HI]`: fails if a release in the range
  does not pass. CI (`fw-compat.yml`) builds every test app, normal and
  webOS 4, and requires webOS 5+ and 4.4-6 respectively.
- Dual surface: only the window's own surface is used while an external
  texture is registered, so `webos_image_texture` images show with Impeller
  (#3; 3 of 3 runs on webOS 6.5.3, was 0 of 4).
- Frame pacing (#4) investigated and closed: the buffer release can't be
  seen before `eglMakeCurrent`, so gating vsync only adds latency.
- Verified on webOS 6.5.3: soloud_test 21/21 (loop fix), video_test and
  plugins_test with local media, and `COMPAT_WEBOS4=1` packages of
  soloud_test and video_test (bundled libsystemd 257, libopus, curl
  forwarder, foreign protocol) with the same results as normal packages.
- plugins_test: pin `flutter_secure_storage: ^9.2.3`; `any` could resolve
  to 4.x, which bypasses the webOS implementation.
- Media shim: `NDL_MediaMuteDisplay`, forwarded where the firmware has it.
- `fw-compat-check.py`: `--webos4`; knows the shims' own libraries and
  `NDL_MediaSetPlayer`, no longer flags plugin registration or EGL symbols.
- CI: script lint, shim build and test app analysis on every pull request.
- `setup.sh` fetches Debian packages it doesn't have yet on re-runs.
- `tools/media-server.sh`: serves the test media locally (with Range
  requests for seeking); video_test, plugins_test and soloud_test use it when
  built with `--dart-define=MEDIA_BASE=...`, so tests no longer depend on
  third-party hosts.

## legacy-v0.1.0 (2026-11-18)

First release. Apps built with LG's flutter-webos (webOS 26) run on
**webOS 6**, verified on webOS 6.5.3 (LG OLED C1, 2021).

### Build
- `flutter-webos build webos --release --legacy` produces a second package for
  older webOS next to the webOS 26 one (`build/webos-compat/<mode>/`).
- `tools/make-compat-ipk.sh` repackages an existing build: bundled glibc 2.41
  and the NDK's C++ runtime, patched ELF interpreter, `DT_RPATH`, bundled
  libraries loaded first, `/etc/ld.so.preload` skipped.
- `setup.sh` downloads every tool into git-ignored folders; `env.sh` sets up
  the shell.

### Runtime shims
- Wayland: `wl_proxy_marshal_flags` on Wayland 1.11, protocol version
  clamping, remote Back delivered as a route pop, `WebPMalloc` for old libwebp.
- `libwayland-egl.so.1` forwarder for firmware that ships only `.so.0`.
- Media (NDL): Flutter's window handed to the old SDL-based NDL, audio-only
  players, PlayReady via the DRM client, `NDL_MediaSetDrmOperation`, and a
  single-player fallback where `NDL_MediaSetPlayer` is missing (webOS 5).
- Video placement: a frame is scheduled after each placement so a moved video
  becomes visible.
- Opt-in (`webos/meta/` switch files): `compat-widevine-cdm` (Widevine DASH
  through the TV's own CDM, L3), `compat-dual-surface` (faster frames; known
  issue with Impeller), `compat-back-as-key`, and `compat-log-*` debug output.

### Tools and tests
- `tools/fw-compat-check.py`: checks a build against firmware symbol data for
  each webOS release.
- `dev-cycle.sh`, `tv-install.sh`, `tv-shot.sh`, `tv-key`, `perf-variant.sh`,
  `video-scenario.sh`, `webos-survey.sh`.
- Test apps: `compat_test`, `input_test`, `plugins_test`, `video_test`,
  `drm_test`, `soloud_test`.
- [Porting guide](docs/PORTING-GUIDE.md).

### Plugins (plugins repository, same tag)
- video_player: loop position, position after Home, default volume, and a
  moved or resized video placed again once visible.
- video_player_drm: stop position polling once the player is gone.
- flutter_secure_storage: app-sandbox AES-GCM fallback when keymanager3 is
  missing.
- webos_service_bridge: non-PlatformException errors on cancel.
- audioplayers_soloud: resume after completion in release mode; `seek()`
  always completes.

### Verified on webOS 6.5.3
UI at 1080p (Skia and Impeller), remote D-pad/OK/Back, networking with
certificate verification, storage, video_player, video_player_drm (clear,
PlayReady, opt-in Widevine), audioplayers, audioplayers_soloud, and the other
LG plugins listed in the README. Impeller at 1080p: 52 fps (animated tiles),
61 fps (list scroll); Skia 17 and 58.

### Known issues
- Dual surface with Impeller: the last change before idle can stay invisible
  (#3).
- audioplayers_soloud: reported position stays at the seek target after
  looping (plugins#1).
- webOS 6 media limits: no VP9/Opus DASH, one video plane, 4–7 s to resume
  after a seek.
- webOS 4 and older need more work (#2); webOS 5 and 22–25 are untested on
  real TVs (#1).
