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
