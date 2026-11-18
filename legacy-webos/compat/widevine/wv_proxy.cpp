// Local decrypting proxy for Widevine DASH on webOS releases whose media
// pipeline cannot do Widevine Modular for URL playback (webOS <= 25).
//
//   wvc_proxy_url(origin_mpd, licence_url) -> "http://127.0.0.1:<port>/m/<id>/<name>"
//
// The media pipeline then plays a clear DASH stream:
//  - /m/<id>/<name>.mpd: the origin manifest with ContentProtection removed;
//    the Widevine PSSH in it is used to get a licence through the TV's CDM;
//  - /m/<id>/<path>: the origin file next to the manifest; the requested byte
//    range is fetched, decrypted in place (same length) and returned.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "cenc_mp4.h"
#include "wv_cdm.h"

namespace wvcompat {
namespace {

void log(const char *fmt, const std::string &a = "", const std::string &b = "") {
  fprintf(stderr, "[wv-proxy] ");
  fprintf(stderr, fmt, a.c_str(), b.c_str());
  fputc('\n', stderr);
}

std::string b64decode(const std::string &in) {
  static const std::string tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  int val = 0, bits = -8;
  for (char c : in) {
    size_t p = tbl.find(c);
    if (p == std::string::npos) continue;
    val = (val << 6) + static_cast<int>(p);
    bits += 6;
    if (bits >= 0) { out.push_back(static_cast<char>((val >> bits) & 0xFF)); bits -= 8; }
  }
  return out;
}

// Removes every <ContentProtection ...>...</ContentProtection> or
// self-closing <ContentProtection .../> element.
std::string stripContentProtection(const std::string &mpd) {
  std::string out;
  size_t pos = 0;
  for (;;) {
    size_t a = mpd.find("<ContentProtection", pos);
    if (a == std::string::npos) break;
    out.append(mpd, pos, a - pos);
    size_t gt = mpd.find('>', a);
    if (gt == std::string::npos) { pos = mpd.size(); break; }
    if (mpd[gt - 1] == '/') { pos = gt + 1; continue; }
    size_t close = mpd.find("</ContentProtection>", gt);
    pos = close == std::string::npos ? gt + 1 : close + strlen("</ContentProtection>");
  }
  out.append(mpd, pos, std::string::npos);
  return out;
}

// First Widevine <cenc:pssh> (base64) in the manifest, decoded.
std::string widevinePssh(const std::string &mpd) {
  size_t pos = 0;
  while ((pos = mpd.find("<ContentProtection", pos)) != std::string::npos) {
    size_t end = mpd.find("</ContentProtection>", pos);
    std::string cp = mpd.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
    pos += 18;
    if (cp.find("edef8ba9-79d6-4ace-a3c8-27dcd51d21ed") == std::string::npos &&
        cp.find("EDEF8BA9-79D6-4ACE-A3C8-27DCD51D21ED") == std::string::npos)
      continue;
    size_t a = cp.find("pssh>");
    if (a == std::string::npos) continue;
    size_t b = cp.find('<', a + 5);
    return b64decode(cp.substr(a + 5, b - (a + 5)));
  }
  return std::string();
}

struct Stream {
  std::string origin_mpd, origin_dir, name, licence_url;
  std::mutex m;
  bool licensed = false;
  std::map<std::string, std::unique_ptr<CencState>> files;  // per origin file
};

class Proxy {
 public:
  bool start() {
    fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) return false;
    int one = 1;
    setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    if (bind(fd_, reinterpret_cast<sockaddr *>(&a), sizeof(a)) != 0 || listen(fd_, 16) != 0) return false;
    socklen_t len = sizeof(a);
    getsockname(fd_, reinterpret_cast<sockaddr *>(&a), &len);
    port_ = ntohs(a.sin_port);
    std::thread([this] { acceptLoop(); }).detach();
    return true;
  }
  int port() const { return port_; }

  int add(const std::string &mpd_url, const std::string &licence_url) {
    auto s = std::make_shared<Stream>();
    s->origin_mpd = mpd_url;
    size_t q = mpd_url.find('?');
    std::string path = mpd_url.substr(0, q);
    size_t slash = path.rfind('/');
    s->origin_dir = path.substr(0, slash + 1);
    s->name = path.substr(slash + 1);
    s->licence_url = licence_url;
    std::lock_guard<std::mutex> l(m_);
    streams_.push_back(s);
    return static_cast<int>(streams_.size() - 1);
  }
  std::shared_ptr<Stream> stream(int id) {
    std::lock_guard<std::mutex> l(m_);
    return id >= 0 && id < static_cast<int>(streams_.size()) ? streams_[id] : nullptr;
  }

