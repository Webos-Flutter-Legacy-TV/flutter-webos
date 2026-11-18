// compat_test: self-checking Flutter app for older webOS releases.
// Every result is printed as "[T] <name> PASS|FAIL|INFO <detail>" so it lands
// in compat.log (stdout is redirected by the compat shim) and can be read
// over SSH, and is also shown on screen.
import 'dart:async';
import 'dart:io';
import 'dart:math' as math;
import 'dart:ui' show FrameTiming, FramePhase, PlatformDispatcher;

import 'package:flutter/material.dart';
import 'package:flutter/scheduler.dart';
import 'package:path_provider/path_provider.dart' as pp;
import 'package:shared_preferences/shared_preferences.dart';
import 'package:sqflite/sqflite.dart';

void main() => runApp(const CompatTestApp());

class TestResult {
  TestResult(this.name, this.status, this.detail);
  final String name;
  final String status; // PASS | FAIL | INFO
  final String detail;
}

final List<TestResult> results = [];
final ValueNotifier<int> resultsVersion = ValueNotifier(0);

void report(String name, String status, String detail) {
  results.add(TestResult(name, status, detail));
  resultsVersion.value++;
  final line = '[T] $name $status $detail';
  // ignore: avoid_print
  print(line);
  // LG's embedder does not route print() to stdout; also append to a file
  // next to the executable so results can be read over SSH.
  try {
    File('${File(Platform.resolvedExecutable).parent.path}/compat_results.log')
        .writeAsStringSync('$line\n', mode: FileMode.append, flush: true);
  } catch (_) {}
}

Future<void> check(String name, Future<String> Function() body,
    {Duration timeout = const Duration(seconds: 10)}) async {
  final sw = Stopwatch()..start();
  try {
    final detail = await body().timeout(timeout);
    report(name, 'PASS', '$detail (${sw.elapsedMilliseconds} ms)');
  } catch (e) {
    final msg = e.toString().replaceAll('\n', ' ');
    report(name, 'FAIL', msg.length > 220 ? '${msg.substring(0, 220)}...' : msg);
  }
}

