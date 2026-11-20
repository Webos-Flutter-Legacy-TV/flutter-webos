// NDL media backport for webOS 6 and older, for non-SDL apps (Flutter).
//
// webOS 26's libNDL_media has NDL_MediaInitWithWindowId(), letting a non-SDL
// app hand NDL the exported video window it created itself. Older releases
// only have NDL_MediaInit(), whose implementation dlopen()s libSDL2-2.0.so.0
// and asks SDL for the current window and an exported window ID. A Flutter
// process has no SDL window, so NDL gets a null window ID and cannot play.
//
// This library is built with SONAME libSDL2-2.0.so.0 and loaded at startup,
// so NDL's dlopen() finds it instead of the system SDL. It provides:
//   - NDL_MediaInitWithWindowId(): remember Flutter's window ID, then call
//     the old NDL_MediaInit();
//   - NDL_MediaUnsubscribeEvents(): absent on old firmware; no-op;
//   - NDL_MediaMuteDisplay(): absent on webOS 4; forwarded where it exists;
//   - the few SDL functions NDL calls, answering with Flutter's window ID.
//     Window placement stays with Flutter's embedder, so the setters are no-ops.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define EXPORT __attribute__((visibility("default")))

static char g_window_id[256];

static void log_msg(const char *fmt, const char *a) {
  fprintf(stderr, "[ndl-compat] ");
  fprintf(stderr, fmt, a ? a : "(null)");
  fputc('\n', stderr);
}

// ---------------------------------------------------------------- NDL ----
EXPORT int NDL_MediaInitWithWindowId(int reserved, const char *windowId) {
  static int (*real_init)(int);
  if (!real_init) real_init = (int (*)(int))dlsym(RTLD_DEFAULT, "NDL_MediaInit");
  if (windowId) {
    strncpy(g_window_id, windowId, sizeof(g_window_id) - 1);
    g_window_id[sizeof(g_window_id) - 1] = '\0';
  }
  log_msg("NDL_MediaInitWithWindowId(windowId=%s) -> NDL_MediaInit", windowId);
  if (!real_init) {
    log_msg("NDL_MediaInit not found%s", "");
    return -1;
  }
  int r = real_init(reserved);
  fprintf(stderr, "[ndl-compat] NDL_MediaInit returned %d\n", r);
  return r;
}

EXPORT int NDL_MediaUnsubscribeEvents(void) {
  log_msg("NDL_MediaUnsubscribeEvents: not on this firmware, ignored%s", "");
  return 0;
}

// ---------------------------------------------------------------- SDL ----
typedef struct { int x, y, w, h; } SDL_Rect_compat;
static int g_fake_window;  // any non-null address stands in for an SDL_Window

EXPORT void *SDL_GL_GetCurrentWindow(void) { return &g_fake_window; }
EXPORT void *SDL_GetWindowFromID(uint32_t id) { (void)id; return &g_fake_window; }
EXPORT void SDL_GetWindowSize(void *w, int *pw, int *ph) {
  (void)w;
  if (pw) *pw = 1920;
  if (ph) *ph = 1080;
}

// Audio-only players (audioplayers) call plain NDL_MediaInit(), which still
// asks SDL for a window on old firmware and crashes on NULL. Without a
// Flutter video window, hand out a placeholder ID: audio needs no surface.
#define NO_WINDOW_ID "_compat_audio_only_"
EXPORT const char *SDL_webOSCreateExportedWindow(int type) {
  const char *id = g_window_id[0] ? g_window_id : NO_WINDOW_ID;
  fprintf(stderr, "[ndl-compat] SDL_webOSCreateExportedWindow(type=%d) -> %s\n", type, id);
  return strdup(id);
}
EXPORT int SDL_webOSSetExportedWindow(const char *id, SDL_Rect_compat *src, SDL_Rect_compat *dst) {
  (void)src;
  if (dst)
    fprintf(stderr, "[ndl-compat] SDL_webOSSetExportedWindow(%s, dst=%d,%d %dx%d) ignored\n",
            id ? id : "", dst->x, dst->y, dst->w, dst->h);
  return 1;  // SDL_TRUE
}
EXPORT int SDL_webOSExportedSetProperty(const char *id, const char *name, const char *value) {
  fprintf(stderr, "[ndl-compat] SDL_webOSExportedSetProperty(%s, %s=%s) ignored\n",
          id ? id : "", name ? name : "", value ? value : "");
  return 1;
}
EXPORT void SDL_webOSDestroyExportedWindow(const char *id) {
  log_msg("SDL_webOSDestroyExportedWindow(%s) ignored (embedder owns it)", id);
}

