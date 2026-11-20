// libwv-l1.so: opt-in hardware (secure video path) Widevine player for
// video_player_drm on webOS releases before 26 (<app dir>/compat-widevine-l1).
//
// The NDL shim hands it Widevine DASH loads. It reads the manifest, gets the
// licence through the TV's CDM wrapper (l1_cdm), and plays the stream with
// the TV's own StarfishMediaAPIs player in buffer-stream mode: video samples
// go through the CDM, which returns them wrapped for the secure video path
// (they are decrypted in the TEE, never in this process); audio samples come
// back clear. Player state goes back to the app as the JSON events NDL
// sends ({"loadCompleted":..}, {"currentTime":..}, ...).
//
// Nothing of LG's is bundled: libplayerAPIs.so.1 and libwvcdm_shared.so are
// the firmware's, looked up at run time.
#include <dlfcn.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../widevine/wv_cdm.h"  // HTTP helpers
#include "dash.h"
#include "fmp4.h"
#include "l1_cdm.h"

#define EXPORT extern "C" __attribute__((visibility("default")))

namespace wvl1 {
namespace {

void logf(const char *fmt, ...) {
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  fprintf(stderr, "[wv-l1] %s\n", buf);
}

bool opt(const char *word);

// ---- StarfishMediaAPIs (libplayerAPIs.so.1), called through its symbols ----
using StarfishCb = void (*)(int type, int64_t num, const char *str, void *data);
struct Starfish {
  void *lib = nullptr;
  void (*ctor)(void *, const char *) = nullptr;
  void (*dtor)(void *) = nullptr;
  bool (*load)(void *, const char *, StarfishCb, void *) = nullptr;
  std::string (*feed)(void *, const char *) = nullptr;
  bool (*play)(void *) = nullptr;
  bool (*pause)(void *) = nullptr;
  bool (*seek)(void *, const char *) = nullptr;
  bool (*unload)(void *) = nullptr;
  bool (*push_eos)(void *) = nullptr;
  int64_t (*playtime)(void *) = nullptr;
  bool (*foreground)(void *) = nullptr;
  bool (*rate)(void *, const char *) = nullptr;
  bool (*flush)(void *) = nullptr;
  bool (*set_hdr)(void *, const char *) = nullptr;                                     // optional
  const char *(*media_id)(void *) = nullptr;
  bool (*change_audio)(void *, const std::string &, const std::string &) = nullptr;  // webOS 6+
  void *obj = nullptr;

