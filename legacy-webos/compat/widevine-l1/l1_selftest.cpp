// Feasibility test for hardware (secure video path) Widevine on webOS:
// loads the TV's /usr/lib/libwvcdm_shared.so through the Chromium CDM
// interface (version 10, webOS variant), gets a licence for a DASH test
// stream and passes the first video and audio samples through Decrypt().
// It prints what Decrypt() returns, to learn the output format the
// secure path uses. Nothing is written to the video pipeline yet.
//
// usage: l1-selftest <storage dir> [mpd url] [licence url]
#include <dlfcn.h>
#include <stdarg.h>
#include <sys/stat.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#define TARGET_WEBOS 1
#include "third_party/cdm/content_decryption_module.h"
#include "../widevine/cenc_mp4.h"
#include "../widevine/wv_cdm.h"

namespace {

using Clock = std::chrono::steady_clock;

// ---- minimal single-thread task loop (CDM callbacks may come from any thread)
std::mutex g_m;
std::condition_variable g_cv;
std::deque<std::function<void()>> g_tasks;
struct Timer { Clock::time_point due; void *ctx; };
std::vector<Timer> g_timers;
cdm::ContentDecryptionModule_10 *g_cdm = nullptr;

void post(std::function<void()> f) {
  std::lock_guard<std::mutex> l(g_m);
  g_tasks.push_back(std::move(f));
  g_cv.notify_all();
}

// Runs tasks and timers until done() or the timeout; returns done().
bool pump(const std::function<bool()> &done, int timeout_ms) {
  auto end = Clock::now() + std::chrono::milliseconds(timeout_ms);
  while (!done()) {
    std::function<void()> task;
    void *timer_ctx = nullptr;
    bool timer = false;
    {
      std::unique_lock<std::mutex> l(g_m);
      if (Clock::now() > end) return done();
      if (g_tasks.empty()) {
        for (size_t i = 0; i < g_timers.size(); i++) {
          if (g_timers[i].due <= Clock::now()) {
            timer_ctx = g_timers[i].ctx;
            g_timers.erase(g_timers.begin() + i);
            timer = true;
            break;
          }
        }
        if (!timer) {
          g_cv.wait_for(l, std::chrono::milliseconds(20));
          continue;
        }
      } else {
        task = std::move(g_tasks.front());
        g_tasks.pop_front();
      }
    }
    if (timer) {
      if (g_cdm) g_cdm->TimerExpired(timer_ctx);
    } else {
      task();
    }
  }
  return true;
}

std::string hex(const uint8_t *p, size_t n) {
  static const char *d = "0123456789abcdef";
  std::string s;
  for (size_t i = 0; i < n; i++) { s += d[p[i] >> 4]; s += d[p[i] & 15]; }
  return s;
}
std::string hex(const std::string &s) { return hex(reinterpret_cast<const uint8_t *>(s.data()), s.size()); }

// ---- CDM host objects -------------------------------------------------------
class HeapBuffer : public cdm::Buffer {
 public:
  explicit HeapBuffer(uint32_t cap) : data_(cap) {}
  void Destroy() override { delete this; }
  uint32_t Capacity() const override { return static_cast<uint32_t>(data_.size()); }
  uint8_t *Data() override { return data_.data(); }
  void SetSize(uint32_t size) override { size_ = size; }
  uint32_t Size() const override { return size_; }
 private:
  std::vector<uint8_t> data_;
  uint32_t size_ = 0;
};

class Block : public cdm::DecryptedBlock {
 public:
  ~Block() override { if (buf_) buf_->Destroy(); }
  void SetDecryptedBuffer(cdm::Buffer *b) override { buf_ = b; }
  cdm::Buffer *DecryptedBuffer() override { return buf_; }
  void SetTimestamp(int64_t t) override { ts_ = t; }
  int64_t Timestamp() const override { return ts_; }
 private:
  cdm::Buffer *buf_ = nullptr;
  int64_t ts_ = 0;
};

// Files in the storage directory; completions are posted to the task loop.
class File : public cdm::FileIO {
 public:
  File(cdm::FileIOClient *c, std::string dir) : client_(c), dir_(std::move(dir)) {}
  void Open(const char *name, uint32_t n) override {
    path_ = dir_ + "/" + std::string(name, n);
    printf("  [file] open %s\n", std::string(name, n).c_str());
    post([this] { client_->OnOpenComplete(cdm::FileIOClient::Status::kSuccess); });
  }
  void Read() override {
    std::ifstream f(path_, std::ios::binary);
    data_.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    post([this] {
      client_->OnReadComplete(cdm::FileIOClient::Status::kSuccess,
                              reinterpret_cast<const uint8_t *>(data_.data()),
                              static_cast<uint32_t>(data_.size()));
    });
  }
  void Write(const uint8_t *p, uint32_t n) override {
    std::ofstream f(path_, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char *>(p), n);
    bool ok = static_cast<bool>(f);
    post([this, ok] {
      client_->OnWriteComplete(ok ? cdm::FileIOClient::Status::kSuccess
                                  : cdm::FileIOClient::Status::kError);
    });
  }
  void Close() override { post([this] { delete this; }); }
 private:
  cdm::FileIOClient *client_;
  std::string dir_, path_, data_;
};

class Host : public cdm::Host_10 {
 public:
  explicit Host(std::string dir) : dir_(std::move(dir)) {}
  std::string dir_;
  int initialized = -1;
  uint32_t resolved = 0, rejected = 0;
  std::string reject_msg, session;
  std::vector<std::pair<cdm::MessageType, std::string>> messages;
  int keys_changes = 0;

