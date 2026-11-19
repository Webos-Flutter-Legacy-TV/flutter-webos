#include "dash.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace wvl1 {
namespace {

// ---- tiny XML element scanner (attributes and nesting; no entities) -------
struct Elem {
  std::string name, attrs;
  size_t body = 0, end = 0;  // [body, end) inner content
  bool empty = false;
};

std::string attr(const std::string &attrs, const char *name) {
  std::string key = std::string(name) + "=";
  size_t i = 0;
  while ((i = attrs.find(key, i)) != std::string::npos) {
    if (i == 0 || attrs[i - 1] == ' ' || attrs[i - 1] == '\n' || attrs[i - 1] == '\t' || attrs[i - 1] == '\r') {
      size_t q = i + key.size();
      if (q < attrs.size() && (attrs[q] == '"' || attrs[q] == '\'')) {
        size_t e = attrs.find(attrs[q], q + 1);
        if (e != std::string::npos) {
          std::string v = attrs.substr(q + 1, e - q - 1);
          size_t a;
          while ((a = v.find("&amp;")) != std::string::npos) v.replace(a, 5, "&");
          return v;
        }
      }
    }
    i += key.size();
  }
  return "";
}

// Local name (without namespace prefix).
std::string local(const std::string &n) {
  size_t c = n.find(':');
  return c == std::string::npos ? n : n.substr(c + 1);
}

// Direct children of [from, to) with the given local name.
std::vector<Elem> children(const std::string &x, size_t from, size_t to, const char *name) {
  std::vector<Elem> out;
  size_t i = from;
  int depth = 0;
  while (i < to) {
    size_t lt = x.find('<', i);
    if (lt == std::string::npos || lt >= to) break;
    if (x.compare(lt, 4, "<!--") == 0) { size_t e = x.find("-->", lt); i = e == std::string::npos ? to : e + 3; continue; }
    if (x[lt + 1] == '?' || x[lt + 1] == '!') { i = x.find('>', lt) + 1; continue; }
    size_t gt = x.find('>', lt);
    if (gt == std::string::npos || gt >= to) break;
    if (x[lt + 1] == '/') { depth--; i = gt + 1; continue; }
    bool self = x[gt - 1] == '/';
    size_t ne = x.find_first_of(" \t\r\n/>", lt + 1);
    std::string n = x.substr(lt + 1, ne - lt - 1);
    if (depth == 0 && local(n) == name) {
      Elem e;
      e.name = n;
      e.attrs = x.substr(ne, (self ? gt - 1 : gt) - ne);
      e.empty = self;
      e.body = gt + 1;
      e.end = gt + 1;
      if (!self) {
        // find the matching close tag
        int d = 1;
        size_t j = gt + 1;
        while (d > 0) {
          size_t a = x.find('<', j);
          if (a == std::string::npos) { j = x.size(); break; }
          size_t b = x.find('>', a);
          if (b == std::string::npos) { j = x.size(); break; }
          if (x.compare(a, 4, "<!--") == 0) { size_t c = x.find("-->", a); j = c == std::string::npos ? x.size() : c + 3; continue; }
          if (x[a + 1] == '/') { if (--d == 0) { e.end = a; j = b + 1; break; } }
          else if (x[a + 1] != '?' && x[a + 1] != '!' && x[b - 1] != '/') d++;
          j = b + 1;
        }
        i = j;
      } else {
        i = gt + 1;
      }
      out.push_back(e);
      continue;
    }
    if (!self) depth++;
    i = gt + 1;
  }
  return out;
}

std::string text(const std::string &x, const Elem &e) {
  std::string t = x.substr(e.body, e.end - e.body);
  size_t a = t.find_first_not_of(" \t\r\n"), b = t.find_last_not_of(" \t\r\n");
  return a == std::string::npos ? "" : t.substr(a, b - a + 1);
}

// ISO 8601 duration (PT#H#M#S) -> seconds.
double isoDuration(const std::string &s) {
  double total = 0, num = 0;
  bool time = false;
  std::string n;
  for (char c : s) {
    if (c == 'T') { time = true; continue; }
    if ((c >= '0' && c <= '9') || c == '.') { n += c; continue; }
    num = n.empty() ? 0 : atof(n.c_str());
    n.clear();
    if (c == 'H') total += num * 3600;
    else if (c == 'M') total += time ? num * 60 : num * 30 * 86400;
    else if (c == 'S') total += num;
    else if (c == 'D') total += num * 86400;
  }
  return total;
}

std::string resolve(const std::string &base, const std::string &rel) {
  if (rel.find("://") != std::string::npos) return rel;
  if (!rel.empty() && rel[0] == '/') {
    size_t s = base.find("://");
    size_t h = s == std::string::npos ? std::string::npos : base.find('/', s + 3);
    return (h == std::string::npos ? base : base.substr(0, h)) + rel;
  }
  size_t q = base.find('?');
  std::string b = base.substr(0, q);
  return b.substr(0, b.rfind('/') + 1) + rel;
}

void range(const std::string &r, int64_t *a, int64_t *b) {
  size_t d = r.find('-');
  if (r.empty() || d == std::string::npos) return;
  *a = atoll(r.substr(0, d).c_str());
  *b = atoll(r.substr(d + 1).c_str());
}

std::string fill(std::string t, const std::string &id, uint32_t bw, uint64_t number, uint64_t time) {
  auto rep = [&](const char *key, const std::string &val) {
    size_t i;
    while ((i = t.find(key)) != std::string::npos) t.replace(i, strlen(key), val);
  };
  rep("$RepresentationID$", id);
  rep("$Bandwidth$", std::to_string(bw));
  rep("$Time$", std::to_string(time));
  // $Number$ and $Number%05d$
  size_t i;
  while ((i = t.find("$Number")) != std::string::npos) {
    size_t e = t.find('$', i + 1);
    if (e == std::string::npos) break;
    std::string fmt = t.substr(i + 7, e - i - 7);
    char buf[32];
    if (fmt.empty()) snprintf(buf, sizeof(buf), "%llu", (unsigned long long)number);
    else snprintf(buf, sizeof(buf), fmt.c_str(), (unsigned long long)number);
    t.replace(i, e - i + 1, buf);
  }
  return t;
}

struct TemplateInfo {
  std::string attrs;
  std::vector<Elem> timeline;
  std::string xml;
  bool present = false;
};

}  // namespace