// --------------------------------------------------------------------------
// 1. Environment + networking + file I/O
// --------------------------------------------------------------------------
Future<void> runBasicChecks() async {
  report('env.os', 'INFO', Platform.operatingSystemVersion);
  report('env.cpus', 'INFO', '${Platform.numberOfProcessors}');
  report('env.locale', 'INFO', Platform.localeName);
  report('env.tz', 'INFO',
      '${DateTime.now().timeZoneName} offset=${DateTime.now().timeZoneOffset}');
  report('env.dart', 'INFO', Platform.version.split(' ').first);
  report('env.exe', 'INFO', Platform.resolvedExecutable);
  for (final d in PlatformDispatcher.instance.displays) {
    report('env.display', 'INFO',
        'id=${d.id} size=${d.size.width.toInt()}x${d.size.height.toInt()} dpr=${d.devicePixelRatio} refreshRate=${d.refreshRate}');
  }
  for (final v in PlatformDispatcher.instance.views) {
    report('env.view', 'INFO',
        'physicalSize=${v.physicalSize.width.toInt()}x${v.physicalSize.height.toInt()} dpr=${v.devicePixelRatio}');
  }

  await check('fs.tmp_write', () async {
    final f = File('${Directory.systemTemp.path}/compat_test.txt');
    await f.writeAsString('hello ${DateTime.now()}');
    final back = await f.readAsString();
    await f.delete();
    return '${Directory.systemTemp.path} ok, ${back.length} bytes';
  });

  await check('fs.app_dir_write', () async {
    final dir = File(Platform.resolvedExecutable).parent;
    final f = File('${dir.path}/compat_test_write.txt');
    await f.writeAsString('x');
    await f.delete();
    return dir.path;
  });

  await check('fs.ssl_certs', () async {
    const candidates = [
      '/etc/ssl/certs/ca-certificates.crt',
      '/etc/ssl/certs',
      '/etc/pki/tls/certs/ca-bundle.crt',
      '/usr/share/ca-certificates',
    ];
    final found = <String>[];
    for (final c in candidates) {
      if (FileSystemEntity.typeSync(c) != FileSystemEntityType.notFound) {
        found.add(c);
      }
    }
    if (found.isEmpty) throw 'no CA bundle found in ${candidates.join(", ")}';
    return found.join(', ');
  });

  await check('net.dns', () async {
    final addrs = await InternetAddress.lookup('example.com');
    return addrs.map((a) => a.address).take(3).join(', ');
  });

  await check('net.http', () async {
    final client = HttpClient();
    final req = await client.getUrl(Uri.parse('http://example.com/'));
    final res = await req.close();
    final len = await res.fold<int>(0, (n, chunk) => n + chunk.length);
    client.close();
    return 'HTTP ${res.statusCode}, $len bytes';
  });

  await check('net.https', () async {
    final client = HttpClient();
    final req = await client.getUrl(Uri.parse('https://example.com/'));
    final res = await req.close();
    final len = await res.fold<int>(0, (n, chunk) => n + chunk.length);
    client.close();
    return 'HTTP ${res.statusCode}, $len bytes';
  });

  await check('net.https_no_verify', () async {
    // Distinguishes "TLS works but no CA store" from "TLS broken".
    final client = HttpClient()..badCertificateCallback = (_, __, ___) => true;
    final req = await client.getUrl(Uri.parse('https://example.com/'));
    final res = await req.close();
    await res.drain<void>();
    client.close();
    return 'HTTP ${res.statusCode}';
  });

  await check('isolate.compute', () async {
    final r = await Future(() {
      var x = 0;
      for (var i = 0; i < 2000000; i++) {
        x = (x + i * 31) % 1000003;
      }
      return x;
    });
    return 'result $r';
  });

  // Text layout micro-benchmark: N unique paragraphs, timed per layout.
  for (final family in [null, 'DejaVuSans']) {
    for (final size in [14.0, 16.0]) {
      final label = '${family ?? "system"}_${size.toInt()}';
      final sw = Stopwatch()..start();
      const n = 200;
      for (var i = 0; i < n; i++) {
        final tp = TextPainter(
          text: TextSpan(
              text: 'Row $i - the quick brown fox jumps over the lazy dog',
              style: TextStyle(fontFamily: family, fontSize: size, letterSpacing: 0.5)),
          textDirection: TextDirection.ltr,
        )..layout(maxWidth: 1500);
        tp.dispose();
      }
      report('text.layout_$label', 'INFO',
          '${(sw.elapsedMicroseconds / n).toStringAsFixed(0)} us/paragraph over $n unique');
      // Same paragraph laid out again: shows whether results are cached.
      final sw2 = Stopwatch()..start();
      for (var i = 0; i < n; i++) {
        final tp = TextPainter(
          text: TextSpan(text: 'Row 1 - the quick brown fox jumps over the lazy dog',
              style: TextStyle(fontFamily: family, fontSize: size, letterSpacing: 0.5)),
          textDirection: TextDirection.ltr,
        )..layout(maxWidth: 1500);
        tp.dispose();
      }
      report('text.relayout_$label', 'INFO',
          '${(sw2.elapsedMicroseconds / n).toStringAsFixed(0)} us/paragraph, same text');
    }
  }

  report('mem.rss_after_basic', 'INFO', '${ProcessInfo.currentRss ~/ (1024 * 1024)} MB');
}


// --------------------------------------------------------------------------
// 3. LG webOS plugins
// --------------------------------------------------------------------------
Future<void> runPluginChecks() async {
  report('env.FLUTTER_HOME', 'INFO', Platform.environment['FLUTTER_HOME'] ?? '(unset)');
  report('env.FLUTTER_APP_ID', 'INFO', Platform.environment['FLUTTER_APP_ID'] ?? '(unset)');

  Future<String> dirCheck(Future<Directory?> Function() get) async {
    final d = await get();
    if (d == null) throw 'null directory';
    await d.create(recursive: true);
    final f = File('${d.path}/probe.txt');
    await f.writeAsString('ok');
    await f.delete();
    return d.path;
  }

  await check('plugin.path.temp', () => dirCheck(pp.getTemporaryDirectory));
  await check('plugin.path.support', () => dirCheck(pp.getApplicationSupportDirectory));
  await check('plugin.path.documents', () => dirCheck(pp.getApplicationDocumentsDirectory));
  await check('plugin.path.downloads', () => dirCheck(pp.getDownloadsDirectory));

  await check('plugin.prefs.roundtrip', () async {
    final prefs = await SharedPreferences.getInstance();
    final launches = (prefs.getInt('launches') ?? 0) + 1;
    await prefs.setInt('launches', launches);
    await prefs.setString('last', DateTime.now().toIso8601String());
    await prefs.setStringList('list', ['a', 'b', 'c']);
    await prefs.reload();
    if (prefs.getInt('launches') != launches) throw 'int mismatch';
    if (prefs.getStringList('list')?.join() != 'abc') throw 'list mismatch';
    return 'launch #$launches (persists across runs if > 1)';
  });

  await check('plugin.sqflite.crud', () async {
    final dir = await getDatabasesPath();
    final db = await openDatabase('$dir/compat.db', version: 1,
        onCreate: (db, v) => db.execute('CREATE TABLE t(id INTEGER PRIMARY KEY, name TEXT)'));
    await db.delete('t');
    final batch = db.batch();
    for (var i = 0; i < 500; i++) {
      batch.insert('t', {'name': 'row $i'});
    }
    await batch.commit(noResult: true);
    final rows = await db.rawQuery('SELECT COUNT(*) AS c FROM t WHERE name LIKE ?', ['row 1%']);
    final total = Sqflite.firstIntValue(await db.rawQuery('SELECT COUNT(*) FROM t'));
    await db.close();
    return 'db=$dir/compat.db total=$total like=${rows.first['c']}';
  });
}

