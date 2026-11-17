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
| Remote Back | Not verified yet (see Known issues) |
| DNS, HTTP, HTTPS (certificate verification) | Works |
| path_provider, shared_preferences, sqflite | Works; data survives reinstall |
| video_player (network MP4) | Works: play, pause, seek, 2x, resized window, two players, Home/resume |
| Performance | 50-60 fps for lists and light UI with Impeller; heavy shadow/opacity grids are slow |

## How it works

`tools/make-compat-ipk.sh` takes the bundle flutter-webos built and fixes, in
order, everything that stopped it from starting on webOS 6:

| Problem on older webOS | Fix |
| --- | --- |
| LG's binaries need glibc 2.38 / `GLIBCXX_3.4.32`; the NDK's glibc also needs kernel >= 5.15 (`statx`) | Bundle Debian armel glibc 2.41 + libstdc++ (built for kernel >= 3.2) in `lib/compat/` |
| The app must be the `main` executable, or webOS denies its Luna bus permissions | No wrapper script: patch the binary's ELF interpreter to the bundled loader |
| The devmode jailer sets `LD_LIBRARY_PATH`, which overrides `RUNPATH` | Use `DT_RPATH` |
| System libraries would pull in the TV's old libpthread/libdl/librt | Bundled ones added as direct `NEEDED` entries so they load first |
| Firmware preloads `/lib/libSegFault.so` (old glibc) into every process | Bundled loader patched to skip `/etc/ld.so.preload` |
| `wl_proxy_marshal_flags` missing (Wayland < 1.20) | `compat/wayland-shim` implements it on the 1.11 API |
| `wl_webos_shell` v2 requested, compositor offers v1 | Shim records advertised versions and clamps binds |
| Back key never delivered to the app | Shim sets `_WEBOS_ACCESS_POLICY_KEYS_BACK` on the shell surface |
| `libwayland-egl.so.1` missing | Retarget to the vendor `libwayland-egl.so.0` (same API) |
| NDL has no `NDL_MediaInitWithWindowId`; old NDL takes its video window from SDL | `compat/ndl-sdl-shim`: a stand-in `libSDL2-2.0.so.0` that hands NDL Flutter's exported window (added only when the app uses video_player) |

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
`compat-log-globals` (Wayland globals), `compat-log-ndl` (raw NDL media events),
`compat-log-pmlog` (plugin PmLog messages), `compat-swap0` (EGL swap interval 0).

## Test apps

- `tests/compat_test`: environment, network, storage plugins, text layout and
  rendering performance; results in `compat_results.log`.
- `tests/video_test`: scripted video_player checks, using the patched plugin
  from [Webos-Flutter-Legacy-TV/plugins](https://github.com/Webos-Flutter-Legacy-TV/plugins).

## Known issues

- **Back key**: the shim requests it, but delivery has not been confirmed yet.
- **Frame pacing**: about 14-17 ms of each frame is spent waiting in the
  raster thread, so light scenes run at 50-55 fps rather than 60.
- **Video**: after a seek, playback resumes 4-7 s later; switching 2x back to
  1x makes the reported position jump back a few seconds; with two players
  only the newest is visible; per-stream volume is not supported by LG's plugin.
- `PlatformDispatcher.displays.first.refreshRate` reports 16666 (the embedder
  passes the frame period in microseconds).
- Only webOS 6.5.3 has been tested. Releases older than webOS 4 may need more
  work (older kernels and GPU drivers).

## Licensing

The bundled glibc and libstdc++ are open source (LGPL / GPL with runtime
exception) from Debian. LG's engine, embedder and NDK stay under LG's terms;
check those before distributing repackaged apps outside Developer Mode.