  bool ensureCdm() {
    std::lock_guard<std::mutex> l(cdm_m_);
    if (host_) return true;
    std::string dir = appDataDir() + "/widevine";
    auto h = std::make_unique<Host>(dir);
    if (!h->open() || !h->ensureProvisioned()) {
      log("CDM unavailable: %s", h->error());
      return false;
    }
    log("CDM %s ready", h->version());
    host_ = std::move(h);
    return true;
  }

  bool license(Stream &s, const std::string &pssh) {
    if (!ensureCdm()) return false;
    std::lock_guard<std::mutex> l(cdm_m_);
    std::string challenge, licence;
    if (!host_->startSession(pssh, &session_, &challenge)) { log("session: %s", host_->error()); return false; }
    long http = Host::httpPost(s.licence_url, challenge, "application/octet-stream", &licence);
    if (http != 200 || !host_->updateSession(session_, licence)) {
      log("licence failed (HTTP %s) %s", std::to_string(http), host_->error());
      return false;
    }
    log("licence ok, session %s", session_);
    return true;
  }

  bool decrypt(const TrackProtection &t, std::vector<DecryptSample> &in) {
    std::lock_guard<std::mutex> l(cdm_m_);
    if (!host_) return false;
    // Reserve every sample's subsamples first: Sample keeps raw pointers into
    // this vector, so it must not reallocate while being filled.
    size_t total_subs = 0;
    for (auto &s : in) total_subs += s.subsamples.size();
    std::vector<Subsample> subs;
    subs.reserve(total_subs);
    std::vector<Sample> samples;
    std::vector<std::string> outs(in.size());
    for (size_t i = 0; i < in.size(); i++) {
      size_t first = subs.size();
      for (auto &p : in[i].subsamples) subs.push_back({p.first, p.second});
      outs[i].resize(in[i].size);
      Sample s{};
      s.iv = in[i].iv;
      s.iv_length = 16;
      s.data = in[i].data;
      s.data_length = in[i].size;
      s.subsamples = subs.data() + first;  // subs is reserved: pointers stay valid
      s.subsamples_length = static_cast<uint32_t>(in[i].subsamples.size());
      s.output = reinterpret_cast<uint8_t *>(&outs[i][0]);
      s.output_offset = 0;
      s.output_length = in[i].size;
      samples.push_back(s);
    }
    DecryptionBatch b{};
    b.samples = samples.data();
    b.samples_length = static_cast<uint32_t>(samples.size());
    b.key_id = reinterpret_cast<const uint8_t *>(t.kid.data());
    b.key_id_length = static_cast<uint32_t>(t.kid.size());
    bool cbcs = t.scheme == "cbcs";
    b.pattern_encrypted = cbcs ? t.crypt_blocks : 0;
    b.pattern_clear = cbcs ? t.skip_blocks : 0;
    b.is_secure = false;
    b.scheme = cbcs ? kSchemeCbc : kSchemeCtr;
    b.is_video = t.is_video;
    int32_t status = 0;
    if (!host_->decrypt(session_, b, &status)) {
      log("decrypt status %s", std::to_string(status));
      return false;
    }
    for (size_t i = 0; i < in.size(); i++) memcpy(in[i].data, outs[i].data(), in[i].size);
    return true;
  }

 private:
  static std::string appDataDir() {
    const char *base = getenv("FLUTTER_APPDATA_HOME");
    const char *app = getenv("FLUTTER_APP_ID");
    if (base && *base && app && *app) return std::string(base) + "/" + app;
    char exe[512];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    std::string p = n > 0 ? std::string(exe, static_cast<size_t>(n)) : std::string("/tmp/x");
    return p.substr(0, p.rfind('/')) + "/appdata";
  }

  void acceptLoop() {
    for (;;) {
      int c = accept(fd_, nullptr, nullptr);
      if (c < 0) continue;
      std::thread([this, c] { serve(c); close(c); }).detach();
    }
  }

  static void sendAll(int c, const std::string &s) {
    size_t off = 0;
    while (off < s.size()) {
      ssize_t n = send(c, s.data() + off, s.size() - off, MSG_NOSIGNAL);
      if (n <= 0) return;
      off += static_cast<size_t>(n);
    }
  }

