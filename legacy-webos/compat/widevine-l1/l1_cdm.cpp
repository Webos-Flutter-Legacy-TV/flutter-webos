#include "l1_cdm.h"

#include <dlfcn.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>

#define TARGET_WEBOS 1
#include "third_party/cdm/content_decryption_module.h"

namespace wvl1 {
namespace {

using Clock = std::chrono::steady_clock;

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

}  // namespace

struct Cdm::Impl : public cdm::Host_10 {
  void *lib = nullptr;
  cdm::ContentDecryptionModule_10 *cdm = nullptr;
  const char *(*get_version)() = nullptr;

  std::mutex call_m;  // serializes every call into the CDM
  std::mutex m;
  std::condition_variable cv;
  int initialized = -1;
  uint32_t next_promise = 1;
  std::map<uint32_t, int> promises;  // 1 resolved, 0 rejected
  std::string reject_msg, session, message;
  bool have_message = false;
  int usable_keys = 0;

  std::multimap<Clock::time_point, void *> timers;
  std::thread timer_thread;
  std::atomic<bool> stop{false};

  // ---- Host_10
  cdm::Buffer *Allocate(uint32_t cap) override { return new HeapBuffer(cap); }
  void SetTimer(int64_t ms, void *ctx) override {
    std::lock_guard<std::mutex> l(m);
    timers.emplace(Clock::now() + std::chrono::milliseconds(ms), ctx);
    cv.notify_all();
  }
  cdm::Time GetCurrentWallTime() override {
    return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
  }
  void OnInitialized(bool ok) override { settle([&] { initialized = ok; }); }
  void OnResolveKeyStatusPromise(uint32_t id, cdm::KeyStatus) override { settle([&] { promises[id] = 1; }); }
  void OnResolveNewSessionPromise(uint32_t id, const char *sid, uint32_t n) override {
    settle([&] { session.assign(sid, n); promises[id] = 1; });
  }
  void OnResolvePromise(uint32_t id) override { settle([&] { promises[id] = 1; }); }
  void OnRejectPromise(uint32_t id, cdm::Exception e, uint32_t code, const char *msg, uint32_t n) override {
    settle([&] {
      reject_msg = "exception " + std::to_string(e) + " code " + std::to_string(code) + ": " +
                   std::string(msg ? msg : "", msg ? n : 0);
      promises[id] = 0;
    });
  }
  void OnSessionMessage(const char *, uint32_t, cdm::MessageType, const char *msg, uint32_t n) override {
    settle([&] { message.assign(msg, n); have_message = true; });
  }
  void OnSessionKeysChange(const char *, uint32_t, bool, const cdm::KeyInformation *k, uint32_t count) override {
    int usable = 0;
    for (uint32_t i = 0; i < count; i++) usable += k[i].status == cdm::kUsable;
    settle([&] { usable_keys = usable; });
  }
  void OnExpirationChange(const char *, uint32_t, cdm::Time) override {}
  void OnSessionClosed(const char *, uint32_t) override {}
  void SendPlatformChallenge(const char *, uint32_t, const char *, uint32_t) override {}
  void EnableOutputProtection(uint32_t) override {}
  void QueryOutputProtectionStatus() override {}
  void OnDeferredInitializationDone(cdm::StreamType, cdm::Status) override {}
  cdm::FileIO *CreateFileIO(cdm::FileIOClient *) override { return nullptr; }  // the wrapper has its own storage
  void RequestStorageId(uint32_t) override {}

  template <typename F> void settle(F f) {
    std::lock_guard<std::mutex> l(m);
    f();
    cv.notify_all();
  }
  template <typename P> bool waitFor(P pred, int ms) {
    std::unique_lock<std::mutex> l(m);
    return cv.wait_for(l, std::chrono::milliseconds(ms), pred);
  }
  bool waitPromise(uint32_t id, int ms) {
    if (!waitFor([&] { return promises.count(id) > 0; }, ms)) return false;
    std::lock_guard<std::mutex> l(m);
    return promises[id] == 1;
  }

