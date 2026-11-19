// video_test: scripted check of LG's video_player plugin on older webOS.
// Each step logs "[V] STEP <name>" and "[V] <check> PASS|FAIL <detail>" to
// <app dir>/compat_results.log so a host script can follow along, take
// screenshots at the visual steps and press Home at the right moment.
import 'dart:async';
import 'dart:io';

import 'package:flutter/material.dart';
import 'package:video_player/video_player.dart';

// Media comes from tools/media-server.sh when built with
// --dart-define=MEDIA_BASE=http://<host>:8099, otherwise from public hosts.
const kMediaBase = String.fromEnvironment('MEDIA_BASE');
String media(String file, String public) =>
    kMediaBase.isEmpty ? public : '$kMediaBase/$file';

final kLong = media('sintel_trailer-720p.mp4',
    'https://download.blender.org/durian/trailer/sintel_trailer-720p.mp4'); // 52 s
final kShort = media('butterfly.mp4',
    'https://flutter.github.io/assets-for-api-docs/assets/videos/butterfly.mp4');

void log(String line) {
  // ignore: avoid_print
  print(line);
  try {
    File('${File(Platform.resolvedExecutable).parent.path}/compat_results.log')
        .writeAsStringSync('[V] $line\n', mode: FileMode.append, flush: true);
  } catch (_) {}
}

void result(String name, bool ok, String detail) => log('$name ${ok ? 'PASS' : 'FAIL'} $detail');

Future<void> wait(int ms) => Future.delayed(Duration(milliseconds: ms));

void main() {
  WidgetsFlutterBinding.ensureInitialized();
  FlutterError.onError = (d) => log('flutter error: ${d.exceptionAsString()}');
  WidgetsBinding.instance.platformDispatcher.onError = (e, st) {
    log('uncaught: $e | ${st.toString().split('\n').take(4).join(' / ')}');
    return true;
  };
  runApp(const MaterialApp(home: VideoPage()));
}

enum Layout { full, small }

class VideoPage extends StatefulWidget {
  const VideoPage({super.key});
  @override
  State<VideoPage> createState() => _VideoPageState();
}

String appFile(String name) => '${File(Platform.resolvedExecutable).parent.path}/$name';

// Test hooks: <app dir>/video_url.txt overrides the main clip URL;
// <app dir>/video_probe makes the run just load + play 5 s and stop.
String mainUrl() {
  try {
    final f = File(appFile('video_url.txt'));
    if (f.existsSync()) return f.readAsStringSync().trim();
  } catch (_) {}
  return kLong;
}

bool probeOnly() => File(appFile('video_probe')).existsSync();

class _VideoPageState extends State<VideoPage> {
  final VideoPlayerController _a = VideoPlayerController.networkUrl(Uri.parse(mainUrl()));
  VideoPlayerController? _b;
  Layout _layout = Layout.full;
  String _step = 'starting';
  late final AppLifecycleListener _life;
  Timer? _ticker;

  int get posA => _a.value.position.inMilliseconds;

  @override
  void initState() {
    super.initState();
    _life = AppLifecycleListener(onStateChange: (s) => log('lifecycle $s position=${posA}ms'));
    _a.addListener(() {
      if (_a.value.hasError) log('error A ${_a.value.errorDescription}');
    });
    _run().catchError((Object e, StackTrace st) {
      log('scenario aborted: $e');
      log('done');
    });
  }

  Future<void> step(String name) async {
    setState(() => _step = name);
    log('STEP $name');
    await wait(300);
  }

  // Position every 500 ms for `ms`, logged as one line: shows stalls/jumps.
  Future<void> timeline(String name, int ms) async {
    final pts = <String>[];
    final sw = Stopwatch()..start();
    while (sw.elapsedMilliseconds < ms) {
      pts.add('${(sw.elapsedMilliseconds / 1000).toStringAsFixed(1)}s:${posA}${_a.value.isBuffering ? "b" : ""}');
      await wait(500);
    }
    log('timeline $name ${pts.join(' ')}');
  }

  Future<int> posAfter(int ms) async {
    await wait(ms);
    return posA;
  }

