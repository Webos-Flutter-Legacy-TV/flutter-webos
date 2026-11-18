// soloud_test: checks audioplayers_soloud (SoLoud mixer + ALSA output) on
// older webOS releases. Each result is "[S] <check> PASS|FAIL|INFO <detail>",
// shown on screen and appended to <app dir>/compat_results.log (print() does
// not reach stdout on webOS). Audibility is not checked here; the host can
// look at the PulseAudio sink inputs while a "STEP" line is showing.
import 'dart:async';
import 'dart:io';

import 'package:audioplayers/audioplayers.dart';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

// Media comes from tools/media-server.sh when built with
// --dart-define=MEDIA_BASE=http://<host>:8099, otherwise from public hosts.
const kMediaBase = String.fromEnvironment('MEDIA_BASE');
String media(String file, String public) =>
    kMediaBase.isEmpty ? public : '$kMediaBase/$file';

final kMp3Url = media('sample-15s.mp3', 'https://download.samplelib.com/mp3/sample-15s.mp3');
final kWavUrl = media('sample-15s.wav', 'https://download.samplelib.com/wav/sample-15s.wav');

final results = ValueNotifier<List<String>>([]);

String _clip(Object o, [int n = 220]) {
  final s = o.toString().replaceAll('\n', ' ');
  return s.length > n ? '${s.substring(0, n)}...' : s;
}

void log(String line) {
  final l = '[S] $line';
  results.value = [...results.value, l];
  try {
    File('${File(Platform.resolvedExecutable).parent.path}/compat_results.log')
        .writeAsStringSync('$l\n', mode: FileMode.append, flush: true);
  } catch (_) {}
}

Future<void> check(String name, Future<String> Function() body,
    {Duration timeout = const Duration(seconds: 20)}) async {
  final sw = Stopwatch()..start();
  try {
    final detail = await body().timeout(timeout);
    log('$name PASS ${_clip(detail)} (${sw.elapsedMilliseconds} ms)');
  } catch (e) {
    log('$name FAIL ${_clip(e)}');
  }
}

Future<void> wait(int ms) => Future.delayed(Duration(milliseconds: ms));

Future<int> posMs(AudioPlayer p) async =>
    (await p.getCurrentPosition())?.inMilliseconds ?? -1;

/// Position advance over [ms] of wall time.
Future<int> advance(AudioPlayer p, int ms) async {
  final a = await posMs(p);
  await wait(ms);
  return await posMs(p) - a;
}

/// The plugin completes setSource() before a URL has been downloaded and
/// decoded; the duration event marks the point where it is really loaded.
Future<Duration> loaded(AudioPlayer p, {int timeoutS = 45}) => p
    .onDurationChanged
    .firstWhere((d) => d > Duration.zero)
    .timeout(Duration(seconds: timeoutS));

/// Plays [src] and requires the position to move by most of the wall time.
Future<String> playsAndAdvances(Source src) async {
  final p = AudioPlayer();
  final logs = <String>[];
  final sub = p.onLog.listen(logs.add);
  try {
    final sw = Stopwatch()..start();
    final ready = loaded(p);
    await p.play(src);
    final dur = await ready;
    final loadMs = sw.elapsedMilliseconds;
    await wait(300);
    final adv = await advance(p, 1500);
    if (adv < 1000) {
      throw 'advanced ${adv}ms in 1500ms state=${p.state} dur=$dur '
          '${logs.isEmpty ? '' : logs.last}';
    }
    return 'loaded in ${loadMs}ms, advanced ${adv}ms in 1500ms, '
        'duration=$dur';
  } finally {
    await sub.cancel();
    await p.dispose();
  }
}

void main() {
  WidgetsFlutterBinding.ensureInitialized();
  WidgetsBinding.instance.platformDispatcher.onError = (e, st) {
    log('uncaught INFO ${_clip(e)}');
    return true;
  };
  runApp(const MaterialApp(home: SoloudPage()));
}

class SoloudPage extends StatefulWidget {
  const SoloudPage({super.key});
  @override
  State<SoloudPage> createState() => _SoloudPageState();
}

