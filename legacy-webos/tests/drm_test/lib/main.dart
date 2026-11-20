// drm_test: video_player_drm on older webOS. Plays clear DASH, clear HLS,
// Widevine DASH and PlayReady Smooth Streaming public test streams in turn,
// logging "[D] ..." lines to <app dir>/compat_results.log and a
// "STEP <case>" marker while each one plays (for host screenshots).
import 'dart:async';
import 'dart:io';
import 'dart:typed_data';

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
  Case('widevine_callback', 'https://storage.googleapis.com/wvmedia/cenc/h264/tears/tears.mpd',
      VideoFormat.dash,
      DrmConfigs(type: DrmType.widevine, licenseCallback: _widevineLicense)),
  Case('widevine_seek', 'https://storage.googleapis.com/wvmedia/cenc/h264/tears/tears.mpd',
      VideoFormat.dash,
      DrmConfigs(type: DrmType.widevine, licenseServerUrl: 'https://proxy.uat.widevine.com/proxy?provider=widevine_test')),
  Case('widevine_uhd_h264', 'https://storage.googleapis.com/wvmedia/cenc/h264/tears/tears_uhd.mpd',
      VideoFormat.dash,
      DrmConfigs(type: DrmType.widevine, licenseServerUrl: 'https://proxy.uat.widevine.com/proxy?provider=widevine_test')),
  Case('widevine_hevc', 'https://storage.googleapis.com/wvmedia/cenc/hevc/tears/tears.mpd',
      VideoFormat.dash,
      DrmConfigs(type: DrmType.widevine, licenseServerUrl: 'https://proxy.uat.widevine.com/proxy?provider=widevine_test')),
  Case('widevine_uhd_hevc', 'https://storage.googleapis.com/wvmedia/cenc/hevc/tears/tears_uhd.mpd',
      VideoFormat.dash,
      DrmConfigs(type: DrmType.widevine, licenseServerUrl: 'https://proxy.uat.widevine.com/proxy?provider=widevine_test')),
  Case('widevine_audio_switch',
      'https://storage.googleapis.com/shaka-demo-assets/angel-one-widevine/dash.mpd',
      VideoFormat.dash,
      DrmConfigs(type: DrmType.widevine, licenseServerUrl: 'https://cwip-shaka-proxy.appspot.com/no_auth')),
  // Axinom public test vector: two periods with different keys, separate
  // keys per resolution tier and for audio (licence token from Axinom's
  // public test vector list).
  Case('widevine_multiperiod',
      'https://media.axprod.net/TestVectors/v7-MultiDRM-MultiKey-MultiPeriod/Manifest_1080p.mpd',
      VideoFormat.dash,
      DrmConfigs(type: DrmType.widevine, licenseCallback: _axinomLicense)),
  Case('playready_smooth',
      'https://test.playready.microsoft.com/smoothstreaming/SSWSS720H264PR/SuperSpeedway_720.ism/Manifest',
      VideoFormat.ss,
      DrmConfigs(type: DrmType.playready, licenseServerUrl: 'https://test.playready.microsoft.com/service/rightsmanager.asmx')),
];

