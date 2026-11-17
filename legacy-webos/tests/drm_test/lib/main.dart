// drm_test: video_player_drm on older webOS. Plays clear DASH, clear HLS,
// Widevine DASH and PlayReady Smooth Streaming public test streams in turn,
// logging "[D] ..." lines to <app dir>/compat_results.log and a
// "STEP <case>" marker while each one plays (for host screenshots).
import 'dart:async';
import 'dart:io';

import 'package:flutter/material.dart';
import 'package:video_player_drm/video_player.dart';

class Case {
  const Case(this.name, this.url, this.format, [this.drm]);
  final String name;
  final String url;
  final VideoFormat format;
  final DrmConfigs? drm;
}

const cases = <Case>[
  Case('clear_dash', 'https://storage.googleapis.com/shaka-demo-assets/angel-one/dash.mpd',
      VideoFormat.dash),
  Case('clear_dash_akamai', 'https://dash.akamaized.net/akamai/bbb_30fps/bbb_30fps.mpd',
      VideoFormat.dash),
  Case('clear_hls', 'https://test-streams.mux.dev/x36xhzz/x36xhzz.m3u8', VideoFormat.hls),
  Case('widevine_dash',
      'https://storage.googleapis.com/shaka-demo-assets/angel-one-widevine/dash.mpd',
      VideoFormat.dash,
      DrmConfigs(type: DrmType.widevine, licenseServerUrl: 'https://cwip-shaka-proxy.appspot.com/no_auth')),
  Case('widevine_dash_h264', 'https://storage.googleapis.com/wvmedia/cenc/h264/tears/tears.mpd',
      VideoFormat.dash,
      DrmConfigs(type: DrmType.widevine, licenseServerUrl: 'https://proxy.uat.widevine.com/proxy?provider=widevine_test')),
  Case('playready_smooth',
      'https://test.playready.microsoft.com/smoothstreaming/SSWSS720H264PR/SuperSpeedway_720.ism/Manifest',
      VideoFormat.ss,
      DrmConfigs(type: DrmType.playready, licenseServerUrl: 'https://test.playready.microsoft.com/service/rightsmanager.asmx')),
];

void log(String line) {
  try {
    File('${File(Platform.resolvedExecutable).parent.path}/compat_results.log')
        .writeAsStringSync('[D] $line\n', mode: FileMode.append, flush: true);
  } catch (_) {}
}

void main() {
  WidgetsFlutterBinding.ensureInitialized();
  WidgetsBinding.instance.platformDispatcher.onError = (e, st) {
    log('uncaught $e');
    return true;
  };
  runApp(const MaterialApp(home: DrmPage()));
}

class DrmPage extends StatefulWidget {
  const DrmPage({super.key});
  @override
  State<DrmPage> createState() => _DrmPageState();
}

class _DrmPageState extends State<DrmPage> {
  VideoPlayerController? _c;
  String _label = 'starting';

  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addPostFrameCallback((_) => _run());
  }

  Future<void> _run() async {
    // <app dir>/drm_case.txt selects one case per launch, so a failing
    // pipeline cannot leave a dirty player slot for the next case.
    final sel = File('${File(Platform.resolvedExecutable).parent.path}/drm_case.txt');
    final only = sel.existsSync() ? sel.readAsStringSync().trim() : '';
    for (final k in cases.where((c) => only.isEmpty || c.name == only)) {
      setState(() => _label = k.name);
      final c = VideoPlayerController.network(Uri.parse(k.url), formatHint: k.format, drmConfigs: k.drm);
      String err = '';
      c.addListener(() {
        if (c.value.hasError) err = c.value.errorDescription ?? 'error';
      });
      final sw = Stopwatch()..start();
      try {
        await c.initialize().timeout(const Duration(seconds: 30));
        setState(() => _c = c);
        await c.play();
        await Future.delayed(const Duration(seconds: 6));
        final p0 = c.value.position;
        log('STEP ${k.name}');
        await Future.delayed(const Duration(seconds: 4));
        final p1 = c.value.position;
        final adv = (p1 - p0).inMilliseconds;
        final ok = adv > 2500 && err.isEmpty;
        log('${k.name} ${ok ? 'PASS' : 'FAIL'} init ${sw.elapsedMilliseconds - 10000}ms '
            'size=${c.value.size.width.toInt()}x${c.value.size.height.toInt()} '
            'duration=${c.value.duration.inSeconds}s advanced ${adv}ms in 4000ms '
            'tracks a/t=${_tracks(c)} ${err.isEmpty ? '' : 'error: $err'}');
      } catch (e) {
        log('${k.name} FAIL $e ${err.isEmpty ? '' : 'error: $err'}');
      }
      setState(() => _c = null);
      await Future.delayed(const Duration(milliseconds: 300));
      await c.dispose();
      await Future.delayed(const Duration(seconds: 2));
    }
    log('done');
    setState(() => _label = 'done');
  }

  String _tracks(VideoPlayerController c) {
    try {
      final v = c.value as dynamic;
      return '${v.audioTracks?.length ?? '?'}/${v.textTracks?.length ?? '?'}';
    } catch (_) {
      return '?';
    }
  }

  @override
  Widget build(BuildContext context) {
    final c = _c;
    return Scaffold(
      backgroundColor: Colors.transparent,
      body: Stack(children: [
        if (c != null && c.value.isInitialized)
          Center(child: AspectRatio(aspectRatio: c.value.aspectRatio, child: VideoPlayer(c))),
        Positioned(
          left: 24,
          top: 24,
          child: Container(
            color: Colors.black54,
            padding: const EdgeInsets.all(8),
            child: Text('drm_test: $_label', style: const TextStyle(color: Colors.white, fontSize: 22)),
          ),
        ),
      ]),
    );
  }
}
