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
| `build/webos/arm/release/*.ipk` | webOS 26 (unchanged LG build) |
| `build/webos-compat/release/<id>_<version>_arm.ipk` | older webOS |

The legacy package contains the same app plus, in `lib/compat/`, glibc 2.41,
the NDK's C++ runtime and the shims. It is about 15 MB larger. `--legacy`
needs `--target-arch=arm` (the default); older TVs run 32-bit software.

Without the flag, the same step can be run on an existing build:

```sh
../flutter-webos/legacy-webos/tools/make-compat-ipk.sh . release
```

## 3. Configure

Recommended in `webos/meta/flutter-conf.json`: Impeller, 2-3x faster than
Skia on the webOS 6 GPU.

```json
{ "engine-switches": { "enable-impeller": true } }
```

Optional, as empty files in `webos/meta/`:

| File | Effect |
| --- | --- |
| `compat-widevine-cdm` | Widevine playback through the TV's own CDM (see README, opt-in, L3) |
| `compat-back-as-key` | Deliver Back as a key event instead of a route pop |
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
| `video_player_drm` | Use the fork: fixes error handling. PlayReady works; Widevine needs `compat-widevine-cdm` |
| `flutter_secure_storage` | Use the fork: webOS 6 has no usable key service; the fork encrypts in the app sandbox |
| `webos_service_bridge` | Use the fork: fixes subscription cancel errors |
| `audioplayers`, `gamepads`, `webos_image_texture`, `webos_app_manager`, `webos_display`, `connectivity_plus`, `package_info_plus`, `path_provider`, `shared_preferences`, `sqflite`, `url_launcher`, `custom_mouse_cursor`, `flutter_keyboard_visibility` | Work unchanged |
| `device_info_plus` | Works; call `DeviceInfoWebOSPlugin().webosInfo`, not the generic getter |
| `audioplayers_soloud`, `flutterfire` | Not tested |

## 5. Things that behave differently

- **Back:** the remote's Back key arrives as a route pop
  (`WidgetsBindingObserver.didPopRoute`), like Android, so `Navigator` and
  `PopScope` work as usual. Check how your app behaves on its first page.
- **Video:** one video plane at a time; seeking resumes after 4-7 s; DASH
  streams with VP9 or Opus tracks do not load.
- **Frame rate:** light UI runs at 50-55 fps; avoid large shadows, blurs and
  `Opacity` over big areas.
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
- The Widevine route uses the TV's CDM outside the terms it ships under. Do
  not enable it in a published app without legal advice.
