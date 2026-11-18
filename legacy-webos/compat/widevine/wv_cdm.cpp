// Host for the TV's Widevine CE CDM. See wv_cdm.h.
#include "wv_cdm.h"

#include <dirent.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <strings.h>

#include <atomic>
#include <cstring>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>

namespace wvcompat {
namespace {

const char *kCdmPaths[] = {"/usr/lib/libwidevine_ce_cdm_shared.so",
                           "/usr/lib/libwidevine_ce_cdm_shared.so.16.3.0.0", nullptr};
const char *kProvisioningUrl =
    "https://www.googleapis.com/certificateprovisioning/v1/devicecertificates/create"
    "?key=AIzaSyB-5OLKTx2iU5mko18DfdwK5611JIjbUhE";

// Cdm vtable slots (CDM 16.3; Cdm derives from TimerClient, so slots 0-2 are
// onTimerExpired and the two destructor entries).
enum Slot {
  kIsProvisioned = 9,
  kGetProvisioningRequest = 10,
  kHandleProvisioningResponse = 11,
  kCreateSession = 18,
  kGenerateRequest = 19,
  kUpdate = 21,
  kGetKeyStatuses = 24,
  kClose = 27,
  kDecrypt = 33,
};

template <typename R, typename... A>
R vcall(void *obj, int slot, A... args) {
  void **vtable = *reinterpret_cast<void ***>(obj);
  return reinterpret_cast<R (*)(void *, A...)>(vtable[slot])(obj, args...);
}

class FileStorage : public Storage {
 public:
  explicit FileStorage(std::string dir) : dir_(std::move(dir)) {
    // mkdir -p: the app data folder may not exist yet.
    for (size_t i = 1; i <= dir_.size(); i++)
      if (i == dir_.size() || dir_[i] == '/') mkdir(dir_.substr(0, i).c_str(), 0700);
  }
  bool read(const std::string &name, std::string *data) override {
    std::ifstream f(path(name), std::ios::binary);
    if (!f) return false;
    std::ostringstream s;
    s << f.rdbuf();
    *data = s.str();
    return true;
  }
  bool write(const std::string &name, const std::string &data) override {
    std::ofstream f(path(name), std::ios::binary | std::ios::trunc);
    f.write(data.data(), static_cast<std::streamsize>(data.size()));
    return static_cast<bool>(f);
  }
  bool exists(const std::string &name) override { return access(path(name).c_str(), F_OK) == 0; }
  bool remove(const std::string &name) override { return unlink(path(name).c_str()) == 0; }
  int32_t size(const std::string &name) override {
    struct stat st;
    return stat(path(name).c_str(), &st) == 0 ? static_cast<int32_t>(st.st_size) : -1;
  }
  bool list(std::vector<std::string> *names) override {
    DIR *d = opendir(dir_.c_str());
    if (!d) return false;
    while (dirent *e = readdir(d))
      if (e->d_name[0] != '.') names->push_back(e->d_name);
    closedir(d);
    return true;
  }
  ~FileStorage() override {}

 private:
  std::string path(const std::string &name) const { return dir_ + "/" + name; }
  std::string dir_;
};

class EpochClock : public Clock {
 public:
  int64_t now() override {
    timeval tv;
    gettimeofday(&tv, nullptr);
    return static_cast<int64_t>(tv.tv_sec) * 1000 + tv.tv_usec / 1000;
  }
  ~EpochClock() override {}
};

// One detached thread per timeout; cancel() bumps the client's generation so a
// pending timeout for it does nothing when it fires.
class ThreadTimer : public Timer {
 public:
  void setTimeout(int64_t delay_ms, TimerClient *client, void *context) override {
    uint64_t gen;
    {
      std::lock_guard<std::mutex> l(m_);
      gen = ++gens_[client];
    }
    std::thread([this, delay_ms, client, context, gen] {
      std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
      {
        std::lock_guard<std::mutex> l(m_);
        if (gens_[client] != gen) return;
      }
      client->onTimerExpired(context);
    }).detach();
  }
  void cancel(TimerClient *client) override {
    std::lock_guard<std::mutex> l(m_);
    ++gens_[client];
  }
  ~ThreadTimer() override {}

