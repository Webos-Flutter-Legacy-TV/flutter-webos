/// Frame, key and image timings for Flutter apps on older webOS TVs.
///
/// Everything is off unless the app is built with
/// `--dart-define=LEGACY_PERF_PROBE=true`; in other builds the calls do
/// nothing and the compiler drops them. Lines go to stderr, which the legacy
/// package writes to `compat.log` in the app's folder on the TV
/// (`print` does not reach it on LG's engine). `tools/perf-summary.py`
/// turns a log into one line of numbers.
///
/// ```text
/// [perf] slow frame at 21.37s: build 2.6 raster 171.0 total 184.5 ms
/// [perf] summary 120 frames: build avg/p90/max 2.0/3.2/5.7  raster ...
/// [perf] key Arrow Down at 29.72s: 31.0 ms to a drawn frame (build 2.0 raster 16.3)
/// [perf] image poster: disk 51 KB, fetch 12 ms, decode 30 ms, 576x324
/// ```
///
/// *build* is the UI thread (your widgets' build, layout and paint);
/// *raster* is the GPU thread drawing the frame. Which one is large tells
/// you where to look; see docs/PERFORMANCE.md.
library;

import 'dart:developer' show Timeline;
import 'dart:io' show stderr;
import 'dart:ui' as ui;

import 'package:flutter/scheduler.dart';
import 'package:flutter/services.dart';

/// True in builds made with `--dart-define=LEGACY_PERF_PROBE=true`.
const bool perfProbeEnabled = bool.fromEnvironment('LEGACY_PERF_PROBE');

/// The probe. Call [start] once after `WidgetsFlutterBinding.ensureInitialized()`.
abstract final class PerfProbe {
  /// Frames whose build + raster time is above this are logged one by one.
  static double slowFrameMs = 20;

  /// Frames per summary line.
  static int summaryFrames = 120;

  /// Where lines go (stderr by default; replaceable in tests).
  static void Function(String line) log = stderr.writeln;

  static bool _started = false;
  static int _t0 = 0;
  static int? _keyUs;
  static String _key = '';
  static final List<double> _build = <double>[], _raster = <double>[], _total = <double>[];

  /// Starts logging slow frames, summaries and remote key latency.
  static void start() {
    if (!perfProbeEnabled || _started) return;
    _started = true;
    _t0 = Timeline.now;
    log('[perf] probe on');
    HardwareKeyboard.instance.addHandler(_onKey);
    SchedulerBinding.instance.addTimingsCallback(onTimings);
  }

  static bool _onKey(KeyEvent e) {
    if (e is KeyDownEvent && _keyUs == null) {
      _keyUs = Timeline.now;
      _key = e.logicalKey.keyLabel;
    }
    return false; // never consumes the key
  }

  /// Handles frame timings (public for tests).
  static void onTimings(List<ui.FrameTiming> timings) {
    for (final ui.FrameTiming t in timings) {
      final double b = t.buildDuration.inMicroseconds / 1000;
      final double r = t.rasterDuration.inMicroseconds / 1000;
      final double total = t.totalSpan.inMicroseconds / 1000;
      if (b + r > slowFrameMs) {
        final double at = (t.timestampInMicroseconds(ui.FramePhase.vsyncStart) - _t0) / 1e6;
        log('[perf] slow frame at ${at.toStringAsFixed(2)}s: build ${_f(b)} raster ${_f(r)} total ${_f(total)} ms');
      }
      final int? k = _keyUs;
      if (k != null && t.timestampInMicroseconds(ui.FramePhase.buildStart) >= k) {
        final int done = t.timestampInMicroseconds(ui.FramePhase.rasterFinish);
        log('[perf] key $_key at ${((k - _t0) / 1e6).toStringAsFixed(2)}s: ${_f((done - k) / 1000)} ms '
            'to a drawn frame (build ${_f(b)} raster ${_f(r)})');
        _keyUs = null;
      }
      _build.add(b);
      _raster.add(r);
      _total.add(total);
      if (_build.length >= summaryFrames) {
        log('[perf] summary ${_build.length} frames: build avg/p90/max ${_stat(_build)}  '
            'raster ${_stat(_raster)}  total ${_stat(_total)}');
        _build.clear();
        _raster.clear();
        _total.clear();
      }
    }
  }

  /// Wraps an image codec so the first frame's decode time is logged with
  /// [label], where the bytes came from ([source], e.g. `disk` or `net`),
  /// their size and how long fetching them took. Use it in an
  /// `ImageProvider`'s `loadImage`; returns [codec] unchanged when the probe
  /// is off.
  static ui.Codec timeCodec(ui.Codec codec, {required String label, String source = '', int bytes = 0, double fetchMs = 0}) =>
      perfProbeEnabled ? _TimedCodec(codec, label, source, bytes, fetchMs) : codec;

  static String _f(double v) => v.toStringAsFixed(1);

  static String _stat(List<double> v) {
    final List<double> s = <double>[...v]..sort();
    final double avg = s.reduce((double a, double b) => a + b) / s.length;
    return '${_f(avg)}/${_f(s[s.length * 9 ~/ 10])}/${_f(s.last)}';
  }
}

class _TimedCodec implements ui.Codec {
  _TimedCodec(this._codec, this._label, this._source, this._bytes, this._fetchMs);
  final ui.Codec _codec;
  final String _label, _source;
  final int _bytes;
  final double _fetchMs;
  bool _logged = false;

  @override
  int get frameCount => _codec.frameCount;
  @override
  int get repetitionCount => _codec.repetitionCount;
  @override
  void dispose() => _codec.dispose();

  @override
  Future<ui.FrameInfo> getNextFrame() async {
    final Stopwatch sw = Stopwatch()..start();
    final ui.FrameInfo f = await _codec.getNextFrame();
    if (!_logged) {
      _logged = true;
      PerfProbe.log('[perf] image $_label: $_source ${(_bytes / 1024).round()} KB, '
          'fetch ${_fetchMs.toStringAsFixed(0)} ms, decode ${sw.elapsedMilliseconds} ms, ${f.image.width}x${f.image.height}');
    }
    return f;
  }
}