  bool open(const char *app_id) {
    lib = dlopen("libplayerAPIs.so.1", RTLD_NOW);
    if (!lib) { logf("dlopen libplayerAPIs.so.1: %s", dlerror()); return false; }
#define SYM(field, name) field = reinterpret_cast<decltype(field)>(dlsym(lib, name))
    SYM(ctor, "_ZN17StarfishMediaAPIsC1EPKc");
    SYM(dtor, "_ZN17StarfishMediaAPIsD1Ev");
    SYM(load, "_ZN17StarfishMediaAPIs4LoadEPKcPFvixS1_PvES2_");
    SYM(feed, "_ZN17StarfishMediaAPIs4FeedB5cxx11EPKc");
    SYM(play, "_ZN17StarfishMediaAPIs4PlayEv");
    SYM(pause, "_ZN17StarfishMediaAPIs5PauseEv");
    SYM(seek, "_ZN17StarfishMediaAPIs4SeekEPKc");
    SYM(unload, "_ZN17StarfishMediaAPIs6UnloadEv");
    SYM(push_eos, "_ZN17StarfishMediaAPIs7pushEOSEv");
    SYM(playtime, "_ZN17StarfishMediaAPIs18getCurrentPlaytimeEv");
    SYM(foreground, "_ZN17StarfishMediaAPIs16notifyForegroundEv");
    SYM(rate, "_ZN17StarfishMediaAPIs11SetPlayRateEPKc");
    SYM(flush, "_ZN17StarfishMediaAPIs5flushEv");
    SYM(set_hdr, "_ZN17StarfishMediaAPIs10setHdrInfoEPKc");
    SYM(media_id, "_ZN17StarfishMediaAPIs10getMediaIDEv");
    SYM(change_audio,
        "_ZN17StarfishMediaAPIs16changeAudioCodecERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEES7_");
#undef SYM
    if (!ctor || !dtor || !load || !feed || !play || !pause || !seek || !unload || !push_eos || !playtime) {
      logf("libplayerAPIs.so.1: missing StarfishMediaAPIs functions");
      return false;
    }
    // The class is a few hundred bytes on current firmware; leave room.
    obj = calloc(1, 16384);
    // Kodi constructs it without an id (the argument is a media/client id).
    ctor(obj, opt("uid-app") ? app_id : nullptr);
    return true;
  }
  void close() {
    if (obj) { dtor(obj); free(obj); obj = nullptr; }
  }
};

#define kL1Service "legacy-webos-widevine-l1"

std::string b64encode(const std::string &in) {
  static const char a[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  size_t i = 0;
  for (; i + 2 < in.size(); i += 3) {
    uint32_t v = (uint8_t(in[i]) << 16) | (uint8_t(in[i + 1]) << 8) | uint8_t(in[i + 2]);
    out += a[v >> 18]; out += a[(v >> 12) & 63]; out += a[(v >> 6) & 63]; out += a[v & 63];
  }
  if (i < in.size()) {
    uint32_t v = uint8_t(in[i]) << 16;
    if (i + 1 < in.size()) v |= uint8_t(in[i + 1]) << 8;
    out += a[v >> 18]; out += a[(v >> 12) & 63];
    out += i + 1 < in.size() ? a[(v >> 6) & 63] : '=';
    out += '=';
  }
  return out;
}

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

// Value of "key":"..." or "key":<number/bool> in a flat search of json.
std::string jsonValue(const std::string &json, const char *key) {
  std::string k = std::string("\"") + key + "\"";
  size_t i = json.find(k);
  if (i == std::string::npos) return "";
  i = json.find(':', i + k.size());
  if (i == std::string::npos) return "";
  i++;
  while (i < json.size() && (json[i] == ' ' || json[i] == '\t')) i++;
  if (i < json.size() && json[i] == '"') {
    std::string out;
    for (size_t j = i + 1; j < json.size() && json[j] != '"'; j++) {
      if (json[j] == '\\' && j + 1 < json.size()) { j++; out += json[j] == '/' ? '/' : json[j]; }
      else out += json[j];
    }
    return out;
  }
  size_t e = json.find_first_of(",}]", i);
  return json.substr(i, e == std::string::npos ? std::string::npos : e - i);
}

std::string hexAddr(const void *p) {
  char b[24];
  snprintf(b, sizeof(b), "%#lx", static_cast<unsigned long>(reinterpret_cast<uintptr_t>(p)));
  return b;
}

// ---- Luna bus (luna-service2), looked up at run time ----------------------
// One registration per process, with its own GLib main loop for replies.
// Used for the video output service: with StarfishMediaAPIs in the app's
// own process nothing else tells the TV where to show the video (YouTube's
// app makes the same call), so the decoder stays connected but unseen.
struct LSErr {
  int code;
  char *message;
  const char *file;
  int line;
  const char *func;
  void *padding;
  unsigned long magic;
};
class Luna {
 public:
  static Luna &get() {
    static Luna l;
    return l;
  }
  bool open(const std::string &app_id) {
    std::lock_guard<std::mutex> l(m_);
    if (sh_) return true;
    if (tried_) return false;
    tried_ = true;
    void *ls = dlopen("libluna-service2.so.3", RTLD_NOW);
    void *gl = dlopen("libglib-2.0.so.0", RTLD_NOW);
    if (!ls || !gl) { logf("luna: %s", dlerror()); return false; }
#define LSYM(lib, f, n) f = reinterpret_cast<decltype(f)>(dlsym(lib, n))
    LSYM(ls, reg_, "LSRegister");
    LSYM(ls, reg_app_, "LSRegisterApplicationService");
    LSYM(ls, attach_, "LSGmainContextAttach");
    LSYM(ls, call_, "LSCallOneReply");
    LSYM(ls, call_multi_, "LSCall");
    LSYM(ls, err_init_, "LSErrorInit");
    LSYM(ls, err_free_, "LSErrorFree");
    LSYM(ls, payload_, "LSMessageGetPayload");
    LSYM(gl, ctx_new_, "g_main_context_new");
    LSYM(gl, loop_new_, "g_main_loop_new");
    LSYM(gl, loop_run_, "g_main_loop_run");
#undef LSYM
    if (!reg_ || !attach_ || !call_ || !err_init_ || !err_free_ || !payload_ || !ctx_new_ || !loop_new_ || !loop_run_) {
      logf("luna: functions missing");
      return false;
    }
    // Names the app's role may allow: its own id with a suffix, then none.
    std::string name = app_id + "-wvl1";
    LSErr e;
    bool ok = false;
    if (reg_app_) {
      err_init_(&e);
      ok = reg_app_(name.c_str(), app_id.c_str(), &sh_, &e);
      if (!ok) { logf("luna: register %s for %s: %s", name.c_str(), app_id.c_str(), e.message ? e.message : "?"); err_free_(&e); }
    }
    if (!ok) {
      err_init_(&e);
      ok = reg_(name.c_str(), &sh_, &e);
      if (!ok) { logf("luna: register %s: %s", name.c_str(), e.message ? e.message : "?"); err_free_(&e); }
    }
    if (!ok) {
      err_init_(&e);
      ok = reg_(nullptr, &sh_, &e);
      if (!ok) { logf("luna: anonymous register: %s", e.message ? e.message : "?"); err_free_(&e); return false; }
      name = "(anonymous)";
    }
    void *ctx = ctx_new_();
    void *loop = loop_new_(ctx, 0);
    err_init_(&e);
    if (!attach_(sh_, ctx, &e)) { logf("luna: attach: %s", e.message ? e.message : "?"); err_free_(&e); sh_ = nullptr; return false; }
    std::thread([this, loop] { loop_run_(loop); }).detach();
    logf("luna: registered as %s", name.c_str());
    return true;
  }
  // Asynchronous call; replies are logged and each asks the app for a frame.
  // subscribe: keep receiving replies (LSCall).
  bool call(const std::string &uri, const std::string &payload, bool subscribe = false) {
    if (!sh_) return false;
    LSErr e;
    err_init_(&e);
    unsigned long token = 0;
    auto fn = subscribe && call_multi_ ? call_multi_ : call_;
    if (!fn(sh_, uri.c_str(), payload.c_str(), &Luna::onReply, this, &token, &e)) {
      logf("luna: %s: %s", uri.c_str(), e.message ? e.message : "?");
      err_free_(&e);
      return false;
    }
    return true;
  }

 private:
  static bool onReply(void *, void *msg, void *self) {
    const char *p = static_cast<Luna *>(self)->payload_(msg);
    logf("luna reply: %.300s", p ? p : "");
    requestFrame();
    return true;
  }

 public:
  // Asks the app's engine for a frame (through the Wayland shim).
  static void requestFrame() {
    static int (*fn)(void) = reinterpret_cast<int (*)(void)>(dlsym(RTLD_DEFAULT, "compat_request_frame"));
    if (fn) fn();
  }

 private:
  std::mutex m_;
  bool tried_ = false;
  void *sh_ = nullptr;
  bool (*reg_)(const char *, void **, LSErr *) = nullptr;
  bool (*reg_app_)(const char *, const char *, void **, LSErr *) = nullptr;
  bool (*attach_)(void *, void *, LSErr *) = nullptr;
  bool (*call_)(void *, const char *, const char *, bool (*)(void *, void *, void *), void *, unsigned long *, LSErr *) = nullptr;
  bool (*call_multi_)(void *, const char *, const char *, bool (*)(void *, void *, void *), void *, unsigned long *, LSErr *) = nullptr;
  bool (*err_init_)(LSErr *) = nullptr;
  void (*err_free_)(LSErr *) = nullptr;
  const char *(*payload_)(void *) = nullptr;
  void *(*ctx_new_)() = nullptr;
  void *(*loop_new_)(void *, int) = nullptr;
  void (*loop_run_)(void *) = nullptr;
};

// Debug options: words in <app dir>/compat-l1-opts (noaudio, nodrm, svp30,
// h264: never pick HEVC, noabr: no adaptive quality switching, slownet:
// count downloads as 3 Mbit/s to test switching).
bool opt(const char *word) {
  static std::string opts;
  static bool read = false;
  if (!read) {
    read = true;
    char exe[512];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n > 0) {
      exe[n] = '\0';
      std::string path = std::string(exe).substr(0, std::string(exe).rfind('/')) + "/compat-l1-opts";
      if (FILE *f = fopen(path.c_str(), "r")) {
        char buf[256];
        size_t r = fread(buf, 1, sizeof(buf) - 1, f);
        buf[r] = 0;
        opts = buf;
        fclose(f);
        logf("debug options: %s", opts.c_str());
      }
    }
  }
  return opts.find(word) != std::string::npos;
}

// Major webOS TV version from the platform's os_info ("webos_release":
// "6.5.3" -> 6; webOS 22 and later report 7 and up). 0 if unknown.
int webosMajor() {
  static int v = -1;
  if (v < 0) {
    v = 0;
    if (FILE *f = fopen("/var/run/nyx/os_info.json", "r")) {
      char buf[8192];
      size_t r = fread(buf, 1, sizeof(buf) - 1, f);
      buf[r] = 0;
      fclose(f);
      v = atoi(jsonValue(buf, "webos_release").c_str());
    }
    logf("webOS %d", v);
  }
  return v;
}
bool oldWebos() { return webosMajor() > 0 && webosMajor() < 5; }

// Video codec ranking at equal resolution (Dolby Vision, HEVC, AV1, VP9,
// H.264; 0: unusable), and the codec family of a
// representation (quality switching stays within one family).
int videoFamily(const Representation &r) {
  const std::string &c = r.codecs;
  bool mp4 = r.mime.find("mp4") != std::string::npos;
  if (c.compare(0, 4, "dvh1") == 0 || c.compare(0, 4, "dvhe") == 0) return opt("nodv") ? 0 : 5;  // Dolby Vision
  if (c.compare(0, 4, "hev1") == 0 || c.compare(0, 4, "hvc1") == 0) return 4;
  if (c.compare(0, 4, "av01") == 0 && mp4) return opt("noav1") ? 0 : 3;
  if ((c.compare(0, 4, "vp09") == 0 || c == "vp9") && mp4) return 2;
  if (c.compare(0, 4, "avc1") == 0 || c.compare(0, 4, "avc3") == 0) return 1;
  return 0;
}
bool audioSupported(const Representation &r) {
  const std::string &c = r.codecs;
  return r.mime.find("mp4") != std::string::npos &&
         (c.compare(0, 4, "mp4a") == 0 || c == "ac-3" || c == "ec-3" || c.compare(0, 4, "opus") == 0 ||
          c.compare(0, 4, "Opus") == 0);
}

// Audio decoder settings for the Load payload and changeAudioCodec (a JSON
// member without braces; empty when the codec needs none).
std::string audioInfo(const Track &a) {
  char b[160];
  if (a.codec == "AAC") {
    snprintf(b, sizeof(b), "\"aacInfo\":{\"channels\":%u,\"profile\":%u,\"format\":\"raw\",\"frequency\":%.3f}",
             a.channels, a.aac_object_type, a.sample_rate / 1000.0);
    return b;
  }
  if (a.codec == "AC3 PLUS") {
    snprintf(b, sizeof(b), "\"ac3PlusInfo\":{\"channels\":%u,\"frequency\":%.3f}", a.channels + (a.atmos ? 2 : 0),
             a.sample_rate / 1000.0);
    return b;
  }
  if (a.codec == "OPUS") {
    snprintf(b, sizeof(b), "\"opusInfo\":{\"channels\":%u,\"frequency\":%.3f,\"streamHeader\":\"", a.channels,
             a.sample_rate / 1000.0);
    return b + b64encode(a.opus_head) + "\"}";
  }
  return "";
}

// setHdrInfo payload from the track's mastering display and light level
// metadata; empty without them (the TV then reads the stream's own).
std::string hdrPayload(const Track &v) {
  if (!v.has_mdcv && !v.has_clli) return "";
  const char *type = v.transfer == 16 ? "HDR10" : v.transfer == 18 ? "HLG" : "none";
  std::string sei = "{";
  char b[512];
  if (v.has_mdcv) {
    snprintf(b, sizeof(b),
             "\"displayPrimariesX0\":%u,\"displayPrimariesY0\":%u,\"displayPrimariesX1\":%u,\"displayPrimariesY1\":%u,"
             "\"displayPrimariesX2\":%u,\"displayPrimariesY2\":%u,\"whitePointX\":%u,\"whitePointY\":%u,"
             "\"minDisplayMasteringLuminance\":%u,\"maxDisplayMasteringLuminance\":%u",
             v.primaries_xy[0], v.primaries_xy[1], v.primaries_xy[2], v.primaries_xy[3], v.primaries_xy[4],
             v.primaries_xy[5], v.white_xy[0], v.white_xy[1], v.min_luminance, v.max_luminance);
    sei += b;
  }
  if (v.has_clli) {
    snprintf(b, sizeof(b), "%s\"maxContentLightLevel\":%u,\"maxPicAverageLightLevel\":%u", v.has_mdcv ? "," : "",
             v.max_cll, v.max_fall);
    sei += b;
  }
  sei += "}";
  snprintf(b, sizeof(b),
           "\"%s\":{\"transferCharacteristics\":%u,\"colorPrimaries\":%u,\"matrixCoeffs\":%u,\"videoFullRangeFlag\":%s}",
           oldWebos() ? "mediaVui" : "vui", v.transfer, v.colour_primaries, v.matrix, v.full_range ? "true" : "false");
  return std::string("{\"hdrType\":\"") + type + "\",\"" + (oldWebos() ? "mediaSei" : "sei") + "\":" + sei + "," + b + "}";
}

using Emit = void (*)(void *, const char *);

class Player {
 public:
  Player(std::string url, std::string option, Emit emit, void *user)
      : url_(std::move(url)), option_(std::move(option)), emit_(emit), user_(user) {}
  ~Player() { stop(); }

