// plugins_test: one check per LG webOS plugin, for older webOS releases.
// Each result is "[P] <plugin.check> PASS|FAIL|INFO <detail>", shown on screen
// and appended to <app dir>/compat_results.log (print() does not reach
// stdout on webOS). Not covered here: video_player (tests/video_test),
// path_provider/shared_preferences/sqflite (tests/compat_test),
// audioplayers_soloud (same audioplayers interface as audioplayers_webos,
// so it needs its own app), video_player_drm (needs a licence server) and
// flutterfire (needs a Firebase project).
import 'dart:async';
import 'dart:io';

import 'package:audioplayers/audioplayers.dart';
import 'package:connectivity_plus/connectivity_plus.dart';
import 'package:custom_mouse_cursor/custom_mouse_cursor.dart';
import 'package:device_info_plus_webos/device_info_plus_webos.dart';
import 'dart:convert';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_keyboard_visibility/flutter_keyboard_visibility.dart';
import 'package:flutter_secure_storage/flutter_secure_storage.dart';
import 'package:gamepads/gamepads.dart';
import 'package:package_info_plus/package_info_plus.dart';
import 'package:url_launcher/url_launcher.dart';
import 'package:webos_app_manager/webos_app_manager.dart';
import 'package:webos_display/webos_display.dart';
import 'package:webos_image_texture/image_texture.dart';
import 'package:webos_service_bridge/webos_service_bridge.dart';

const kAudioUrl = 'https://download.samplelib.com/mp3/sample-15s.mp3';
const kImageUrl = 'https://flutter.github.io/assets-for-api-docs/assets/widgets/owl.jpg';
const kWebUrl = 'https://example.com/';

final results = ValueNotifier<List<String>>([]);

String _clip(Object o, [int n = 220]) {
  final s = o.toString().replaceAll('\n', ' ');
  return s.length > n ? '${s.substring(0, n)}...' : s;
}

void log(String line) {
  final l = '[P] $line';
  results.value = [...results.value, l];
  try {
    File('${File(Platform.resolvedExecutable).parent.path}/compat_results.log')
        .writeAsStringSync('$l\n', mode: FileMode.append, flush: true);
  } catch (_) {}
}

Future<void> check(String name, Future<String> Function() body,
    {Duration timeout = const Duration(seconds: 15)}) async {
  final sw = Stopwatch()..start();
  try {
    final detail = await body().timeout(timeout);
    log('$name PASS ${_clip(detail)} (${sw.elapsedMilliseconds} ms)');
  } catch (e) {
    log('$name FAIL ${_clip(e)}');
  }
}

void main() {
  WidgetsFlutterBinding.ensureInitialized();
  WidgetsBinding.instance.platformDispatcher.onError = (e, st) {
    log('uncaught INFO ${_clip(e)}');
    return true;
  };
  runApp(const MaterialApp(home: PluginsPage()));
}

class PluginsPage extends StatefulWidget {
  const PluginsPage({super.key});
  @override
  State<PluginsPage> createState() => _PluginsPageState();
}