  cdm::Buffer *Allocate(uint32_t cap) override { return new HeapBuffer(cap); }
  void SetTimer(int64_t ms, void *ctx) override {
    std::lock_guard<std::mutex> l(g_m);
    g_timers.push_back({Clock::now() + std::chrono::milliseconds(ms), ctx});
  }
  cdm::Time GetCurrentWallTime() override {
    return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
  }
  void OnInitialized(bool ok) override { printf("  [host] OnInitialized %d\n", ok); initialized = ok; }
  void OnResolveKeyStatusPromise(uint32_t id, cdm::KeyStatus s) override {
    printf("  [host] key status promise %u: %u\n", id, s); resolved = id;
  }
  void OnResolveNewSessionPromise(uint32_t id, const char *sid, uint32_t n) override {
    session.assign(sid, n);
    printf("  [host] new session %s (promise %u)\n", session.c_str(), id);
    resolved = id;
  }
  void OnResolvePromise(uint32_t id) override { printf("  [host] promise %u resolved\n", id); resolved = id; }
  void OnRejectPromise(uint32_t id, cdm::Exception e, uint32_t code, const char *msg, uint32_t n) override {
    reject_msg.assign(msg ? msg : "", msg ? n : 0);
    printf("  [host] promise %u REJECTED exception %u code %u: %s\n", id, e, code, reject_msg.c_str());
    rejected = id;
  }
  void OnSessionMessage(const char *, uint32_t, cdm::MessageType t, const char *m, uint32_t n) override {
    printf("  [host] session message type %u, %u bytes\n", t, n);
    std::lock_guard<std::mutex> l(g_m);
    messages.emplace_back(t, std::string(m, n));
  }
  void OnSessionKeysChange(const char *, uint32_t, bool more, const cdm::KeyInformation *k, uint32_t count) override {
    printf("  [host] keys change: %u key(s), additional usable %d\n", count, more);
    for (uint32_t i = 0; i < count; i++)
      printf("         kid %s status %u system %u\n", hex(k[i].key_id, k[i].key_id_size).c_str(), k[i].status,
             k[i].system_code);
    keys_changes++;
  }
  void OnExpirationChange(const char *, uint32_t, cdm::Time t) override { printf("  [host] expiry %.0f\n", t); }
  void OnSessionClosed(const char *, uint32_t) override { printf("  [host] session closed\n"); }
  void SendPlatformChallenge(const char *, uint32_t, const char *, uint32_t) override {
    printf("  [host] platform challenge (unsupported)\n");
  }
  void EnableOutputProtection(uint32_t mask) override { printf("  [host] EnableOutputProtection %u\n", mask); }
  void QueryOutputProtectionStatus() override { printf("  [host] QueryOutputProtectionStatus\n"); }
  void OnDeferredInitializationDone(cdm::StreamType t, cdm::Status s) override {
    printf("  [host] deferred init %u: %u\n", t, s);
  }
  cdm::FileIO *CreateFileIO(cdm::FileIOClient *c) override { return new File(c, dir_); }
  void RequestStorageId(uint32_t v) override { printf("  [host] RequestStorageId %u\n", v); }
};

Host *g_host = nullptr;
void *GetHost(int version, void *) {
  printf("  [cdm] asks for host interface %d\n", version);
  return version == cdm::Host_10::kVersion ? static_cast<cdm::Host_10 *>(g_host) : nullptr;
}

uint32_t be32(const uint8_t *p) { return (uint32_t(p[0]) << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }

std::string b64decode(const std::string &in) {
  static const std::string a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  int val = 0, bits = -8;
  for (char c : in) {
    size_t i = a.find(c);
    if (i == std::string::npos) continue;
    val = (val << 6) + static_cast<int>(i);
    bits += 6;
    if (bits >= 0) { out += char((val >> bits) & 0xff); bits -= 8; }
  }
  return out;
}

std::string between(const std::string &s, const std::string &a, const std::string &b, size_t from = 0) {
  size_t i = s.find(a, from);
  if (i == std::string::npos) return "";
  i += a.size();
  size_t j = s.find(b, i);
  return j == std::string::npos ? "" : s.substr(i, j - i);
}

// Fetches init segment + sidx of one representation, then its first media
// segment; returns the whole thing concatenated (starts at a box boundary).
bool fetchFirstSegment(const std::string &url, const std::string &init_end_s, std::string *out) {
  std::string body, cr, ct;
  long http = wvcompat::Host::httpGet(url, "0-65535", &body, &cr, &ct);
  if (http != 206 && http != 200) { printf("FAIL GET %s: HTTP %ld\n", url.c_str(), http); return false; }
  size_t init_end = std::stoul(init_end_s) + 1;
  const uint8_t *p = reinterpret_cast<const uint8_t *>(body.data());
  if (body.size() < init_end + 32 || memcmp(p + init_end + 4, "sidx", 4) != 0) {
    printf("FAIL no sidx after init\n");
    return false;
  }
  const uint8_t *s = p + init_end;
  uint32_t sidx_size = be32(s);
  int version = s[8];
  const uint8_t *q = s + 12 + 4 + 4;  // header+version/flags, reference_ID, timescale
  uint64_t first_offset;
  if (version == 0) { first_offset = be32(q + 4); q += 8; } else { first_offset = (uint64_t(be32(q + 8)) << 32) | be32(q + 12); q += 16; }
  uint32_t refs = be32(q) & 0xffff;
  q += 4;  // reserved + reference_count
  // The test streams start with clear segments: take the 6th (or the last).
  uint64_t seg_start = init_end + sidx_size + first_offset;
  uint32_t pick = refs > 5 ? 5 : refs - 1;
  for (uint32_t i = 0; i < pick; i++) seg_start += be32(q + 12 * i) & 0x7fffffff;
  uint32_t seg_size = be32(q + 12 * pick) & 0x7fffffff;
  printf("media segment %u of %u: offset %llu, %u bytes\n", pick + 1, refs, (unsigned long long)seg_start, seg_size);
  std::string seg;
  http = wvcompat::Host::httpGet(url, std::to_string(seg_start) + "-" + std::to_string(seg_start + seg_size - 1), &seg,
                                 &cr, &ct);
  if (http != 206 && http != 200) { printf("FAIL GET segment: HTTP %ld\n", http); return false; }
  *out = body.substr(0, init_end) + seg;
  return true;
}

// SignedMessage { 1: type (varint), 2: msg (bytes), ... } -> msg, or the
// input unchanged if it does not parse.
std::string signedMessageBody(const std::string &in) {
  const uint8_t *p = reinterpret_cast<const uint8_t *>(in.data()), *end = p + in.size();
  auto varint = [&](uint64_t *v) {
    *v = 0;
    for (int shift = 0; p < end && shift < 64; shift += 7) {
      uint8_t b = *p++;
      *v |= uint64_t(b & 0x7f) << shift;
      if (!(b & 0x80)) return true;
    }
    return false;
  };
  while (p < end) {
    uint64_t key, v;
    if (!varint(&key)) break;
    int wire = key & 7;
    if (wire == 0) { if (!varint(&v)) break; continue; }
    if (wire != 2 || !varint(&v) || v > uint64_t(end - p)) break;
    if ((key >> 3) == 2) return std::string(reinterpret_cast<const char *>(p), v);
    p += v;
  }
  return in;
}

}  // namespace

// Prints the CDM wrapper's PmLog messages (it logs with PmLogDebug/PmLogInfo
// to the "widevine-modular" context); the executable's definition wins over
// libPmLogLib's when linked with -rdynamic.
extern "C" __attribute__((visibility("default"))) int _PmLogMsgKV(void *, int level, unsigned int, const char *msgid,
                                                                  size_t kv_count, const char *, const char *,
                                                                  const char *fmt, ...) {
  if (!fmt) return 0;
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  printf("  [pmlog %d%s%s%s] %s\n", level, msgid ? " " : "", msgid ? msgid : "", kv_count ? " kv" : "", buf);
  return 0;
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s <storage dir> [mpd url] [licence url]\n", argv[0]);
    return 2;
  }
  setvbuf(stdout, nullptr, _IOLBF, 0);
  std::string dir = argv[1];
  std::string mpd_url = argc > 2 ? argv[2] : "https://storage.googleapis.com/wvmedia/cenc/h264/tears/tears.mpd";
  std::string lic_url = argc > 3 ? argv[3] : "https://proxy.uat.widevine.com/proxy?provider=widevine_test";
  mkdir(dir.c_str(), 0700);