// ------------------------------------------------- NDL event tap (debug) ----
// Tracks the slot selected by NDL_MediaSetPlayer and wraps the callback the
// app registers with NDL_MediaSubscribeEvents, so every raw event can be
// logged (when <app dir>/compat-log-ndl exists) before being forwarded.
#include <libgen.h>
#include <time.h>
#include <unistd.h>

#define NDL_MAX_SLOTS 8
typedef void (*ndl_cb)(char *);
static ndl_cb g_user_cb[NDL_MAX_SLOTS];
static int g_cur_slot;
static int g_log_ndl = -1;

static int app_file_exists(const char *name) {
  char exe[512], path[600];
  ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
  if (n <= 0) return 0;
  exe[n] = '\0';
  snprintf(path, sizeof(path), "%s/%s", dirname(exe), name);
  return access(path, F_OK) == 0;
}

static void forward(int slot, char *msg) {
  if (g_log_ndl < 0) g_log_ndl = app_file_exists("compat-log-ndl");
  if (g_log_ndl) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    fprintf(stderr, "[ndl-event %ld.%03ld slot%d] %.200s\n", (long)ts.tv_sec, ts.tv_nsec / 1000000,
            slot, msg ? msg : "(null)");
  }
  if (g_user_cb[slot]) g_user_cb[slot](msg);
}
#define TRAMP(n) static void tramp_##n(char *m) { forward(n, m); }
TRAMP(0) TRAMP(1) TRAMP(2) TRAMP(3) TRAMP(4) TRAMP(5) TRAMP(6) TRAMP(7)
static ndl_cb g_tramps[NDL_MAX_SLOTS] = {tramp_0, tramp_1, tramp_2, tramp_3,
                                         tramp_4, tramp_5, tramp_6, tramp_7};

EXPORT int NDL_MediaSetPlayer(int slot) {
  static int (*real)(int);
  if (!real) real = (int (*)(int))dlsym(RTLD_NEXT, "NDL_MediaSetPlayer");
  if (slot >= 0 && slot < NDL_MAX_SLOTS) g_cur_slot = slot;
  // webOS <= 5 NDL has a single player and no NDL_MediaSetPlayer: accept slot 0
  // (one player at a time) and refuse others.
  return real ? real(slot) : (slot == 0 ? 0 : -1);
}

EXPORT int NDL_MediaSubscribeEvents(ndl_cb cb) {
  static int (*real)(ndl_cb);
  if (!real) real = (int (*)(ndl_cb))dlsym(RTLD_NEXT, "NDL_MediaSubscribeEvents");
  int slot = g_cur_slot;
  g_user_cb[slot] = cb;
  fprintf(stderr, "[ndl-compat] NDL_MediaSubscribeEvents(slot %d) wrapped\n", slot);
  return real ? real(cb ? g_tramps[slot] : NULL) : -1;
}

// webOS 4 NDL has no NDL_MediaMuteDisplay (video_player uses it to hide the
// video while the app is in the background). Forward to the firmware's
// function where it exists; otherwise report failure, which video_player
// only logs.
EXPORT int NDL_MediaMuteDisplay(_Bool onoff) {
  static int (*real)(_Bool);
  static int looked;
  if (!looked) { real = (int (*)(_Bool))dlsym(RTLD_NEXT, "NDL_MediaMuteDisplay"); looked = 1; }
  if (real) return real(onoff);
  fprintf(stderr, "[ndl-compat] NDL_MediaMuteDisplay(%d): not on this firmware\n", (int)onoff);
  return -1;
}