class _PluginsPageState extends State<PluginsPage> {
  bool _showImage = false;
  MouseCursor _cursor = SystemMouseCursors.basic;

  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addPostFrameCallback((_) => _run());
  }

  // Probe mode (<app dir>/km_probe exists): log raw keymanager2 replies, to
  // learn its API on firmware without keymanager3.
  Future<void> _kmProbe() async {
    const ch = MethodChannel('webOS/luna-service2/request', JSONMethodCodec());
    Future<dynamic> call(String m, Map<String, dynamic> p) async {
      try {
        // Same fields webos_service_bridge sends; the embedder requires them all.
        final uri = 'luna://com.webos.service.keymanager2/$m';
        final r = await ch.invokeMethod<dynamic>('callOneReply', {
          'uri': uri, 'payload': p, 'hashCode': '$uri${p.hashCode}'.hashCode,
          'optHashCode': p.hashCode, 'serviceName': '',
        }).timeout(const Duration(seconds: 5));
        log('km $m ${jsonEncode(p)} -> ${jsonEncode(r)}');
        return r;
      } catch (e) {
        log('km $m ${jsonEncode(p)} -> EXC $e');
        return null;
      }
    }
    final plain = base64.encode(utf8.encode('hello world'));
    await call('remove', {'keyname': 'kmprobe'});
    final g = await call('generate', {'keyname': 'kmprobe', 'type': 'AES', 'size': 32});
    if (!(g is Map && g['returnValue'] == true)) {
      await call('generate', {'keyname': 'kmprobe', 'type': 'AES', 'size': 16});
    }
    await call('getAttrs', {'keyname': 'kmprobe'});
    for (final opts in [
      {'mode': 'CBC', 'padding': 'PKCS7'},
      {'mode': 'ECB', 'padding': 'PKCS7'},
      {'mode': 'GCM'},
      <String, String>{},
    ]) {
      final r = await call('encrypt', {'keyname': 'kmprobe', 'data': plain, ...opts});
      if (r is Map && r['returnValue'] == true) {
        final back = await call('decrypt', {'keyname': 'kmprobe', 'data': r['data'], ...opts,
          if (r['iv'] != null) 'iv': r['iv']});
        if (back is Map && back['data'] is String) {
          log('km roundtrip ${jsonEncode(opts)} -> ${utf8.decode(base64.decode(back['data']), allowMalformed: true)}');
        }
      }
    }
    log('run INFO done');
  }

  Future<void> _run() async {
    if (File('${File(Platform.resolvedExecutable).parent.path}/km_probe').existsSync()) {
      await _kmProbe();
      return;
    }
    log('run INFO start ${DateTime.now()}');

    // ---- webos_service_bridge: Luna calls from Dart
    await check('service_bridge.callOneReply', () async {
      final r = await WebOSServiceBridge.callOneReply(
          'luna://com.webos.settingsservice/getSystemSettings',
          payload: {'keys': ['localeInfo']});
      if (r == null || r['returnValue'] != true) throw 'reply: $r';
      return 'localeInfo=${r['settings']?['localeInfo']?['locales']?['UI']}';
    });
    await check('service_bridge.subscribe', () async {
      final bridge = WebOSServiceBridge('luna://com.palm.systemservice/time/getSystemTime',
          payload: {'subscribe': true});
      final first = await bridge.subscribe().first.timeout(const Duration(seconds: 8));
      if (first['returnValue'] == false) throw 'reply: $first';
      return 'utc=${first['utc']} timezone=${first['timezone']}';
    });

    // ---- webos_app_manager
    await check('app_manager.getAppLoadStatus', () async {
      final appId = Platform.environment['FLUTTER_APP_ID'] ?? 'com.flutter.app.plugins-test';
      final s = await WebOSAppManager().getAppLoadStatus(appId);
      if (s == null) throw 'null status for $appId';
      return '$appId -> $s';
    });

    // ---- webos_display
    await check('display.getBounds', () async {
      final d = WebOSDisplay();
      final b = await d.getBounds();
      final rot = await d.getRotationDegree();
      if (b == null || b.width <= 0) throw 'bounds=$b rotation=$rot';
      return 'bounds=${b.width.toInt()}x${b.height.toInt()} rotation=$rot';
    });

    // ---- device_info_plus
    await check('device_info.webosInfo', () async {
      // The generic DeviceInfoPlugin().deviceInfo casts to LinuxDeviceInfo on
      // webOS and throws; LG's webOS API is webosInfo.
      final info = await DeviceInfoWebOSPlugin().webosInfo;
      final m = info.data;
      if (m.isEmpty) throw 'empty device info';
      final keys = m.keys.take(8).join(',');
      final model = m['modelName'] ?? m['model'] ?? m['productId'];
      return 'model=$model keys=$keys';
    });

    // ---- package_info_plus
    await check('package_info.fromPlatform', () async {
      final p = await PackageInfo.fromPlatform();
      if (p.packageName.isEmpty) throw 'empty packageName';
      return '${p.packageName} ${p.appName} v${p.version}+${p.buildNumber}';
    });

    // ---- connectivity_plus
    await check('connectivity.check', () async {
      final r = await Connectivity().checkConnectivity();
      if (r.isEmpty || r.contains(ConnectivityResult.none)) throw 'result=$r';
      return '$r';
    });

    // ---- flutter_secure_storage
    await check('secure_storage.roundtrip', () async {
      const s = FlutterSecureStorage();
      final v = 'secret-${DateTime.now().millisecondsSinceEpoch}';
      await s.write(key: 'k', value: v);
      final back = await s.read(key: 'k');
      final all = await s.readAll();
      await s.delete(key: 'k');
      final gone = await s.read(key: 'k');
      if (back != v || gone != null) throw 'read=$back afterDelete=$gone';
      return 'write/read/delete ok, readAll had ${all.length} key(s)';
    });
    await check('secure_storage.persists', () async {
      const s = FlutterSecureStorage();
      final previous = await s.read(key: 'persist');
      await s.write(key: 'persist', value: 'run ${DateTime.now().toIso8601String()}');
      return 'value from previous run: ${previous ?? '(none: first run)'}';
    });

    // ---- gamepads
    await check('gamepads.list', () async {
      final pads = await Gamepads.list();
      return '${pads.length} gamepad(s): ${pads.map((g) => g.name).join(', ')}';
    });

    // ---- flutter_keyboard_visibility
    await check('keyboard_visibility.controller', () async {
      final c = KeyboardVisibilityController();
      final sub = c.onChange.listen((_) {});
      await Future.delayed(const Duration(milliseconds: 300));
      await sub.cancel();
      return 'isVisible=${c.isVisible}';
    });

    // ---- custom_mouse_cursor
    await check('mouse_cursor.asset', () async {
      final cur = await CustomMouseCursor.asset('assets/cursor.png', hotX: 0, hotY: 0);
      setState(() => _cursor = cur);
      return 'created ${cur.runtimeType}';
    });

    // ---- audioplayers (audioplayers_webos)
    await check('audioplayers.play', () async {
      final p = AudioPlayer();
      String err = '';
      final sub = p.onLog.listen((m) => err = m);
      await p.play(UrlSource(kAudioUrl));
      await Future.delayed(const Duration(seconds: 6));
      final pos1 = await p.getCurrentPosition();
      await Future.delayed(const Duration(seconds: 2));
      final pos2 = await p.getCurrentPosition();
      final dur = await p.getDuration();
      final state = p.state;
      await p.stop();
      await sub.cancel();
      await p.dispose();
      final adv = (pos2?.inMilliseconds ?? 0) - (pos1?.inMilliseconds ?? 0);
      if (adv < 1000) throw 'position $pos1 -> $pos2 state=$state dur=$dur $err';
      return 'advanced ${adv}ms in 2000ms, duration=$dur, state=$state';
    }, timeout: const Duration(seconds: 30));

    // ---- webos_image_texture (verified by screenshot: owl photo on the right)
    setState(() => _showImage = true);
    log('image_texture INFO network image shown at right; check screenshot');
    await Future.delayed(const Duration(seconds: 5));
    log('STEP image_visible');
    await Future.delayed(const Duration(seconds: 5));

    // ---- url_launcher (last: it opens the TV browser)
    await check('url_launcher.canLaunch', () async {
      final ok = await canLaunchUrl(Uri.parse(kWebUrl));
      if (!ok) throw 'canLaunchUrl=false';
      return 'canLaunchUrl=true';
    });
    log('STEP launching_browser');
    await check('url_launcher.launch', () async {
      final ok = await launchUrl(Uri.parse(kWebUrl));
      if (!ok) throw 'launchUrl=false';
      return 'launchUrl=true (host checks the foreground app)';
    });

    final fails = results.value.where((r) => r.contains(' FAIL ')).length;
    log('run INFO done, $fails failed');
  }

  @override
  Widget build(BuildContext context) {
    return MouseRegion(
      cursor: _cursor,
      child: Scaffold(
        appBar: AppBar(title: const Text('webOS plugins test')),
        body: Row(children: [
          Expanded(
            child: ValueListenableBuilder<List<String>>(
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
                                    : Colors.blueGrey)),
                ],
              ),
            ),
          ),
          if (_showImage)
            SizedBox(
              width: 480,
              height: 360,
              child: ImageTexture.network(kImageUrl, fit: BoxFit.contain),
            ),
        ]),
      ),
    );
  }
}