 private:
  std::mutex m_;
  std::map<TimerClient *, uint64_t> gens_;
};

class Listener : public EventListener {
 public:
  void onMessage(const std::string &session, int32_t type, const std::string &msg) override {
    std::lock_guard<std::mutex> l(m_);
    if (type == kMsgLicenseRequest) challenges_[session] = msg;
    cv_.notify_all();
  }
  void onKeyStatusesChange(const std::string &, bool) override {}
  void onRemoveComplete(const std::string &) override {}
  void onDeferredComplete(const std::string &, int32_t) override {}
  void onDirectIndividualizationRequest(const std::string &, const std::string &) override {}
  ~Listener() override {}

  bool takeChallenge(const std::string &session, std::string *out, int timeout_ms) {
    std::unique_lock<std::mutex> l(m_);
    if (!cv_.wait_for(l, std::chrono::milliseconds(timeout_ms),
                      [&] { return challenges_.count(session) != 0; }))
      return false;
    *out = challenges_[session];
    challenges_.erase(session);
    return true;
  }

 private:
  std::mutex m_;
  std::condition_variable cv_;
  std::map<std::string, std::string> challenges_;
};

}  // namespace

struct Host::Impl {
  std::string dir;
  void *lib = nullptr;
  void *cdm = nullptr;
  FileStorage *storage = nullptr;
  EpochClock clock;
  ThreadTimer timer;
  Listener listener;
  const char *(*version)() = nullptr;
};

Host::Host(const std::string &storage_dir) : impl_(new Impl) { impl_->dir = storage_dir; }

Host::~Host() {
  // The CDM instance and library are kept until process exit on purpose:
  // the CDM may still run timer callbacks.
  delete impl_;
}

const char *Host::version() const { return impl_->version ? impl_->version() : "?"; }

bool Host::open() {
  for (int i = 0; kCdmPaths[i] && !impl_->lib; i++) impl_->lib = dlopen(kCdmPaths[i], RTLD_NOW);
  if (!impl_->lib) { error_ = std::string("dlopen CDM: ") + dlerror(); return false; }
  using InitFn = int32_t (*)(int32_t, const ClientInfo &, Storage *, Clock *, Timer *, int32_t);
  using CreateFn = void *(*)(EventListener *, Storage *, bool);
  auto init = reinterpret_cast<InitFn>(dlsym(impl_->lib,
      "_ZN8widevine3Cdm10initializeENS0_16SecureOutputTypeERKNS0_10ClientInfoEPNS0_8IStorageE"
      "PNS0_6IClockEPNS0_6ITimerENS0_8LogLevelE"));
  auto create = reinterpret_cast<CreateFn>(
      dlsym(impl_->lib, "_ZN8widevine3Cdm6createEPNS0_14IEventListenerEPNS0_8IStorageEb"));
  impl_->version = reinterpret_cast<const char *(*)()>(dlsym(impl_->lib, "_ZN8widevine3Cdm7versionEv"));
  if (!init || !create) { error_ = "CDM entry points not found"; return false; }
  impl_->storage = new FileStorage(impl_->dir);
  ClientInfo info{"webOS Flutter legacy", "Webos-Flutter-Legacy-TV", "webOS TV", "LGTV",
                  "armv7", "legacy-webos"};
  int32_t r = init(kSecureOutputNone, info, impl_->storage, &impl_->clock, &impl_->timer, kLogErrors);
  if (r != kOk) { error_ = "initialize: status " + std::to_string(r); return false; }
  impl_->cdm = create(&impl_->listener, impl_->storage, false);
  if (!impl_->cdm) { error_ = "create returned null"; return false; }
  return true;
}

bool Host::ensureProvisioned() {
  if (vcall<bool>(impl_->cdm, kIsProvisioned)) return true;
  std::string request, response;
  int32_t r = vcall<int32_t>(impl_->cdm, kGetProvisioningRequest, &request);
  if (r != kOk) { error_ = "getProvisioningRequest: " + std::to_string(r); return false; }
  long http = httpPost(std::string(kProvisioningUrl) + "&signedRequest=" + request, "",
                       "application/json", &response);
  if (http != 200) { error_ = "provisioning HTTP " + std::to_string(http); return false; }
  r = vcall<int32_t>(impl_->cdm, kHandleProvisioningResponse, &response);
  if (r != kOk) { error_ = "handleProvisioningResponse: " + std::to_string(r); return false; }
  return vcall<bool>(impl_->cdm, kIsProvisioned);
}

bool Host::startSession(const std::string &pssh_box, std::string *session, std::string *challenge) {
  int32_t r = vcall<int32_t>(impl_->cdm, kCreateSession, static_cast<int32_t>(kSessionTemporary), session);
  if (r != kOk) { error_ = "createSession: " + std::to_string(r); return false; }
  r = vcall<int32_t>(impl_->cdm, kGenerateRequest, static_cast<const std::string *>(session),
                     static_cast<int32_t>(kInitCenc), &pssh_box);
  if (r != kOk) { error_ = "generateRequest: " + std::to_string(r); return false; }
  if (!impl_->listener.takeChallenge(*session, challenge, 5000)) {
    error_ = "no licence request from CDM";
    return false;
  }
  return true;
}

bool Host::updateSession(const std::string &session, const std::string &licence) {
  int32_t r = vcall<int32_t>(impl_->cdm, kUpdate, &session, &licence);
  if (r != kOk) { error_ = "update: " + std::to_string(r); return false; }
  return true;
}

bool Host::keyStatuses(const std::string &session, std::map<std::string, int32_t> *keys) {
  int32_t r = vcall<int32_t>(impl_->cdm, kGetKeyStatuses, &session, keys);
  if (r != kOk) { error_ = "getKeyStatuses: " + std::to_string(r); return false; }
  return true;
}

bool Host::decrypt(const std::string &session, const DecryptionBatch &batch, int32_t *status) {
  *status = vcall<int32_t>(impl_->cdm, kDecrypt, &session, &batch);
  return *status == kOk;
}

void Host::closeSession(const std::string &session) { vcall<int32_t>(impl_->cdm, kClose, &session); }

// ---- HTTP via the TV's libcurl (loaded at run time) -------------------------
namespace {
struct Curl {
  void *(*easy_init)();
  int (*easy_setopt)(void *, int, ...);
  int (*easy_perform)(void *);
  int (*easy_getinfo)(void *, int, ...);
  void (*easy_cleanup)(void *);
  void *(*slist_append)(void *, const char *);
  void (*slist_free_all)(void *);
  bool ok = false;
};
Curl &curl() {
  static Curl c;
  static std::once_flag once;
  std::call_once(once, [] {
    void *h = dlopen("libcurl.so.4", RTLD_NOW);
    if (!h) return;
    c.easy_init = reinterpret_cast<void *(*)()>(dlsym(h, "curl_easy_init"));
    c.easy_setopt = reinterpret_cast<int (*)(void *, int, ...)>(dlsym(h, "curl_easy_setopt"));
    c.easy_perform = reinterpret_cast<int (*)(void *)>(dlsym(h, "curl_easy_perform"));
    c.easy_getinfo = reinterpret_cast<int (*)(void *, int, ...)>(dlsym(h, "curl_easy_getinfo"));
    c.easy_cleanup = reinterpret_cast<void (*)(void *)>(dlsym(h, "curl_easy_cleanup"));
    c.slist_append = reinterpret_cast<void *(*)(void *, const char *)>(dlsym(h, "curl_slist_append"));
    c.slist_free_all = reinterpret_cast<void (*)(void *)>(dlsym(h, "curl_slist_free_all"));
    c.ok = c.easy_init && c.easy_setopt && c.easy_perform && c.easy_getinfo && c.easy_cleanup &&
           c.slist_append && c.slist_free_all;
  });
  return c;
}
size_t onData(char *p, size_t size, size_t n, void *out) {
  static_cast<std::string *>(out)->append(p, size * n);
  return size * n;
}
// libcurl option numbers (stable ABI)
enum {
  kOptWriteData = 10001, kOptUrl = 10002, kOptPostFields = 10015, kOptHttpHeader = 10023,
  kOptWriteFunction = 20011, kOptPostFieldSize = 60, kOptPost = 47, kOptFollow = 52,
  kOptTimeout = 13, kInfoResponseCode = 0x200002, kOptRange = 10007,
  kOptHeaderFunction = 20079, kOptHeaderData = 10029,
};
struct Headers { std::string content_range, content_type; };
size_t onHeader(char *p, size_t size, size_t n, void *out) {
  std::string line(p, size * n);
  auto *h = static_cast<Headers *>(out);
  auto value = [&](const char *name) -> std::string * {
    size_t nl = strlen(name);
    if (line.size() <= nl || strncasecmp(line.c_str(), name, nl) != 0) return nullptr;
    return name[8] == 'R' || name[8] == 'r' ? &h->content_range : &h->content_type;
  };
  if (std::string *v = value("Content-Range:")) *v = line.substr(14);
  else if (std::string *t = value("Content-Type:")) *t = line.substr(13);
  for (std::string *v : {&h->content_range, &h->content_type}) {
    while (!v->empty() && (v->back() == '\r' || v->back() == '\n' || v->back() == ' ')) v->pop_back();
    while (!v->empty() && v->front() == ' ') v->erase(0, 1);
  }
  return size * n;
}
}  // namespace

long Host::httpPost(const std::string &url, const std::string &body, const std::string &content_type,
                    std::string *response) {
  Curl &c = curl();
  if (!c.ok) return 0;
  void *h = c.easy_init();
  if (!h) return 0;
  std::string ct = "Content-Type: " + content_type;
  void *headers = c.slist_append(nullptr, ct.c_str());
  c.easy_setopt(h, kOptUrl, url.c_str());
  c.easy_setopt(h, kOptPost, 1L);
  c.easy_setopt(h, kOptPostFields, body.data());
  c.easy_setopt(h, kOptPostFieldSize, static_cast<long>(body.size()));
  c.easy_setopt(h, kOptHttpHeader, headers);
  c.easy_setopt(h, kOptWriteFunction, onData);
  c.easy_setopt(h, kOptWriteData, response);
  c.easy_setopt(h, kOptFollow, 1L);
  c.easy_setopt(h, kOptTimeout, 20L);
  long code = 0;
  if (c.easy_perform(h) == 0) c.easy_getinfo(h, kInfoResponseCode, &code);
  c.slist_free_all(headers);
  c.easy_cleanup(h);
  return code;
}

long Host::httpGet(const std::string &url, const std::string &range, std::string *body,
                   std::string *content_range, std::string *content_type) {
  Curl &c = curl();
  if (!c.ok) return 0;
  void *h = c.easy_init();
  if (!h) return 0;
  Headers hdrs;
  c.easy_setopt(h, kOptUrl, url.c_str());
  if (!range.empty()) c.easy_setopt(h, kOptRange, range.c_str());
  c.easy_setopt(h, kOptWriteFunction, onData);
  c.easy_setopt(h, kOptWriteData, body);
  c.easy_setopt(h, kOptHeaderFunction, onHeader);
  c.easy_setopt(h, kOptHeaderData, &hdrs);
  c.easy_setopt(h, kOptFollow, 1L);
  c.easy_setopt(h, kOptTimeout, 30L);
  long code = 0;
  if (c.easy_perform(h) == 0) c.easy_getinfo(h, kInfoResponseCode, &code);
  c.easy_cleanup(h);
  if (content_range) *content_range = hdrs.content_range;
  if (content_type) *content_type = hdrs.content_type;
  return code;
}

}  // namespace wvcompat
