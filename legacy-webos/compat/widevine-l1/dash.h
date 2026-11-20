// Minimal DASH manifest reader for the L1 player (periods joined).
// Supports SegmentBase with a 'sidx' index and SegmentTemplate with
// $Number$ or $Time$ (fixed duration or SegmentTimeline). Live (dynamic)
// manifests: the segments available at `now` (SegmentTemplate only, all
// periods joined); the caller parses the manifest again to get newer ones.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace wvl1 {

struct Segment {
  std::string url;
  int64_t first = -1, last = -1;  // byte range, -1: whole resource
  double start = 0, duration = 0;  // seconds, presentation time
  int part = 0;                    // Representation::parts index (period)
};

// A representation's piece in one Period: its init segment, 'sidx' range,
// key info and the offset from media time to presentation time.
struct Part {
  Segment init;
  int64_t index_first = -1, index_last = -1;
  double offset = 0;     // seconds: presentation = media time + offset
  std::string pssh_b64;  // Widevine <cenc:pssh> of that period, if any
  bool indexed = false;
};

struct Representation {
  std::string id, codecs, mime, content;  // content: "video" / "audio"
  uint32_t bandwidth = 0, width = 0, height = 0, fps = 0;  // fps rounded up
  std::string pssh_b64;                    // Widevine <cenc:pssh>, if any
  std::string lang;                        // language (audio)
  int group = -1;                          // index of its AdaptationSet
  Segment init;  // first period's (parts[0])
  // SegmentBase: index range, filled into segments by loadIndex().
  int64_t index_first = -1, index_last = -1;
  // One per Period, in order (multi-period manifests join the matching
  // representations of later periods).
  std::vector<Part> parts;
  std::vector<Segment> segments;
};

struct Manifest {
  double duration = 0;  // seconds (0 for live)
  bool dynamic = false;
  double availability_start = 0;  // live: UTC seconds
  double min_update = 0;          // live: minimumUpdatePeriod, seconds
  double presentation_delay = 0;  // live: suggestedPresentationDelay, seconds
  std::vector<Representation> reps;
};

// now: current UTC time in seconds (live manifests).
bool parseMpd(const std::string &xml, const std::string &mpd_url, Manifest *out, std::string *err, double now = 0);
// For SegmentBase parts: parses the 'sidx' box (data starts at byte
// index_first of the part's resource) into that part's segments.
bool loadIndex(Representation *rep, const std::string &sidx, int part = 0);

}  // namespace wvl1