  void start() { worker_ = std::thread([this] { run(); }); }

  void stop() {
    {
      std::lock_guard<std::mutex> l(m_);
      quit_ = true;
      cv_.notify_all();
    }
    if (worker_.joinable()) worker_.join();
    if (clock_.joinable()) clock_.join();
    if (sf_.obj) {
      sf_.unload(sf_.obj);
      sf_.close();
    }
  }

  void play() {
    std::lock_guard<std::mutex> l(m_);
    want_play_ = true;
    if (loaded_ && sf_.obj) sf_.play(sf_.obj);
  }
  void pause() {
    std::lock_guard<std::mutex> l(m_);
    want_play_ = false;
    if (loaded_ && sf_.obj) sf_.pause(sf_.obj);
  }
  void seek(int64_t ms) {
    std::lock_guard<std::mutex> l(m_);
    seek_to_ms_ = ms;
    cv_.notify_all();
  }
  void setRate(double r) {
    std::lock_guard<std::mutex> l(m_);
    if (!loaded_ || !sf_.obj || !sf_.rate) return;
    char b[64];
    snprintf(b, sizeof(b), "{\"playRate\":%.2f,\"audioOutput\":true}", r);
    sf_.rate(sf_.obj, b);
  }

  // Audio track (index into the sourceInfo audioTrackInfo list).
  bool selectAudio(int index) {
    std::lock_guard<std::mutex> l(m_);
    if (index < 0 || index >= static_cast<int>(audio_opts_.size())) return false;
    if (static_cast<size_t>(index) != audio_cur_) audio_pending_ = index;
    cv_.notify_all();
    return true;
  }

  // Reply to a drmEncrypted request (licence callback).
  bool deliver(const std::string &session, const std::string &data) {
    std::lock_guard<std::mutex> l(m_);
    if (session.compare(0, 5, "wvl1-") != 0) return false;
    replies_[session] = data;
    cv_.notify_all();
    return true;
  }

 private:
  void emit(const std::string &json) {
    if (emit_) emit_(user_, json.c_str());
  }
  void fail(const std::string &why) {
    logf("error: %s", why.c_str());
    emit("{\"error\":{\"errorCode\":-1,\"errorText\":\"legacy-webos widevine-l1: " + why + "\"}}");
  }

  static void onEvent(int type, int64_t num, const char *str, void *data) {
    static_cast<Player *>(data)->event(type, num, str);
  }
  void event(int type, int64_t num, const char *str) {
    // webOS 4 and older number the events after end-of-stream 2 lower.
    if (oldWebos() && type > 0x1c) type += 2;
    // need/enough data, frame ready, buffer full/low: too chatty to log
    if (type != 0x26 && type != 0x27 && type != 0x0 && type != 0x2c && type != 0x2d && type != 0x2e)
      logf("event 0x%x num %lld %.200s", type, (long long)num, str ? str : "");
    switch (type) {
      case 0x0:  // FRAMEREADY: num is the presented frame's pts (ns)
        frame_pts_ns_ = num;
        if (++frames_ == 1 || frames_ % 240 == 0) logf("frame %lld pts %lld ms", (long long)frames_, (long long)(num / 1000000));
        break;
      case 0x16: {  // LOADCOMPLETED
        std::lock_guard<std::mutex> l(m_);
        loaded_ = true;
        cv_.notify_all();
        break;
      }
      case 0x4: {  // video info: {"context":..,"video":{"width":..,"height":..}}
        std::string js = str ? str : "";
        std::string ctx = jsonValue(js, "context");
        size_t v = js.find("\"video\"");
        int w = v != std::string::npos ? atoi(jsonValue(js.substr(v), "width").c_str()) : 0;
        int h = v != std::string::npos ? atoi(jsonValue(js.substr(v), "height").c_str()) : 0;
        if (!ctx.empty()) {
          std::lock_guard<std::mutex> l(m_);
          media_ctx_ = ctx;
        }
        if (w > 0 && h > 0) placeVideo(w, h);
        break;
      }
      case 0x1a: emit("{\"playing\":true}"); break;
      case 0x1b: emit("{\"paused\":true}"); break;
      case 0x19:
        if (internal_seeks_ > 0) internal_seeks_--;  // audio track switch: not the app's seek
        else emit("{\"seekDone\":true}");
        break;
      case 0x1c: emit("{\"endOfStream\":true}"); break;
      case 0x12: fail("pipeline error " + std::to_string(num)); break;
      case 0x13: fail(std::string("pipeline error ") + (str ? str : "")); break;
      default: break;
    }
  }