// ------------------------------------------------------ DRM (webOS 26) ----
// webOS 26 NDL relays app-fetched DRM licences with NDL_MediaSetDrmOperation
// (licence-callback DRM). Older NDL does not have it, and video_player_drm is
// linked -z now, so without this symbol the app would not start. Report
// failure: the licence-callback path is unavailable, licence-URL DRM is not
// affected.
static int widevine_deliver(const char *service, const char *session_id, const char *drm_data);
static int widevine_l1_deliver(const char *service, const char *session_id, const char *drm_data);
EXPORT int NDL_MediaSetDrmOperation(const char *system_id, const char *drm_data,
                                    const char *drm_msg_type, const char *service,
                                    const char *kid, const char *session_id, int request_id) {
  (void)kid; (void)request_id;
  // Licences for sessions of the Widevine CDM proxy go back to it.
  if (widevine_deliver(service, session_id, drm_data) == 0) return 0;
  // ... and those of the hardware (L1) route to libwv-l1.
  if (widevine_l1_deliver(service, session_id, drm_data) == 0) return 0;
  fprintf(stderr, "[ndl-compat] NDL_MediaSetDrmOperation(%s, %s, session %s): not on this firmware\n",
          system_id ? system_id : "", drm_msg_type ? drm_msg_type : "", session_id ? session_id : "");
  return -1;
}

// ------------------------------------- DRM: webOS 26 load option adapter ----
// webOS 26 NDL accepts licence settings inside the media load request:
//   "drm":{"type":"WIDEVINE","licenseUrl":"...","useLicenseCallback":false}
// Older releases ignore that and expect the DRM-client flow instead:
//   NDL_MediaDRMLoad(type) -> clientId, NDL_MediaDRMSendMessage(<licence
//   server XML>), then load with "drm":{"type":"widevine","clientId":"..."}.
// Translate the first form into the second.

static int (*real_drm_init)(void);
static int (*real_drm_load)(const char *);
static int (*real_drm_is_loaded)(int *, char **, char **);
static int (*real_drm_send)(const char *, const char *, const char *, char **, int *, char **);
static const char *(*real_drm_err)(void);

static int json_get_string(const char *obj, const char *key, char *out, size_t n) {
  char pat[64];
  snprintf(pat, sizeof(pat), "\"%s\":\"", key);
  const char *p = strstr(obj, pat);
  if (!p) return 0;
  p += strlen(pat);
  size_t i = 0;
  while (*p && *p != '"' && i + 1 < n) {
    if (*p == '\\' && p[1]) p++;
    out[i++] = *p++;
  }
  out[i] = '\0';
  return 1;
}

static void xml_escape(const char *in, char *out, size_t n) {
  size_t o = 0;
  for (; *in && o + 6 < n; in++) {
    const char *r = NULL;
    if (*in == '&') r = "&amp;"; else if (*in == '<') r = "&lt;"; else if (*in == '>') r = "&gt;";
    if (r) { size_t l = strlen(r); memcpy(out + o, r, l); o += l; } else out[o++] = *in;
  }
  out[o] = '\0';
}

// Experiment switch: an empty file compat-drm-type-<name> in the app dir forces
// the DRM type written into the load option.
static void drm_type_override(char *out, size_t n) {
  static const char *names[] = {"widevine_modular", "WIDEVINE_MODULAR", "widevine", "WIDEVINE",
                                "playready", "PLAYREADY", "playready_1.0", NULL};
  char f[64];
  for (int i = 0; names[i]; i++) {
    snprintf(f, sizeof(f), "compat-drm-type-%s", names[i]);
    if (app_file_exists(f)) { snprintf(out, n, "%s", names[i]); return; }
  }
}

// Callback mode ("useLicenseCallback":true, no licenseUrl): keep the request
// but fix the type name the old pipeline expects.
static char *drm_adapt_callback_type(const char *option) {
  const char *d = option ? strstr(option, "\"drm\":{\"type\":\"WIDEVINE\"") : NULL;
  if (!d || !strstr(d, "\"useLicenseCallback\":true")) return NULL;
  char t[48] = "widevine_modular";
  drm_type_override(t, sizeof(t));
  const char *v = d + strlen("\"drm\":{\"type\":\"");
  size_t olen = strlen(option);
  char *out = malloc(olen + 64);
  if (!out) return NULL;
  size_t pre = (size_t)(v - option);
  memcpy(out, option, pre);
  int w = sprintf(out + pre, "%s", t);
  strcpy(out + pre + w, v + strlen("WIDEVINE"));
  return out;
}