const _axinomToken =
    'eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJ2ZXJzaW9uIjoxLCJjb21fa2V5X2lkIjoiYjMzNjRlYjUtNTFmNi00YWUzLThjOTgtMzNjZWQ1ZTMxYzc4IiwibWVzc2FnZSI6eyJ0eXBlIjoiZW50aXRsZW1lbnRfbWVzc2FnZSIsInZlcnNpb24iOjIsImxpY2Vuc2UiOnsiYWxsb3dfcGVyc2lzdGVuY2UiOnRydWV9LCJjb250ZW50X2tleXNfc291cmNlIjp7ImlubGluZSI6W3siaWQiOiIwODcyNzg2ZS1mOWU3LTQ2NWYtYTNhMi00ZTViMGVmOGZhNDUiLCJlbmNyeXB0ZWRfa2V5IjoiUHc2K2VFWU5jdmpZYmZzaDNYM2ZtZz09IiwidXNhZ2VfcG9saWN5IjoiUG9saWN5IEEifSx7ImlkIjoiYzE0ZjA3MDktZjJiOS00NDI3LTkxNmItNjFiNTI1ODY1MDZhIiwiZW5jcnlwdGVkX2tleSI6Ii8xK2ZOaWgzOG1xUnY0eWNZem50L3c9PSIsInVzYWdlX3BvbGljeSI6IlBvbGljeSBBIn0seyJpZCI6IjhiMDI5ZTUxLWQ1NmEtNDRiZC05MTBmLWQ0YjVmZDkwZmJhMiIsImVuY3J5cHRlZF9rZXkiOiJrcTBKdVpFanBGTjhzYVRtdDU2ME9nPT0iLCJ1c2FnZV9wb2xpY3kiOiJQb2xpY3kgQSJ9LHsiaWQiOiIyZDZlOTM4Ny02MGNhLTQxNDUtYWVjMi1jNDA4MzdiNGIwMjYiLCJlbmNyeXB0ZWRfa2V5IjoiVGNSUWVCV3hFb0hPS0hyYWQ0U2VWUT09IiwidXNhZ2VfcG9saWN5IjoiUG9saWN5IEEifSx7ImlkIjoiZGUwMmYwN2YtYTA5OC00ZWUwLWI1NTYtOTA3YzBkMTdmYmJjIiwiZW5jcnlwdGVkX2tleSI6InBvZW5jUzdHZ21lR0Zlb0o2UEVBVFE9PSIsInVzYWdlX3BvbGljeSI6IlBvbGljeSBBIn0seyJpZCI6IjkxNGU2OWY0LTBhYjMtNDUzNC05ZTlmLTk4NTM2MTVlMjZmNiIsImVuY3J5cHRlZF9rZXkiOiJlaUkvTXNsbHJRNHdDbFJUL0xObUNBPT0iLCJ1c2FnZV9wb2xpY3kiOiJQb2xpY3kgQSJ9LHsiaWQiOiJkYTQ0NDVjMi1kYjVlLTQ4ZWYtYjA5Ni0zZWYzNDdiMTZjN2YiLCJlbmNyeXB0ZWRfa2V5IjoiMncremR2cXJwRFYzeFIwYkp5NHVndz09IiwidXNhZ2VfcG9saWN5IjoiUG9saWN5IEEifSx7ImlkIjoiMjlmMDVlOGYtYTFhZS00NmU0LTgwZTktMjJkY2Q0NGNkN2ExIiwiZW5jcnlwdGVkX2tleSI6Ii94bFNIcHh3cXUzZ28vZ2xwbVNnYVE9PSIsInVzYWdlX3BvbGljeSI6IlBvbGljeSBBIn0seyJpZCI6IjY5ZmU3MDc3LWRhZGQtNGI1NS05NmNkLWMzZWRiMzk5MTg1MyIsImVuY3J5cHRlZF9rZXkiOiJ6dTZpdXpOMnBzaTBaU3hRaUFUa1JRPT0iLCJ1c2FnZV9wb2xpY3kiOiJQb2xpY3kgQSJ9XX0sImNvbnRlbnRfa2V5X3VzYWdlX3BvbGljaWVzIjpbeyJuYW1lIjoiUG9saWN5IEEiLCJwbGF5cmVhZHkiOnsibWluX2RldmljZV9zZWN1cml0eV9sZXZlbCI6MTUwLCJwbGF5X2VuYWJsZXJzIjpbIjc4NjYyN0Q4LUMyQTYtNDRCRS04Rjg4LTA4QUUyNTVCMDFBNyJdfX1dfX0.R7aHvOHPBV-f2VfjeQC0AOi9axZI_xGIlpB-OScAzy4';