  void *lib = dlopen("/usr/lib/libwvcdm_shared.so", RTLD_NOW);
  if (!lib) { printf("FAIL dlopen: %s\n", dlerror()); return 1; }
  auto init_module = reinterpret_cast<void (*)()>(dlsym(lib, "InitializeCdmModule_10"));
  auto create = reinterpret_cast<void *(*)(int, const char *, uint32_t, GetCdmHostFunc, void *)>(
      dlsym(lib, "CreateCdmInstance"));
  auto get_version = reinterpret_cast<const char *(*)()>(dlsym(lib, "GetCdmVersion"));
  if (!init_module || !create || !get_version) { printf("FAIL missing CDM entry points\n"); return 1; }
  init_module();
  printf("CDM version %s\n", get_version());

  g_host = new Host(dir);
  const char ks[] = "com.widevine.alpha";
  void *inst = create(10, ks, sizeof(ks) - 1, GetHost, nullptr);
  if (!inst) { printf("FAIL CreateCdmInstance(10) returned null\n"); return 1; }
  g_cdm = static_cast<cdm::ContentDecryptionModule_10 *>(inst);
  printf("CDM instance created\n");

  g_cdm->Initialize(true, true, true);
  if (!pump([] { return g_host->initialized >= 0; }, 10000) || g_host->initialized != 1) {
    printf("FAIL Initialize (state %d)\n", g_host->initialized);
    return 1;
  }

