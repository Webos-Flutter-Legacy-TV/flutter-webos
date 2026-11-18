# soloud_test

Checks `audioplayers_soloud` (SoLoud mixer, ALSA output through PulseAudio)
on older webOS. Results are written as `[S] <check> PASS|FAIL|INFO` lines to
`compat_results.log` in the installed app directory.

Covered: WAV/MP3/Opus from asset, bytes, file and URL; bad host error;
load/play/pause/resume/seek/2x/volume/balance; completion and replay
(default ReleaseMode.release); loop; stop; three players mixed; low-latency
retrigger. The network checks use download.samplelib.com.

Run: `flutter-webos build webos --release`, then
`TV_HOST=<ip> tools/dev-cycle.sh tests/soloud_test 110 --luna` from
`legacy-webos/`.

Assets: `coins.*` and `sample3.opus` come from the audioplayers_soloud
example; `tone5s.wav` is a generated 440 Hz tone.

webOS 6.5.3 (2026-11-18, plugins bfb91dc): 21/22 pass. `control.loop`
fails: after wrapping, the reported position stays at the last seek target
(`GetPosition()` returns `max(seek_position_, position)`).