  void timerLoop() {
    std::unique_lock<std::mutex> l(m);
    while (!stop) {
      if (timers.empty()) { cv.wait_for(l, std::chrono::milliseconds(200)); continue; }
      auto due = timers.begin()->first;
      if (due > Clock::now()) { cv.wait_until(l, due); continue; }
      void *ctx = timers.begin()->second;
      timers.erase(timers.begin());
      l.unlock();
      {
        std::lock_guard<std::mutex> c(call_m);
        if (cdm) cdm->TimerExpired(ctx);
      }
      l.lock();
    }
  }
};

static Cdm::Impl *g_impl_for_host = nullptr;
static void *GetHost(int version, void *) {
  return version == cdm::Host_10::kVersion ? static_cast<cdm::Host_10 *>(g_impl_for_host) : nullptr;
}

Cdm::Cdm() : impl_(new Impl) {}

Cdm::~Cdm() {
  impl_->stop = true;
  impl_->cv.notify_all();
  if (impl_->timer_thread.joinable()) impl_->timer_thread.join();
  if (impl_->cdm) {
    std::lock_guard<std::mutex> c(impl_->call_m);
    if (!impl_->session.empty())
      impl_->cdm->CloseSession(impl_->next_promise++, impl_->session.data(),
                               static_cast<uint32_t>(impl_->session.size()));
    impl_->cdm->Destroy();
  }
  // The library stays loaded: the wrapper keeps global state.
  delete impl_;
}

const char *Cdm::version() const { return impl_->get_version ? impl_->get_version() : "?"; }

bool Cdm::open() {
  Impl &d = *impl_;
  // webOS 5 to 23 (firmware 5.x-8.x): libwvcdm_shared.so; webOS 24 and later
  // (9.x-11.x) ship the same CDM_10 wrapper as libwidevine-wrapper.so.1.
  // webOS 4 and older have an older, unversioned CDM interface (unsupported).
  d.lib = dlopen("/usr/lib/libwvcdm_shared.so", RTLD_NOW);
  if (!d.lib) d.lib = dlopen("libwidevine-wrapper.so.1", RTLD_NOW);
  if (!d.lib) { error_ = std::string("no Widevine CDM wrapper: ") + dlerror(); return false; }
  auto init_module = reinterpret_cast<void (*)()>(dlsym(d.lib, "InitializeCdmModule_10"));
  auto create = reinterpret_cast<void *(*)(int, const char *, uint32_t, GetCdmHostFunc, void *)>(
      dlsym(d.lib, "CreateCdmInstance"));
  d.get_version = reinterpret_cast<const char *(*)()>(dlsym(d.lib, "GetCdmVersion"));
  if (!init_module || !create) { error_ = "libwvcdm_shared.so: no CDM_10 entry points"; return false; }
  static std::once_flag once;
  std::call_once(once, init_module);
  g_impl_for_host = &d;
  const char ks[] = "com.widevine.alpha";
  d.cdm = static_cast<cdm::ContentDecryptionModule_10 *>(create(10, ks, sizeof(ks) - 1, GetHost, nullptr));
  if (!d.cdm) { error_ = "CreateCdmInstance(10) failed"; return false; }
  d.timer_thread = std::thread([&d] { d.timerLoop(); });
  {
    std::lock_guard<std::mutex> c(d.call_m);
    d.cdm->Initialize(true, true, true);  // hardware secure codecs
  }
  if (!d.waitFor([&] { return d.initialized >= 0; }, 10000) || d.initialized != 1) {
    error_ = "CDM Initialize failed";
    return false;
  }
  return true;
}

bool Cdm::setServiceCertificate(const std::string &raw) {
  Impl &d = *impl_;
  std::string cert = signedMessageBody(raw);
  uint32_t id;
  {
    std::lock_guard<std::mutex> c(d.call_m);
    id = d.next_promise++;
    d.cdm->SetServerCertificate(id, reinterpret_cast<const uint8_t *>(cert.data()),
                                static_cast<uint32_t>(cert.size()));
  }
  if (!d.waitPromise(id, 10000)) { error_ = "service certificate rejected: " + d.reject_msg; return false; }
  // LG's SetServerCertificate installs it for provisioning only (role 1);
  // licence requests in privacy mode need the licensing role (2), as LG's
  // own C export SetServiceCertificate uses. The wrapped widevine::Cdm is at
  // offset 8 of the CDM_10 object; setServiceCertificate is its vtable slot 3.
  struct CeCdm { void **vtbl; };
  CeCdm *ce = *reinterpret_cast<CeCdm **>(reinterpret_cast<char *>(d.cdm) + 8);
  if (!ce) { error_ = "no inner CDM"; return false; }
  auto set_cert = reinterpret_cast<int (*)(CeCdm *, int, const std::string &)>(ce->vtbl[3]);
  int st;
  {
    std::lock_guard<std::mutex> c(d.call_m);
    st = set_cert(ce, 2, cert);
  }
  if (st != 0) { error_ = "setServiceCertificate(licensing) status " + std::to_string(st); return false; }
  return true;
}

bool Cdm::generateRequest(const std::string &pssh, std::string *challenge) {
  Impl &d = *impl_;
  uint32_t id;
  {
    std::lock_guard<std::mutex> l(d.m);
    d.have_message = false;
  }
  {
    std::lock_guard<std::mutex> c(d.call_m);
    id = d.next_promise++;
    d.cdm->CreateSessionAndGenerateRequest(id, cdm::kTemporary, cdm::kCenc,
                                           reinterpret_cast<const uint8_t *>(pssh.data()),
                                           static_cast<uint32_t>(pssh.size()));
  }
  if (!d.waitFor([&] { return d.have_message || (d.promises.count(id) && d.promises[id] == 0); }, 15000) ||
      !d.have_message) {
    error_ = "no licence request: " + d.reject_msg;
    return false;
  }
  std::lock_guard<std::mutex> l(d.m);
  *challenge = d.message;
  return true;
}

bool Cdm::update(const std::string &licence) {
  Impl &d = *impl_;
  uint32_t id;
  {
    std::lock_guard<std::mutex> c(d.call_m);
    id = d.next_promise++;
    d.cdm->UpdateSession(id, d.session.data(), static_cast<uint32_t>(d.session.size()),
                         reinterpret_cast<const uint8_t *>(licence.data()), static_cast<uint32_t>(licence.size()));
  }
  if (!d.waitPromise(id, 15000)) { error_ = "licence rejected: " + d.reject_msg; return false; }
  d.waitFor([&] { return d.usable_keys > 0; }, 3000);
  if (d.usable_keys == 0) { error_ = "licence has no usable key"; return false; }
  return true;
}

bool Cdm::decrypt(const CdmSample &s, bool video, std::vector<uint8_t> *out) {
  Impl &d = *impl_;
  std::vector<cdm::SubsampleEntry> subs;
  for (auto &ss : s.subsamples) subs.push_back({ss.first, ss.second});
  cdm::InputBuffer_2 in{};
  in.data = s.data;
  in.data_size = s.size;
  in.encryption_scheme = !s.encrypted ? cdm::EncryptionScheme::kUnencrypted
                         : s.cbcs     ? cdm::EncryptionScheme::kCbcs
                                      : cdm::EncryptionScheme::kCenc;
  if (s.encrypted) {
    in.key_id = reinterpret_cast<const uint8_t *>(s.kid->data());
    in.key_id_size = static_cast<uint32_t>(s.kid->size());
    in.iv = s.iv;
    in.iv_size = 16;
    in.subsamples = subs.data();
    in.num_subsamples = static_cast<uint32_t>(subs.size());
    in.pattern = {s.crypt_blocks, s.skip_blocks};
  }
  in.timestamp = s.timestamp_us;
  Block blk;
  cdm::Status st;
  {
    std::lock_guard<std::mutex> c(d.call_m);
    st = d.cdm->Decrypt(in, &blk, video ? cdm::kStreamTypeVideo : cdm::kStreamTypeAudio);
  }
  cdm::Buffer *b = blk.DecryptedBuffer();
  if (st != cdm::kSuccess || !b) { error_ = "Decrypt status " + std::to_string(st); return false; }
  out->assign(b->Data(), b->Data() + b->Size());
  return true;
}

// SignedMessage { 1: type (varint), 2: msg (bytes), ... } -> msg, or the
// input unchanged if it does not parse as one.
std::string signedMessageBody(const std::string &in) {
  // A SignedMessage starts with its type (field 1, varint: 0x08). Anything
  // else, such as a bare SignedDrmCertificate (field 1 is the certificate,
  // field 2 its signature, as Axinom serves it), goes to the CDM as it is.
  if (in.empty() || static_cast<uint8_t>(in[0]) != 0x08) return in;
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

}  // namespace wvl1
