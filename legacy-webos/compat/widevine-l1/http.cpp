// HTTP for libwv-l1 through the TV's own libcurl. See http.h.
#include "http.h"

#include <dlfcn.h>
#include <strings.h>

#include <cstring>
#include <mutex>

namespace wvl1 {
namespace http {

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

long post(const std::string &url, const std::string &body, const std::string &content_type,
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

long get(const std::string &url, const std::string &range, std::string *body,
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

}  // namespace http
}  // namespace wvl1