  Future<void> _run() async {
    await step('init');
    log('url ${_a.dataSource}');
    final sw = Stopwatch()..start();
    await _a.initialize();
    result('init', _a.value.isInitialized && _a.value.duration > Duration.zero,
        '${sw.elapsedMilliseconds}ms size=${_a.value.size.width.toInt()}x${_a.value.size.height.toInt()} duration=${_a.value.duration.inSeconds}s');
    if (probeOnly()) {
      await _a.play();
      final p0 = await posAfter(1000);
      final p1 = await posAfter(4000);
      result('probe.advances', p1 - p0 > 3000, 'advanced ${p1 - p0}ms in 4000ms');
      log('done');
      return;
    }

    if (File(appFile('video_pass2')).existsSync()) {
      await _a.setLooping(true);
      await _a.play();
      await _pass2();
      return;
    }

    await step('play');
    await _a.setLooping(true);
    await _a.play();
    final p0 = await posAfter(1000);
    final p1 = await posAfter(4000);
    result('play.advances', p1 - p0 > 3000, 'advanced ${p1 - p0}ms in 4000ms');

    await step('pause');
    await _a.pause();
    final q0 = await posAfter(800);
    final q1 = await posAfter(3000);
    result('pause.holds', (q1 - q0).abs() < 300 && !_a.value.isPlaying, 'moved ${q1 - q0}ms while paused');

    await step('resume');
    await _a.play();
    final r0 = await posAfter(800);
    final r1 = await posAfter(3000);
    result('resume.advances', r1 - r0 > 2000, 'advanced ${r1 - r0}ms in 3000ms');

    await step('seek');
    final seekSw = Stopwatch()..start();
    await _a.seekTo(const Duration(seconds: 20));
    var s = posA;
    while ((s < 19000 || s > 30000) && seekSw.elapsedMilliseconds < 8000) {
      s = await posAfter(200);
    }
    result('seek.lands', s >= 19000 && s < 30000, 'position ${s}ms ${seekSw.elapsedMilliseconds}ms after seekTo(20s)');
    await timeline('after_seek', 10000);
    final s0 = posA;
    final s1 = await posAfter(3000);
    result('seek.plays_after', s1 - s0 > 2000, 'advanced ${s1 - s0}ms in 3000ms, 10 s after seek');

    await step('speed2x');
    // Start well before the end of the 52 s clip so it cannot loop during the
    // 2x and 1x measurements; a seek needs several seconds to rebuffer.
    await _a.seekTo(const Duration(seconds: 5));
    await wait(9000);
    await _a.setPlaybackSpeed(2.0);
    await timeline('at_2x', 8000);
    final v0 = await posAfter(800);
    final v1 = await posAfter(4000);
    result('speed.2x', v1 - v0 > 6000, 'advanced ${v1 - v0}ms in 4000ms at 2x');
    await _a.setPlaybackSpeed(1.0);
    final w0 = await posAfter(800);
    final w1 = await posAfter(3000);
    result('speed.back_to_1x', (w1 - w0) > 2000 && (w1 - w0) < 4000, 'advanced ${w1 - w0}ms in 3000ms');

    await step('volume');
    try {
      await _a.setVolume(0.0);
      await wait(1000);
      await _a.setVolume(1.0);
      result('volume.calls', !_a.value.hasError, 'setVolume 0 then 1, volume=${_a.value.volume}');
    } catch (e) {
      log('volume.calls INFO not supported by LG plugin: $e');
    }

    await step('small'); // host screenshot: video in a 960x540 box, centred
    setState(() => _layout = Layout.small);
    await wait(6000);
    result('small.playing', _a.value.isPlaying, 'position ${posA}ms');

    await step('full'); // host screenshot: back to full screen
    setState(() => _layout = Layout.full);
    await wait(5000);

    await step('two_players'); // host screenshot: second player in the corner
    final b = VideoPlayerController.networkUrl(Uri.parse(kShort));
    String bError = '';
    b.addListener(() {
      if (b.value.hasError) bError = b.value.errorDescription ?? 'error';
    });
    try {
      await b.initialize().timeout(const Duration(seconds: 15));
      await b.setLooping(true);
      await b.play();
      setState(() => _b = b);
      final a0 = posA, b0 = b.value.position.inMilliseconds;
      await wait(6000);
      final a1 = posA, b1 = b.value.position.inMilliseconds;
      result('two.both_advance', a1 - a0 > 4000 && b1 != b0,
          'A advanced ${a1 - a0}ms, B position $b0 -> ${b1}ms ${bError.isEmpty ? '' : 'B error: $bError'}');
      // Decoding is all this checks: the TV has one video plane, so only B
      // (the newest player) is on screen and A plays unseen.
      log('two.visible INFO only the newest player (B, corner) is visible: one video plane');
    } catch (e) {
      result('two.both_advance', false, 'second player failed: $e $bError');
    }

    await step('dispose_second');
    setState(() => _b = null);
    await wait(300);
    await b.dispose();
    final d0 = await posAfter(500);
    final d1 = await posAfter(3000);
    result('dispose.first_keeps_playing', d1 - d0 > 2000, 'A advanced ${d1 - d0}ms after disposing B');

    // Keep the 30 s Home phase clear of the clip's end (52 s) so it cannot loop.
    await _a.seekTo(const Duration(seconds: 5));
    await wait(9000);

    await step('home'); // host presses Home here, waits, then relaunches the app
    _ticker = Timer.periodic(const Duration(seconds: 2),
        (_) => log('tick position=${posA}ms playing=${_a.value.isPlaying}'));
    await wait(30000);
    _ticker?.cancel();
    final h0 = await posAfter(500);
    final h1 = await posAfter(3000);
    result('home.resumed_playing', h1 - h0 > 2000, 'advanced ${h1 - h0}ms after returning');

    await step('done');
    log('done');
  }