// --------------------------------------------------------------------------
// 2. Performance: animate N tiles for a fixed time and collect frame timings
// --------------------------------------------------------------------------
class PerfStats {
  final build = <double>[];
  final raster = <double>[];
  final total = <double>[];
  final vsync = <int>[]; // vsyncStart timestamps, microseconds

  void add(List<FrameTiming> timings) {
    for (final t in timings) {
      vsync.add(t.timestampInMicroseconds(FramePhase.vsyncStart));
      build.add(t.buildDuration.inMicroseconds / 1000);
      raster.add(t.rasterDuration.inMicroseconds / 1000);
      total.add(t.totalSpan.inMicroseconds / 1000);
    }
  }

  static double pct(List<double> v, double p) {
    if (v.isEmpty) return 0;
    final s = [...v]..sort();
    return s[math.min(s.length - 1, (s.length * p).floor())];
  }

  static double avg(List<double> v) =>
      v.isEmpty ? 0 : v.reduce((a, b) => a + b) / v.length;

  String summary(Duration elapsed) {
    final fps = total.length / (elapsed.inMilliseconds / 1000);
    final slow = total.where((t) => t > 16.7).length;
    String f(double d) => d.toStringAsFixed(1);
    // Spacing between consecutive vsyncs the engine was given: the median shows
    // the real display refresh (16.7 ms = 60 Hz, 20 ms = 50 Hz).
    final iv = <double>[];
    for (var i = 1; i < vsync.length; i++) {
      final d = (vsync[i] - vsync[i - 1]) / 1000;
      if (d > 0) iv.add(d);
    }
    final minIv = iv.isEmpty ? 0.0 : iv.reduce(math.min);
    return 'frames=${total.length} fps=${f(fps)} '
        'vsync-interval median/min=${f(pct(iv, .5))}/${f(minIv)}ms '
        'build avg/p90=${f(avg(build))}/${f(pct(build, .9))}ms '
        'raster avg/p90=${f(avg(raster))}/${f(pct(raster, .9))}ms '
        'janky(>16.7ms)=$slow '
        'max build/raster=${f(build.isEmpty ? 0 : build.reduce(math.max))}/${f(raster.isEmpty ? 0 : raster.reduce(math.max))}ms';
  }
}

class StressPage extends StatefulWidget {
  const StressPage({super.key, required this.tiles, required this.seconds, required this.onDone});
  final int tiles;
  final int seconds;
  final void Function(String summary) onDone;

  @override
  State<StressPage> createState() => _StressPageState();
}

class _StressPageState extends State<StressPage> with SingleTickerProviderStateMixin {
  late final AnimationController _c =
      AnimationController(vsync: this, duration: const Duration(seconds: 2))..repeat();
  final _stats = PerfStats();
  final _sw = Stopwatch();
  Timer? _timer;

  @override
  void initState() {
    super.initState();
    SchedulerBinding.instance.addTimingsCallback(_stats.add);
    _sw.start();
    _timer = Timer(Duration(seconds: widget.seconds), () {
      SchedulerBinding.instance.removeTimingsCallback(_stats.add);
      widget.onDone(_stats.summary(_sw.elapsed));
    });
  }