  bool fetch(const Segment &s, std::string *out) {
    std::string range, cr, ct;
    if (s.first >= 0) range = std::to_string(s.first) + "-" + std::to_string(s.last);
    for (int attempt = 0; attempt < 3; attempt++) {
      long http = wvcompat::Host::httpGet(s.url, range, out, &cr, &ct);
      if (http == 200 || http == 206) return true;
      logf("GET %s [%s]: HTTP %ld", s.url.c_str(), range.c_str(), http);
      if (quit()) return false;
    }
    return false;
  }

  bool quit() {
    std::lock_guard<std::mutex> l(m_);
    return quit_;
  }

  // SegmentBase: the 'sidx' of every period's part.
  bool loadIndexes(Representation *rep) {
    for (size_t i = 0; i < rep->parts.size(); i++) {
      Part &pt = rep->parts[i];
      if (pt.index_first < 0 || pt.indexed) continue;
      std::string sidx;
      Segment idx{pt.init.url, pt.index_first, pt.index_last, 0, 0};
      if (!fetch(idx, &sidx) || !loadIndex(rep, sidx, static_cast<int>(i))) return false;
    }
    return true;
  }

  bool prepareTrack(Representation *rep, Track *track, std::string *pssh) {
    if (!loadIndexes(rep)) return false;
    std::string init;
    if (!fetch(rep->init, &init)) return false;
    return parseInit(init, track, pssh);
  }

  // The Track (codec setup, keys) of a representation's period part:
  // fetched once, with a licence for its key set if that is new.
  const Track *trackFor(Representation *rep, int part) {
    auto key = std::make_pair(static_cast<const Representation *>(rep), part);
    auto it = tracks_.find(key);
    if (it != tracks_.end()) return &it->second;
    if (part < 0 || part >= static_cast<int>(rep->parts.size())) return nullptr;
    Track t;
    std::string init, pssh;
    if (!fetch(rep->parts[part].init, &init) || !parseInit(init, &t, &pssh)) {
      logf("%s part %d: init segment failed", rep->id.c_str(), part);
      return nullptr;
    }
    const Track &cur = t.video ? vtrack_ : atrack_;
    if (t.codec != cur.codec) logf("%s part %d: codec %s, the pipeline has %s", rep->id.c_str(), part, t.codec.c_str(), cur.codec.c_str());
    std::string m = b64decode(rep->parts[part].pssh_b64);
    if (!addLicence(m.empty() ? pssh : m)) return nullptr;
    return &(tracks_[key] = t);
  }

  // One licence-server round trip: by URL, or through the app (licence
  // callback: the request goes out as the drmEncrypted event webOS 26 NDL
  // sends, the reply comes back through NDL_MediaSetDrmOperation ->
  // wvl1_deliver_licence()).
  bool exchange(const std::string &request, std::string *reply, const char *what) {
    std::string lic_url = jsonValue(option_, "licenseUrl");
    if (!lic_url.empty()) {
      long http = wvcompat::Host::httpPost(lic_url, request, "application/octet-stream", reply);
      if (http == 200) return true;
      fail(std::string(what) + ": licence server HTTP " + std::to_string(http));
      return false;
    }
    std::string session = "wvl1-" + std::to_string(++request_no_);
    {
      std::lock_guard<std::mutex> l(m_);
      replies_.erase(session);
    }
    emit("{\"drmEncrypted\":{\"systemId\":\"urn:dvb:casystemid:19156\",\"service\":\"" kL1Service
         "\",\"key\":\"\",\"sessionId\":\"" + session + "\",\"drmMsgType\":\"licenseRequest\",\"drmData\":\"" +
         b64encode(request) + "\",\"requestId\":" + std::to_string(request_no_) + "}}");
    logf("%s: %zu bytes to the app (%s)", what, request.size(), session.c_str());
    std::unique_lock<std::mutex> l(m_);
    if (!cv_.wait_for(l, std::chrono::seconds(30), [&] { return quit_ || replies_.count(session) != 0; }) || quit_) {
      if (!quit_) fail(std::string(what) + ": no reply from the app's licence callback");
      return false;
    }
    *reply = replies_[session];
    replies_.erase(session);
    return true;
  }

  bool licence(const std::string &pssh) {
    bool callback = jsonValue(option_, "licenseUrl").empty();
    if (callback && jsonValue(option_, "useLicenseCallback") != "true") { fail("no licenseUrl and no licence callback"); return false; }
    if (!cdm_.open()) { fail(cdm_.error()); return false; }
    logf("CDM %s, licence %s", cdm_.version(), callback ? "through the app's callback" : "by URL");
    // The CDM needs the licence server's service certificate (privacy
    // mode): ask for it with a certificate request, as browsers do.
    std::string cert;
    if (!exchange(std::string("\x08\x04", 2), &cert, "service certificate")) return false;
    if (!cdm_.setServiceCertificate(cert)) { fail("service certificate: " + cdm_.error()); return false; }
    return addLicence(pssh);
  }

  // A licence for a key set not licensed yet: key rotation ('pssh' in
  // segments), a new period, or separate audio keys. Each is its own session.
  bool addLicence(const std::string &pssh) {
    if (pssh.empty() || licensed_.count(pssh)) return true;
    std::string challenge, lic;
    if (!cdm_.generateRequest(pssh, &challenge)) { fail(cdm_.error()); return false; }
    if (!exchange(challenge, &lic, "licence")) return false;
    if (!cdm_.update(lic)) { fail(cdm_.error()); return false; }
    licensed_.insert(pssh);
    if (licensed_.size() > 1) logf("licence %zu (new key set)", licensed_.size());
    return true;
  }


  std::string loadPayload() {
    std::string app = jsonValue(option_, "appId"), win = jsonValue(option_, "windowId");
    char es[320];
    snprintf(es, sizeof(es),
             "\"esInfo\":{\"pauseAtDecodeTime\":true,\"seperatedPTS\":true,\"ptsToDecode\":0,"
             "\"videoWidth\":%u,\"videoHeight\":%u,\"videoFpsValue\":%u,\"videoFpsScale\":1}",
             vtrack_.width, vtrack_.height, fps_ ? fps_ : 30);
    std::string contents = "{\"codec\":{\"video\":\"" + vtrack_.codec + "\"";
    if (has_audio_) contents += ",\"audio\":\"" + atrack_.codec + "\"";
    contents += "},\"format\":\"RAW\",\"provider\":\"" + app + "\"," + es;
    if (has_audio_) {
      std::string ai = audioInfo(atrack_);
      if (!ai.empty()) contents += "," + ai;
      if (atrack_.atmos) contents += ",\"immersive\":\"ATMOS\"";
    }
    if (vtrack_.dv_profile >= 0 && !opt("nodv")) {
      contents += std::string(",\"DolbyHdrInfo\":{\"encryptionType\":\"") + (vtrack_.scheme.empty() ? "clear" : "all") +
                  "\",\"profileId\":" + std::to_string(vtrack_.dv_profile) + ",\"trackType\":\"" +
                  (vtrack_.dv_el ? "dual" : "single") + "\"}";
    }
    contents += "}";
    int svp = opt("svp30") || oldWebos() ? 30 : 40;  // secure video path version
    std::string p = "{\"args\":[{\"mediaTransportType\":\"BUFFERSTREAM\",\"option\":{";
    p += "\"appId\":\"" + app + "\",";
    if (!win.empty()) p += "\"windowId\":\"" + win + "\",";
    if (!has_audio_) p += "\"needAudio\":false,";
    if (!opt("nodrm")) p += "\"drm\":{\"type\":\"WIDEVINE_MODULAR\"},";
    p += "\"transmission\":{\"contentsType\":\"LIVE\",\"trickType\":\"client-side\"},\"seekMode\":\"keep-rate\",";
    // The resource calculator sizes the decoder from these (as Kodi sends).
    {
      // UHD streams need the decoder sized for UHD; 4096 covers DCI-width.
      bool uhd = vtrack_.width > 1920 || vtrack_.height > 1080;
      char as[160];
      snprintf(as, sizeof(as), "\"adaptiveStreaming\":{\"adaptiveResolution\":true,\"maxWidth\":%u,\"maxHeight\":%u,\"maxFrameRate\":%u},",
               uhd ? 4096 : 1920, uhd ? 2160 : 1080, fps_ > 30 ? 60 : 30);
      p += as;
    }
    p += "\"externalStreamingInfo\":{" + (opt("nodrm") ? std::string() : "\"svpVersion\":" + std::to_string(svp) + ",") + "\"contents\":" + contents +
         ",\"bufferingCtrInfo\":{\"preBufferByte\":0,\"bufferMinLevel\":0,\"bufferMaxLevel\":0,"
         "\"qBufferLevelVideo\":0,\"srcBufferLevelVideo\":{\"minimum\":0,\"maximum\":8388608},"
         "\"qBufferLevelAudio\":0,\"srcBufferLevelAudio\":{\"minimum\":0,\"maximum\":2097152}}}";
    p += "}}]}";
    return p;
  }

