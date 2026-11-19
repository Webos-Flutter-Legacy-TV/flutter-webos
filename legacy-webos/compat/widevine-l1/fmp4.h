// Minimal fragmented-MP4 (ISO BMFF / CENC) reader for the L1 player:
// codec configuration from an init segment and the samples of a media
// segment, with their timing and per-sample encryption info.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace wvl1 {

struct Track {
  uint32_t id = 0;
  uint32_t timescale = 0;
  bool video = false;
  std::string codec;            // "H264", "AAC" (empty: unsupported)
  uint32_t width = 0, height = 0;
  uint8_t nal_length_size = 4;  // H.264
  std::string annexb_config;    // H.264 SPS/PPS with start codes
  uint32_t aac_object_type = 2, channels = 2, sample_rate = 48000;
  // Protection ('tenc'); scheme empty when the track is clear.
  std::string scheme, kid, constant_iv;
  uint8_t iv_size = 0, crypt_blocks = 0, skip_blocks = 0;
  // 'trex' defaults
  uint32_t default_duration = 0, default_size = 0, default_flags = 0;
};

struct Sample {
  size_t offset = 0;   // in the segment buffer
  uint32_t size = 0;
  int64_t dts = 0, pts = 0;  // track timescale
  bool sync = false;
  bool encrypted = false;
  std::string iv;      // 16 bytes
  std::vector<std::pair<uint32_t, uint32_t>> subsamples;  // clear, cipher
};

// Parses the init segment; fills the first audio or video track found.
bool parseInit(const std::string &init, Track *track, std::string *pssh_widevine);
// Parses every moof/mdat pair in a media segment.
bool parseSegment(const std::string &seg, const Track &track, std::vector<Sample> *out);

}  // namespace wvl1
