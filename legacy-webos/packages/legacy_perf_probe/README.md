# legacy_perf_probe

Frame, key and image timings for Flutter apps on older webOS TVs, written to the app's
`compat.log`. Off unless the app is built with `--dart-define=LEGACY_PERF_PROBE=true`.

```yaml
dependencies:
  legacy_perf_probe:
    git:
      url: https://github.com/Webos-Flutter-Legacy-TV/flutter-webos.git
      path: legacy-webos/packages/legacy_perf_probe
```

```dart
void main() {
  WidgetsFlutterBinding.ensureInitialized();
  PerfProbe.start();
  runApp(const MyApp());
}
```

```sh
flutter-webos build webos --release --legacy --dart-define=LEGACY_PERF_PROBE=true
# install, use the app, then on the PC:
ssh root@$TV_HOST cat /media/developer/apps/usr/palm/applications/<app id>/compat.log > app.log
legacy-webos/tools/perf-summary.py app.log
```

Image timings need one line in your own `ImageProvider.loadImage` (Flutter's own
`NetworkImage` has no hook): wrap the codec with `PerfProbe.timeCodec(codec, label: ..., source: ..., bytes: ..., fetchMs: ...)`.

What the numbers mean and what to do about them: `legacy-webos/docs/PERFORMANCE.md`.