  // Feeds one buffer; waits while the pipeline is full. false: stop/seek.
  bool feedBuffer(const std::vector<uint8_t> &buf, int64_t pts_ns, int es) {
    std::string json = "{\"bufferAddr\":\"" + hexAddr(buf.data()) + "\",\"bufferSize\":" +
                       std::to_string(buf.size()) + ",\"pts\":" + std::to_string(pts_ns) +
                       ",\"esData\":" + std::to_string(es) + "}";
    for (;;) {
      {
        std::lock_guard<std::mutex> l(m_);
        if (quit_ || seek_to_ms_ >= 0) return false;
      }
      std::string r = sf_.feed(sf_.obj, json.c_str());
      if (r.find("Ok") != std::string::npos) return true;
      if (r.find("BufferFull") == std::string::npos) {
        logf("Feed (%s): %s", es == 1 ? "video" : "audio", r.c_str());
        return true;  // drop the sample
      }
      std::unique_lock<std::mutex> l(m_);
      cv_.wait_for(l, std::chrono::milliseconds(20));
    }
  }

  // Feeds one sample of a segment. false: stop/seek.
  // offset: seconds from the part's media time to presentation time.
  bool feedSample(const Track &t, const std::string &data, Sample s, bool video, bool *logged, double offset) {
    {
      const uint8_t *src = reinterpret_cast<const uint8_t *>(data.data()) + s.offset;
      int64_t pts_ns = s.pts * 1000000000LL / t.timescale + static_cast<int64_t>(offset * 1e9);
      std::vector<uint8_t> in, out;
      if (video && t.codec == "VP9") {
        in.assign(src, src + s.size);  // VP9 frames go as they are
        if (!s.encrypted && !opt("nodrm") && !t.kid.empty()) {
          s.encrypted = true;
          s.iv.assign(16, '\0');
          s.subsamples.assign(1, {static_cast<uint32_t>(in.size()), 0u});
        }
      } else if (video) {
        // H.264/HEVC: AVCC -> Annex B (4-byte lengths become start codes,
        // in the clear part of each subsample), parameter sets in front of
        // sync samples. AV1: the sequence header from 'av1C' in front.
        const std::string &cfg = t.codec == "AV1" ? t.av1_config : t.annexb_config;
        if (s.sync) in.assign(cfg.begin(), cfg.end());
        size_t pre = in.size();
        in.insert(in.end(), src, src + s.size);
        if (t.codec != "AV1" && t.nal_length_size == 4) {
          for (size_t o = pre; o + 4 <= in.size();) {
            uint32_t n = (uint32_t(in[o]) << 24) | (in[o + 1] << 16) | (in[o + 2] << 8) | in[o + 3];
            in[o] = in[o + 1] = in[o + 2] = 0;
            in[o + 3] = 1;
            o += 4 + n;
          }
        }
        if (s.encrypted && pre) s.subsamples[0].first += static_cast<uint32_t>(pre);
        // Clear samples (e.g. a clear lead) go down the secure path too: as
        // 'cenc' samples with no protected bytes, under the track's key id.
        if (!s.encrypted && !opt("nodrm") && !t.kid.empty()) {
          s.encrypted = true;
          s.iv.assign(16, '\0');
          s.subsamples.assign(1, {static_cast<uint32_t>(in.size()), 0u});
        }
      } else {
        in.assign(src, src + s.size);
      }
      CdmSample cs{in.data(), static_cast<uint32_t>(in.size()), s.encrypted, t.scheme == "cbcs" && !s.subsamples.empty() && s.subsamples[0].second,
                   s.kid.empty() ? &t.kid : &s.kid,
                   reinterpret_cast<const uint8_t *>(s.iv.data()), s.subsamples, t.crypt_blocks, t.skip_blocks,
                   pts_ns / 1000};
      const std::vector<uint8_t> *feed = &in;
      if (s.encrypted) {
        if (!cdm_.decrypt(cs, video, &out)) {
          if (!*logged) logf("%s Decrypt failed: %s (encrypted %d)", video ? "video" : "audio", cdm_.error().c_str(), s.encrypted);
          *logged = true;
          if (s.encrypted) return true;
        } else {
          feed = &out;
        }
      }
      return feedBuffer(*feed, pts_ns, video ? 1 : 2);
    }
  }

  void clockLoop() {
    int64_t last = -1;
    while (!quit()) {
      int64_t ms = -1;
      {
        std::lock_guard<std::mutex> l(m_);
        if (loaded_ && frame_pts_ns_ >= 0) ms = frame_pts_ns_ / 1000000 - origin_ms_;
        if (ms < 0) ms = 0;
      }
      if (ms >= 0 && ms != last) {
        emit("{\"currentTime\":{\"currentTime\":" + std::to_string(ms) + "}}");
        last = ms;
      }
      std::unique_lock<std::mutex> l(m_);
      cv_.wait_for(l, std::chrono::milliseconds(250));
    }
  }

  // Prepares audio track i and makes the pipeline take it (changing the
  // decoder if the codec differs, webOS 6 and later). The caller restarts
  // feeding from the current position.
  bool switchAudio(int i) {
    Representation *r = audio_opts_[i];
    Track t;
    std::string ignored;
    if (!prepareTrack(r, &t, &ignored) || t.codec.empty()) {
      logf("audio track %d (%s): unusable", i, r->codecs.c_str());
      return false;
    }
    std::string apssh = b64decode(r->pssh_b64);
    if (!addLicence(apssh.empty() ? ignored : apssh)) return false;
    // Same codec: the decoder takes the new track as it is (only a different
    // codec needs changeAudioCodec, which fails for an unchanged one).
    if (t.codec != atrack_.codec) {
      if (!sf_.change_audio) {
        logf("audio track %d: codec %s needs changeAudioCodec (webOS 6 and later)", i, t.codec.c_str());
        return false;
      }
      std::string info = "{" + audioInfo(t) + "}";
      logf("changeAudioCodec %s %s", t.codec.c_str(), info.c_str());
      if (!sf_.change_audio(sf_.obj, t.codec, info)) {
        logf("changeAudioCodec failed");
        return false;
      }
    }
    logf("audio track %d: %s %s %u ch", i, r->lang.c_str(), t.codec.c_str(), t.channels);
    atrack_ = t;
    tracks_[{r, 0}] = t;
    arep_ = r;
    audio_cur_ = i;
    return true;
  }

