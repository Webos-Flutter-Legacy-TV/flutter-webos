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

// hvcC (ISO/IEC 14496-15 8.3.3): NAL length size, then arrays of VPS, SPS,
// PPS (and SEI) NAL units, each prefixed with a 16-bit length.
void parseHvcC(const uint8_t *b, size_t n, Track *t) {
  if (n < 23) return;
  t->nal_length_size = (b[21] & 3) + 1;
  unsigned arrays = b[22];
  size_t o = 23;
  for (unsigned a = 0; a < arrays && o + 3 <= n; a++) {
    unsigned count = be16(b + o + 1);
    o += 3;
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

// 'colr' with colour type 'nclx': primaries, transfer, matrix, full range.
void parseColr(const uint8_t *b, size_t n, Track *t) {
  if (n < 11 || !is(b, "nclx")) return;
  t->colour_primaries = static_cast<uint8_t>(be16(b + 4));
  t->transfer = static_cast<uint8_t>(be16(b + 6));
  t->matrix = static_cast<uint8_t>(be16(b + 8));
  t->full_range = b[10] & 0x80;
  t->has_colour = true;
}

// 'vpcC' (VP codec configuration, version 1): colour information.
void parseVpcC(const uint8_t *b, size_t n, Track *t) {
  if (n < 12 || b[0] != 1 || t->has_colour) return;
  t->full_range = b[6] & 1;
  t->colour_primaries = b[7];
  t->transfer = b[8];
  t->matrix = b[9];
  t->has_colour = true;
}

// 'mdcv' (mastering display colour volume) and 'clli' (content light level).
void parseMdcv(const uint8_t *b, size_t n, Track *t) {
  if (n < 24) return;
  for (int i = 0; i < 6; i++) t->primaries_xy[i] = be16(b + 2 * i);
  t->white_xy[0] = be16(b + 12);
  t->white_xy[1] = be16(b + 14);
  t->max_luminance = be32(b + 16);
  t->min_luminance = be32(b + 20);
  t->has_mdcv = true;
}
void parseClli(const uint8_t *b, size_t n, Track *t) {
  if (n < 4) return;
  t->max_cll = be16(b);
  t->max_fall = be16(b + 2);
  t->has_clli = true;
}

// Channels of an AC-3 audio coding mode, plus the LFE channel.
uint32_t ac3Channels(unsigned acmod, unsigned lfe) {
  static const uint8_t ch[8] = {2, 1, 2, 3, 3, 4, 4, 5};
  return ch[acmod & 7] + (lfe ? 1 : 0);
}

// Bit reader for 'dac3' / 'dec3'.
struct Bits {
  const uint8_t *p;
  size_t n, pos = 0;
  unsigned get(int count) {
    unsigned v = 0;
    for (int i = 0; i < count; i++, pos++) v = (v << 1) | (pos / 8 < n ? (p[pos / 8] >> (7 - pos % 8)) & 1 : 0);
    return v;
  }
};

// 'dac3' (ETSI TS 102 366 F.4): fscod, bsid, bsmod, acmod, lfeon.
void parseDac3(const uint8_t *b, size_t n, Track *t) {
  if (n < 3) return;
  Bits r{b, n};
  static const uint32_t rates[3] = {48000, 44100, 32000};
  unsigned fscod = r.get(2);
  r.get(5);
  r.get(3);
  unsigned acmod = r.get(3), lfe = r.get(1);
  if (fscod < 3) t->sample_rate = rates[fscod];
  t->channels = ac3Channels(acmod, lfe);
}

// 'dec3' (ETSI TS 102 366 F.6): the first independent substream gives the
// rate and the base channels; dependent substreams add the channels in
// chan_loc; the extension flag marks Joint Object Coding (Atmos).
void parseDec3(const uint8_t *b, size_t n, Track *t) {
  if (n < 5) return;
  Bits r{b, n};
  static const uint32_t rates[3] = {48000, 44100, 32000};
  static const uint8_t loc_ch[9] = {2, 2, 1, 1, 2, 2, 2, 1, 1};  // Lc/Rc, Lrs/Rrs, Cs, Ts, Lsd/Rsd, Lw/Rw, Lvh/Rvh, Cvh, LFE2
  r.get(13);
  unsigned subs = r.get(3) + 1;
  for (unsigned i = 0; i < subs; i++) {
    unsigned fscod = r.get(2);
    r.get(5);
    r.get(1);
    r.get(1);
    r.get(3);
    unsigned acmod = r.get(3), lfe = r.get(1);
    r.get(3);
    unsigned deps = r.get(4);
    uint32_t ch = ac3Channels(acmod, lfe);
    if (deps) {
      unsigned loc = r.get(9);
      for (int k = 0; k < 9; k++)
        if (loc & (1u << k)) ch += loc_ch[k];  // bit 0 (LSB): Lc/Rc
    } else {
      r.get(1);
    }
    if (i == 0) {
      if (fscod < 3) t->sample_rate = rates[fscod];
      t->channels = ch;
    }
  }
  if (r.pos + 16 <= n * 8) {
    r.get(7);
    if (r.get(1)) t->atmos = true;  // flag_ec3_extension_type_a
  }
}

// 'dOps' (Opus in ISO BMFF) -> the OpusHead header (RFC 7845) the decoder takes.
void parseDops(const uint8_t *b, size_t n, Track *t) {
  if (n < 11 || b[0] != 0) return;
  std::string h("OpusHead", 8);
  h += char(1);              // version
  h += char(b[1]);           // channels
  h += char(b[3]);           // pre-skip, little-endian
  h += char(b[2]);
  h += char(b[7]); h += char(b[6]); h += char(b[5]); h += char(b[4]);  // input sample rate
  h += char(b[9]);           // output gain
  h += char(b[8]);
  h += char(b[10]);          // channel mapping family
  if (b[10] && n >= 13u + b[1]) h.append(reinterpret_cast<const char *>(b + 11), 2 + b[1]);
  t->opus_head = h;
  t->channels = b[1];
  t->sample_rate = 48000;
}

void parseSampleEntry(const uint8_t *e, size_t n, const uint8_t *type, Track *t) {
  bool v = is(type, "avc1") || is(type, "avc3") || is(type, "hev1") || is(type, "hvc1") || is(type, "dvh1") ||
           is(type, "dvhe") || is(type, "vp09") || is(type, "av01") || is(type, "encv");
  bool a = is(type, "mp4a") || is(type, "ac-3") || is(type, "ec-3") || is(type, "Opus") || is(type, "enca");
  if (!v && !a) return;
  size_t skip = v ? 78 : 28;
  if (n < skip) return;
  if (v) { t->width = be16(e + 24); t->height = be16(e + 26); }
  else { t->channels = be16(e + 16); t->sample_rate = be32(e + 24) >> 16; }
  std::string original(reinterpret_cast<const char *>(type), 4);
  boxes(e + skip, n - skip, e, [&](const uint8_t *b, size_t len, const uint8_t *ct, size_t) {
    if (is(ct, "avcC")) parseAvcC(b, len, t);
    if (is(ct, "hvcC")) parseHvcC(b, len, t);
    if (is(ct, "vpcC")) parseVpcC(b, len, t);
    if (is(ct, "av1C") && len > 4) t->av1_config.assign(reinterpret_cast<const char *>(b + 4), len - 4);
    if ((is(ct, "dvcC") || is(ct, "dvvC")) && len >= 4) {
      t->dv_profile = b[2] >> 1;
      t->dv_el = (b[3] >> 1) & 1;
    }
    if (is(ct, "colr")) parseColr(b, len, t);
    if (is(ct, "mdcv")) parseMdcv(b, len, t);
    if (is(ct, "clli")) parseClli(b, len, t);
    if (is(ct, "dac3")) parseDac3(b, len, t);
    if (is(ct, "dec3")) parseDec3(b, len, t);
    if (is(ct, "dOps")) parseDops(b, len, t);
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
  else if (original == "hev1" || original == "hvc1" || original == "dvh1" || original == "dvhe") t->codec = "H265";
  else if (original == "vp09") t->codec = "VP9";
  else if (original == "av01") t->codec = "AV1";
  else if (original == "mp4a") t->codec = "AAC";
  else if (original == "ac-3") t->codec = "AC3";
  else if (original == "ec-3") t->codec = "AC3 PLUS";
  else if (original == "Opus" && !t->opus_head.empty()) t->codec = "OPUS";
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

bool parseSegment(const std::string &seg, const Track &track, std::vector<Sample> *out, std::vector<std::string> *pssh) {
  const uint8_t *p = reinterpret_cast<const uint8_t *>(seg.data());
  bool ok = true;
  boxes(p, seg.size(), p, [&](const uint8_t *moof, size_t moof_len, const uint8_t *type, size_t moof_off) {
    if (!is(type, "moof")) return;
    boxes(moof, moof_len, p, [&](const uint8_t *traf, size_t tlen, const uint8_t *tt, size_t toff) {
      if (pssh && is(tt, "pssh") && tlen >= 20 && memcmp(traf + 4, kWidevineSystemId, 16) == 0)
        pssh->push_back(seg.substr(toff, tlen + 8));
      if (!is(tt, "traf")) return;
      // 'seig' sample group entries (key rotation): per group protection,
      // IV size and key id; sample -> group from 'sbgp'.
      struct Seig {
        bool prot;
        uint8_t iv_size;
        std::string kid, constant_iv;
      };
      std::vector<Seig> groups;
      std::vector<uint32_t> sample_group;  // 0: none, else 1-based index (traf-local)
      const uint8_t *senc = nullptr;
      size_t senc_len = 0;
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
          senc = b;
          senc_len = len;
        } else if (is(bt, "sgpd") && len >= 12 && is(b + 4, "seig")) {
          int version = b[0];
          size_t o = 8;
          uint32_t default_len = 0;
          if (version == 1) { default_len = be32(b + o); o += 4; }
          if (version >= 2) o += 4;
          if (o + 4 > len) return;
          uint32_t count = be32(b + o);
          o += 4;
          for (uint32_t i = 0; i < count; i++) {
            uint32_t elen = default_len;
            if (version == 1 && default_len == 0) { if (o + 4 > len) break; elen = be32(b + o); o += 4; }
            if (o + 20 > len) break;
            Seig g;
            g.prot = b[o + 2] != 0;
            g.iv_size = b[o + 3];
            g.kid.assign(reinterpret_cast<const char *>(b + o + 4), 16);
            size_t used = 20;
            if (g.prot && g.iv_size == 0 && o + 21 <= len) {
              uint8_t n = b[o + 20];
              if (o + 21 + n <= len) g.constant_iv.assign(reinterpret_cast<const char *>(b + o + 21), n);
              used = 21 + n;
            }
            groups.push_back(g);
            o += elen ? elen : used;
          }
        } else if (is(bt, "sbgp") && len >= 12 && is(b + 4, "seig")) {
          size_t o = 8 + (b[0] == 1 ? 4 : 0);
          if (o + 4 > len) return;
          uint32_t count = be32(b + o);
          o += 4;
          for (uint32_t i = 0; i < count && o + 8 <= len; i++, o += 8) {
            uint32_t n = be32(b + o), idx = be32(b + o + 4);
            if (idx > 0x10000) idx -= 0x10000;  // traf-local description
            sample_group.insert(sample_group.end(), n, idx);
          }
        }
      });
      if (track_id != track.id) return;
      auto group_of = [&](size_t i) -> const Seig * {
        if (i >= sample_group.size() || sample_group[i] == 0 || sample_group[i] > groups.size()) return nullptr;
        return &groups[sample_group[i] - 1];
      };
      if (senc) {
        uint32_t flags = be32(senc) & 0xffffff, count = be32(senc + 4);
        size_t o = 8;
        for (uint32_t i = 0; i < count; i++) {
          const Seig *g = group_of(i);
          uint8_t iv_size = g ? (g->prot ? g->iv_size : 0) : track.iv_size;
          if (o + iv_size > senc_len) break;
          ivs.emplace_back(reinterpret_cast<const char *>(senc + o), iv_size);
          o += iv_size;
          std::vector<std::pair<uint32_t, uint32_t>> ss;
          if (flags & 0x02) {
            if (o + 2 > senc_len) break;
            uint16_t n = be16(senc + o);
            o += 2;
            for (uint16_t k = 0; k < n && o + 6 <= senc_len; k++, o += 6) ss.emplace_back(be16(senc + o), be32(senc + o + 2));
          }
          subs.push_back(std::move(ss));
        }
      }
      bool protected_track = !track.scheme.empty() && (have_senc || !track.constant_iv.empty() || !groups.empty());
      for (size_t i = 0; i < samples.size(); i++) {
        Sample &s = samples[i];
        if (s.offset + s.size > seg.size()) { ok = false; break; }
        const Seig *g = group_of(i);
        if (protected_track && (!g || g->prot)) {
          s.encrypted = true;
          const std::string &civ = g && !g->constant_iv.empty() ? g->constant_iv : track.constant_iv;
          s.iv = i < ivs.size() && !ivs[i].empty() ? ivs[i] : civ;
          s.iv.resize(16, '\0');
          if (g) s.kid = g->kid;
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
