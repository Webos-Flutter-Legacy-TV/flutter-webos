# Hardware (L1) Widevine route — experimental

Opt-in with an empty `webos/meta/compat-widevine-l1` in the app (checked before
`compat-widevine-cdm`). Widevine DASH loads from `video_player_drm` then play
through `libwv-l1.so` instead of NDL:

- **Licence:** the TV's Widevine CE CDM wrapper (`/usr/lib/libwvcdm_shared.so`,
  Chromium CDM_10 interface, loaded at run time). Privacy mode: the service
  certificate is requested with `08 04` first. By `licenseServerUrl`, or with
  `licenseCallback`: both requests go to the app as `drmEncrypted` events
  (service `legacy-webos-widevine-l1`) and the replies come back through
  `NDL_MediaSetDrmOperation`.
- **Decrypting:** the CDM returns video samples wrapped for the secure video
  path (decrypted in the TEE, never in the app); audio comes back clear.
- **Playback:** the TV's `StarfishMediaAPIs` (`libplayerAPIs.so.1`) in
  buffer-stream mode. Video and audio samples are fed interleaved in decode
  order.
- **Streams:** DASH, single period: on demand (SegmentBase with `sidx`,
  SegmentTemplate with `$Number$`/`$Time$`, fixed duration or
  SegmentTimeline) and live (SegmentTemplate; started a few segments behind
  the edge, manifest read again as it runs out; no seeking). Fragmented MP4,
  CENC `cenc`/`cbcs`.
- **Video:** H.264, HEVC (`avc1`/`avc3`/`hev1`/`hvc1`, 4-byte NAL lengths,
  converted to Annex B with parameter sets before key frames) and VP9 in MP4
  (`vp09`), up to 2160p. At equal height HEVC, then VP9, then H.264. HDR10/HLG
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

| Part | Status |
| --- | --- |
| Licence by URL | Works |
| Secure decoding, H.264 1080p and 4K, with audio, seeking | Works (decoder reserved, frames decoded, position advances) |
| **Picture on screen** | **Black.** The TV connects the decoder to its main video output, but at opacity 0. Under investigation |
| HEVC, VP9, licence callback, adaptive quality, audio tracks, E-AC-3/AC-3/Opus, HDR metadata, live DASH | Built, not tested on a TV yet. Parsing checked on a PC with public streams (HEVC, E-AC-3 7.1 and stereo, multi-language AAC, live SegmentTemplate and SegmentTimeline); VP9, Opus, Atmos and `mdcv` boxes only against the specs |
| Multi-period, live seeking, subtitles, Dolby Vision, key rotation | Not done |
| webOS 3–5, 7+ | Untested |

## Debug options

Words in `<app dir>/compat-l1-opts`:

| Word | Effect |
| --- | --- |
| `noaudio` | Video only |
| `nodrm` | Feed without the secure path (clear streams or the clear lead only) |
| `svp30` | Secure path version 30 (webOS 4 and older) instead of 40 |
| `h264` | Never pick HEVC |
| `noabr` | No quality switching |
| `slownet` | Count downloads as 3 Mbit/s, to test switching |
| `uid-app` | Create the player with the app id (gives error 601 on webOS 6) |

`out/l1-selftest` (built by `tools/build-shims.sh`) checks the CDM, licence
and decrypting from inside an app's jail without the player.
