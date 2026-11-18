// In-place CENC handling for fragmented MP4. See cenc_mp4.h.
#include "cenc_mp4.h"

#include <cstring>

namespace wvcompat {
namespace {

uint32_t be32(const uint8_t *p) { return (uint32_t(p[0]) << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
uint16_t be16(const uint8_t *p) { return uint16_t((p[0] << 8) | p[1]); }
uint64_t be64(const uint8_t *p) { return (uint64_t(be32(p)) << 32) | be32(p + 4); }
bool is(const uint8_t *type, const char *t) { return memcmp(type, t, 4) == 0; }
void setType(uint8_t *type, const char *t) { memcpy(type, t, 4); }

// Iterates the boxes in [p, p+n). The callback gets the box start, its total
// size, the header size and the type.
template <typename F>
void forEachBox(uint8_t *p, size_t n, F f) {
  size_t off = 0;
  while (off + 8 <= n) {
    uint64_t size = be32(p + off);
    size_t hdr = 8;
    if (size == 1) {
      if (off + 16 > n) return;
      size = be64(p + off + 8);
      hdr = 16;
    } else if (size == 0) {
      size = n - off;
    }
    if (size < hdr || off + size > n) return;  // truncated box: stop
    f(p + off, static_cast<size_t>(size), hdr, p + off + 4);
    off += static_cast<size_t>(size);
  }
}

constexpr size_t kVisualEntryHeader = 78;  // bytes after the box header
constexpr size_t kAudioEntryHeader = 28;

}  // namespace

void CencState::handleMoov(uint8_t *p, size_t n) {
  uint32_t track_id = 0;
  std::function<void(uint8_t *, size_t)> walk = [&](uint8_t *b, size_t len) {
    forEachBox(b, len, [&](uint8_t *box, size_t size, size_t hdr, uint8_t *type) {
      uint8_t *body = box + hdr;
      size_t blen = size - hdr;
      if (is(type, "trak") || is(type, "mdia") || is(type, "minf") || is(type, "stbl")) {
        walk(body, blen);
      } else if (is(type, "tkhd") && blen >= 24) {
        track_id = be32(body + (body[0] == 1 ? 20 : 12));
      } else if (is(type, "pssh")) {
        std::lock_guard<std::mutex> l(m_);
        pssh_.emplace_back(reinterpret_cast<char *>(box), size);
        setType(type, "free");
      } else if (is(type, "stsd") && blen >= 8) {
        forEachBox(body + 8, blen - 8, [&](uint8_t *entry, size_t esize, size_t ehdr, uint8_t *etype) {
          bool video = is(etype, "encv");
          if (!video && !is(etype, "enca")) return;
          size_t skip = ehdr + (video ? kVisualEntryHeader : kAudioEntryHeader);
          if (esize <= skip) return;
          TrackProtection prot;
          prot.is_video = video;
          uint8_t original[4] = {0, 0, 0, 0};
          forEachBox(entry + skip, esize - skip, [&](uint8_t *sinf, size_t ssize, size_t shdr, uint8_t *stype) {
            if (!is(stype, "sinf")) return;
            forEachBox(sinf + shdr, ssize - shdr, [&](uint8_t *c, size_t csize, size_t chdr, uint8_t *ctype) {
              uint8_t *cb = c + chdr;
              size_t clen = csize - chdr;
              if (is(ctype, "frma") && clen >= 4) memcpy(original, cb, 4);
              if (is(ctype, "schm") && clen >= 8) prot.scheme.assign(reinterpret_cast<char *>(cb + 4), 4);
              if (!is(ctype, "schi")) return;
              forEachBox(cb, clen, [&](uint8_t *t, size_t tsize, size_t thdr, uint8_t *ttype) {
                if (!is(ttype, "tenc") || tsize - thdr < 24) return;
                uint8_t *tb = t + thdr;  // version, flags(3), reserved, pattern/reserved, ...
                if (tb[0] >= 1) {
                  prot.crypt_blocks = tb[5] >> 4;
                  prot.skip_blocks = tb[5] & 15;
                }
                prot.iv_size = tb[7];
                prot.kid.assign(reinterpret_cast<char *>(tb + 8), 16);
                if (tb[6] && prot.iv_size == 0 && tsize - thdr >= 25) {
                  uint8_t civ = tb[24];
                  if (tsize - thdr >= 25u + civ) prot.constant_iv.assign(reinterpret_cast<char *>(tb + 25), civ);
                }
              });
            });
            setType(stype, "free");  // drop the protection info, same size
          });
          if (original[0]) setType(etype, reinterpret_cast<char *>(original));
          std::lock_guard<std::mutex> l(m_);
          tracks_[track_id] = prot;
        });
      }
    });
  };
  walk(p, n);
}

int CencState::handleMoof(uint8_t *moof, size_t moof_len, uint8_t *buf, size_t buf_len,
                          const DecryptFn &decrypt) {
  int total = 0;
  bool failed = false;
  size_t moof_off = static_cast<size_t>(moof - buf);
  forEachBox(moof + 8, moof_len - 8, [&](uint8_t *traf, size_t tsize, size_t thdr, uint8_t *ttype) {
    if (!is(ttype, "traf") || failed) return;
    uint32_t track_id = 0, default_size = 0;
    std::vector<uint32_t> sizes;
    int64_t data_offset = 0;
    bool have_offset = false;
    std::vector<std::string> ivs;
    std::vector<std::vector<std::pair<uint32_t, uint32_t>>> subs;
    forEachBox(traf + thdr, tsize - thdr, [&](uint8_t *b, size_t size, size_t hdr, uint8_t *type) {
      uint8_t *body = b + hdr;
      size_t len = size - hdr;
      if (is(type, "tfhd") && len >= 8) {
        uint32_t flags = be32(body) & 0xFFFFFF;
        track_id = be32(body + 4);
        size_t o = 8;
        if (flags & 0x01) o += 8;   // base_data_offset (absolute: not supported, see below)
        if (flags & 0x02) o += 4;
        if (flags & 0x08) o += 4;
        if ((flags & 0x10) && o + 4 <= len) default_size = be32(body + o);
      } else if (is(type, "trun") && len >= 8) {
        uint32_t flags = be32(body) & 0xFFFFFF, count = be32(body + 4);
        size_t o = 8;
        if (flags & 0x01) {
          if (!have_offset) data_offset = static_cast<int32_t>(be32(body + o));
          have_offset = true;
          o += 4;
        }
        if (flags & 0x04) o += 4;
        for (uint32_t i = 0; i < count && o <= len; i++) {
          if (flags & 0x100) o += 4;
          uint32_t s = default_size;
          if (flags & 0x200) { if (o + 4 > len) break; s = be32(body + o); o += 4; }
          if (flags & 0x400) o += 4;
          if (flags & 0x800) o += 4;
          sizes.push_back(s);
        }
      } else if (is(type, "senc") && len >= 8) {
        std::lock_guard<std::mutex> l(m_);
        auto it = tracks_.find(track_id);
        uint8_t iv_size = it != tracks_.end() ? it->second.iv_size : 8;
        uint32_t flags = be32(body) & 0xFFFFFF, count = be32(body + 4);
        size_t o = 8;
        for (uint32_t i = 0; i < count; i++) {
          if (o + iv_size > len) break;
          ivs.emplace_back(reinterpret_cast<char *>(body + o), iv_size);
          o += iv_size;
          std::vector<std::pair<uint32_t, uint32_t>> s;
          if (flags & 0x02) {
            if (o + 2 > len) break;
            uint16_t n = be16(body + o);
            o += 2;
            for (uint16_t k = 0; k < n && o + 6 <= len; k++, o += 6)
              s.emplace_back(be16(body + o), be32(body + o + 2));
          }
          subs.push_back(std::move(s));
        }
        setType(type, "free");
      } else if (is(type, "saiz") || is(type, "saio")) {
        setType(type, "free");
      }
    });
    TrackProtection prot;
    {
      std::lock_guard<std::mutex> l(m_);
      auto it = tracks_.find(track_id);
      if (it == tracks_.end()) return;  // clear track
      prot = it->second;
    }
    if (ivs.empty() && prot.constant_iv.empty()) return;
    // Sample data: default-base-is-moof (what DASH/CMAF packagers write).
    size_t pos = moof_off + static_cast<size_t>(data_offset);
    std::vector<DecryptSample> samples;
    std::vector<std::string> iv16(sizes.size());
    for (size_t i = 0; i < sizes.size(); i++) {
      if (pos + sizes[i] > buf_len) break;  // sample outside this buffer
      iv16[i] = i < ivs.size() && !ivs[i].empty() ? ivs[i] : prot.constant_iv;
      iv16[i].resize(16, '\0');
      DecryptSample s;
      s.iv = reinterpret_cast<const uint8_t *>(iv16[i].data());
      s.data = buf + pos;
      s.size = sizes[i];
      if (i < subs.size() && !subs[i].empty()) s.subsamples = subs[i];
      else s.subsamples.emplace_back(0, sizes[i]);
      samples.push_back(std::move(s));
      pos += sizes[i];
    }
    if (samples.empty()) return;
    if (!decrypt(prot, samples)) { failed = true; return; }
    total += static_cast<int>(samples.size());
  });
  return failed ? -1 : total;
}

int CencState::process(std::string &str, const DecryptFn &decrypt) {
  uint8_t *buf = reinterpret_cast<uint8_t *>(&str[0]);
  size_t len = str.size();
  int total = 0;
  bool failed = false;
  forEachBox(buf, len, [&](uint8_t *box, size_t size, size_t hdr, uint8_t *type) {
    if (is(type, "moov")) {
      handleMoov(box + hdr, size - hdr);
    } else if (is(type, "pssh")) {
      std::lock_guard<std::mutex> l(m_);
      pssh_.emplace_back(reinterpret_cast<char *>(box), size);
      setType(type, "free");
    } else if (is(type, "moof")) {
      int n = handleMoof(box, size, buf, len, decrypt);
      if (n < 0) failed = true; else total += n;
    }
  });
  return failed ? -1 : total;
}

}  // namespace wvcompat
