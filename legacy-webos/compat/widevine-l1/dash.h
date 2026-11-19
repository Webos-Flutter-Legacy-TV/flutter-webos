// Minimal DASH (static, single period) manifest reader for the L1 player.
// Supports SegmentBase with a 'sidx' index and SegmentTemplate with
// $Number$ or $Time$ (fixed duration or SegmentTimeline).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace wvl1 {

struct Segment {
  std::string url;
  int64_t first = -1, last = -1;  // byte range, -1: whole resource
  double start = 0, duration = 0;  // seconds
};

struct Representation {
  std::string id, codecs, mime, content;  // content: "video" / "audio"
  uint32_t bandwidth = 0, width = 0, height = 0, fps = 0;  // fps rounded up
  std::string pssh_b64;                    // Widevine <cenc:pssh>, if any
  Segment init;
  // SegmentBase: index range, filled into segments by loadIndex().
  int64_t index_first = -1, index_last = -1;
  std::vector<Segment> segments;
};

struct Manifest {
  double duration = 0;  // seconds
  std::vector<Representation> reps;
};

bool parseMpd(const std::string &xml, const std::string &mpd_url, Manifest *out, std::string *err);
// For SegmentBase representations: parses the 'sidx' box (data starts at
// byte index_first of the resource) into segments.
bool loadIndex(Representation *rep, const std::string &sidx);

}  // namespace wvl1
