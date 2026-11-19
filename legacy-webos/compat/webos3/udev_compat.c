// libudev.so.1 stand-in for COMPAT_WEBOS3=1 packages.
//
// The gamepads plugin links libudev.so.1. webOS 3 (and older) only has
// libudev.so.0 (udev 182-era; the functions used here have the same
// signatures, except that the unref functions returned void there). webOS 4+ has
// libudev.so.1. A plain forwarder with a DT_NEEDED on libudev.so.0 would
// therefore break a COMPAT_WEBOS3 package on webOS 4-6, where it would also
// hide the firmware's libudev.so.1, so this library loads libudev.so.0 if the
// firmware has it and the firmware's libudev.so.1 by path otherwise.
//
// Only the functions LG's gamepads plugin imports are forwarded. If a plugin
// starts using another one, tools/fw-compat-check.py --webos3 reports it as
// unresolved (CI).
#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>

struct udev;
struct udev_device;

static void *g_udev;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;

static void load_udev(void) {
  static const char *const candidates[] = {
      "libudev.so.0",          // webOS 1-3
      "/lib/libudev.so.1",     // webOS 4+ (this library holds the soname, so by path)
      "/usr/lib/libudev.so.1",
  };
  for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
    g_udev = dlopen(candidates[i], RTLD_NOW | RTLD_LOCAL);
    if (g_udev) return;
  }
  fprintf(stderr, "[compat] libudev: no firmware libudev.so.0 or libudev.so.1\n");
}

static void *udev_sym(const char *name) {
  pthread_once(&g_once, load_udev);
  return g_udev ? dlsym(g_udev, name) : NULL;
}

#define FORWARD(ret, name, params, args, fail)            \
  __attribute__((visibility("default"))) ret name params { \
    static ret(*real) params;                              \
    if (!real) real = (ret(*) params)udev_sym(#name);      \
    return real ? real args : fail;                        \
  }

FORWARD(struct udev *, udev_new, (void), (), NULL)
FORWARD(struct udev_device *, udev_device_new_from_subsystem_sysname,
        (struct udev *udev, const char *subsystem, const char *sysname),
        (udev, subsystem, sysname), NULL)
FORWARD(struct udev_device *, udev_device_get_parent_with_subsystem_devtype,
        (struct udev_device *dev, const char *subsystem, const char *devtype),
        (dev, subsystem, devtype), NULL)
FORWARD(const char *, udev_device_get_sysattr_value,
        (struct udev_device *dev, const char *sysattr), (dev, sysattr), NULL)

// In libudev.so.0 the unref functions return void, so their result is never
// used; both always return NULL, as libudev.so.1 does once the last
// reference is gone (callers drop their pointer anyway).
__attribute__((visibility("default"))) struct udev *udev_unref(struct udev *udev) {
  static void (*real)(struct udev *);
  if (!real) real = (void (*)(struct udev *))udev_sym("udev_unref");
  if (real) real(udev);
  return NULL;
}

__attribute__((visibility("default"))) struct udev_device *udev_device_unref(struct udev_device *dev) {
  static void (*real)(struct udev_device *);
  if (!real) real = (void (*)(struct udev_device *))udev_sym("udev_device_unref");
  if (real) real(dev);
  return NULL;
}
