# Changelog: legacy-webos

Releases are tagged `legacy-vX.Y.Z` on both
[flutter-webos](https://github.com/Webos-Flutter-Legacy-TV/flutter-webos) and
[plugins](https://github.com/Webos-Flutter-Legacy-TV/plugins); use the same
tag on both.

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