  // Tells the TV's video output service where to show this player's video
  // (1920x1080 screen coordinates, aspect kept, centred). Needed with an
  // in-process StarfishMediaAPIs player; repeated when the size changes.
  void placeVideo(int w, int h) {
    if (opt("noplace") || w <= 0 || h <= 0) return;
    std::string ctx;
    {
      std::lock_guard<std::mutex> l(m_);
      if (w == placed_w_ && h == placed_h_) return;
      ctx = media_ctx_;
    }
    if (ctx.empty() && sf_.media_id && sf_.obj) {
      const char *id = sf_.media_id(sf_.obj);
      if (id) ctx = id;
    }
    if (ctx.empty()) { logf("placeVideo: no media id yet"); return; }
    if (!Luna::get().open(jsonValue(option_, "appId"))) return;
    {
      std::lock_guard<std::mutex> l(m_);
      placed_w_ = w;
      placed_h_ = h;
    }
    double scale = std::min(1920.0 / w, 1080.0 / h);
    int ow = static_cast<int>(w * scale + 0.5), oh = static_cast<int>(h * scale + 0.5);
    int ox = (1920 - ow) / 2, oy = (1080 - oh) / 2;
    char p[512];
    snprintf(p, sizeof(p),
             "{\"context\":\"%s\",\"fullScreen\":false,\"originalInput\":{\"x\":0,\"y\":0,\"width\":%d,\"height\":%d},"
             "\"sourceInput\":{\"x\":0,\"y\":0,\"width\":%d,\"height\":%d},"
             "\"displayOutput\":{\"x\":%d,\"y\":%d,\"width\":%d,\"height\":%d}}",
             ctx.c_str(), w, h, w, h, ox, oy, ow, oh);
    logf("setDisplayWindow %s", p);
    Luna::get().call("luna://com.webos.service.videooutput/display/setDisplayWindow", p);
    // The TV applies the area asynchronously; the app's surface (with the
    // video widget's transparent hole) must be committed after that, or the
    // area stays black. Ask for frames when it reports the area applied
    // (as YouTube's app subscribes), and a few times anyway.
    if (!apply_subscribed_) {
      apply_subscribed_ = true;
      Luna::get().call("luna://com.webos.service.videooutput/display/getApplyDoneWindowRegion",
                       "{\"subscribe\":true,\"context\":\"" + ctx + "\"}", true);
    }
    std::thread([] {
      for (int ms : {500, 500, 1000, 2000}) {
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        Luna::requestFrame();
      }
    }).detach();
  }

  static double nowUtc() {
    return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
  }
  // Media time of segment idx (or the end of the list).
  static double nextStart(const std::vector<Segment> &segs, size_t idx) {
    if (idx < segs.size()) return segs[idx].start;
    return segs.empty() ? 0 : segs.back().start + segs.back().duration;
  }
  // First segment starting at or after t.
  static size_t firstFrom(const std::vector<Segment> &segs, double t) {
    size_t i = 0;
    while (i < segs.size() && segs[i].start < t - 0.01) i++;
    return i;
  }
  // Live: reads the manifest again and takes the new segment lists.
  bool refreshLive(Manifest *man) {
    std::string mpd, cr, ct, err;
    Manifest fresh;
    if (wvcompat::Host::httpGet(url_, "", &mpd, &cr, &ct) != 200 || !parseMpd(mpd, url_, &fresh, &err, nowUtc())) {
      logf("live manifest refresh failed %s", err.c_str());
      return false;
    }
    for (auto &r : man->reps)
      for (auto &f : fresh.reps)
        if (f.group == r.group && f.id == r.id) {
          r.segments = std::move(f.segments);
          break;
        }
    return true;
  }

  // Download speed of video segments, smoothed (bit/s).
  void measure(size_t bytes, std::chrono::steady_clock::duration d) {
    double s = std::chrono::duration<double>(d).count();
    if (s <= 0 || bytes < 100000) return;
    double bps = bytes * 8 / s;
    if (opt("slownet") && bps > 3e6) bps = 3e6;  // test: force lower quality
    speed_bps_ = speed_bps_ > 0 ? 0.7 * speed_bps_ + 0.3 * bps : bps;
  }

  // Picks the representation for the next video segment: the best one whose
  // bandwidth fits in 70% of the measured speed (the lowest if none does).
  void chooseVariant() {
    if (variants_.size() < 2 || speed_bps_ <= 0) return;
    size_t want = 0;
    for (size_t i = 0; i < variants_.size(); i++)
      if (variants_[i].rep->bandwidth <= 0.7 * speed_bps_) want = i;
    if (want == cur_) return;
    // Step up one at a time; drop straight down.
    if (want > cur_) want = cur_ + 1;
    Variant &v = variants_[want];
    if (!v.prepared) {
      std::string ignored;
      if (!prepareTrack(v.rep, &v.track, &ignored) || v.track.codec != vtrack_.codec ||
          ((v.track.codec == "H264" || v.track.codec == "H265") && v.track.nal_length_size != 4)) {
        logf("representation %s unusable, staying on %s", v.rep->id.c_str(), variants_[cur_].rep->id.c_str());
        variants_.erase(variants_.begin() + want);
        if (want < cur_) cur_--;
        return;
      }
      v.prepared = true;
      tracks_[{v.rep, 0}] = v.track;
    }
    logf("quality: %s -> %s (%ux%u, %u bit/s; measured %.0f bit/s)", variants_[cur_].rep->id.c_str(), v.rep->id.c_str(),
         v.track.width, v.track.height, v.rep->bandwidth, speed_bps_);
    cur_ = want;
    char info[96];
    snprintf(info, sizeof(info), "{\"videoInfo\":{\"width\":%u,\"height\":%u}}", v.track.width, v.track.height);
    emit(info);
  }

