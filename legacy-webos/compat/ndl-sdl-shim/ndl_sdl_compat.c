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
//   - the few SDL functions NDL calls, answering with Flutter's window ID.
//     Window placement stays with Flutter's embedder, so the setters are no-ops.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
  return real ? real(slot) : -1;
}

EXPORT int NDL_MediaSubscribeEvents(ndl_cb cb) {
  static int (*real)(ndl_cb);
  if (!real) real = (int (*)(ndl_cb))dlsym(RTLD_NEXT, "NDL_MediaSubscribeEvents");
  int slot = g_cur_slot;
  g_user_cb[slot] = cb;
  fprintf(stderr, "[ndl-compat] NDL_MediaSubscribeEvents(slot %d) wrapped\n", slot);
  return real ? real(cb ? g_tramps[slot] : NULL) : -1;
}

// ------------------------------------------------------ DRM (webOS 26) ----
// webOS 26 NDL relays app-fetched DRM licences with NDL_MediaSetDrmOperation
// (licence-callback DRM). Older NDL does not have it, and video_player_drm is
// linked -z now, so without this symbol the app would not start. Report
// failure: the licence-callback path is unavailable, licence-URL DRM is not
// affected.
EXPORT int NDL_MediaSetDrmOperation(const char *system_id, const char *drm_data,
                                    const char *drm_msg_type, const char *service,
                                    const char *kid, const char *session_id, int request_id) {
  (void)drm_data; (void)service; (void)kid; (void)request_id;
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

EXPORT int NDL_MediaLoadWithOption(const char *url, const char *option) {
  static int (*real)(const char *, const char *);
  if (!real) real = (int (*)(const char *, const char *))dlsym(RTLD_NEXT, "NDL_MediaLoadWithOption");
  char *adapted = drm_adapt_option(url, option);
  if (!adapted) adapted = drm_adapt_callback_type(option);
  if (adapted) fprintf(stderr, "[ndl-compat] drm adapter: load option -> %.400s\n", adapted);
  int r = real ? real(url, adapted ? adapted : option) : -1;
  free(adapted);
  return r;
}
