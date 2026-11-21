# Porting a flutter-webos app to older webOS TVs

This guide takes an app that already builds with flutter-webos (LG's webOS 26
SDK) and makes it run on older TVs with legacy-webos. The app code normally
needs no changes; the work is in the build, the plugins and testing.

Status: verified on webOS 6.5.3 (LG OLED C1). webOS 22-25 pass the firmware
paper check (see the README); webOS 5 and older are untested.

## 1. Set up once

```sh
git clone https://github.com/Webos-Flutter-Legacy-TV/flutter-webos.git
cd flutter-webos/legacy-webos
WEBOS_NDK_ACCEPT_EULA=1 ./setup.sh
. ./env.sh      # each new shell: flutter-webos, ares and the NDK on PATH
```

## 2. Build

```sh
cd my_app
flutter-webos build webos --release --legacy
```

This produces two packages:

| Package | For |
| --- | --- |
| `build/webos/arm/release/ipk/<id>_<version>_arm.ipk` | webOS 26 (unchanged LG build) |
| `build/webos-compat/release/<id>_<version>_arm.ipk` | older webOS |

The legacy package contains the same app plus, in `lib/compat/`, glibc 2.41,
the NDK's C++ runtime and the shims. It is about 15 MB larger. `--legacy`
needs `--target-arch=arm` (the default); older TVs run 32-bit software.

Without the flag, the same step can be run on an existing build:

```sh
../flutter-webos/legacy-webos/tools/make-compat-ipk.sh . release
```

For a **webOS 4** TV (untested on real hardware), package with
`COMPAT_WEBOS4=1 flutter-webos build webos --release --legacy`. That package
is for webOS 4-6 only; see the README section "webOS 4". For **webOS 3**
(also untested), use `COMPAT_WEBOS3=1` instead (webOS 3-6; README section
"webOS 3").

## 3. Configure

Renderer: legacy packages use Impeller with two window surfaces unless the
app says otherwise (`"enable-impeller": false` in
`webos/meta/flutter-conf.json`, or an empty `webos/meta/compat-single-surface`).
Together they halve GPU time per frame on webOS 6. Apps with `video_player`
need the plugins fork (7a53a97 or later), which keeps a moved or resized video
visible. See the README section "Renderer and frame rate", and
[PERFORMANCE.md](PERFORMANCE.md) to measure your app and for what makes
frames expensive on these TVs.

Optional, as empty files in `webos/meta/`:

| File | Effect |
| --- | --- |
| `compat-widevine-l1` | Widevine at the hardware security level through the TV's own CDM and player (see README; opt-in, experimental, research only: not for published apps) |
| `compat-back-as-key` | Deliver Back as a key event instead of a route pop |
| `compat-single-surface` | Turn off the legacy default `compat-dual-surface` (two surfaces remove a ~10 ms driver wait per frame; the shim uses one by itself while media is loaded or an external texture is registered) |
| `compat-log-*`, `compat-swap0` | Debug output, see README |

## 4. Plugins

Use the patched plugins from
[Webos-Flutter-Legacy-TV/plugins](https://github.com/Webos-Flutter-Legacy-TV/plugins)
for the four that needed fixes. Replace LG's entry with:

```yaml
dependencies:
  video_player_webos:
    git:
      url: https://github.com/Webos-Flutter-Legacy-TV/plugins.git
      ref: main
      path: packages/video_player
```

| Plugin | On webOS 6 |
| --- | --- |
| `video_player` | Use the fork: fixes looping, lifecycle events and volume |
| `video_player_drm` | Use the fork: fixes error handling. PlayReady works; Widevine needs `compat-widevine-l1` |
| `flutter_secure_storage` | Use the fork: webOS 6 has no usable key service; the fork encrypts in the app sandbox |
| `webos_service_bridge` | Use the fork: fixes subscription cancel errors |
| `audioplayers`, `gamepads`, `webos_image_texture`, `webos_app_manager`, `webos_display`, `connectivity_plus`, `package_info_plus`, `path_provider`, `shared_preferences`, `sqflite`, `url_launcher`, `custom_mouse_cursor`, `flutter_keyboard_visibility` | Work unchanged |
| `device_info_plus` | Works; call `DeviceInfoWebOSPlugin().webosInfo`, not the generic getter |
| `audioplayers_soloud` | Works with the plugins fork after its 2026-11-18 fixes (resume after completion, seek never hangs); no compat changes needed. Known issue: reported position in loop mode |
| `flutterfire` | Not tested |

## 5. Things that behave differently

- **Back:** the remote's Back key arrives as a route pop
  (`WidgetsBindingObserver.didPopRoute`), like Android, so `Navigator` and
  `PopScope` work as usual. Check how your app behaves on its first page.
- **Video:** one video plane at a time; seeking resumes after 4-7 s; DASH
  streams with VP9 or Opus tracks do not load through the media pipeline
  (Widevine streams with them play through `compat-widevine-l1`).
- **Frame rate:** light UI runs at 50-60 fps with Impeller; avoid large
  shadows, blurs and `Opacity` over big areas, which the GPU handles slowly.
- **refreshRate** reports 16666; assume 60 Hz.

## 6. Test

On a rooted TV (`TV_HOST` set):

```sh
legacy-webos/tools/tv-install.sh build/webos-compat/release/<id>_<version>_arm.ipk --launch
```

Or with Developer Mode: `ares-install` the IPK, then launch it from the
home screen. Check:

1. The app starts and draws; the remote's arrows and OK work.
2. Back goes back one page, and leaves the app from the first page.
3. Network, storage and each plugin the app uses.
4. Video: play, pause, seek, and the DRM type the app uses.

If the app does not start, read `compat.log` next to the installed binary
(`/media/developer/apps/usr/palm/applications/<id>/compat.log`). It holds
the app's stdout and stderr and the shims' messages.

## 7. Before publishing

- No legacy package has been submitted to LG's Content Store yet; whether
  LG accepts the repackaged runtime is unknown.
- The package bundles glibc (LGPL) and the GCC runtime (GPL with the runtime
  exception); see the README's licensing section for what to ship.
- **Do not ship `compat-widevine-l1` in a published app.**
  - It uses the TV's Widevine CDM and player library without a licence
    from Google or LG.
  - Studio-licensed services are usually required to use certified
    clients.
  - Google can revoke the TV's Widevine device certificate.
  - Its legal status (for example under DMCA section 1201) is untested.
  - It is for research on your own TV only; get legal advice first. See
    the README.
