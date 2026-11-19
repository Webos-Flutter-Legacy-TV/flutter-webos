#include "fmp4.h"

#include <cstring>
#include <functional>

namespace wvl1 {
namespace {

uint16_t be16(const uint8_t *p) { return uint16_t((p[0] << 8) | p[1]); }
uint32_t be32(const uint8_t *p) { return (uint32_t(p[0]) << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
uint64_t be64(const uint8_t *p) { return (uint64_t(be32(p)) << 32) | be32(p + 4); }
bool is(const uint8_t *t, const char *s) { return memcmp(t, s, 4) == 0; }

using BoxFn = std::function<void(const uint8_t *body, size_t len, const uint8_t *type, size_t box_off)>;

// Calls fn for every box in [p, p+n); box_off is relative to `base`.
void boxes(const uint8_t *p, size_t n, const uint8_t *base, const BoxFn &fn) {
  size_t o = 0;
  while (o + 8 <= n) {
    uint64_t size = be32(p + o);
    size_t hdr = 8;
    if (size == 1) {
      if (o + 16 > n) return;
      size = be64(p + o + 8);
      hdr = 16;
    } else if (size == 0) {
      size = n - o;
    }
    if (size < hdr || o + size > n) return;
    fn(p + o + hdr, size - hdr, p + o + 4, static_cast<size_t>(p + o - base));
    o += size;
  }
}

const uint8_t kWidevineSystemId[16] = {0xed, 0xef, 0x8b, 0xa9, 0x79, 0xd6, 0x4a, 0xce,
                                       0xa3, 0xc8, 0x27, 0xdc, 0xd5, 0x1d, 0x21, 0xed};

void parseAvcC(const uint8_t *b, size_t n, Track *t) {
  if (n < 7) return;
  t->nal_length_size = (b[4] & 3) + 1;
  size_t o = 5;
  for (int pass = 0; pass < 2; pass++) {
    if (o >= n) return;
    unsigned count = pass == 0 ? (b[o] & 0x1f) : b[o];
    o++;
    for (unsigned i = 0; i < count && o + 2 <= n; i++) {
      uint16_t len = be16(b + o);
      o += 2;
      if (o + len > n) return;
      t->annexb_config.append("\0\0\0\1", 4);
      t->annexb_config.append(reinterpret_cast<const char *>(b + o), len);
      o += len;
    }
  }
}

// esds -> AudioSpecificConfig: object type, sampling frequency, channels.
void parseEsds(const uint8_t *b, size_t n, Track *t) {
  size_t o = 4;  // version/flags
  auto desc = [&](uint8_t *tag, size_t *len) {
    if (o >= n) return false;
    *tag = b[o++];
    *len = 0;
    for (int i = 0; i < 4 && o < n; i++) {
      uint8_t c = b[o++];
      *len = (*len << 7) | (c & 0x7f);
      if (!(c & 0x80)) break;
    }
    return true;
  };
  uint8_t tag;
  size_t len;
  while (desc(&tag, &len)) {
    if (tag == 3) { o += 3; continue; }           // ES_Descriptor: ES_ID, flags (no optional fields assumed)
    if (tag == 4) { o += 13; continue; }          // DecoderConfigDescriptor fixed part
    if (tag == 5 && o + 2 <= n) {                 // DecoderSpecificInfo = AudioSpecificConfig
      static const uint32_t rates[] = {96000, 88200, 64000, 48000, 44100, 32000, 24000,
                                       22050, 16000, 12000, 11025, 8000, 7350};
      uint32_t aot = b[o] >> 3;
      uint32_t sfi = ((b[o] & 7) << 1) | (b[o + 1] >> 7);
      uint32_t ch = (b[o + 1] >> 3) & 15;
      t->aac_object_type = aot;
      if (sfi < 13) t->sample_rate = rates[sfi];
      if (ch) t->channels = ch;
      return;
    }
    o += len;
  }
}

void parseSampleEntry(const uint8_t *e, size_t n, const uint8_t *type, Track *t) {
  bool v = is(type, "avc1") || is(type, "avc3") || is(type, "encv");
  bool a = is(type, "mp4a") || is(type, "enca");
  if (!v && !a) return;
  size_t skip = v ? 78 : 28;
  if (n < skip) return;
  if (v) { t->width = be16(e + 24); t->height = be16(e + 26); }
  else { t->channels = be16(e + 16); t->sample_rate = be32(e + 24) >> 16; }
  std::string original(reinterpret_cast<const char *>(type), 4);
  boxes(e + skip, n - skip, e, [&](const uint8_t *b, size_t len, const uint8_t *ct, size_t) {
    if (is(ct, "avcC")) parseAvcC(b, len, t);
    if (is(ct, "esds")) parseEsds(b, len, t);
    if (!is(ct, "sinf")) return;
    boxes(b, len, b, [&](const uint8_t *sb, size_t slen, const uint8_t *st, size_t) {
      if (is(st, "frma") && slen >= 4) original.assign(reinterpret_cast<const char *>(sb), 4);
      if (is(st, "schm") && slen >= 8) t->scheme.assign(reinterpret_cast<const char *>(sb + 4), 4);
      if (!is(st, "schi")) return;
      boxes(sb, slen, sb, [&](const uint8_t *tb, size_t tlen, const uint8_t *tt, size_t) {
        if (!is(tt, "tenc") || tlen < 24) return;
        if (tb[0] >= 1) { t->crypt_blocks = tb[5] >> 4; t->skip_blocks = tb[5] & 15; }
        t->iv_size = tb[7];
        t->kid.assign(reinterpret_cast<const char *>(tb + 8), 16);
        if (tb[6] && t->iv_size == 0 && tlen >= 25 && tlen >= 25u + tb[24])
          t->constant_iv.assign(reinterpret_cast<const char *>(tb + 25), tb[24]);
      });
    });
  });
  if (original == "avc1" || original == "avc3") t->codec = "H264";
  else if (original == "mp4a") t->codec = "AAC";
}

}  // namespace

bool parseInit(const std::string &init, Track *t, std::string *pssh) {
  const uint8_t *p = reinterpret_cast<const uint8_t *>(init.data());
  bool found = false;
  auto take_pssh = [&](const uint8_t *b, size_t len, size_t off) {
    if (pssh && pssh->empty() && len >= 20 && memcmp(b + 4, kWidevineSystemId, 16) == 0)
      pssh->assign(init, off, len + 8);
  };
  boxes(p, init.size(), p, [&](const uint8_t *b, size_t len, const uint8_t *type, size_t off) {
    if (is(type, "pssh")) take_pssh(b, len, off);
    if (!is(type, "moov")) return;
    boxes(b, len, p, [&](const uint8_t *mb, size_t mlen, const uint8_t *mt, size_t moff) {
      if (is(mt, "pssh")) take_pssh(mb, mlen, moff);
      if (is(mt, "mvex")) {
        boxes(mb, mlen, p, [&](const uint8_t *xb, size_t xlen, const uint8_t *xt, size_t) {
          if (is(xt, "trex") && xlen >= 24) {
            t->default_duration = be32(xb + 12);
            t->default_size = be32(xb + 16);
            t->default_flags = be32(xb + 20);
          }
        });
      }
      if (!is(mt, "trak") || found) return;
      Track tr = *t;
      bool media = false;
      std::function<void(const uint8_t *, size_t)> walk = [&](const uint8_t *wb, size_t wlen) {
        boxes(wb, wlen, p, [&](const uint8_t *cb, size_t clen, const uint8_t *ct, size_t) {
          if (is(ct, "tkhd") && clen >= 24) tr.id = be32(cb + (cb[0] == 1 ? 20 : 12));
          else if (is(ct, "mdhd") && clen >= 24) tr.timescale = be32(cb + (cb[0] == 1 ? 20 : 12));
          else if (is(ct, "hdlr") && clen >= 12) {
            tr.video = is(cb + 8, "vide");
            media = tr.video || is(cb + 8, "soun");
          } else if (is(ct, "mdia") || is(ct, "minf") || is(ct, "stbl")) walk(cb, clen);
          else if (is(ct, "stsd") && clen >= 8) {
            boxes(cb + 8, clen - 8, p, [&](const uint8_t *eb, size_t elen, const uint8_t *et, size_t) {
              if (tr.codec.empty()) parseSampleEntry(eb, elen, et, &tr);
            });
          }
        });
      };
      walk(mb, mlen);
      if (media && !tr.codec.empty()) { *t = tr; found = true; }
    });
  });
  return found;
}

bool parseSegment(const std::string &seg, const Track &track, std::vector<Sample> *out) {
  const uint8_t *p = reinterpret_cast<const uint8_t *>(seg.data());
  bool ok = true;
  boxes(p, seg.size(), p, [&](const uint8_t *moof, size_t moof_len, const uint8_t *type, size_t moof_off) {
    if (!is(type, "moof")) return;
    boxes(moof, moof_len, p, [&](const uint8_t *traf, size_t tlen, const uint8_t *tt, size_t) {
      if (!is(tt, "traf")) return;
      uint32_t track_id = 0, dflt_dur = track.default_duration, dflt_size = track.default_size,
               dflt_flags = track.default_flags;
      uint64_t base_dts = 0;
      bool have_next = false;
      int64_t next_dts = 0;
      size_t next_pos = 0;
      uint64_t base_off = moof_off;
      std::vector<Sample> samples;
      std::vector<std::string> ivs;
      std::vector<std::vector<std::pair<uint32_t, uint32_t>>> subs;
      bool have_senc = false;
      boxes(traf, tlen, p, [&](const uint8_t *b, size_t len, const uint8_t *bt, size_t) {
        if (is(bt, "tfhd") && len >= 8) {
          uint32_t flags = be32(b) & 0xffffff;
          track_id = be32(b + 4);
          size_t o = 8;
          if (flags & 0x01) { base_off = be64(b + o); o += 8; }
          if (flags & 0x02) o += 4;
          if (flags & 0x08) { dflt_dur = be32(b + o); o += 4; }
          if (flags & 0x10) { dflt_size = be32(b + o); o += 4; }
          if (flags & 0x20) { dflt_flags = be32(b + o); o += 4; }
        } else if (is(bt, "tfdt") && len >= 8) {
          base_dts = b[0] == 1 ? be64(b + 4) : be32(b + 4);
        } else if (is(bt, "trun") && len >= 8) {
          uint32_t flags = be32(b) & 0xffffff, count = be32(b + 4);
          int version = b[0];
          size_t o = 8;
          int64_t data_off = 0;
          if (flags & 0x01) { data_off = static_cast<int32_t>(be32(b + o)); o += 4; }
          uint32_t first_flags = dflt_flags;
          bool has_first = false;
          if (flags & 0x04) { first_flags = be32(b + o); has_first = true; o += 4; }
          // A later trun continues where the previous one ended unless it
          // carries its own data offset.
          size_t pos = (flags & 0x01) || !have_next ? static_cast<size_t>(base_off + data_off) : next_pos;
          int64_t dts = have_next ? next_dts : static_cast<int64_t>(base_dts);
          for (uint32_t i = 0; i < count; i++) {
            uint32_t dur = dflt_dur, size = dflt_size, sflags = (i == 0 && has_first) ? first_flags : dflt_flags;
            int64_t cto = 0;
            if (flags & 0x100) { if (o + 4 > len) break; dur = be32(b + o); o += 4; }
            if (flags & 0x200) { if (o + 4 > len) break; size = be32(b + o); o += 4; }
            if (flags & 0x400) { if (o + 4 > len) break; sflags = be32(b + o); o += 4; }
            if (flags & 0x800) {
              if (o + 4 > len) break;
              cto = version == 0 ? int64_t(be32(b + o)) : int64_t(int32_t(be32(b + o)));
              o += 4;
            }
            Sample s;
            s.offset = pos;
            s.size = size;
            s.dts = dts;
            s.pts = s.dts + cto;
            s.sync = track.video ? !(sflags & 0x10000) : true;
            samples.push_back(s);
            pos += size;
            dts = s.dts + dur;
          }
          next_dts = dts;
          next_pos = pos;
          have_next = true;
        } else if (is(bt, "senc") && len >= 8) {
          have_senc = true;
          uint32_t flags = be32(b) & 0xffffff, count = be32(b + 4);
          size_t o = 8;
          for (uint32_t i = 0; i < count; i++) {
            if (o + track.iv_size > len) break;
            ivs.emplace_back(reinterpret_cast<const char *>(b + o), track.iv_size);
            o += track.iv_size;
            std::vector<std::pair<uint32_t, uint32_t>> ss;
            if (flags & 0x02) {
              if (o + 2 > len) break;
              uint16_t n = be16(b + o);
              o += 2;
              for (uint16_t k = 0; k < n && o + 6 <= len; k++, o += 6) ss.emplace_back(be16(b + o), be32(b + o + 2));
            }
            subs.push_back(std::move(ss));
          }
        }
      });
      if (track_id != track.id) return;
      bool protected_track = !track.scheme.empty() && (have_senc || !track.constant_iv.empty());
      for (size_t i = 0; i < samples.size(); i++) {
        Sample &s = samples[i];
        if (s.offset + s.size > seg.size()) { ok = false; break; }
        if (protected_track) {
          s.encrypted = true;
          s.iv = i < ivs.size() && !ivs[i].empty() ? ivs[i] : track.constant_iv;
          s.iv.resize(16, '\0');
          if (i < subs.size() && !subs[i].empty()) s.subsamples = subs[i];
          else s.subsamples.emplace_back(0, s.size);
        }
        out->push_back(std::move(s));
      }
    });
  });
  return ok;
}

}  // namespace wvl1
