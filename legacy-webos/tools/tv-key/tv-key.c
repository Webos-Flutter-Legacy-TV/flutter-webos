// tv-key: inject key presses into an existing evdev device on the TV.
// Usage: tv-key [-d /dev/input/eventN] [-w ms] KEY [KEY...]
//   KEY = up|down|left|right|ok|back|home|<numeric linux keycode>
// Writing input_event structs to an evdev node injects them as if the device
// produced them, so the compositor/IME see a real remote button press.
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

static int code_for(const char *k) {
  static const struct { const char *n; int c; } t[] = {
      {"up", KEY_UP},       {"down", KEY_DOWN}, {"left", KEY_LEFT},
      {"right", KEY_RIGHT}, {"ok", KEY_ENTER},  {"back", 412 /* KEY_PREVIOUS */},
      {"home", KEY_HOMEPAGE}};
  for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++)
    if (!strcmp(k, t[i].n)) return t[i].c;
  return atoi(k);
}

static void emit(int fd, int type, int code, int value) {
  struct input_event ev;
  memset(&ev, 0, sizeof(ev));
  gettimeofday(&ev.time, NULL);
  ev.type = type; ev.code = code; ev.value = value;
  if (write(fd, &ev, sizeof(ev)) != sizeof(ev)) perror("write");
}

int main(int argc, char **argv) {
  const char *dev = "/dev/input/event1";
  int wait_ms = 400, i = 1;
  for (; i < argc && argv[i][0] == '-'; i += 2) {
    if (i + 1 >= argc) break;
    if (!strcmp(argv[i], "-d")) dev = argv[i + 1];
    else if (!strcmp(argv[i], "-w")) wait_ms = atoi(argv[i + 1]);
  }
  int fd = open(dev, O_WRONLY);
  if (fd < 0) { perror(dev); return 1; }
  for (; i < argc; i++) {
    int c = code_for(argv[i]);
    emit(fd, EV_KEY, c, 1); emit(fd, EV_SYN, SYN_REPORT, 0);
    usleep(60 * 1000);
    emit(fd, EV_KEY, c, 0); emit(fd, EV_SYN, SYN_REPORT, 0);
    printf("key %s (%d)\n", argv[i], c);
    usleep(wait_ms * 1000);
  }
  close(fd);
  return 0;
}
