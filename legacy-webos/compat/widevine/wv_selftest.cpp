// Stage-1 check for the Widevine host: load the TV's CDM, provision, get a
// licence for a test stream and list its keys. Run inside an app's devmode
// jail (the jail user has the 'se'/'kadaptor' groups the CDM needs):
//   wv-selftest <storage dir> <base64 Widevine PSSH box> <licence server URL>
#include <cstdio>
#include <string>

#include "wv_cdm.h"

static std::string b64decode(const std::string &in) {
  static const std::string tbl =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  int val = 0, bits = -8;
  for (char c : in) {
    size_t p = tbl.find(c);
    if (p == std::string::npos) continue;
    val = (val << 6) + static_cast<int>(p);
    bits += 6;
    if (bits >= 0) {
      out.push_back(static_cast<char>((val >> bits) & 0xFF));
      bits -= 8;
    }
  }
  return out;
}

static std::string hex(const std::string &s) {
  static const char *d = "0123456789abcdef";
  std::string o;
  for (unsigned char c : s) { o += d[c >> 4]; o += d[c & 15]; }
  return o;
}

int main(int argc, char **argv) {
  if (argc < 4) {
    fprintf(stderr, "usage: %s <storage dir> <pssh base64> <licence url>\n", argv[0]);
    return 2;
  }
  wvcompat::Host host(argv[1]);
  if (!host.open()) { printf("FAIL open: %s\n", host.error().c_str()); return 1; }
  printf("CDM version %s\n", host.version());
  if (!host.ensureProvisioned()) { printf("FAIL provisioning: %s\n", host.error().c_str()); return 1; }
  printf("provisioned\n");
  std::string session, challenge, licence;
  if (!host.startSession(b64decode(argv[2]), &session, &challenge)) {
    printf("FAIL session: %s\n", host.error().c_str());
    return 1;
  }
  printf("session %s, licence request %zu bytes\n", session.c_str(), challenge.size());
  long http = wvcompat::Host::httpPost(argv[3], challenge, "application/octet-stream", &licence);
  printf("licence server HTTP %ld, %zu bytes\n", http, licence.size());
  if (http != 200 || !host.updateSession(session, licence)) {
    printf("FAIL licence: %s\n", host.error().c_str());
    return 1;
  }
  std::map<std::string, int32_t> keys;
  if (!host.keyStatuses(session, &keys)) { printf("FAIL keys: %s\n", host.error().c_str()); return 1; }
  for (auto &k : keys) printf("key %s status %d\n", hex(k.first).c_str(), k.second);
  printf("PASS %zu key(s)\n", keys.size());
  host.closeSession(session);
  return 0;
}
