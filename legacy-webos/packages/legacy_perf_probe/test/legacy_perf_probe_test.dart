import 'package:flutter_test/flutter_test.dart';
import 'package:legacy_perf_probe/legacy_perf_probe.dart';

void main() {
  test('off unless built with LEGACY_PERF_PROBE=true', () {
    expect(perfProbeEnabled, isFalse);
    final List<String> lines = <String>[];
    PerfProbe.log = lines.add;
    PerfProbe.start();
    expect(lines, isEmpty);
  });
}