  void run() {
    std::string mpd, cr, ct, err;
    if (wvcompat::Host::httpGet(url_, "", &mpd, &cr, &ct) != 200) { fail("manifest download failed"); return; }
    Manifest man;
    if (!parseMpd(mpd, url_, &man, &err, nowUtc())) { fail(err); return; }
    live_ = man.dynamic;
    // Video: the highest resolution up to 2160p; at the same height HEVC,
    // then VP9, then H.264; then the highest bandwidth.
    auto usable = [&](const Representation &r) {
      int f = videoFamily(r);
      return r.content == "video" && r.height <= 2160 && (f == 1 || (f > 1 && !opt("h264")));
    };
    auto better = [&](const Representation &a, const Representation &b) {  // a better than b
      if (a.height != b.height) return a.height > b.height;
      if (videoFamily(a) != videoFamily(b)) return videoFamily(a) > videoFamily(b);
      return a.bandwidth > b.bandwidth;
    };
    Representation *vrep = nullptr;
    for (auto &r : man.reps)
      if (usable(r) && (!vrep || better(r, *vrep))) vrep = &r;
    if (!vrep) { fail("no H.264, HEVC, VP9 or AV1 (MP4) video representation"); return; }
    // Audio: one choice per AdaptationSet (its highest bandwidth supported
    // representation), listed to the app as audio tracks; AAC first by default.
    {
      std::map<int, Representation *> best;
      for (auto &r : man.reps)
        if (r.content == "audio" && audioSupported(r) && (!best[r.group] || r.bandwidth > best[r.group]->bandwidth))
          best[r.group] = &r;
      for (auto &g : best)
        if (g.second) audio_opts_.push_back(g.second);
      for (size_t i = 0; i < audio_opts_.size(); i++)
        if (audio_opts_[i]->codecs.compare(0, 4, "mp4a") == 0) { audio_cur_ = i; break; }
    }
    Representation *arep = audio_opts_.empty() ? nullptr : audio_opts_[audio_cur_];
    std::string pssh = b64decode(vrep->pssh_b64), init_pssh;
    if (!prepareTrack(vrep, &vtrack_, &init_pssh)) { fail("video init segment"); return; }
    if (vtrack_.codec != "H264" && vtrack_.codec != "H265" && vtrack_.codec != "VP9" && vtrack_.codec != "AV1") {
      fail("unsupported video codec " + vrep->codecs);
      return;
    }
    if ((vtrack_.codec == "H264" || vtrack_.codec == "H265") && vtrack_.nal_length_size != 4) {
      fail("NAL length size " + std::to_string(vtrack_.nal_length_size) + " is not supported");
      return;
    }
    fps_ = vrep->fps;
    std::string audio_init_pssh;
    has_audio_ = !opt("noaudio") && arep && prepareTrack(arep, &atrack_, &audio_init_pssh) && !atrack_.codec.empty();
    arep_ = has_audio_ ? arep : nullptr;
    if (pssh.empty()) pssh = init_pssh;
    if (pssh.empty()) { fail("no Widevine PSSH"); return; }
    logf("video %s %s %ux%u %u bit/s, %zu segments; audio %s", vrep->id.c_str(), vtrack_.codec.c_str(), vtrack_.width, vtrack_.height,
         vrep->bandwidth, vrep->segments.size(), has_audio_ ? arep->id.c_str() : "none");
    // Adaptive quality: the other representations of the same codec,
    // switched at segment boundaries by measured download speed.
    variants_.push_back({vrep, vtrack_, true});
    if (!opt("noabr")) {
      for (auto &r : man.reps)
        if (&r != vrep && usable(r) && videoFamily(r) == videoFamily(*vrep) && r.bandwidth && r.bandwidth < vrep->bandwidth)
          variants_.push_back({&r, Track(), false});
    }
    std::sort(variants_.begin(), variants_.end(),
              [](const Variant &a, const Variant &b) { return a.rep->bandwidth < b.rep->bandwidth; });
    cur_ = variants_.size() - 1;
    if (variants_.size() > 1) logf("adaptive quality: %zu representations", variants_.size());
    if (!licence(pssh)) return;
    tracks_[{vrep, 0}] = vtrack_;
    if (has_audio_) {
      // Audio may have its own key set.
      std::string apssh = b64decode(arep->pssh_b64);
      if (!addLicence(apssh.empty() ? audio_init_pssh : apssh)) return;
      tracks_[{arep, 0}] = atrack_;
    }

    std::string app = jsonValue(option_, "appId");
    if (!sf_.open(app.c_str())) { fail("StarfishMediaAPIs unavailable"); return; }
    if (sf_.foreground) sf_.foreground(sf_.obj);
    std::string payload = loadPayload();
    logf("Load %s", payload.c_str());
    if (!sf_.load(sf_.obj, payload.c_str(), &Player::onEvent, this)) { fail("StarfishMediaAPIs Load failed"); return; }
    std::string hdr = hdrPayload(vtrack_);
    if (!hdr.empty()) {
      logf("setHdrInfo %s", hdr.c_str());
      if (!sf_.set_hdr || !sf_.set_hdr(sf_.obj, hdr.c_str())) logf("setHdrInfo failed");
    }
    {
      std::unique_lock<std::mutex> l(m_);
      if (!cv_.wait_for(l, std::chrono::seconds(10), [this] { return loaded_ || quit_; }) || quit_) {
        if (!quit_) fail("pipeline did not load");
        return;
      }
    }
    size_t vi = 0, ai = 0;
    if (live_) {
      // Start a few segments behind the live edge (or the suggested delay).
      const auto &segs = variants_[cur_].rep->segments;
      if (segs.empty()) { fail("live manifest has no segments yet"); return; }
      double back = std::max(man.presentation_delay, 3 * segs.back().duration);
      double target = segs.back().start + segs.back().duration - back;
      while (vi + 1 < segs.size() && segs[vi].start < target) vi++;
      if (arep_)
        while (ai + 1 < arep_->segments.size() && arep_->segments[ai + 1].start <= segs[vi].start) ai++;
      // Position 0 is the start of the time-shift window, the duration its
      // length now (the app's seekTo stays within it).
      origin_ms_ = static_cast<int64_t>(segs.front().start * 1000);
      live_window_ms_ = static_cast<int64_t>((segs.back().start + segs.back().duration - segs.front().start) * 1000);
      logf("live: %zu segments available, starting at %.1f s (%.1f s behind the edge)", segs.size(), segs[vi].start,
           segs.back().start + segs.back().duration - segs[vi].start);
    }
    // videoInfo first: the plugin takes the size from its cache when the
    // sourceInfo arrives.
    char info[256];
    snprintf(info, sizeof(info), "{\"videoInfo\":{\"width\":%u,\"height\":%u}}", vtrack_.width, vtrack_.height);
    emit(info);
    {
      std::string tracks;
      if (has_audio_)
        for (size_t i = 0; i < audio_opts_.size(); i++)
          tracks += std::string(i ? "," : "") + "{\"language\":\"" + (audio_opts_[i]->lang.empty() ? "und" : audio_opts_[i]->lang) +
                    "\",\"trackId\":" + std::to_string(i) + "}";
      emit(std::string("{\"sourceInfo\":{\"seekable\":true,\"programInfo\":[{\"duration\":") +
           std::to_string(live_ ? static_cast<long long>(live_window_ms_) : static_cast<long long>(man.duration * 1000)) + ",\"numVideoTracks\":1,\"numAudioTracks\":" +
           std::to_string(has_audio_ ? audio_opts_.size() : 0) + ",\"audioTrackInfo\":[" + tracks + "]}]}}");
    }
    emit("{\"loadCompleted\":true}");
    placeVideo(static_cast<int>(vtrack_.width), static_cast<int>(vtrack_.height));
    {
      std::lock_guard<std::mutex> l(m_);
      if (want_play_) sf_.play(sf_.obj);
    }
    clock_ = std::thread([this] { clockLoop(); });

    // Samples of the current video and audio segments are fed interleaved in
    // decode order: a whole UHD segment (~40 MB) does not fit the pipeline's
    // video buffer, and it will not start without audio.
    struct Cursor {
      const Track *track = nullptr;
      double offset = 0;  // the segment's part: media -> presentation time
      std::string data;
      std::vector<Sample> samples;
      size_t next = 0;
      bool loaded = false;
      bool logged = false;
    } vc, ac;
    bool eos = false;
    auto load = [&](Cursor &c, size_t &idx, Representation *rep) -> bool {
      if (c.loaded && c.next >= c.samples.size()) {
        c.loaded = false;
        idx++;
      }
      bool video = &c == &vc;
      if (video && !c.loaded) {
        chooseVariant();
        rep = variants_[cur_].rep;
      }
      if (c.loaded || !rep || idx >= rep->segments.size()) return true;
      const Segment &sg = rep->segments[idx];
      const Track *t = trackFor(rep, sg.part);  // the period's init (and keys)
      if (!t) return false;
      c.data.clear();
      c.samples.clear();
      c.next = 0;
      auto t0 = std::chrono::steady_clock::now();
      if (!fetch(sg, &c.data)) return false;
      if (video) measure(c.data.size(), std::chrono::steady_clock::now() - t0);
      std::vector<std::string> psshs;
      if (!parseSegment(c.data, *t, &c.samples, &psshs)) logf("segment parse: samples beyond the data");
      for (auto &ps : psshs)
        if (!addLicence(ps)) return false;  // key rotation
      c.track = t;
      c.offset = rep->parts[sg.part].offset;
      c.loaded = true;
      return true;
    };
    for (;;) {
      int64_t seek_ms;
      int audio_switch;
      {
        std::unique_lock<std::mutex> l(m_);
        if (eos) cv_.wait_for(l, std::chrono::milliseconds(200));
        if (quit_) return;
        seek_ms = seek_to_ms_;
        seek_to_ms_ = -1;
        audio_switch = audio_pending_;
        audio_pending_ = -1;
      }
      if (seek_ms >= 0) seek_ms += origin_ms_;  // app position -> presentation time
      if (audio_switch >= 0 && has_audio_ && switchAudio(audio_switch) && seek_ms < 0) {
        // Restart from the current position with the new track.
        seek_ms = frame_pts_ns_ >= 0 ? frame_pts_ns_ / 1000000 : origin_ms_;
        internal_seeks_++;
      }
      if (seek_ms >= 0) {
        double t = seek_ms / 1000.0;
        vi = ai = 0;  // (live: within the current time-shift window)
        const auto &segs = variants_[cur_].rep->segments;
        while (vi + 1 < segs.size() && segs[vi + 1].start <= t) vi++;
        if (has_audio_)
          while (ai + 1 < arep_->segments.size() && arep_->segments[ai + 1].start <= segs[vi].start) ai++;
        vc.loaded = ac.loaded = false;
        eos = false;
        sf_.seek(sf_.obj, std::to_string(static_cast<long long>(segs[vi].start * 1000)).c_str());
        logf("seek to %lld ms: video segment %zu", (long long)seek_ms, vi);
      }
      if (eos) continue;
      if (!load(vc, vi, vrep) || !load(ac, ai, arep_)) {
        if (quit()) return;
        fail("segment, init segment or licence failed");
        return;
      }
      bool v_ready = vc.loaded && vc.next < vc.samples.size();
      bool a_ready = ac.loaded && ac.next < ac.samples.size();
      if (!v_ready && !a_ready) {
        if (vc.loaded || ac.loaded) continue;  // empty segment: load the next
        if (live_) {
          // At the live edge: wait, read the manifest again, carry on from
          // the next segment by time (the lists slide).
          double vwant = nextStart(variants_[cur_].rep->segments, vi);
          double awant = arep_ ? nextStart(arep_->segments, ai) : 0;
          {
            std::unique_lock<std::mutex> l(m_);
            cv_.wait_for(l, std::chrono::milliseconds(static_cast<int>(1000 * std::min(std::max(man.min_update, 1.0), 4.0))),
                         [this] { return quit_; });
            if (quit_) return;
          }
          if (refreshLive(&man)) {
            vi = firstFrom(variants_[cur_].rep->segments, vwant);
            if (arep_) ai = firstFrom(arep_->segments, awant);
          }
          continue;
        }
        sf_.push_eos(sf_.obj);
        eos = true;
        continue;
      }
      bool take_video = v_ready;
      if (v_ready && a_ready)
        take_video = double(vc.samples[vc.next].dts) / vc.track->timescale + vc.offset <=
                     double(ac.samples[ac.next].dts) / ac.track->timescale + ac.offset;
      Cursor &c = take_video ? vc : ac;
      if (feedSample(*c.track, c.data, c.samples[c.next], take_video, &c.logged, c.offset)) c.next++;
    }
  }