class _SoloudPageState extends State<SoloudPage> {
  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addPostFrameCallback((_) => _run());
  }

  Future<void> _run() async {
    log('run INFO start');

    // ---- sources and formats
    await check('asset.wav', () => playsAndAdvances(AssetSource('tone5s.wav')));
    await check('asset.mp3', () async {
      // coins.mp3 is ~0.5 s: require a duration and a completion event.
      final p = AudioPlayer();
      try {
        final done = p.onPlayerComplete.first;
        final ready = loaded(p, timeoutS: 5);
        await p.play(AssetSource('coins.mp3'));
        final dur = await ready;
        await done.timeout(const Duration(seconds: 5));
        return 'completed, duration=$dur';
      } finally {
        await p.dispose();
      }
    });
    await check('asset.opus', () => playsAndAdvances(
        AssetSource('sample3.opus', mimeType: 'audio/ogg; codecs=opus')));
    await check('bytes.wav', () async {
      final b = await rootBundle.load('assets/tone5s.wav');
      return playsAndAdvances(BytesSource(b.buffer.asUint8List()));
    });
    await check('file.wav', () async {
      final b = await rootBundle.load('assets/tone5s.wav');
      final f = File('${Directory.systemTemp.path}/soloud_tone.wav');
      await f.writeAsBytes(b.buffer.asUint8List());
      return playsAndAdvances(DeviceFileSource(f.path));
    });
    await check('url.mp3', () => playsAndAdvances(UrlSource(kMp3Url)),
        timeout: const Duration(seconds: 40));
    await check('url.wav', () => playsAndAdvances(UrlSource(kWavUrl)),
        timeout: const Duration(seconds: 60));
    await check('url.bad_host', () async {
      final p = AudioPlayer();
      try {
        final err = Completer<String>();
        final sub = p.eventStream.listen((_) {}, onError: (Object e) {
          if (!err.isCompleted) err.complete('$e');
        });
        await p.setSource(UrlSource('https://no-such-host.invalid/a.mp3'));
        final e = await err.future.timeout(const Duration(seconds: 30));
        await sub.cancel();
        return 'error event: ${_clip(e, 120)}';
      } finally {
        await p.dispose();
      }
    }, timeout: const Duration(seconds: 40));

    // ---- controls on one player (network MP3, ~19 s)
    final p = AudioPlayer();
    final logs = <String>[];
    final sub = p.onLog.listen(logs.add);
    String last() => logs.isEmpty ? '' : ' log=${logs.last}';
    var dur = Duration.zero;
    await check('control.load', () async {
      final ready = loaded(p);
      await p.setSource(UrlSource(kMp3Url));
      dur = await ready;
      if (dur.inSeconds < 10) throw 'duration=$dur${last()}';
      return 'duration=$dur';
    }, timeout: const Duration(seconds: 50));
    await check('control.play', () async {
      await p.resume();
      await wait(1000);
      final adv = await advance(p, 2000);
      if (adv < 1500) throw 'advanced ${adv}ms in 2000ms${last()}';
      return 'advanced ${adv}ms in 2000ms, state=${p.state}';
    });
    log('STEP playing');
    await check('control.pause', () async {
      await p.pause();
      await wait(300);
      final adv = await advance(p, 1000);
      if (adv.abs() > 100) throw 'moved ${adv}ms while paused';
      return 'held, state=${p.state}';
    });
    await check('control.resume', () async {
      await p.resume();
      await wait(500);
      final adv = await advance(p, 1000);
      if (adv < 700) throw 'advanced ${adv}ms in 1000ms${last()}';
      return 'advanced ${adv}ms in 1000ms';
    });
    await check('control.seek', () async {
      await p.seek(const Duration(seconds: 10));
      await wait(500);
      final pos = await posMs(p);
      if (pos < 9800 || pos > 11500) throw 'position ${pos}ms after seek to 10s';
      return 'position ${pos}ms after seek to 10s';
    });
    await check('control.rate', () async {
      await p.seek(const Duration(seconds: 2));
      await p.setPlaybackRate(2.0);
      await wait(300);
      final fast = await advance(p, 1500);
      await p.setPlaybackRate(1.0);
      await wait(300);
      final normal = await advance(p, 1500);
      if (fast < 2400 || normal > 1900) {
        throw '2x advanced ${fast}ms, 1x advanced ${normal}ms (in 1500ms)';
      }
      return '2x advanced ${fast}ms, 1x advanced ${normal}ms (in 1500ms)';
    });
    await check('control.volume_balance', () async {
      await p.setVolume(0.2);
      await p.setBalance(-1.0);
      await wait(300);
      await p.setBalance(0.0);
      await p.setVolume(1.0);
      final adv = await advance(p, 500);
      if (adv < 300) throw 'stalled after volume/balance (${adv}ms)';
      return 'calls accepted, still playing';
    });
    await check('control.complete', () async {
      final done = p.onPlayerComplete.first;
      await p.seek(dur - const Duration(seconds: 2));
      await done.timeout(const Duration(seconds: 6));
      return 'completed, state=${p.state}';
    });
    await check('control.replay_after_complete', () async {
      // Default ReleaseMode.release: the source must survive completion.
      await p.resume();
      await wait(500);
      final adv = await advance(p, 1000);
      if (adv < 700) throw 'advanced ${adv}ms in 1000ms after resume';
      return 'advanced ${adv}ms in 1000ms after resume';
    });
    await check('control.loop', () async {
      await p.setReleaseMode(ReleaseMode.loop);
      await p.resume();
      await p.seek(dur - const Duration(milliseconds: 1500));
      await wait(3500);
      final pos = await posMs(p);
      await p.setReleaseMode(ReleaseMode.stop);
      if (p.state != PlayerState.playing || pos < 0 || pos > 3000) {
        throw 'state=${p.state} position=${pos}ms after wrapping';
      }
      return 'wrapped, position=${pos}ms';
    });
    await check('control.stop', () async {
      await p.stop();
      await wait(300);
      final pos = await posMs(p);
      return 'state=${p.state} position=${pos}ms';
    });
    await sub.cancel();
    await p.dispose();

    // ---- mixing: SoLoud mixes in-process, so several players at once.
    await check('mix.three_players', () async {
      final ps = [AudioPlayer(), AudioPlayer(), AudioPlayer()];
      try {
        final ready = loaded(ps[0]);
        await ps[0].play(UrlSource(kMp3Url));
        await ready;
        await ps[1].play(AssetSource('sample3.opus',
            mimeType: 'audio/ogg; codecs=opus'));
        await ps[2].play(AssetSource('tone5s.wav'));
        await wait(1000);
        log('STEP mixing');
        final advs = await Future.wait(ps.map((x) => advance(x, 1500)));
        if (advs.any((a) => a < 1000)) throw 'advances $advs ms in 1500ms';
        return 'advances $advs ms in 1500ms';
      } finally {
        for (final x in ps) {
          await x.dispose();
        }
      }
    }, timeout: const Duration(seconds: 60));

    // ---- low latency: short effect fired repeatedly.
    await check('latency.retrigger', () async {
      final fx = AudioPlayer();
      try {
        await fx.setPlayerMode(PlayerMode.lowLatency);
        await fx.setSource(AssetSource('coins.wav'));
        final sw = Stopwatch()..start();
        final times = <int>[];
        for (var i = 0; i < 5; i++) {
          sw.reset();
          await fx.stop();
          await fx.resume();
          times.add(sw.elapsedMilliseconds);
          await wait(250);
        }
        return 'stop+resume took $times ms';
      } finally {
        await fx.dispose();
      }
    });

    final fails = results.value.where((r) => r.contains(' FAIL ')).length;
    log('run INFO done, $fails failed');
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(title: const Text('webOS audioplayers_soloud test')),
      body: ValueListenableBuilder<List<String>>(
        valueListenable: results,
        builder: (context, lines, _) => ListView(
          padding: const EdgeInsets.all(12),
          children: [
            for (final l in lines)
              Text(l,
                  style: TextStyle(
                      fontSize: 13,
                      color: l.contains(' FAIL ')
                          ? Colors.red.shade700
                          : l.contains(' PASS ')
                              ? Colors.green.shade800
                              : Colors.black87)),
          ],
        ),
      ),
    );
  }
}
