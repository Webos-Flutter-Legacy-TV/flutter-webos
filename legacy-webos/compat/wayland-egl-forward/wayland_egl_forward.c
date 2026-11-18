// libwayland-egl.so.1 stand-in that forwards to the TV's own wayland-egl.
//
// LG's embedder needs libwayland-egl.so.1. Firmware differs: some webOS 6
// images ship it as libwayland-egl.so.1, others (e.g. webOS 6.5.3 on the
// OLED C1) only as the older libwayland-egl.so.0, same API. The vendor (Mali)
// implementation must be used, because the GPU driver shares the
// wl_egl_window layout with it. This library is bundled in lib/compat and
// forwards the four calls to whichever of the two exists, loaded by absolute
// path (so it is not confused with this file, which has the same soname).
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>

struct wl_egl_window;
struct wl_surface;

static void *real(const char *sym) {
  static void *h;
  if (!h) {
    static const char *paths[] = {"/usr/lib/libwayland-egl.so.1", "/usr/lib/libwayland-egl.so.0",
                                  "/lib/libwayland-egl.so.1", "/lib/libwayland-egl.so.0", NULL};
    for (int i = 0; paths[i] && !h; i++) h = dlopen(paths[i], RTLD_NOW | RTLD_LOCAL);
    if (!h) {
      fprintf(stderr, "[compat] no system libwayland-egl found\n");
      abort();
    }
  }
  void *f = dlsym(h, sym);
  if (!f) {
    fprintf(stderr, "[compat] libwayland-egl lacks %s\n", sym);
    abort();
  }
  return f;
}

#define EXPORT __attribute__((visibility("default")))

// Window table, so the shim can ask for a "twin" window on the same
// wl_surface (compat-dual-surface: two EGL surfaces, two buffer pools).
#define MAX_WIN 8
static struct { struct wl_egl_window *win, *twin; struct wl_surface *surf; int w, h; } g_win[MAX_WIN];
static int win_slot(struct wl_egl_window *win) {
  for (int i = 0; i < MAX_WIN; i++)
    if (g_win[i].win == win) return i;
  return -1;
}

static struct wl_egl_window *real_create(struct wl_surface *s, int w, int h) {
  static struct wl_egl_window *(*f)(struct wl_surface *, int, int);
  if (!f) f = real("wl_egl_window_create");
  return f(s, w, h);
}
static void real_destroy(struct wl_egl_window *win) {
  static void (*f)(struct wl_egl_window *);
  if (!f) f = real("wl_egl_window_destroy");
  f(win);
}
static void real_resize(struct wl_egl_window *win, int w, int h, int dx, int dy) {
  static void (*f)(struct wl_egl_window *, int, int, int, int);
  if (!f) f = real("wl_egl_window_resize");
  f(win, w, h, dx, dy);
}

EXPORT struct wl_egl_window *wl_egl_window_create(struct wl_surface *s, int w, int h) {
  struct wl_egl_window *win = real_create(s, w, h);
  int i = win_slot(NULL);
  fprintf(stderr, "[compat] wl_egl_window_create %dx%d -> %p\n", w, h, (void *)win);
  if (win && i >= 0) {
    g_win[i].win = win;
    g_win[i].twin = NULL;
    g_win[i].surf = s;
    g_win[i].w = w;
    g_win[i].h = h;
  }
  return win;
}
EXPORT void wl_egl_window_destroy(struct wl_egl_window *win) {
  int i = win_slot(win);
  if (i >= 0) {
    if (g_win[i].twin) real_destroy(g_win[i].twin);
    g_win[i].win = g_win[i].twin = NULL;
  }
  real_destroy(win);
}
EXPORT void wl_egl_window_resize(struct wl_egl_window *win, int w, int h, int dx, int dy) {
  int i = win_slot(win);
  if (i >= 0) {
    g_win[i].w = w;
    g_win[i].h = h;
    if (g_win[i].twin) real_resize(g_win[i].twin, w, h, dx, dy);
  }
  real_resize(win, w, h, dx, dy);
}
EXPORT void wl_egl_window_get_attached_size(struct wl_egl_window *win, int *w, int *h) {
  static void (*f)(struct wl_egl_window *, int *, int *);
  if (!f) f = real("wl_egl_window_get_attached_size");
  f(win, w, h);
}

// Second wl_egl_window on the same wl_surface as `win`, same size, created
// once; NULL for unknown or small (resource-context) windows.
EXPORT struct wl_egl_window *compat_wl_egl_window_twin(struct wl_egl_window *win) {
  int i = win_slot(win);
  if (i < 0 || g_win[i].w < 64 || g_win[i].h < 64) return NULL;
  if (!g_win[i].twin) g_win[i].twin = real_create(g_win[i].surf, g_win[i].w, g_win[i].h);
  return g_win[i].twin;
}
