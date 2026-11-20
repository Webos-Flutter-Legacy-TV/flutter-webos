# Hardware (L1) Widevine route — experimental

Opt-in with an empty `webos/meta/compat-widevine-l1` in the app (checked before
`compat-widevine-cdm`). Widevine DASH loads from `video_player_drm` then play
through `libwv-l1.so` instead of NDL:

- **Licence:** the TV's Widevine CE CDM wrapper (`/usr/lib/libwvcdm_shared.so`,
  Chromium CDM_10 interface, loaded at run time). Privacy mode: the service
  certificate is requested with `08 04` first. By `licenseServerUrl`, or with
  `licenseCallback`: both requests go to the app as `drmEncrypted` events
  (service `legacy-webos-widevine-l1`) and the replies come back through
  `NDL_MediaSetDrmOperation`. The first request is the 2-byte certificate
  request `08 04`: most Widevine licence servers answer it, others (Axinom)
  serve the certificate at their own address, which the app's callback then
  returns.
- **Keys:** a licence per key set: the video's, the audio's if it has its
  own, each period's, and new `pssh` boxes in segments (key rotation). Each
  is its own CDM session. Per-sample key ids from `seig` sample groups.
- **Decrypting:** the CDM returns video samples wrapped for the secure video
  path (decrypted in the TEE, never in the app); audio comes back clear.
- **Playback:** the TV's `StarfishMediaAPIs` (`libplayerAPIs.so.1`) in
  buffer-stream mode. Video and audio samples are fed interleaved in decode
  order.
- **Streams:** DASH on demand (SegmentBase with `sidx`, SegmentTemplate with
  `$Number$`/`$Time$`, fixed duration or SegmentTimeline; several periods
  joined into one timeline, each with its own init segment, keys and time
  offset) and live (SegmentTemplate, current period; started a few segments
  behind the edge, manifest read again as it runs out; position 0 is the
  start of the time-shift window, so the app can seek within it).
  Fragmented MP4, CENC `cenc`/`cbcs`.
- **Video:** H.264, HEVC (`avc1`/`avc3`/`hev1`/`hvc1`, 4-byte NAL lengths,
  converted to Annex B with parameter sets before key frames), Dolby Vision
  (`dvh1`/`dvhe`, or HEVC with `dvcC`: `DolbyHdrInfo` in the Load payload),
  AV1 (`av01`, sequence header from `av1C` before key frames) and VP9
  (`vp09`), all in MP4, up to 2160p. At equal height Dolby Vision, HEVC, AV1,
  VP9, then H.264. HDR10/HLG
  mastering metadata (`mdcv`/`clli` with `colr`/`vpcC`) goes to the player with
  `setHdrInfo`; without it the TV reads the stream's own.
- **Audio:** AAC, AC-3, E-AC-3 (Atmos flagged from `dec3`) and Opus in MP4.
  Each audio AdaptationSet is offered to the app as a track (with its
  language), AAC first by default; `selectAudioTrack` switches by restarting
  from the current position, with `changeAudioCodec` when the codec differs
  (webOS 6 and later).
- **Quality:** starts with the highest resolution, then switches between
  representations of the same codec at segment boundaries, by measured
  download speed.
- **Older webOS:** the version comes from `/var/run/nyx/os_info.json`;
  webOS 4 and older get secure path version 30, their shifted event numbers
  and the older HDR field names.
- **Events:** the JSON events NDL sends (`loadCompleted`, `playing`,
  `currentTime`, `videoInfo`, `sourceInfo`, `seekDone`, `endOfStream`, `error`).

Nothing of LG's or Google's is bundled; only the BSD-licensed Chromium CDM
headers are in `third_party/cdm`. Using the TV's CDM this way is outside the
terms it ships under.

## Status (webOS 6.5.3, LG C1)

Checked on the TV, picture seen on screen (20 November 2026):

| Case | Result |
| --- | --- |
| 1080p H.264, 1080p HEVC | Plays |
| 4K H.264 (27 Mbit/s), 4K HEVC (20 Mbit/s) | Plays |
| Licence by URL, licence callback | Works |
| Seeking | Works |
| Audio track switch (5 languages, same codec) | Works |
| Multi-period, several keys (Axinom test vector) | Not verified: Axinom's licence server answers 403 to the published token |

Built and checked on a PC with public streams, not yet on the TV: VP9, AV1,
Dolby Vision, E-AC-3/AC-3/Opus, HDR metadata, live DASH and seeking, key
rotation, adaptive quality switching. Not supported: in-stream subtitles (no
renderer on this route; use Flutter's `closedCaptionFile`), HLS, PlayReady on
this route, live period changes.

Other webOS releases are untested on a TV. Firmware symbol check (webosbrew
inventory 0.10.0): webOS 5, 22 and 23 have every library and function the
route uses (webOS 5 lacks `changeAudioCodec`: no audio switch between
codecs); webOS 24 to 26 name the CDM wrapper `libwidevine-wrapper.so.1`
(loaded as a fallback); webOS 4 and older have an older CDM interface, and
webOS 3 an older player ABI (unsupported).

### Showing the picture

The player runs in the app, under the Flutter UI; the video widget leaves a
transparent hole over it. Two things make it visible:

- The player places its video with the video output service
  (`luna://com.webos.service.videooutput/display/setDisplayWindow`, media id
  as context, 1920x1080 coordinates), subscribes to
  `display/getApplyDoneWindowRegion`, and asks the app for a frame on each
  reply (`compat_request_frame` from the Wayland shim): the app's surface has
  to be committed after the TV applied the area.
- The Wayland shim reports buffer age 0 while media is loaded, so the
  embedder redraws whole frames: with Mali's buffer age its partial redraw
  left the hole in one buffer only, and the black came back.

## Debug options

Words in `<app dir>/compat-l1-opts`:

| Word | Effect |
| --- | --- |
| `noaudio` | Video only |
| `nodrm` | Feed without the secure path (clear streams or the clear lead only) |
| `svp30` | Secure path version 30 (webOS 4 and older) instead of 40 |
| `h264` | H.264 only (no Dolby Vision, HEVC, AV1 or VP9) |
| `nodv` / `noav1` | Never pick Dolby Vision / AV1 |
| `noabr` | No quality switching |
| `slownet` | Count downloads as 3 Mbit/s, to test switching |
| `uid-app` | Create the player with the app id (gives error 601 on webOS 6) |

`out/l1-selftest` (built by `tools/build-shims.sh`) checks the CDM, licence
and decrypting from inside an app's jail without the player.