// Licence callback for the Axinom test vector: its licence server wants the
// test vector's token in the X-AxDRM-Message header.
Future<Uint8List> _axinomLicense(Uint8List challenge) async {
  log('axinom licenseCallback: challenge ${challenge.length} bytes');
  final client = HttpClient();
  try {
    // The L1 route first asks for the licence server's service certificate
    // (the 2-byte request 08 04); Axinom serves it at its own address.
    if (challenge.length == 2 && challenge[0] == 8 && challenge[1] == 4) {
      final res = await (await client.getUrl(Uri.parse('https://drm-widevine-licensing.axprod.net/ServiceCertificate'))).close();
      final bytes = await res.fold<List<int>>(<int>[], (a, b) => a..addAll(b));
      log('axinom service certificate: HTTP ${res.statusCode}, ${bytes.length} bytes');
      return Uint8List.fromList(bytes);
    }
    final req = await client.postUrl(Uri.parse('https://drm-widevine-licensing.axprod.net/AcquireLicense'));
    req.headers.contentType = ContentType('application', 'octet-stream');
    req.headers.set('X-AxDRM-Message', _axinomToken);
    req.add(challenge);
    final res = await req.close();
    final bytes = await res.fold<List<int>>(<int>[], (a, b) => a..addAll(b));
    log('axinom licenseCallback: HTTP ${res.statusCode}, ${bytes.length} bytes');
    return Uint8List.fromList(bytes);
  } finally {
    client.close();
  }
}

// App-side licence acquisition (licenseCallback path): POST the CDM
// challenge to Google's Widevine test proxy and return the licence bytes.
Future<Uint8List> _widevineLicense(Uint8List challenge) async {
  log('licenseCallback: challenge ${challenge.length} bytes');
  final client = HttpClient();
  try {
    final req = await client.postUrl(Uri.parse('https://proxy.uat.widevine.com/proxy?provider=widevine_test'));
    req.headers.contentType = ContentType('application', 'octet-stream');
    req.add(challenge);
    final res = await req.close();
    final bytes = await res.fold<List<int>>(<int>[], (a, b) => a..addAll(b));
    log('licenseCallback: HTTP ${res.statusCode}, licence ${bytes.length} bytes');
    return Uint8List.fromList(bytes);
  } finally {
    client.close();
  }
}

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
        if (k.name == 'widevine_multiperiod') {
          // Into the second period (starts at 734 s), whose keys differ.
          await Future.delayed(const Duration(seconds: 5));
          log('seek from ${c.value.position.inMilliseconds}ms to 740000ms (period 2)');
          await c.seekTo(const Duration(seconds: 740));
          await Future.delayed(const Duration(seconds: 8));
          log('period 2: position ${c.value.position.inMilliseconds}ms');
          log('STEP multiperiod_done');
        }
        if (k.name == 'widevine_audio_switch') {
          // Multi-language stream: list the audio tracks, switch to the
          // second one and check that playback keeps going.
          await Future.delayed(const Duration(seconds: 4));
          log('audio tracks: ${_tracks(c)} ${(c.value as dynamic).audioTracks}');
          final before = c.value.position.inMilliseconds;
          await c.selectAudioTrack(1);
          log('selected audio track 1 at ${before}ms');
          await Future.delayed(const Duration(seconds: 6));
          log('after switch: position ${c.value.position.inMilliseconds}ms');
          log('STEP audio_switch_done');
        }
        if (k.name == 'widevine_seek') {
          // Seek through the proxy, then keep playing so the adaptive logic
          // can switch bitrates (watch videoInfo events in compat.log).
          await Future.delayed(const Duration(seconds: 5));
          log('seek from ${c.value.position.inMilliseconds}ms to 300000ms');
          await c.seekTo(const Duration(seconds: 300));
          final pts = <String>[];
          for (var i = 0; i < 24; i++) {
            await Future.delayed(const Duration(milliseconds: 500));
            pts.add('${c.value.position.inMilliseconds}');
          }
          log('after seek: ${pts.join(' ')}');
          log('STEP seek_done');
          for (var i = 0; i < 6; i++) {
            await Future.delayed(const Duration(seconds: 5));
            log('playing: position ${c.value.position.inMilliseconds}ms');
          }
          log('STEP abr_done');
        }
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