  std::string url_, option_;
  Emit emit_;
  void *user_;
  std::thread worker_, clock_;
  std::mutex m_;
  std::condition_variable cv_;
  bool quit_ = false, loaded_ = false, want_play_ = false, has_audio_ = false;
  int64_t seek_to_ms_ = -1;
  int request_no_ = 0;
  std::map<std::string, std::string> replies_;  // licence-callback replies by session
  uint32_t fps_ = 0;
  std::atomic<int64_t> frame_pts_ns_{-1};
  std::atomic<long long> frames_{0};
  Track vtrack_, atrack_;
  struct Variant {
    Representation *rep;
    Track track;
    bool prepared;
  };
  std::vector<Variant> variants_;  // by bandwidth, ascending
  std::vector<Representation *> audio_opts_;  // audio tracks offered to the app
  size_t audio_cur_ = 0;
  int audio_pending_ = -1;
  Representation *arep_ = nullptr;
  std::atomic<int> internal_seeks_{0};
  std::string media_ctx_;  // the pipeline's media id ("context" in its events)
  int placed_w_ = 0, placed_h_ = 0;
  bool apply_subscribed_ = false;
  bool live_ = false;
  int64_t origin_ms_ = 0;       // presentation time shown as position 0 (live: window start)
  int64_t live_window_ms_ = 0;  // live: time-shift window length at load
  std::map<std::pair<const Representation *, int>, Track> tracks_;  // by representation and period part
  std::set<std::string> licensed_;  // Widevine pssh boxes with a licence
  size_t cur_ = 0;
  double speed_bps_ = 0;
  Cdm cdm_;
  Starfish sf_;
};

std::mutex g_m;
Player *g_player = nullptr;

}  // namespace
}  // namespace wvl1

using wvl1::g_m;
using wvl1::g_player;

// Starts loading; events go to emit(user, json). 0 on success.
EXPORT int wvl1_load(const char *url, const char *option, void (*emit)(void *, const char *), void *user) {
  std::lock_guard<std::mutex> l(g_m);
  delete g_player;
  g_player = new wvl1::Player(url ? url : "", option ? option : "", emit, user);
  g_player->start();
  return 0;
}
EXPORT int wvl1_play(void) {
  std::lock_guard<std::mutex> l(g_m);
  if (!g_player) return -1;
  g_player->play();
  return 0;
}
EXPORT int wvl1_pause(void) {
  std::lock_guard<std::mutex> l(g_m);
  if (!g_player) return -1;
  g_player->pause();
  return 0;
}
EXPORT int wvl1_seek(long long ms) {
  std::lock_guard<std::mutex> l(g_m);
  if (!g_player) return -1;
  g_player->seek(ms);
  return 0;
}
EXPORT int wvl1_set_rate(double rate) {
  std::lock_guard<std::mutex> l(g_m);
  if (!g_player) return -1;
  g_player->setRate(rate);
  return 0;
}
// Track selection (NDL_MediaSelectTrack): audio tracks only. 0: taken.
EXPORT int wvl1_select_track(const char *type, int index) {
  std::lock_guard<std::mutex> l(g_m);
  if (!g_player || !type || strcmp(type, "audio") != 0) return -1;
  return g_player->selectAudio(index) ? 0 : -1;
}
// Licence-callback reply (base64-decoded by the caller). 0: taken.
EXPORT int wvl1_deliver_licence(const char *session, const uint8_t *data, size_t len) {
  std::lock_guard<std::mutex> l(g_m);
  if (!g_player || !session || !data) return -1;
  return g_player->deliver(session, std::string(reinterpret_cast<const char *>(data), len)) ? 0 : -1;
}
EXPORT int wvl1_unload(void) {
  std::lock_guard<std::mutex> l(g_m);
  delete g_player;
  g_player = nullptr;
  return 0;
}