  @override
  void dispose() {
    _timer?.cancel();
    _c.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final cols = (math.sqrt(widget.tiles * 16 / 9)).ceil();
    return Scaffold(
      body: AnimatedBuilder(
        animation: _c,
        builder: (context, _) {
          final t = _c.value * 2 * math.pi;
          return GridView.count(
            crossAxisCount: cols,
            physics: const NeverScrollableScrollPhysics(),
            children: List.generate(widget.tiles, (i) {
              final phase = t + i * 0.3;
              return Transform.rotate(
                angle: phase,
                child: Opacity(
                  opacity: 0.5 + 0.5 * math.sin(phase).abs(),
                  child: Container(
                    margin: const EdgeInsets.all(4),
                    decoration: BoxDecoration(
                      borderRadius: BorderRadius.circular(12),
                      gradient: LinearGradient(colors: [
                        HSVColor.fromAHSV(1, (i * 37) % 360, .7, .9).toColor(),
                        HSVColor.fromAHSV(1, (i * 37 + 120) % 360, .7, .6).toColor(),
                      ]),
                      boxShadow: const [BoxShadow(blurRadius: 6, color: Colors.black26)],
                    ),
                    alignment: Alignment.center,
                    child: Text('$i', style: const TextStyle(color: Colors.white, fontSize: 18)),
                  ),
                ),
              );
            }),
          );
        },
      ),
    );
  }
}

class ScrollStressPage extends StatefulWidget {
  const ScrollStressPage({super.key, required this.seconds, required this.onDone, this.mode = 'listtile', this.pxPerSec = 1000});
  final int seconds;
  final double pxPerSec;
  final String mode; // listtile | plain | plain_dash
  final void Function(String summary) onDone;

  @override
  State<ScrollStressPage> createState() => _ScrollStressPageState();
}

class _ScrollStressPageState extends State<ScrollStressPage> {
  final _ctrl = ScrollController();
  final _stats = PerfStats();
  final _sw = Stopwatch();

  @override
  void initState() {
    super.initState();
    SchedulerBinding.instance.addTimingsCallback(_stats.add);
    _sw.start();
    WidgetsBinding.instance.addPostFrameCallback((_) => _run());
  }

  Future<void> _run() async {
    // Constant-speed scroll (like holding the D-pad): pxPerSec pixels per
    // second, driven by wall time so slow frames do not slow the scroll.
    final pxPerSec = widget.pxPerSec;
    final start = _sw.elapsedMicroseconds;
    while (_sw.elapsed.inSeconds < widget.seconds && mounted) {
      await SchedulerBinding.instance.endOfFrame;
      if (!mounted) break;
      final t = (_sw.elapsedMicroseconds - start) / 1e6;
      final max = _ctrl.position.maxScrollExtent;
      _ctrl.jumpTo(math.min(t * pxPerSec, max));
      SchedulerBinding.instance.scheduleFrame();
    }
    SchedulerBinding.instance.removeTimingsCallback(_stats.add);
    widget.onDone(_stats.summary(_sw.elapsed));
  }

  Widget _row(int i) {
    if (widget.mode.endsWith('_bundled')) {
      // Same row, but every text style uses the bundled DejaVu Sans instead of
      // the TV's system fonts (resolved through fontconfig).
      final base = widget.mode.substring(0, widget.mode.length - '_bundled'.length);
      final t = Theme.of(context);
      return Theme(
        data: t.copyWith(textTheme: t.textTheme.apply(fontFamily: 'DejaVuSans')),
        child: DefaultTextStyle.merge(
          style: const TextStyle(fontFamily: 'DejaVuSans'),
          child: _rowFor(base, i),
        ),
      );
    }
    return _rowFor(widget.mode, i);
  }

