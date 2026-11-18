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

EXPORT struct wl_egl_window *wl_egl_window_create(struct wl_surface *s, int w, int h) {
  static struct wl_egl_window *(*f)(struct wl_surface *, int, int);
  if (!f) f = real("wl_egl_window_create");
  return f(s, w, h);
}
EXPORT void wl_egl_window_destroy(struct wl_egl_window *win) {
  static void (*f)(struct wl_egl_window *);
  if (!f) f = real("wl_egl_window_destroy");
  f(win);
}
EXPORT void wl_egl_window_resize(struct wl_egl_window *win, int w, int h, int dx, int dy) {
  static void (*f)(struct wl_egl_window *, int, int, int, int);
  if (!f) f = real("wl_egl_window_resize");
  f(win, w, h, dx, dy);
}
EXPORT void wl_egl_window_get_attached_size(struct wl_egl_window *win, int *w, int *h) {
  static void (*f)(struct wl_egl_window *, int *, int *);
  if (!f) f = real("wl_egl_window_get_attached_size");
  f(win, w, h);
}
