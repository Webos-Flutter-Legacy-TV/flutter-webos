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
| video_player_drm | Clear HLS/DASH and PlayReady (licence URL) play; Widevine does not (see Known issues) |
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
| `libwayland-egl.so.1` missing | Retarget to the vendor `libwayland-egl.so.0` (same API) |
| NDL has no `NDL_MediaInitWithWindowId`; old NDL takes its window from SDL, even for audio | `compat/ndl-sdl-shim`: a stand-in `libSDL2-2.0.so.0` that hands NDL Flutter's exported window, or a placeholder for audio-only players (added when any bundled library links `libNDL_media`) |
| `drm.licenseUrl` in the load request (webOS 26) is ignored; old releases use a DRM client (`com.webos.service.drm`) | NDL shim translates it: `NDL_MediaDRMLoad`, `NDL_MediaDRMSendMessage` with a `PlayReadyInitiator` (`LA_URL`), then loads with `drm.clientId`; unloads the client at exit |
| `NDL_MediaSetDrmOperation` missing (video_player_drm, `-z now`) | Shim exports a stub that reports failure (licence-callback DRM unavailable) |
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
cd my_app && flutter-webos build webos --release
../flutter-webos/legacy-webos/tools/make-compat-ipk.sh . release
#   -> build/webos-compat/release/<app-id>_<version>_all.ipk
```

Install it like any IPK (`ares-install`, or `tools/tv-install.sh` on a rooted TV).

On webOS 6 enable Impeller in `webos/meta/flutter-conf.json`; it is 2-3x faster
than Skia on the Mali GPU:

```json
{ "engine-switches": { "enable-impeller": true } }
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
`compat-log-pmlog` (plugin PmLog messages), `compat-swap0` (EGL swap interval 0).

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

- **Frame pacing**: about 14-17 ms of each frame is spent waiting in the
  raster thread, so light scenes run at 50-55 fps rather than 60.
- **Video**: after a seek, playback resumes 4-7 s later; switching 2x back to
  1x makes the reported position jump back a few seconds; with two players
  only the newest is visible; per-stream volume is not supported by LG's plugin.
- **flutter_secure_storage is not hardware-backed on webOS 6.** LG's embedder
  encrypts through `com.webos.service.keymanager3`, which webOS 6 lacks, and
  its `keymanager2` returns the plaintext (16 random bytes + data). The
  patched plugin in Webos-Flutter-Legacy-TV/plugins detects this and
  encrypts with AES-256-GCM using a key in the app's private data folder
  (mode 0600): safe from other apps, not from root on the TV.
- **Widevine (video_player_drm) does not play on webOS 6.** The URL pipeline
  has a Widevine CENC decryptor, but LG documents Widevine Modular only via
  MSE/EME (an in-app CDM, as LG's Chromium does); every load-option form
  tried (`WIDEVINE`, `widevine_modular`, DRM client + `widevine+xml`) ends in
  `552 Encrypted Content`. PlayReady works through the DRM-client adapter.
  The licence-callback mode (`useLicenseCallback`) is not available.
- **Streams with VP9/Opus tracks** in DASH fail to load on webOS 6.
- **device_info_plus**: use `DeviceInfoWebOSPlugin().webosInfo`; the generic
  `DeviceInfoPlugin().deviceInfo` casts to `LinuxDeviceInfo` and throws.
- `PlatformDispatcher.displays.first.refreshRate` reports 16666 (the embedder
  passes the frame period in microseconds).
- Only webOS 6.5.3 has been tested. Releases older than webOS 4 may need more
  work (older kernels and GPU drivers).

## Licensing

The bundled glibc and libstdc++ are open source (LGPL / GPL with runtime
exception) from Debian. LG's engine, embedder and NDK stay under LG's terms;
check those before distributing repackaged apps outside Developer Mode.