// LG's guide: unload the DRM client before the app exits. The DRM service
// keeps a client tied to the app ID across processes, and NDL can only unload
// clients its own process loaded, so a leftover client of another type would
// block the next DRM type (sendDrmMessage: "doesn't be supported in the
// activated drm").
static void drm_unload_at_exit(void) {
  int (*unload)(void) = dlsym(RTLD_DEFAULT, "NDL_MediaDRMUnload");
  if (unload) unload();
}

// Returns a malloc'd rewritten option string, or NULL to keep the original.
static char *drm_adapt_option(const char *url, const char *option) {
  const char *d = option ? strstr(option, "\"drm\":{") : NULL;
  if (!d) return NULL;
  const char *end = strchr(d, '}');
  if (!end) return NULL;
  size_t dl = (size_t)(end - d + 1);
  char drm[1024];
  if (dl >= sizeof(drm)) return NULL;
  memcpy(drm, d, dl);
  drm[dl] = '\0';
  char type[32] = "", lic[1024] = "";
  if (!json_get_string(drm, "type", type, sizeof(type)) ||
      !json_get_string(drm, "licenseUrl", lic, sizeof(lic)) || !lic[0])
    return NULL;  // not the webOS 26 licence-URL form (callback mode is separate)
  for (char *c = type; *c; c++) if (*c >= 'A' && *c <= 'Z') *c += 32;

  if (!real_drm_load) {
    real_drm_init = dlsym(RTLD_DEFAULT, "NDL_MediaDRMInit");
    real_drm_load = dlsym(RTLD_DEFAULT, "NDL_MediaDRMLoad");
    real_drm_is_loaded = dlsym(RTLD_DEFAULT, "NDL_MediaDRMIsLoaded");
    real_drm_send = dlsym(RTLD_DEFAULT, "NDL_MediaDRMSendMessage");
    real_drm_err = dlsym(RTLD_DEFAULT, "NDL_MediaDRMGetError");
  }
  if (!real_drm_init || !real_drm_load || !real_drm_is_loaded || !real_drm_send) {
    fprintf(stderr, "[ndl-compat] drm adapter: old DRM API not found\n");
    return NULL;
  }
  int r = real_drm_init();
  fprintf(stderr, "[ndl-compat] drm adapter: NDL_MediaDRMInit -> %d %s\n", r,
          r && real_drm_err ? real_drm_err() : "");
  int state = 0;
  char *ltype = NULL, *client = NULL;
  real_drm_is_loaded(&state, &ltype, &client);
  if (state && ltype && strcmp(ltype, type) != 0) {
    // The DRM client belongs to the app and outlives the process; replace a
    // client of another type (e.g. widevine left over, playready wanted).
    static int (*real_drm_unload)(void);
    if (!real_drm_unload) real_drm_unload = dlsym(RTLD_DEFAULT, "NDL_MediaDRMUnload");
    if (real_drm_unload) {
      r = real_drm_unload();
      fprintf(stderr, "[ndl-compat] drm adapter: NDL_MediaDRMUnload(%s) -> %d\n", ltype, r);
    }
    state = 0;
  }
  if (!state) {
    static int registered;
    if (!registered) { registered = 1; atexit(drm_unload_at_exit); }
    r = real_drm_load(type);
    fprintf(stderr, "[ndl-compat] drm adapter: NDL_MediaDRMLoad(%s) -> %d %s\n", type, r,
            r && real_drm_err ? real_drm_err() : "");
    real_drm_is_loaded(&state, &ltype, &client);
  }
  fprintf(stderr, "[ndl-compat] drm adapter: client state=%d type=%s clientId=%s\n", state,
          ltype ? ltype : "", client ? client : "");
  if (!state || !client || !client[0]) return NULL;

  char elic[1200], eurl[2100], msg[4096];
  const char *msg_type, *system_id;
  xml_escape(lic, elic, sizeof(elic));
  xml_escape(url ? url : "", eurl, sizeof(eurl));
  if (strcmp(type, "playready") == 0) {
    msg_type = "application/vnd.ms-playready.initiator+xml";
    system_id = "urn:dvb:casystemid:19219";
    snprintf(msg, sizeof(msg),
             "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
             "<PlayReadyInitiator xmlns=\"http://schemas.microsoft.com/DRM/2007/03/protocols/\">"
             "<LicenseServerUriOverride><LA_URL>%s</LA_URL></LicenseServerUriOverride>"
             "</PlayReadyInitiator>", elic);
  } else if (strcmp(type, "widevine") == 0) {
    msg_type = "application/widevine+xml";
    system_id = "urn:dvb:casystemid:19156";
    snprintf(msg, sizeof(msg),
             "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
             "<WidevineCredentialsInfo xmlns=\"http://www.smarttv-alliance.org/DRM/widevine/2012/protocols/\">"
             "<ContentURL>%s</ContentURL><DeviceID></DeviceID><StreamID></StreamID>"
             "<ClientIP></ClientIP><DRMServerURL>%s</DRMServerURL><DRMAckServerURL></DRMAckServerURL>"
             "<DRMHeartBeatURL></DRMHeartBeatURL><DRMHeartBeatPeriod>0</DRMHeartBeatPeriod>"
             "<UserData></UserData><Portal></Portal><StoreFront></StoreFront>"
             "<BandwidthCheckURL></BandwidthCheckURL><BandwidthCheckInterval></BandwidthCheckInterval>"
             "</WidevineCredentialsInfo>", eurl, elic);
  } else {
    return NULL;
  }
  char *msg_id = NULL, *result_msg = NULL;
  int result_code = -1;
  r = real_drm_send(msg_type, msg, system_id, &msg_id, &result_code, &result_msg);
  fprintf(stderr, "[ndl-compat] drm adapter: NDL_MediaDRMSendMessage(%s) -> %d result=%d msg=%s id=%s %s\n",
          msg_type, r, result_code, result_msg ? result_msg : "", msg_id ? msg_id : "",
          r && real_drm_err ? real_drm_err() : "");

  // Pipeline DRM type: webOS 6 reads "WIDEVINE" as Widevine Classic; CENC
  // (DASH) Widevine is "widevine_modular". compat-drm-type-<name> overrides.
  char load_type[48];
  snprintf(load_type, sizeof(load_type), "%s",
           strcmp(type, "widevine") == 0 ? "widevine_modular" : type);
  drm_type_override(load_type, sizeof(load_type));
  size_t olen = strlen(option);
  char *out = malloc(olen + 256);
  if (!out) return NULL;
  size_t pre = (size_t)(d - option);
  memcpy(out, option, pre);
  int w = sprintf(out + pre, "\"drm\":{\"type\":\"%s\",\"clientId\":\"%s\"}", load_type, client);
  strcpy(out + pre + w, end + 1);
  return out;
}