bool parseMpd(const std::string &x, const std::string &mpd_url, Manifest *out, std::string *err) {
  auto mpds = children(x, 0, x.size(), "MPD");
  if (mpds.empty()) { *err = "no MPD element"; return false; }
  const Elem &mpd = mpds[0];
  if (attr(mpd.attrs, "type") == "dynamic") { *err = "live (dynamic) manifests are not supported"; return false; }
  out->duration = isoDuration(attr(mpd.attrs, "mediaPresentationDuration"));
  std::string base = mpd_url;
  for (auto &b : children(x, mpd.body, mpd.end, "BaseURL")) base = resolve(base, text(x, b));
  auto periods = children(x, mpd.body, mpd.end, "Period");
  if (periods.empty()) { *err = "no Period"; return false; }
  const Elem &period = periods[0];
  if (out->duration <= 0) out->duration = isoDuration(attr(period.attrs, "duration"));
  for (auto &b : children(x, period.body, period.end, "BaseURL")) base = resolve(base, text(x, b));

  for (auto &as : children(x, period.body, period.end, "AdaptationSet")) {
    std::string as_base = base;
    for (auto &b : children(x, as.body, as.end, "BaseURL")) as_base = resolve(as_base, text(x, b));
    std::string content = attr(as.attrs, "contentType"), as_mime = attr(as.attrs, "mimeType"),
                as_codecs = attr(as.attrs, "codecs");
    std::string as_pssh;
    for (auto &cp : children(x, as.body, as.end, "ContentProtection")) {
      std::string scheme = attr(cp.attrs, "schemeIdUri");
      for (auto &c : scheme) c = static_cast<char>(tolower(c));
      if (scheme.find("edef8ba9-79d6-4ace-a3c8-27dcd51d21ed") == std::string::npos) continue;
      for (auto &ps : children(x, cp.body, cp.end, "pssh")) as_pssh = text(x, ps);
    }
    TemplateInfo as_tpl;
    for (auto &st : children(x, as.body, as.end, "SegmentTemplate")) {
      as_tpl.present = true;
      as_tpl.attrs = st.attrs;
      for (auto &tl : children(x, st.body, st.end, "SegmentTimeline")) as_tpl.timeline = children(x, tl.body, tl.end, "S");
    }
    for (auto &r : children(x, as.body, as.end, "Representation")) {
      Representation rep;
      rep.id = attr(r.attrs, "id");
      rep.bandwidth = static_cast<uint32_t>(atol(attr(r.attrs, "bandwidth").c_str()));
      rep.width = static_cast<uint32_t>(atol(attr(r.attrs, "width").c_str()));
      rep.height = static_cast<uint32_t>(atol(attr(r.attrs, "height").c_str()));
      std::string fr = attr(r.attrs, "frameRate");
      if (fr.empty()) fr = attr(as.attrs, "frameRate");
      if (!fr.empty()) {
        double num = atof(fr.c_str()), den = 1;
        size_t sl = fr.find('/');
        if (sl != std::string::npos) den = atof(fr.c_str() + sl + 1);
        if (den > 0) rep.fps = static_cast<uint32_t>(std::ceil(num / den - 0.01));
      }
      rep.codecs = attr(r.attrs, "codecs");
      if (rep.codecs.empty()) rep.codecs = as_codecs;
      rep.mime = attr(r.attrs, "mimeType");
      if (rep.mime.empty()) rep.mime = as_mime;
      rep.content = content;
      if (rep.content.empty()) rep.content = rep.mime.substr(0, rep.mime.find('/'));
      rep.pssh_b64 = as_pssh;
      for (auto &cp : children(x, r.body, r.end, "ContentProtection"))
        for (auto &ps : children(x, cp.body, cp.end, "pssh")) rep.pssh_b64 = text(x, ps);
      std::string rbase = as_base;
      for (auto &b : children(x, r.body, r.end, "BaseURL")) rbase = resolve(rbase, text(x, b));

      auto sb = children(x, r.body, r.end, "SegmentBase");
      TemplateInfo tpl = as_tpl;
      for (auto &st : children(x, r.body, r.end, "SegmentTemplate")) {
        tpl.present = true;
        tpl.attrs = st.attrs;
        tpl.timeline.clear();
        for (auto &tl : children(x, st.body, st.end, "SegmentTimeline")) tpl.timeline = children(x, tl.body, tl.end, "S");
      }
      if (!sb.empty()) {
        rep.init.url = rbase;
        rep.index_first = rep.index_last = -1;
        range(attr(sb[0].attrs, "indexRange"), &rep.index_first, &rep.index_last);
        for (auto &in : children(x, sb[0].body, sb[0].end, "Initialization"))
          range(attr(in.attrs, "range"), &rep.init.first, &rep.init.last);
        if (rep.index_first < 0) continue;  // no index: unsupported
      } else if (tpl.present) {
        double timescale = atof(attr(tpl.attrs, "timescale").c_str());
        if (timescale <= 0) timescale = 1;
        uint64_t number = attr(tpl.attrs, "startNumber").empty() ? 1 : strtoull(attr(tpl.attrs, "startNumber").c_str(), nullptr, 10);
        std::string init_t = attr(tpl.attrs, "initialization"), media_t = attr(tpl.attrs, "media");
        rep.init.url = resolve(rbase, fill(init_t, rep.id, rep.bandwidth, 0, 0));
        if (!tpl.timeline.empty()) {
          uint64_t t = 0;
          for (auto &s : tpl.timeline) {
            if (!attr(s.attrs, "t").empty()) t = strtoull(attr(s.attrs, "t").c_str(), nullptr, 10);
            uint64_t d = strtoull(attr(s.attrs, "d").c_str(), nullptr, 10);
            long r = atol(attr(s.attrs, "r").c_str());
            if (r < 0) r = d ? static_cast<long>(std::ceil((out->duration * timescale - t) / d)) - 1 : 0;
            for (long k = 0; k <= r; k++) {
              Segment seg;
              seg.url = resolve(rbase, fill(media_t, rep.id, rep.bandwidth, number++, t));
              seg.start = t / timescale;
              seg.duration = d / timescale;
              rep.segments.push_back(seg);
              t += d;
            }
          }
        } else {
          double d = atof(attr(tpl.attrs, "duration").c_str());
          if (d <= 0 || out->duration <= 0) continue;
          size_t count = static_cast<size_t>(std::ceil(out->duration * timescale / d));
          for (size_t k = 0; k < count; k++) {
            Segment seg;
            seg.url = resolve(rbase, fill(media_t, rep.id, rep.bandwidth, number + k, 0));
            seg.start = k * d / timescale;
            seg.duration = d / timescale;
            rep.segments.push_back(seg);
          }
        }
      } else {
        continue;
      }
      out->reps.push_back(rep);
    }
  }
  if (out->reps.empty()) { *err = "no supported representation"; return false; }
  return true;
}

