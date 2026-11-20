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
  // Codec name as StarfishMediaAPIs takes it: "H264", "H265", "VP9", "AV1", "AAC",
  // "AC3", "AC3 PLUS", "OPUS" (empty: unsupported).
  std::string codec;
  uint32_t width = 0, height = 0;
  uint8_t nal_length_size = 4;  // H.264 / H.265
  std::string annexb_config;    // parameter sets (SPS/PPS, HEVC also VPS) with start codes
  std::string av1_config;       // AV1: configOBUs from 'av1C' (sequence header)
  int dv_profile = -1;          // Dolby Vision ('dvcC'/'dvvC'), -1: none
  bool dv_el = false;           // Dolby Vision enhancement layer present
  uint32_t aac_object_type = 2, channels = 2, sample_rate = 48000;
  bool atmos = false;           // E-AC-3 with Joint Object Coding
  std::string opus_head;        // Opus: OpusHead built from 'dOps'
  // Colour ('colr' nclx or 'vpcC') and HDR metadata ('mdcv', 'clli').
  bool has_colour = false, full_range = false;
  uint8_t colour_primaries = 2, transfer = 2, matrix = 2;  // 2: unspecified
  bool has_mdcv = false, has_clli = false;
  uint16_t primaries_xy[6] = {}, white_xy[2] = {};  // mdcv order (G, B, R), 0.00002 units
  uint32_t max_luminance = 0, min_luminance = 0;     // 0.0001 cd/m2
  uint16_t max_cll = 0, max_fall = 0;
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
  std::string kid;     // key id from a 'seig' sample group (empty: the track's)
  std::vector<std::pair<uint32_t, uint32_t>> subsamples;  // clear, cipher
};

// Parses the init segment; fills the first audio or video track found.
bool parseInit(const std::string &init, Track *track, std::string *pssh_widevine);
// Parses every moof/mdat pair in a media segment. pssh (optional) gets the
// Widevine 'pssh' boxes carried in the segment (key rotation).
bool parseSegment(const std::string &seg, const Track &track, std::vector<Sample> *out,
                  std::vector<std::string> *pssh = nullptr);

}  // namespace wvl1