// ---- libwv-compat (loaded on demand) ----
typedef void (*wv_challenge_fn)(void *, const char *, const uint8_t *, size_t);
static const char *(*wv_proxy_url)(const char *, const char *);
static const char *(*wv_proxy_url_cb)(const char *, wv_challenge_fn, void *);
static int (*wv_deliver)(const char *, const uint8_t *, size_t);
static int wv_load(void) {
  if (wv_proxy_url) return 1;
  void *h = dlopen("libwv-compat.so", RTLD_NOW);
  if (!h) { fprintf(stderr, "[ndl-compat] widevine: %s\n", dlerror()); return 0; }
  wv_proxy_url_cb = (const char *(*)(const char *, wv_challenge_fn, void *))dlsym(h, "wvc_proxy_url_cb");
  wv_deliver = (int (*)(const char *, const uint8_t *, size_t))dlsym(h, "wvc_deliver_licence");
  wv_proxy_url = (const char *(*)(const char *, const char *))dlsym(h, "wvc_proxy_url");
  return wv_proxy_url && wv_proxy_url_cb && wv_deliver;
}

static const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static char *b64_encode(const uint8_t *in, size_t n) {
  char *out = malloc(4 * ((n + 2) / 3) + 1), *o = out;
  if (!out) return NULL;
  for (size_t i = 0; i < n; i += 3) {
    uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < n ? (uint32_t)in[i + 1] << 8 : 0) | (i + 2 < n ? in[i + 2] : 0);
    *o++ = kB64[v >> 18 & 63];
    *o++ = kB64[v >> 12 & 63];
    *o++ = i + 1 < n ? kB64[v >> 6 & 63] : '=';
    *o++ = i + 2 < n ? kB64[v & 63] : '=';
  }
  *o = '\0';
  return out;
}
static size_t b64_decode(const char *in, uint8_t *out) {
  uint32_t v = 0;
  int bits = 0;
  size_t n = 0;
  for (; *in; in++) {
    const char *p = strchr(kB64, *in);
    if (!p || *in == '\0') continue;
    v = v << 6 | (uint32_t)(p - kB64);
    if ((bits += 6) >= 8) { bits -= 8; out[n++] = (uint8_t)(v >> bits); }
  }
  return n;
}

