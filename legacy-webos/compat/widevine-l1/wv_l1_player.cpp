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

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstring>
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

// Debug options: words in <app dir>/compat-l1-opts (noaudio, nodrm, svp30).
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
      case 0x1a: emit("{\"playing\":true}"); break;
      case 0x1b: emit("{\"paused\":true}"); break;
      case 0x19: emit("{\"seekDone\":true}"); break;
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

  bool prepareTrack(Representation *rep, Track *track, std::string *pssh) {
    if (rep->index_first >= 0) {
      std::string sidx;
      Segment idx{rep->init.url, rep->index_first, rep->index_last, 0, 0};
      if (!fetch(idx, &sidx) || !loadIndex(rep, sidx)) return false;
    }
    std::string init;
    if (!fetch(rep->init, &init)) return false;
    return parseInit(init, track, pssh);
  }

  bool licence(const std::string &pssh) {
    std::string lic_url = jsonValue(option_, "licenseUrl");
    if (lic_url.empty()) { fail("only licenseUrl licences are supported"); return false; }
    if (!cdm_.open()) { fail(cdm_.error()); return false; }
    logf("CDM %s", cdm_.version());
    std::string cert;
    long http = wvcompat::Host::httpPost(lic_url, std::string("\x08\x04", 2), "application/octet-stream", &cert);
    if (http != 200 || !cdm_.setServiceCertificate(cert)) {
      fail("service certificate: HTTP " + std::to_string(http) + " " + cdm_.error());
      return false;
    }
    std::string challenge, lic;
    if (!cdm_.generateRequest(pssh, &challenge)) { fail(cdm_.error()); return false; }
    http = wvcompat::Host::httpPost(lic_url, challenge, "application/octet-stream", &lic);
    if (http != 200) { fail("licence server HTTP " + std::to_string(http)); return false; }
    if (!cdm_.update(lic)) { fail(cdm_.error()); return false; }
    return true;
  }

  std::string loadPayload() {
    std::string app = jsonValue(option_, "appId"), win = jsonValue(option_, "windowId");
    char es[320];
    snprintf(es, sizeof(es),
             "\"esInfo\":{\"pauseAtDecodeTime\":true,\"seperatedPTS\":true,\"ptsToDecode\":0,"
             "\"videoWidth\":%u,\"videoHeight\":%u,\"videoFpsValue\":%u,\"videoFpsScale\":1}",
             vtrack_.width, vtrack_.height, fps_ ? fps_ : 30);
    std::string contents = "{\"codec\":{\"video\":\"H264\"";
    if (has_audio_) contents += ",\"audio\":\"AAC\"";
    contents += "},\"format\":\"RAW\",\"provider\":\"" + app + "\"," + es;
    if (has_audio_) {
      char aac[160];
      snprintf(aac, sizeof(aac), ",\"aacInfo\":{\"channels\":%u,\"profile\":%u,\"format\":\"raw\",\"frequency\":%.3f}",
               atrack_.channels, atrack_.aac_object_type, atrack_.sample_rate / 1000.0);
      contents += aac;
    }
    contents += "}";
    int svp = opt("svp30") ? 30 : 40;
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
  bool feedSample(const Track &t, const std::string &data, Sample s, bool video, bool *logged) {
    {
      const uint8_t *src = reinterpret_cast<const uint8_t *>(data.data()) + s.offset;
      int64_t pts_ns = s.pts * 1000000000LL / t.timescale;
      std::vector<uint8_t> in, out;
      if (video) {
        // AVCC -> Annex B (4-byte lengths become start codes, in the clear
        // part of each subsample), SPS/PPS in front of sync samples.
        if (s.sync) in.assign(t.annexb_config.begin(), t.annexb_config.end());
        size_t pre = in.size();
        in.insert(in.end(), src, src + s.size);
        if (t.nal_length_size == 4) {
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
      CdmSample cs{in.data(), static_cast<uint32_t>(in.size()), s.encrypted, t.scheme == "cbcs" && !s.subsamples.empty() && s.subsamples[0].second, &t.kid,
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
        if (loaded_ && frame_pts_ns_ >= 0) ms = frame_pts_ns_ / 1000000;
      }
      if (ms >= 0 && ms != last) {
        emit("{\"currentTime\":{\"currentTime\":" + std::to_string(ms) + "}}");
        last = ms;
      }
      std::unique_lock<std::mutex> l(m_);
      cv_.wait_for(l, std::chrono::milliseconds(250));
    }
  }

  void run() {
    std::string mpd, cr, ct, err;
    if (wvcompat::Host::httpGet(url_, "", &mpd, &cr, &ct) != 200) { fail("manifest download failed"); return; }
    Manifest man;
    if (!parseMpd(mpd, url_, &man, &err)) { fail(err); return; }
    // Video: highest bandwidth H.264 up to 2160p. Audio: first AAC.
    Representation *vrep = nullptr, *arep = nullptr;
    for (auto &r : man.reps) {
      if (r.content == "video" && r.codecs.compare(0, 4, "avc1") == 0 && r.height <= 2160 &&
          (!vrep || r.bandwidth > vrep->bandwidth))
        vrep = &r;
      if (r.content == "audio" && r.codecs.compare(0, 4, "mp4a") == 0 && !arep) arep = &r;
    }
    if (!vrep) { fail("no H.264 video representation"); return; }
    std::string pssh = b64decode(vrep->pssh_b64), init_pssh;
    if (!prepareTrack(vrep, &vtrack_, &init_pssh)) { fail("video init segment"); return; }
    fps_ = vrep->fps;
    has_audio_ = !opt("noaudio") && arep && prepareTrack(arep, &atrack_, &init_pssh);
    if (pssh.empty()) pssh = init_pssh;
    if (pssh.empty()) { fail("no Widevine PSSH"); return; }
    logf("video %s %ux%u %u bit/s, %zu segments; audio %s", vrep->id.c_str(), vtrack_.width, vtrack_.height,
         vrep->bandwidth, vrep->segments.size(), has_audio_ ? arep->id.c_str() : "none");
    if (!licence(pssh)) return;

    std::string app = jsonValue(option_, "appId");
    if (!sf_.open(app.c_str())) { fail("StarfishMediaAPIs unavailable"); return; }
    if (sf_.foreground) sf_.foreground(sf_.obj);
    std::string payload = loadPayload();
    logf("Load %s", payload.c_str());
    if (!sf_.load(sf_.obj, payload.c_str(), &Player::onEvent, this)) { fail("StarfishMediaAPIs Load failed"); return; }
    {
      std::unique_lock<std::mutex> l(m_);
      if (!cv_.wait_for(l, std::chrono::seconds(10), [this] { return loaded_ || quit_; }) || quit_) {
        if (!quit_) fail("pipeline did not load");
        return;
      }
    }
    // videoInfo first: the plugin takes the size from its cache when the
    // sourceInfo arrives.
    char info[256];
    snprintf(info, sizeof(info), "{\"videoInfo\":{\"width\":%u,\"height\":%u}}", vtrack_.width, vtrack_.height);
    emit(info);
    snprintf(info, sizeof(info),
             "{\"sourceInfo\":{\"seekable\":true,\"programInfo\":[{\"duration\":%lld,\"numVideoTracks\":1,"
             "\"numAudioTracks\":%d}]}}",
             (long long)(man.duration * 1000), has_audio_ ? 1 : 0);
    emit(info);
    emit("{\"loadCompleted\":true}");
    {
      std::lock_guard<std::mutex> l(m_);
      if (want_play_) sf_.play(sf_.obj);
    }
    clock_ = std::thread([this] { clockLoop(); });

    // Samples of the current video and audio segments are fed interleaved in
    // decode order: a whole UHD segment (~40 MB) does not fit the pipeline's
    // video buffer, and it will not start without audio.
    struct Cursor {
      std::string data;
      std::vector<Sample> samples;
      size_t next = 0;
      bool loaded = false;
      bool logged = false;
    } vc, ac;
    size_t vi = 0, ai = 0;
    bool eos = false;
    auto load = [&](Cursor &c, size_t &idx, Representation *rep, const Track &t) -> bool {
      if (c.loaded && c.next >= c.samples.size()) {
        c.loaded = false;
        idx++;
      }
      if (c.loaded || !rep || idx >= rep->segments.size()) return true;
      c.data.clear();
      c.samples.clear();
      c.next = 0;
      if (!fetch(rep->segments[idx], &c.data)) return false;
      if (!parseSegment(c.data, t, &c.samples)) logf("segment parse: samples beyond the data");
      c.loaded = true;
      return true;
    };
    for (;;) {
      int64_t seek_ms;
      {
        std::unique_lock<std::mutex> l(m_);
        if (eos) cv_.wait_for(l, std::chrono::milliseconds(200));
        if (quit_) return;
        seek_ms = seek_to_ms_;
        seek_to_ms_ = -1;
      }
      if (seek_ms >= 0) {
        double t = seek_ms / 1000.0;
        vi = ai = 0;
        while (vi + 1 < vrep->segments.size() && vrep->segments[vi + 1].start <= t) vi++;
        if (has_audio_)
          while (ai + 1 < arep->segments.size() && arep->segments[ai + 1].start <= vrep->segments[vi].start) ai++;
        vc.loaded = ac.loaded = false;
        eos = false;
        sf_.seek(sf_.obj, std::to_string(static_cast<long long>(vrep->segments[vi].start * 1000)).c_str());
        logf("seek to %lld ms: video segment %zu", (long long)seek_ms, vi);
      }
      if (eos) continue;
      if (!load(vc, vi, vrep, vtrack_) || !load(ac, ai, has_audio_ ? arep : nullptr, atrack_)) {
        if (quit()) return;
        fail("segment download failed");
        return;
      }
      bool v_ready = vc.loaded && vc.next < vc.samples.size();
      bool a_ready = ac.loaded && ac.next < ac.samples.size();
      if (!v_ready && !a_ready) {
        if (vc.loaded || ac.loaded) continue;  // empty segment: load the next
        sf_.push_eos(sf_.obj);
        eos = true;
        continue;
      }
      bool take_video = v_ready;
      if (v_ready && a_ready)
        take_video = double(vc.samples[vc.next].dts) / vtrack_.timescale <= double(ac.samples[ac.next].dts) / atrack_.timescale;
      Cursor &c = take_video ? vc : ac;
      if (feedSample(take_video ? vtrack_ : atrack_, c.data, c.samples[c.next], take_video, &c.logged)) c.next++;
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
  uint32_t fps_ = 0;
  std::atomic<int64_t> frame_pts_ns_{-1};
  std::atomic<long long> frames_{0};
  Track vtrack_, atrack_;
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
EXPORT int wvl1_unload(void) {
  std::lock_guard<std::mutex> l(g_m);
  delete g_player;
  g_player = nullptr;
  return 0;
}