bool loadIndex(Representation *rep, const std::string &sidx) {
  const uint8_t *s = reinterpret_cast<const uint8_t *>(sidx.data());
  auto be32 = [](const uint8_t *p) { return (uint32_t(p[0]) << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; };
  if (sidx.size() < 32 || memcmp(s + 4, "sidx", 4) != 0) return false;
  uint32_t box = be32(s);
  int version = s[8];
  const uint8_t *q = s + 12 + 4;
  uint32_t timescale = be32(q);
  q += 4;
  uint64_t ept, first_offset;
  if (version == 0) { ept = be32(q); first_offset = be32(q + 4); q += 8; }
  else { ept = (uint64_t(be32(q)) << 32) | be32(q + 4); first_offset = (uint64_t(be32(q + 8)) << 32) | be32(q + 12); q += 16; }
  uint32_t refs = be32(q) & 0xffff;
  q += 4;
  if (sidx.size() < size_t(q - s) + 12u * refs || !timescale) return false;
  uint64_t off = static_cast<uint64_t>(rep->index_first) + box + first_offset;
  uint64_t t = ept;
  rep->segments.clear();
  for (uint32_t i = 0; i < refs; i++, q += 12) {
    uint32_t size = be32(q) & 0x7fffffff, dur = be32(q + 4);
    Segment seg;
    seg.url = rep->init.url;
    seg.first = static_cast<int64_t>(off);
    seg.last = static_cast<int64_t>(off + size - 1);
    seg.start = double(t) / timescale;
    seg.duration = double(dur) / timescale;
    rep->segments.push_back(seg);
    off += size;
    t += dur;
  }
  return !rep->segments.empty();
}

}  // namespace wvl1