#define WV_PROXY_SERVICE "legacy-webos-widevine-proxy"
static void forward(int slot, char *msg);

// Called by the proxy (its thread) with the CDM's licence request: deliver it
// to the app's player as the drmEncrypted event webOS 26 NDL would send.
static void widevine_on_challenge(void *user, const char *session, const uint8_t *challenge, size_t len) {
  int slot = (int)(intptr_t)user;
  char *b64 = b64_encode(challenge, len);
  if (!b64) return;
  size_t n = strlen(b64) + strlen(session) + 256;
  char *json = malloc(n);
  if (json) {
    snprintf(json, n,
             "{\"drmEncrypted\":{\"systemId\":\"urn:dvb:casystemid:19156\",\"service\":\"" WV_PROXY_SERVICE
             "\",\"key\":\"\",\"sessionId\":\"%s\",\"drmMsgType\":\"licenseRequest\",\"drmData\":\"%s\","
             "\"requestId\":1}}", session, b64);
    fprintf(stderr, "[ndl-compat] widevine: licence request (%zu bytes) to app, slot %d\n", len, slot);
    forward(slot, json);
    free(json);
  }
  free(b64);
}

static int widevine_deliver(const char *service, const char *session_id, const char *drm_data) {
  if (!service || strcmp(service, WV_PROXY_SERVICE) != 0 || !session_id || !drm_data || !wv_deliver) return -1;
  uint8_t *lic = malloc(strlen(drm_data));
  if (!lic) return -1;
  size_t n = b64_decode(drm_data, lic);
  int r = wv_deliver(session_id, lic, n);
  fprintf(stderr, "[ndl-compat] widevine: licence from app (%zu bytes) -> proxy: %d\n", n, r);
  free(lic);
  return r;
}

// Opt-in Widevine route (<app dir>/compat-widevine-cdm): webOS <= 25 cannot
// play Widevine Modular through URL playback, so serve the stream through
// libwv-compat's local decrypting proxy (the TV's CDM, software security
// level) and load the clear stream instead. Returns 1 and fills *out_url /
// *out_option (malloc'd) when it took over.
static int widevine_proxy(const char *url, const char *option, char **out_url, char **out_option) {
  if (!url || !option || !app_file_exists("compat-widevine-cdm")) return 0;
  const char *d = strstr(option, "\"drm\":{");
  if (!d) return 0;
  const char *end = strchr(d, '}');
  if (!end) return 0;
  char drm[1024], type[32] = "", lic[1024] = "";
  size_t dl = (size_t)(end - d + 1);
  if (dl >= sizeof(drm)) return 0;
  memcpy(drm, d, dl);
  drm[dl] = '\0';
  if (!json_get_string(drm, "type", type, sizeof(type)) || strcasecmp(type, "widevine") != 0)
    return 0;
  int callback = strstr(drm, "\"useLicenseCallback\":true") != NULL;
  if (!json_get_string(drm, "licenseUrl", lic, sizeof(lic))) lic[0] = '\0';
  if (!lic[0] && !callback) return 0;
  if (!wv_load()) return 0;
  const char *local = callback ? wv_proxy_url_cb(url, widevine_on_challenge, (void *)(intptr_t)g_cur_slot)
                               : wv_proxy_url(url, lic);
  if (!local) return 0;
  // Drop the drm block (and the comma before it): the stream is now clear.
  size_t olen = strlen(option);
  char *o = malloc(olen + 1);
  if (!o) return 0;
  const char *cut = d;
  if (cut > option && cut[-1] == ',') cut--;
  memcpy(o, option, (size_t)(cut - option));
  strcpy(o + (cut - option), end + 1);
  *out_url = strdup(local);
  *out_option = o;
  fprintf(stderr, "[ndl-compat] widevine: %s -> %s (CDM proxy)\n", url, local);
  return 1;
}