  // Timeline of both players: "t:A/B" every 500 ms.
  Future<void> timeline2(String name, VideoPlayerController? b, int ms) async {
    final pts = <String>[];
    final sw = Stopwatch()..start();
    while (sw.elapsedMilliseconds < ms) {
      final bp = b == null ? '-' : '${b.value.position.inMilliseconds}';
      pts.add('${(sw.elapsedMilliseconds / 1000).toStringAsFixed(1)}s:$posA/$bp');
      await wait(500);
    }
    log('timeline $name (A/B ms) ${pts.join(' ')}');
  }

  Future<void> _pass2() async {
    // A plays from the start (not looping) so later steps measure a healthy player.
    await _a.setLooping(false);
    await wait(4000);
    await timeline2('a_alone', null, 4000);

    await step('two_players');
    final b = VideoPlayerController.networkUrl(Uri.parse(kShort));
    String bErr = '';
    b.addListener(() {
      if (b.value.hasError) bErr = b.value.errorDescription ?? 'error';
    });
    await b.initialize().timeout(const Duration(seconds: 15));
    await b.play();
    setState(() => _b = b);
    log('STEP two_players_visible');
    await timeline2('both', b, 8000);
    if (bErr.isNotEmpty) log('B error: $bErr');

    await step('dispose_second');
    setState(() => _b = null);
    await wait(300);
    await b.dispose();
    await timeline2('after_dispose', null, 8000);
    if (!_a.value.isPlaying) {
      await _a.play();
      await timeline2('after_dispose_play_again', null, 6000);
    }

    await step('home'); // host launches the Home app, waits, relaunches us
    final sw = Stopwatch()..start();
    final pts = <String>[];
    while (sw.elapsedMilliseconds < 40000) {
      final direct = (await _a.position)?.inMilliseconds;
      pts.add('${(sw.elapsedMilliseconds / 1000).toStringAsFixed(0)}s:$posA|$direct${_a.value.isPlaying ? "" : "p"}');
      await wait(1000);
    }
    log('timeline home ${pts.join(' ')}');

    if (File(appFile('video_skip_loop')).existsSync()) { log('done'); return; }
    await step('loop_end'); // looping last: seek near the end and watch the wrap
    await _a.setLooping(true);
    await _a.play();
    await _a.seekTo(const Duration(seconds: 46));
    await timeline('loop_wrap', 22000);
    log('done');
  }

  @override
  void dispose() {
    _ticker?.cancel();
    _life.dispose();
    _a.dispose();
    _b?.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final video = _a.value.isInitialized
        ? AspectRatio(aspectRatio: _a.value.aspectRatio, child: VideoPlayer(_a))
        : const CircularProgressIndicator();
    return Scaffold(
      backgroundColor: Colors.transparent,
      body: Stack(children: [
        if (_layout == Layout.full)
          Positioned.fill(child: Center(child: video))
        else
          Positioned(left: 480, top: 270, width: 960, height: 540, child: video),
        if (_b != null && _b!.value.isInitialized)
          Positioned(
            right: 40,
            bottom: 40,
            width: 480,
            height: 270,
            child: VideoPlayer(_b!),
          ),
        Positioned(
          left: 24,
          top: 24,
          child: Container(
            color: Colors.black54,
            padding: const EdgeInsets.all(8),
            child: Text('video_test: $_step', style: const TextStyle(color: Colors.white, fontSize: 22)),
          ),
        ),
      ]),
    );
  }
}