  void reply(int c, int code, const std::string &body, const std::string &type,
             const std::string &content_range, bool head) {
    std::string status = code == 206 ? "206 Partial Content" : code == 200 ? "200 OK"
                       : code == 404 ? "404 Not Found" : "502 Bad Gateway";
    std::string h = "HTTP/1.1 " + status + "\r\nContent-Length: " + std::to_string(body.size()) +
                    "\r\nAccept-Ranges: bytes\r\nConnection: close\r\n";
    if (!type.empty()) h += "Content-Type: " + type + "\r\n";
    if (!content_range.empty()) h += "Content-Range: " + content_range + "\r\n";
    sendAll(c, h + "\r\n");
    if (!head) sendAll(c, body);
  }

  void serve(int c) {
    std::string req;
    char buf[4096];
    while (req.find("\r\n\r\n") == std::string::npos && req.size() < 65536) {
      ssize_t n = recv(c, buf, sizeof(buf), 0);
      if (n <= 0) return;
      req.append(buf, static_cast<size_t>(n));
    }
    bool head = req.compare(0, 5, "HEAD ") == 0;
    size_t sp1 = req.find(' '), sp2 = req.find(' ', sp1 + 1);
    std::string path = req.substr(sp1 + 1, sp2 - sp1 - 1);
    std::string range;
    size_t r = req.find("\r\nRange: bytes=");
    if (r == std::string::npos) r = req.find("\r\nrange: bytes=");
    if (r != std::string::npos) range = req.substr(r + 15, req.find("\r\n", r + 15) - (r + 15));

    // /m/<id>/<rest>
    if (path.compare(0, 3, "/m/") != 0) { reply(c, 404, "", "", "", head); return; }
    size_t s2 = path.find('/', 3);
    int id = atoi(path.substr(3, s2 - 3).c_str());
    std::string rest = s2 == std::string::npos ? "" : path.substr(s2 + 1);
    auto st = stream(id);
    if (!st) { reply(c, 404, "", "", "", head); return; }

    if (rest == st->name) {  // manifest
      std::string mpd, ctype;
      long http = Host::httpGet(st->origin_mpd, "", &mpd, nullptr, &ctype);
      if (http != 200) { reply(c, 502, "", "", "", head); return; }
      {
        std::lock_guard<std::mutex> l(st->m);
        if (!st->licensed) {
          std::string pssh = widevinePssh(mpd);
          if (pssh.empty()) log("no Widevine PSSH in manifest %s", st->origin_mpd);
          else st->licensed = license(*st, pssh);
        }
      }
      reply(c, 200, stripContentProtection(mpd), "application/dash+xml", "", head);
      return;
    }
    // media file next to the manifest
    std::string origin = st->origin_dir + rest;
    std::string body, crange, ctype;
    long http = Host::httpGet(origin, range, &body, &crange, &ctype);
    if (http != 200 && http != 206) { reply(c, http == 404 ? 404 : 502, "", "", "", head); return; }
    CencState *state;
    {
      std::lock_guard<std::mutex> l(st->m);
      auto &p = st->files[origin];
      if (!p) p.reset(new CencState);
      state = p.get();
    }
    int n = state->process(body, [this](const TrackProtection &t, std::vector<DecryptSample> &s) {
      return decrypt(t, s);
    });
    if (n < 0) log("decrypt failed for %s range %s", rest, range);
    reply(c, static_cast<int>(http), body, ctype, http == 206 ? crange : "", head);
  }

  int fd_ = -1;
  int port_ = 0;
  std::mutex m_;
  std::vector<std::shared_ptr<Stream>> streams_;
  std::mutex cdm_m_;
  std::unique_ptr<Host> host_;
  std::string session_;
};

Proxy *proxy() {
  static Proxy *p = nullptr;
  static std::once_flag once;
  std::call_once(once, [] {
    auto *x = new Proxy;
    if (x->start()) p = x;
    else log("cannot listen on 127.0.0.1");
  });
  return p;
}

}  // namespace
}  // namespace wvcompat

// C entry point for the NDL shim. Returns a local URL for the clear stream
// (static buffer, valid until the next call), or NULL.
extern "C" __attribute__((visibility("default")))
const char *wvc_proxy_url(const char *origin_mpd, const char *licence_url) {
  static std::string out;
  auto *p = wvcompat::proxy();
  if (!p || !origin_mpd || !licence_url) return nullptr;
  int id = p->add(origin_mpd, licence_url);
  auto st = p->stream(id);
  out = "http://127.0.0.1:" + std::to_string(p->port()) + "/m/" + std::to_string(id) + "/" + st->name;
  return out.c_str();
}