// Media loaded per player slot. The Wayland shim asks for this through
// compat_ndl_media_active(): while a video plane may be shown it renders to
// the window's own EGL surface only (its dual-surface mode breaks the video
// punch-through).
static int g_loaded[NDL_MAX_SLOTS];
static void mark_loaded(int r) {
  if (r == 0 && g_cur_slot >= 0 && g_cur_slot < NDL_MAX_SLOTS) g_loaded[g_cur_slot] = 1;
}
EXPORT int compat_ndl_media_active(void) {
  int n = 0;
  for (int i = 0; i < NDL_MAX_SLOTS; i++) n += g_loaded[i];
  return n;
}
// Opt-in hardware Widevine route (<app dir>/compat-widevine-l1): Widevine
// DASH with a licence URL plays through libwv-l1.so (the TV's CDM wrapper
// and its StarfishMediaAPIs player, secure video path) instead of NDL. The
// player control calls of that slot go to it, and it sends NDL-style events.
static int g_l1_slot = -1;
static struct {
  int (*load)(const char *, const char *, void (*)(void *, const char *), void *);
  int (*play)(void), (*pause)(void), (*seek)(long long), (*rate)(double), (*unload)(void);
  int (*deliver)(const char *, const uint8_t *, size_t);
  int (*select)(const char *, int);
} l1;
static int l1_active(void) { return g_l1_slot >= 0 && g_l1_slot == g_cur_slot; }
static void l1_emit(void *user, const char *json) {
  int slot = (int)(intptr_t)user;
  char *m = strdup(json);
  if (m) { forward(slot, m); free(m); }
}
static int widevine_l1(const char *url, const char *option) {
  if (!url || !option || !app_file_exists("compat-widevine-l1")) return 0;
  const char *d = strstr(option, "\"drm\":{");
  if (!d) return 0;
  char type[32] = "", lic[16] = "";
  if (!json_get_string(d, "type", type, sizeof(type)) || strcasecmp(type, "widevine") != 0) return 0;
  json_get_string(d, "licenseUrl", lic, sizeof(lic));
  if (!lic[0] && !strstr(d, "\"useLicenseCallback\":true")) return 0;
  if (!l1.load) {
    void *h = dlopen("libwv-l1.so", RTLD_NOW);
    if (!h) { fprintf(stderr, "[ndl-compat] widevine-l1: %s\n", dlerror()); return 0; }
    l1.load = (int (*)(const char *, const char *, void (*)(void *, const char *), void *))dlsym(h, "wvl1_load");
    l1.play = (int (*)(void))dlsym(h, "wvl1_play");
    l1.pause = (int (*)(void))dlsym(h, "wvl1_pause");
    l1.seek = (int (*)(long long))dlsym(h, "wvl1_seek");
    l1.rate = (int (*)(double))dlsym(h, "wvl1_set_rate");
    l1.unload = (int (*)(void))dlsym(h, "wvl1_unload");
    l1.deliver = (int (*)(const char *, const uint8_t *, size_t))dlsym(h, "wvl1_deliver_licence");
    l1.select = (int (*)(const char *, int))dlsym(h, "wvl1_select_track");
    if (!l1.load || !l1.play || !l1.pause || !l1.seek || !l1.rate || !l1.unload || !l1.deliver || !l1.select) { l1.load = NULL; return 0; }
  }
  g_l1_slot = g_cur_slot;
  fprintf(stderr, "[ndl-compat] widevine-l1: %s (slot %d) -> libwv-l1\n", url, g_l1_slot);
  return l1.load(url, option, l1_emit, (void *)(intptr_t)g_l1_slot) == 0;
}