  // Manifest: Widevine PSSH, first video and audio representation.
  std::string mpd, cr, ct;
  if (wvcompat::Host::httpGet(mpd_url, "", &mpd, &cr, &ct) != 200) { printf("FAIL GET manifest\n"); return 1; }
  std::string pssh = b64decode(between(mpd, "<cenc:pssh>", "</cenc:pssh>"));
  std::string base = mpd_url.substr(0, mpd_url.rfind('/') + 1);
  size_t vset = mpd.find("contentType=\"video\"");
  size_t aset = mpd.find("contentType=\"audio\"");
  std::string vfile = between(mpd, "<BaseURL>", "</BaseURL>", vset);
  std::string vinit = between(mpd, "<Initialization range=\"0-", "\"", vset);
  std::string afile = between(mpd, "<BaseURL>", "</BaseURL>", aset);
  std::string ainit = between(mpd, "<Initialization range=\"0-", "\"", aset);
  printf("pssh %zu bytes; video %s; audio %s\n", pssh.size(), vfile.c_str(), afile.c_str());

  // Service certificate (the CDM runs in privacy mode): the licence server
  // answers a SignedMessage of type SERVICE_CERTIFICATE_REQUEST (08 04).
  {
    std::string cert;
    long http = wvcompat::Host::httpPost(lic_url, std::string("\x08\x04", 2), "application/octet-stream", &cert);
    printf("service certificate: HTTP %ld, %zu bytes\n", http, cert.size());
    if (http != 200) return 1;
    cert = signedMessageBody(cert);
    printf("service certificate body %zu bytes\n", cert.size());
    g_cdm->SetServerCertificate(100, reinterpret_cast<const uint8_t *>(cert.data()),
                                static_cast<uint32_t>(cert.size()));
    pump([] { return g_host->resolved == 100 || g_host->rejected == 100; }, 10000);
    if (g_host->rejected == 100) return 1;
    // LG's CDM_10 SetServerCertificate installs it for provisioning only
    // (ServiceRole 1); licence requests in privacy mode need the licensing
    // role (2), which LG's C export SetServiceCertificate uses. The wrapped
    // widevine::Cdm is at offset 8; setServiceCertificate is vtable slot 3.
    struct CeCdm { void **vtbl; };
    CeCdm *ce = *reinterpret_cast<CeCdm **>(reinterpret_cast<char *>(g_cdm) + 8);
    auto set_cert = reinterpret_cast<int (*)(CeCdm *, int, const std::string &)>(ce->vtbl[3]);
    printf("setServiceCertificate(licensing): status %d\n", set_cert(ce, 2, cert));
  }

