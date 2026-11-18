// In-place CENC (ISO/IEC 23001-7) handling for fragmented MP4, used by the
// local decrypting proxy. Every rewrite keeps byte lengths unchanged, so byte
// ranges taken from the manifest or 'sidx' stay valid:
//  - init segment: encrypted sample entries ('encv'/'enca') get their original
//    type back from 'frma'; 'sinf' and 'pssh' become 'free' boxes;
//  - media segments: samples are decrypted in place; 'senc', 'saiz', 'saio'
//    become 'free' boxes.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace wvcompat {

struct TrackProtection {
  std::string scheme;          // "cenc" or "cbcs"
  uint8_t iv_size = 8;         // per-sample IV size (0 with constant IV)
  std::string constant_iv;     // cbcs constant IV
  std::string kid;             // 16-byte default key id
  uint8_t crypt_blocks = 0, skip_blocks = 0;  // cbcs pattern
  bool is_video = false;
};

// Decrypts one batch of samples for a track; returns false on failure.
struct DecryptSample {
  const uint8_t *iv;           // 16 bytes (zero-padded)
  uint8_t *data;               // decrypted in place
  uint32_t size;
  std::vector<std::pair<uint32_t, uint32_t>> subsamples;  // clear, protected
};
using DecryptFn = std::function<bool(const TrackProtection &, std::vector<DecryptSample> &)>;

// Per media file (one origin URL): protection info learned from its init
// segment, keyed by track id.
class CencState {
 public:
  // Process a buffer that starts at a box boundary. Returns the number of
  // samples decrypted, or -1 on a decrypt failure.
  int process(std::string &buf, const DecryptFn &decrypt);
  // PSSH boxes seen in init segments (for the licence request).
  std::vector<std::string> pssh() {
    std::lock_guard<std::mutex> l(m_);
    return pssh_;
  }

 private:
  void handleMoov(uint8_t *p, size_t n);
  int handleMoof(uint8_t *moof, size_t moof_len, uint8_t *buf, size_t buf_len, const DecryptFn &decrypt);
  std::mutex m_;
  std::map<uint32_t, TrackProtection> tracks_;
  std::vector<std::string> pssh_;
};

}  // namespace wvcompat