static int widevine_l1_deliver(const char *service, const char *session_id, const char *drm_data) {
  if (!service || strcmp(service, "legacy-webos-widevine-l1") != 0 || !session_id || !drm_data || !l1.deliver) return -1;
  uint8_t *lic = malloc(strlen(drm_data) + 1);
  if (!lic) return -1;
  size_t n = b64_decode(drm_data, lic);
  int r = l1.deliver(session_id, lic, n);
  fprintf(stderr, "[ndl-compat] widevine-l1: reply from app (%zu bytes, session %s): %d\n", n, session_id, r);
  free(lic);
  return r;
}

EXPORT int NDL_MediaSelectTrack(const char *type, int index) {
  static int (*real)(const char *, int);
  if (l1_active()) return l1.select(type, index);
  if (!real) real = (int (*)(const char *, int))dlsym(RTLD_NEXT, "NDL_MediaSelectTrack");
  return real ? real(type, index) : -1;
}

EXPORT int NDL_MediaPlay(void) {
  static int (*real)(void);
  if (l1_active()) return l1.play();
  if (!real) real = (int (*)(void))dlsym(RTLD_NEXT, "NDL_MediaPlay");
  return real ? real() : -1;
}
EXPORT int NDL_MediaPause(void) {
  static int (*real)(void);
  if (l1_active()) return l1.pause();
  if (!real) real = (int (*)(void))dlsym(RTLD_NEXT, "NDL_MediaPause");
  return real ? real() : -1;
}
EXPORT int NDL_MediaSeekTo(long long position) {
  static int (*real)(long long);
  if (l1_active()) return l1.seek(position);
  if (!real) real = (int (*)(long long))dlsym(RTLD_NEXT, "NDL_MediaSeekTo");
  return real ? real(position) : -1;
}
EXPORT int NDL_MediaSetPlaybackRate(double rate) {
  static int (*real)(double);
  if (l1_active()) return l1.rate(rate);
  if (!real) real = (int (*)(double))dlsym(RTLD_NEXT, "NDL_MediaSetPlaybackRate");
  return real ? real(rate) : -1;
}

EXPORT int NDL_MediaUnload(void) {
  static int (*real)(void);
  if (!real) real = (int (*)(void))dlsym(RTLD_NEXT, "NDL_MediaUnload");
  if (g_cur_slot >= 0 && g_cur_slot < NDL_MAX_SLOTS) g_loaded[g_cur_slot] = 0;
  if (l1_active()) {
    g_l1_slot = -1;
    return l1.unload();
  }
  return real ? real() : -1;
}

EXPORT int NDL_MediaLoadWithOption(const char *url, const char *option) {
  static int (*real)(const char *, const char *);
  if (!real) real = (int (*)(const char *, const char *))dlsym(RTLD_NEXT, "NDL_MediaLoadWithOption");
  if (widevine_l1(url, option)) {
    mark_loaded(0);
    return 0;
  }
  char *wv_url = NULL, *wv_option = NULL;
  if (widevine_proxy(url, option, &wv_url, &wv_option)) {
    fprintf(stderr, "[ndl-compat] load option -> %.400s\n", wv_option);
    int r = real ? real(wv_url, wv_option) : -1;
    mark_loaded(r);
    free(wv_url);
    free(wv_option);
    return r;
  }
  char *adapted = drm_adapt_option(url, option);
  if (!adapted) adapted = drm_adapt_callback_type(option);
  if (adapted) fprintf(stderr, "[ndl-compat] drm adapter: load option -> %.400s\n", adapted);
  int r = real ? real(url, adapted ? adapted : option) : -1;
  mark_loaded(r);
  free(adapted);
  return r;
}