  // Licence.
  g_cdm->CreateSessionAndGenerateRequest(1, cdm::kTemporary, cdm::kCenc,
                                         reinterpret_cast<const uint8_t *>(pssh.data()),
                                         static_cast<uint32_t>(pssh.size()));
  for (int round = 0; round < 3; round++) {
    if (!pump([] { std::lock_guard<std::mutex> l(g_m); return !g_host->messages.empty() || g_host->rejected; }, 15000)) {
      printf("FAIL no session message\n");
      return 1;
    }
    if (g_host->rejected) return 1;
    auto msg = g_host->messages.front();
    { std::lock_guard<std::mutex> l(g_m); g_host->messages.erase(g_host->messages.begin()); }
    if (msg.first == cdm::kIndividualizationRequest) {
      printf("individualization request %zu bytes: %s...\n", msg.second.size(), hex(msg.second.substr(0, 24)).c_str());
      printf("STOP provisioning through the v10 interface is not handled yet\n");
      return 1;
    }
    std::string licence;
    long http = wvcompat::Host::httpPost(lic_url, msg.second, "application/octet-stream", &licence);
    printf("licence server HTTP %ld, %zu bytes\n", http, licence.size());
    if (http != 200) return 1;
    uint32_t before = g_host->resolved;
    g_cdm->UpdateSession(2 + round, g_host->session.data(), static_cast<uint32_t>(g_host->session.size()),
                         reinterpret_cast<const uint8_t *>(licence.data()), static_cast<uint32_t>(licence.size()));
    pump([before] { return g_host->resolved != before || g_host->rejected; }, 15000);
    if (g_host->rejected) return 1;
    break;
  }
  pump([] { return g_host->keys_changes > 0; }, 3000);

  // Decrypt the first samples of the video and audio segment.
  for (int pass = 0; pass < 2; pass++) {
    bool video = pass == 0;
    std::string data;
    if (!fetchFirstSegment(base + (video ? vfile : afile), video ? vinit : ainit, &data)) return 1;
    int shown = 0, ok = 0, fail = 0;
    size_t in_total = 0, out_total = 0;
    wvcompat::CencState state;
    int n = state.process(data, [&](const wvcompat::TrackProtection &tp, std::vector<wvcompat::DecryptSample> &samples) {
      for (auto &s : samples) {
        std::vector<cdm::SubsampleEntry> subs;
        for (auto &ss : s.subsamples) subs.push_back({ss.first, ss.second});
        cdm::InputBuffer_2 in{};
        in.data = s.data;
        in.data_size = s.size;
        in.encryption_scheme = tp.scheme == "cbcs" ? cdm::EncryptionScheme::kCbcs : cdm::EncryptionScheme::kCenc;
        in.key_id = reinterpret_cast<const uint8_t *>(tp.kid.data());
        in.key_id_size = static_cast<uint32_t>(tp.kid.size());
        in.iv = s.iv;
        in.iv_size = 16;
        in.subsamples = subs.data();
        in.num_subsamples = static_cast<uint32_t>(subs.size());
        in.pattern = {tp.crypt_blocks, tp.skip_blocks};
        Block out;
        cdm::Status st = g_cdm->Decrypt(in, &out, video ? cdm::kStreamTypeVideo : cdm::kStreamTypeAudio);
        cdm::Buffer *b = out.DecryptedBuffer();
        if (st == cdm::kSuccess) ok++; else fail++;
        in_total += s.size;
        if (b) out_total += b->Size();
        if (shown < 3) {
          printf("  %s sample %u bytes, %zu subsample(s): status %u, out %u bytes: %s\n", video ? "video" : "audio",
                 s.size, subs.size(), st, b ? b->Size() : 0,
                 b ? hex(b->Data(), b->Size() < 48 ? b->Size() : 48).c_str() : "-");
          if (b && b->Size() == s.size)
            printf("    in  first bytes: %s\n", hex(s.data, s.size < 48 ? s.size : 48).c_str());
          shown++;
        }
      }
      return true;
    });
    printf("%s: %d sample(s) processed, Decrypt ok %d, failed %d, in %zu bytes, out %zu bytes\n",
           video ? "VIDEO" : "AUDIO", n, ok, fail, in_total, out_total);
  }
  printf("DONE\n");
  return 0;
}