  Widget _rowFor(String mode, int i) {
    const title = 'the quick brown fox jumps over the lazy dog';
    switch (mode) {
      case 'plain':
        return SizedBox(height: 56, child: Text('Row $i - $title'));
      case 'plain_dash':
        return SizedBox(height: 56, child: Text('Row $i \u2014 $title'));
      case 'two_text': // title + subtitle, no ListTile
        return SizedBox(
          height: 72,
          child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
            Text('Row $i - $title'),
            Text('Subtitle text for row $i with some more words'),
          ]),
        );
      case 'tile_text': // ListTile with only title + subtitle
        return ListTile(
          title: Text('Row $i - $title'),
          subtitle: Text('Subtitle text for row $i with some more words'),
        );
      case 'tile_avatar':
        return ListTile(
          leading: CircleAvatar(child: Text('${i % 100}')),
          title: Text('Row $i - $title'),
        );
      case 'tile_icon':
        return ListTile(
          title: Text('Row $i - $title'),
          trailing: const Icon(Icons.chevron_right),
        );
      case 'row_manual': // same content as the full tile, built by hand
        return SizedBox(
          height: 72,
          child: Row(children: [
            CircleAvatar(child: Text('${i % 100}')),
            const SizedBox(width: 16),
            Expanded(
              child: Column(
                mainAxisAlignment: MainAxisAlignment.center,
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Text('Row $i \u2014 $title'),
                  Text('Subtitle text for row $i with some more words'),
                ],
              ),
            ),
            const Icon(Icons.chevron_right),
          ]),
        );
      default: // 'listtile': the original full ListTile
        return ListTile(
          leading: CircleAvatar(child: Text('${i % 100}')),
          title: Text('Row $i \u2014 $title'),
          subtitle: Text('Subtitle text for row $i with some more words'),
          trailing: const Icon(Icons.chevron_right),
        );
    }
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      body: ListView.builder(
        controller: _ctrl,
        itemCount: 2000,
        itemBuilder: (context, i) => _row(i),
      ),
    );
  }
}

// --------------------------------------------------------------------------
// App shell: run phases in order, then show a results list.
// --------------------------------------------------------------------------
class CompatTestApp extends StatelessWidget {
  const CompatTestApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'webOS compat test',
      theme: ThemeData(colorSchemeSeed: Colors.indigo, useMaterial3: true),
      home: const Runner(),
    );
  }
}

class Runner extends StatefulWidget {
  const Runner({super.key});
  @override
  State<Runner> createState() => _RunnerState();
}

class _RunnerState extends State<Runner> {
  String _phase = 'basic checks';
  Widget? _stress;

  @override
  void initState() {
    super.initState();
    _runAll();
  }

  Future<void> _stressRun(String name, Widget Function(void Function(String)) page) {
    final done = Completer<void>();
    setState(() {
      _phase = name;
      _stress = page((summary) {
        report(name, 'INFO', summary);
        report('$name.rss', 'INFO', '${ProcessInfo.currentRss ~/ (1024 * 1024)} MB');
        done.complete();
      });
    });
    return done.future;
  }

  Future<void> _runAll() async {
    report('run', 'INFO', 'start ${DateTime.now()}');
    await runBasicChecks();
    await runPluginChecks();
    // Frame-timing runs, only when the app ships an empty webOS meta file
    // `compat-perf` (installed next to the binary): they take ~30 s.
    if (File('${File(Platform.resolvedExecutable).parent.path}/compat-perf').existsSync()) {
      await _stressRun('perf.light', (done) => StressPage(tiles: 4, seconds: 8, onDone: done));
      await _stressRun('perf.scroll_plain',
          (done) => ScrollStressPage(seconds: 8, mode: 'plain', pxPerSec: 600, onDone: done));
      await _stressRun('perf.tiles64', (done) => StressPage(tiles: 64, seconds: 8, onDone: done));
    }
    final fails = results.where((r) => r.status == 'FAIL').length;
    report('run', 'INFO', 'done, ${results.length} results, $fails failed');
    setState(() {
      _phase = 'done';
      _stress = null;
    });
  }

  @override
  Widget build(BuildContext context) {
    if (_stress != null) return KeyedSubtree(key: ValueKey(_phase), child: _stress!);
    return Scaffold(
      appBar: AppBar(title: Text('webOS compat test — $_phase')),
      body: ValueListenableBuilder<int>(
        valueListenable: resultsVersion,
        builder: (context, _, __) => ListView(
          padding: const EdgeInsets.all(16),
          children: [
            for (final r in results)
              Padding(
                padding: const EdgeInsets.symmetric(vertical: 2),
                child: Text.rich(TextSpan(children: [
                  TextSpan(
                    text: '${r.status.padRight(5)} ',
                    style: TextStyle(
                      fontWeight: FontWeight.bold,
                      color: switch (r.status) {
                        'PASS' => Colors.green.shade700,
                        'FAIL' => Colors.red.shade700,
                        _ => Colors.blueGrey,
                      },
                    ),
                  ),
                  TextSpan(text: '${r.name}  ', style: const TextStyle(fontWeight: FontWeight.w600)),
                  TextSpan(text: r.detail),
                ])),
              ),
          ],
        ),
      ),
    );
  }
}
